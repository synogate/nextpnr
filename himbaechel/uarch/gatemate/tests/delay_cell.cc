/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2024  The Project Peppercorn Authors.
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

// Delay cells (delay_cell.cc): one CPE as a pure delay from IN6 to the output of a tap. The taps compute the identity
// (the cell's own model, and the test simulator with every other CPE input unknown), are configured as the analysis
// in docs/timing_constraints.md has them, have the delay of the timing database for the speed grade selected (also in
// the timing analysis), and work inserted before routing and after routing (before finishRoute), through the
// bitstream and a JSON round trip; removing one undoes the insertion.

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include "json_frontend.h"
#include "jsonwrite.h"
#include "logic_check.h"
#include "netlist_eval.h"
#include "testing.h"
#include "timing.h"

#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

namespace {

// The value of one input port for evaluateCell, the others unconnected
struct OneInput : CellEvalInputs
{
    IdString port;
    uint64_t pattern;
    uint64_t value(IdString p, bool *connected) const override
    {
        if (connected)
            *connected = p == port;
        return p == port ? pattern : 0;
    }
};

} // namespace

class GateMateDelayCellTest : public GateMateTest
{
  protected:
    // A chain of n registers from input d to output q (the packer puts each register's data through the LUT of its
    // CPE: the connections from one register to the next are from a register output to a LUT input)
    void build(int n)
    {
        NetInfo *clk = global_clock("clk");
        NetInfo *q = input("d");
        for (int i = 0; i < n; i++)
            q = dff(stringf("ff%d", i), q, clk)->getPort(id_Q);
        // (an output buffer, no top-level port: a routed design loaded from JSON is not packed again)
        output("q", q);
    }

    // After placement: connections from a register's output to the input of a CPE logic cell
    std::vector<std::pair<NetInfo *, PortRef>> register_connections()
    {
        std::vector<std::pair<NetInfo *, PortRef>> found;
        for (auto &n : ctx->nets) {
            NetInfo *net = n.second.get();
            if (!net->driver.cell || !net->driver.cell->type.in(id_CPE_FF, id_CPE_FF_L, id_CPE_FF_U))
                continue;
            for (auto &user : net->users)
                if (user.cell->type.in(id_CPE_L2T4, id_CPE_FF, id_CPE_FF_L, id_CPE_FF_U) && user.cell->bel != BelId()) {
                    found.emplace_back(net, user);
                    break;
                }
        }
        std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) { return a.first->name < b.first->name; });
        return found;
    }

    // The nearest CPE to `near` that takes a delay cell of `tap`
    BelId free_cpe_near(const CellInfo *near, int tap)
    {
        Loc at = ctx->getBelLocation(near->bel);
        for (int r = 1; r < 40; r++)
            for (int dx = -r; dx <= r; dx++)
                for (int dy = -r; dy <= r; dy++) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r)
                        continue;
                    BelId bel = ctx->getBelByLocation(Loc(at.x + dx, at.y + dy, CPE_LT_FULL_Z));
                    if (bel != BelId() && impl->delay_cell_fits(bel, tap))
                        return bel;
                }
        return BelId();
    }

    // Inserts delay cells of taps 0, 1, 2 on the first three register connections; the cells
    std::vector<CellInfo *> insert_three()
    {
        auto conns = register_connections();
        std::vector<CellInfo *> cells;
        if (conns.size() < 3) {
            ADD_FAILURE() << "only " << conns.size() << " register connections";
            return cells;
        }
        for (int tap = 0; tap < 3; tap++) {
            auto [net, user] = conns.at(tap);
            BelId bel = free_cpe_near(user.cell, tap);
            if (bel == BelId()) {
                ADD_FAILURE() << "no free CPE for tap " << tap;
                continue;
            }
            CellInfo *cell = impl->insert_delay_cell(net, user, bel, tap);
            if (!cell) {
                ADD_FAILURE() << "insert_delay_cell failed for tap " << tap;
                continue;
            }
            cells.push_back(cell);
        }
        return cells;
    }

    // Everything of the netlist and the routing, for comparing before and after
    std::map<std::string, std::string> state()
    {
        std::map<std::string, std::string> s;
        for (auto &net : ctx->nets)
            for (auto &w : net.second->wires)
                s[net.first.str(ctx) + " " + ctx->nameOfWire(w.first)] = stringf(
                        "%s %d", w.second.pip == PipId() ? "-" : ctx->nameOfPip(w.second.pip), int(w.second.strength));
        for (auto &cell : ctx->cells) {
            s[cell.first.str(ctx) + "@"] = cell.second->bel == BelId() ? "-" : ctx->nameOfBel(cell.second->bel);
            for (auto &port : cell.second->ports)
                s[cell.first.str(ctx) + "." + port.first.str(ctx)] =
                        port.second.net ? port.second.net->name.str(ctx) : "-";
            for (auto &param : cell.second->params)
                s[cell.first.str(ctx) + "#" + param.first.str(ctx)] = param.second.to_string();
        }
        for (auto &net : ctx->nets)
            s["net " + net.first.str(ctx)] = stringf("%d users", int(net.second->users.entries()));
        return s;
    }

    int differences(const std::map<std::string, std::string> &before, const std::map<std::string, std::string> &after)
    {
        int n = 0;
        for (auto &entry : before)
            if (!after.count(entry.first) || after.at(entry.first) != entry.second)
                if (n++ < 10)
                    ADD_FAILURE() << entry.first << ": " << entry.second << " -> "
                                  << (after.count(entry.first) ? after.at(entry.first) : "(gone)");
        for (auto &entry : after)
            if (!before.count(entry.first))
                if (n++ < 10)
                    ADD_FAILURE() << entry.first << ": (new) " << entry.second;
        return n;
    }

    // The words of the configuration file for the CPE of `bel`, by name without the CPE prefix
    std::map<std::string, std::string> cpe_words(const std::string &cfg_file, BelId bel)
    {
        std::ifstream in(cfg_file);
        const auto *tile = impl->tile_extra_data(bel.tile);
        std::string header = stringf(".tile %d %d %d", tile->die, tile->bit_x, tile->bit_y);
        std::string prefix = stringf("CPE%d.", tile->prim_id);
        std::map<std::string, std::string> words;
        std::string line;
        bool in_tile = false;
        while (std::getline(in, line)) {
            if (!line.empty() && line[0] == '.') {
                in_tile = line.rfind(header, 0) == 0 && line.size() <= header.size() + 1;
                continue;
            }
            if (!in_tile || line.rfind(prefix, 0) != 0)
                continue;
            auto space = line.find(' ');
            words[line.substr(prefix.size(), space - prefix.size())] = line.substr(space + 1);
        }
        return words;
    }

    // The checks of a routed and finished design with the three delay cells, against the logic of `ref` (taken before
    // they were inserted) and the configuration written to `cfg_file`
    void check_finished(const LogicSnapshot &ref, const std::vector<IdString> &cells, const std::string &cfg_file)
    {
        EXPECT_TRUE(ctx->checkRoutedDesign());
        LogicCheckResult res = checkLogic(ctx, ref);
        EXPECT_EQ(res.mismatched, 0) << res.summary();
        EXPECT_EQ(res.unproven, 0) << res.summary();
        EXPECT_GT(res.proven, 0);
        for (IdString name : cells) {
            ASSERT_TRUE(ctx->cells.count(name)) << name.str(ctx);
            CellInfo *cell = ctx->cells.at(name).get();
            int tap = impl->delay_cell_tap(cell);
            SCOPED_TRACE(stringf("tap %d", tap));
            ASSERT_GE(tap, 0);
            // (finishRoute leaves the delay cell as it is)
            EXPECT_EQ(cell->ports.size(), 2u);
            NetInfo *in = cell->getPort(id_IN6), *out = cell->getPort(impl->delay_cell_output(tap));
            ASSERT_NE(in, nullptr);
            ASSERT_NE(out, nullptr);
            EXPECT_TRUE(in->wires.count(ctx->getNetinfoSinkWire(in, PortRef{cell, id_IN6}, 0)));
            EXPECT_TRUE(out->wires.count(ctx->getNetinfoSourceWire(out)));
            auto words = cpe_words(cfg_file, cell->bel);
            // The tables as configured; L02 inverted where the route to IN6 inverts
            EXPECT_TRUE(words["INIT_L02"] == "1100" || words["INIT_L02"] == "0011") << words["INIT_L02"];
            EXPECT_EQ(words["INIT_L11"], tap == 0 ? "1010" : "0000");
            EXPECT_EQ(words["INIT_L20"], tap == 0 ? "1100" : "0000");
            EXPECT_EQ(words["C_FUNCTION"], tap == 0 ? "000" : "010");
            if (tap == 0) {
                EXPECT_EQ(words["C_O1"], "11"); // COMB1OUT, from the routing
            } else if (tap == 1) {
                EXPECT_EQ(words["C_O2"], "11"); // COMB2OUT, from the routing
            } else {
                EXPECT_EQ(words["C_O2"], "11"); // the cell's
                EXPECT_EQ(words["C_O1"], "10"); // CPOUT1, from the routing
                EXPECT_EQ(words["C_SELX"], "1");
                EXPECT_EQ(words["C_SEL_C"], "0");
                EXPECT_EQ(words["C_CX_I"], "1");
                EXPECT_EQ(words["C_HORIZ"], "1");
                EXPECT_EQ(words["C_C_P"], "1");
            }
            DelayQuad delay, expected;
            ASSERT_TRUE(ctx->getCellDelay(cell, id_IN6, impl->delay_cell_output(tap), delay));
            ASSERT_TRUE(impl->delay_cell_delay(tap, expected));
            EXPECT_EQ(delay.maxDelay(), expected.maxDelay());
        }
    }

    // A JSON round trip of the routed design: the delay cells come back with bel, ports and parameters, the routing
    // with them, and routing again (a design routed before) finishes it
    void check_json_round_trip(const std::vector<IdString> &cell_ids, const std::vector<std::string> &opts)
    {
        // (by name: the new context has IdStrings of its own)
        std::vector<std::string> names;
        for (IdString id : cell_ids)
            names.push_back(id.str(ctx));
        auto before = state();
        std::ostringstream out;
        std::string filename = "delay.json";
        ASSERT_TRUE(write_json_file(out, filename, ctx));
        init_context("CCGM1A1", opts);
        std::istringstream in(out.str());
        ASSERT_TRUE(parse_json(in, filename, ctx));
        EXPECT_TRUE(impl->any_delay_cells);
        std::vector<IdString> cells;
        for (auto &name : names) {
            ASSERT_TRUE(ctx->cells.count(ctx->id(name))) << name;
            cells.push_back(ctx->id(name));
        }
        auto after = state();
        int n = 0;
        for (auto &name : names)
            for (auto &entry : before)
                if (entry.first.rfind(name, 0) == 0 &&
                    (!after.count(entry.first) || after.at(entry.first) != entry.second) && n++ < 10)
                    ADD_FAILURE() << entry.first << ": " << entry.second << " -> "
                                  << (after.count(entry.first) ? after.at(entry.first) : "(gone)");
        // the routing of the nets at the cells
        for (IdString name : cells) {
            CellInfo *cell = ctx->cells.at(name).get();
            EXPECT_TRUE(impl->is_delay_cell(cell));
            for (auto &port : cell->ports)
                for (auto &entry : before)
                    if (port.second.net && entry.first.rfind(port.second.net->name.str(ctx) + " ", 0) == 0 &&
                        (!after.count(entry.first) || after.at(entry.first) != entry.second) && n++ < 10)
                        ADD_FAILURE() << entry.first << ": " << entry.second << " -> "
                                      << (after.count(entry.first) ? after.at(entry.first) : "(gone)");
        }
        EXPECT_EQ(n, 0);
        EXPECT_TRUE(ctx->checkRoutedDesign());
        for (IdString name : cells) {
            CellInfo *cell = ctx->cells.at(name).get();
            DelayQuad delay;
            EXPECT_TRUE(ctx->getCellDelay(cell, id_IN6, impl->delay_cell_output(impl->delay_cell_tap(cell)), delay));
        }
        ctx->route();
        EXPECT_TRUE(ctx->checkRoutedDesign());
        for (IdString name : cells)
            EXPECT_TRUE(ctx->cells.count(name));
    }
};

// Each tap gives out what comes in on IN6: in the cell's model for the logic check (evaluateCell, other inputs 0), and
// in the test simulator with every other input of the CPE unknown (unconnected); not so with a table changed (the
// check can fail)
TEST_F(GateMateDelayCellTest, taps_compute_the_identity)
{
    init_context();
    // (a net with a driver: the simulator takes an undriven one as unknown)
    NetInfo *x = input("x");
    NetlistEvaluator eval(ctx);
    for (int tap = 0; tap < GateMateImpl::delay_cell_tap_count; tap++) {
        SCOPED_TRACE(stringf("tap %d", tap));
        CellInfo *cell = ctx->createCell(ctx->idf("delay%d", tap), impl->delay_cell_type);
        impl->configure_delay_cell(cell, tap);
        cell->connectPort(id_IN6, x);
        IdString out = impl->delay_cell_output(tap);
        EXPECT_EQ(impl->delay_cell_tap(cell), tap);
        OneInput inputs;
        inputs.port = id_IN6;
        inputs.pattern = 0x123456789abcdef0ULL;
        CellEvalResult result;
        ASSERT_TRUE(impl->evaluateCell(cell, inputs, result));
        ASSERT_TRUE(result.outputs.count(out));
        EXPECT_EQ(result.outputs.at(out), inputs.pattern);
        for (Tri v : {Tri::F, Tri::T})
            EXPECT_EQ(eval.eval_cell(cell, out, {{x, v}}), v) << tri_str(v);
        // (a table changed: the output is no longer IN6, or unknown)
        cell->params[id_INIT_L02] = Property(0b1010, 4);
        EXPECT_NE(eval.eval_cell(cell, out, {{x, Tri::T}}), Tri::T);
        CellEvalResult broken;
        ASSERT_TRUE(impl->evaluateCell(cell, inputs, broken));
        EXPECT_NE(broken.outputs.at(out), inputs.pattern);
    }
}

// The configuration of each tap is the one of the analysis (docs/timing_constraints.md), word by word
TEST_F(GateMateDelayCellTest, configuration_is_the_analysed_one)
{
    init_context();
    // width and value of each parameter
    std::map<std::string, std::string> common = {
            {"INIT_L00", "0000"}, {"INIT_L01", "0000"}, {"INIT_L02", "1100"}, {"INIT_L03", "0000"}, {"INIT_L10", "0000"},
    };
    std::vector<std::map<std::string, std::string>> taps = {
            {{"C_FUNCTION", "000"}, {"INIT_L11", "1010"}, {"INIT_L20", "1100"}},
            {{"C_FUNCTION", "010"}, {"INIT_L11", "0000"}, {"INIT_L20", "0000"}},
            {{"C_FUNCTION", "010"},
             {"INIT_L11", "0000"},
             {"INIT_L20", "0000"},
             {"C_O2", "11"},
             {"C_SELX", "1"},
             {"C_SEL_C", "0"},
             {"C_CX_I", "1"},
             {"C_HORIZ", "1"},
             {"C_C_P", "1"}},
    };
    const char *outputs[] = {"OUT1", "OUT2", "CPOUT1"};
    for (int tap = 0; tap < GateMateImpl::delay_cell_tap_count; tap++) {
        SCOPED_TRACE(stringf("tap %d", tap));
        CellInfo *cell = ctx->createCell(ctx->idf("delay%d", tap), impl->delay_cell_type);
        // (configured from another tap first: nothing of that stays)
        impl->configure_delay_cell(cell, (tap + 1) % 3);
        impl->configure_delay_cell(cell, tap);
        auto expected = common;
        for (auto &p : taps[tap])
            expected[p.first] = p.second;
        std::map<std::string, std::string> got;
        for (auto &p : cell->params)
            got[p.first.str(ctx)] = p.second.to_string(); // (most significant bit first)
        EXPECT_EQ(got, expected);
        std::set<std::string> ports;
        for (auto &p : cell->ports)
            ports.insert(p.first.str(ctx) + (p.second.type == PORT_IN ? " in" : " out"));
        EXPECT_EQ(ports, (std::set<std::string>{"IN6 in", std::string(outputs[tap]) + " out"}));
    }
}

// The delays are the sums of the timing database's entries the analysis names, for the speed grade selected: longer
// with each tap, and in each tap from the best to the worst corner
TEST_F(GateMateDelayCellTest, delays_follow_the_speed_grade)
{
    init_context();
    auto entry = [&](const char *name) {
        const auto *sp = reinterpret_cast<const GateMateSpeedGradeExtraDataPOD *>(ctx->speed_grade->extra_data.get());
        for (const auto &t : sp->timings)
            if (IdString(t.name) == ctx->id(name))
                return DelayQuad(t.delay.fast_min, t.delay.fast_max, t.delay.slow_min, t.delay.slow_max);
        ADD_FAILURE() << "no timing entry " << name;
        return DelayQuad(0);
    };
    CellInfo *cells[3];
    for (int tap = 0; tap < 3; tap++) {
        cells[tap] = ctx->createCell(ctx->idf("delay%d", tap), impl->delay_cell_type);
        impl->configure_delay_cell(cells[tap], tap);
    }
    std::vector<std::vector<DelayQuad>> corners;
    for (int mode : {1, 2, 3}) {
        SCOPED_TRACE(stringf("timing mode %d", mode));
        impl->select_speed_grade(mode, 3);
        auto taps = impl->delay_cell_taps();
        ASSERT_EQ(taps.size(), 3u);
        std::vector<DelayQuad> expected = {entry("timing__ARBLUT_IN6_OUT1"), entry("timing__ADDF2Y1_IN6_OUT2"),
                                           entry("timing__ADDF2X_IN6_OUT2")};
        expected[2] += entry("timing_comb12_COUTX");
        expected[2] += entry("timing_CPOUT_cpout");
        expected[2] += entry("timing_cpout_OUT1");
        for (int tap = 0; tap < 3; tap++) {
            SCOPED_TRACE(stringf("tap %d", tap));
            EXPECT_EQ(taps[tap].rise.min_delay, expected[tap].rise.min_delay);
            EXPECT_EQ(taps[tap].rise.max_delay, expected[tap].rise.max_delay);
            EXPECT_EQ(taps[tap].fall.min_delay, expected[tap].fall.min_delay);
            EXPECT_EQ(taps[tap].fall.max_delay, expected[tap].fall.max_delay);
            EXPECT_GT(taps[tap].minDelay(), 0);
            EXPECT_LT(taps[tap].minDelay(), taps[tap].maxDelay());
            if (tap > 0) {
                EXPECT_GT(taps[tap].minDelay(), taps[tap - 1].minDelay());
                EXPECT_GT(taps[tap].maxDelay(), taps[tap - 1].maxDelay());
            }
            DelayQuad cell_delay;
            ASSERT_TRUE(ctx->getCellDelay(cells[tap], id_IN6, impl->delay_cell_output(tap), cell_delay));
            EXPECT_EQ(cell_delay.minDelay(), taps[tap].minDelay());
            EXPECT_EQ(cell_delay.maxDelay(), taps[tap].maxDelay());
            int count = 0;
            EXPECT_EQ(ctx->getPortTimingClass(cells[tap], id_IN6, count), TMG_COMB_INPUT);
            EXPECT_EQ(ctx->getPortTimingClass(cells[tap], impl->delay_cell_output(tap), count), TMG_COMB_OUTPUT);
        }
        corners.push_back(taps);
    }
    for (int tap = 0; tap < 3; tap++) {
        EXPECT_LT(corners[0][tap].maxDelay(), corners[1][tap].maxDelay());
        EXPECT_LT(corners[1][tap].maxDelay(), corners[2][tap].maxDelay());
        EXPECT_LT(corners[0][tap].minDelay(), corners[1][tap].minDelay());
        EXPECT_LT(corners[1][tap].minDelay(), corners[2][tap].minDelay());
    }
}

// On a register-to-register path through a delay cell the analysis takes the tap's delay: the longest path its
// maximum, the shortest (hold) its minimum, for the speed grade selected
TEST_F(GateMateDelayCellTest, analysis_sees_the_tap_delay)
{
    for (int tap = 0; tap < 3; tap++) {
        SCOPED_TRACE(stringf("tap %d", tap));
        init_context();
        build(3);
        ASSERT_TRUE(packs());
        ASSERT_TRUE(ctx->place());
        auto conns = register_connections();
        ASSERT_FALSE(conns.empty());
        auto [net, user] = conns.front();
        CellInfo *cell = impl->insert_delay_cell(net, user, free_cpe_near(user.cell, tap), tap);
        ASSERT_NE(cell, nullptr);
        ctx->assignArchInfo();
        std::vector<delay_t> seen_max;
        for (int mode : {1, 3}) {
            SCOPED_TRACE(stringf("timing mode %d", mode));
            impl->select_speed_grade(mode, 3);
            DelayQuad expected;
            ASSERT_TRUE(impl->delay_cell_delay(tap, expected));
            TimingAnalyser tmg(ctx);
            tmg.setup_only = false;
            tmg.setup();
            int through = 0;
            for (auto &ep : tmg.get_endpoint_timings()) {
                for (bool longest : {true, false}) {
                    CriticalPath path = tmg.get_endpoint_path(ep, longest);
                    for (auto &seg : path.segments)
                        if (seg.type == CriticalPath::Segment::Type::LOGIC && seg.to.first == cell->name) {
                            EXPECT_EQ(seg.from.second, id_IN6);
                            EXPECT_EQ(seg.delay, longest ? expected.maxDelay() : expected.minDelay());
                            through++;
                        }
                }
            }
            EXPECT_GE(through, 2) << "no path through the delay cell";
            seen_max.push_back(expected.maxDelay());
        }
        EXPECT_LT(seen_max[0], seen_max[1]);
    }
}

// Situation (i): delay cells inserted after placement, before routing; router2 routes them, finishRoute and the
// bitstream take them as they are, the logic is the reference's, and a JSON round trip keeps them
TEST_F(GateMateDelayCellTest, inserted_before_routing)
{
    std::string cfg = write_temp_file("before.cfg", "");
    std::vector<std::string> opts = {"--allow-unconstrained", "--out=" + cfg};
    init_context("CCGM1A1", opts);
    build(8);
    ASSERT_TRUE(packs());
    ASSERT_TRUE(ctx->place());
    LogicSnapshot ref = LogicSnapshot::take(ctx);
    auto inserted = insert_three();
    ASSERT_EQ(inserted.size(), 3u);
    std::vector<IdString> names;
    for (CellInfo *cell : inserted)
        names.push_back(cell->name);
    // (the cell takes the whole CPE, and no bridge goes through it)
    for (CellInfo *cell : inserted) {
        EXPECT_FALSE(impl->delay_cell_fits(cell->bel, 0));
        EXPECT_TRUE(ctx->isBelLocationValid(cell->bel));
    }
    ASSERT_TRUE(ctx->route());
    check_finished(ref, names, cfg);
    check_json_round_trip(names, opts);
}

// Situation (ii): delay cells inserted into the finished routing before finishRoute, as a hold fixer does: the route
// to the sink is released, the two new connections are routed with route_ripped, then finishRoute and the bitstream
TEST_F(GateMateDelayCellTest, inserted_after_routing)
{
    std::string cfg = write_temp_file("after.cfg", "");
    std::vector<std::string> opts = {"--allow-unconstrained", "--out=" + cfg};
    init_context("CCGM1A1", opts);
    build(8);
    ASSERT_TRUE(packs());
    ASSERT_TRUE(ctx->place());
    LogicSnapshot ref = LogicSnapshot::take(ctx);
    ctx->defer_route_finish = true;
    ASSERT_TRUE(ctx->route());
    ASSERT_TRUE(ctx->route_finish_pending);
    ASSERT_TRUE(ctx->checkRoutedDesign());
    auto conns = register_connections();
    ASSERT_GE(conns.size(), 3u);
    // The sink wires of the connections, routed now
    std::vector<WireId> sinks;
    for (auto &c : conns)
        sinks.push_back(ctx->getNetinfoSinkWire(c.first, c.second, 0));
    auto inserted = insert_three();
    ASSERT_EQ(inserted.size(), 3u);
    std::vector<IdString> names;
    for (int i = 0; i < 3; i++) {
        CellInfo *cell = inserted[i];
        names.push_back(cell->name);
        // the old route to the sink is released, the new connections are not routed yet
        EXPECT_EQ(ctx->getBoundWireNet(sinks[i]), nullptr);
        NetInfo *in = cell->getPort(id_IN6);
        EXPECT_FALSE(in->wires.count(ctx->getNetinfoSinkWire(in, PortRef{cell, id_IN6}, 0)));
    }
    EXPECT_FALSE(ctx->checkRoutedDesign());
    ASSERT_TRUE(impl->route_ripped({}));
    for (CellInfo *cell : inserted) {
        // The CPE's C_O2 multiplexer stays free for a cell of tap 2 (its path runs through OUT2 unrouted)
        if (impl->delay_cell_tap(cell) != 2)
            continue;
        for (PipId pip : ctx->getPipsUphill(ctx->getWireByName(
                     IdStringList::concat(ctx->getBelName(cell->bel)[0], ctx->id("CPE.OUT2_int")))))
            EXPECT_FALSE(impl->checkPipAvail(pip)) << ctx->nameOfPip(pip);
    }
    ctx->finishRoute();
    check_finished(ref, names, cfg);
    check_json_round_trip(names, opts);
}

// remove_delay_cell undoes insert_delay_cell exactly: netlist, routing (restored, as nothing took its wires) and a
// timing exception naming the split net as an end point
TEST_F(GateMateDelayCellTest, insert_then_remove_restores_the_design)
{
    init_context();
    build(6);
    ASSERT_TRUE(packs());
    ASSERT_TRUE(ctx->place());
    ctx->defer_route_finish = true;
    ASSERT_TRUE(ctx->route());
    auto conns = register_connections();
    ASSERT_GE(conns.size(), 3u);
    for (int tap = 0; tap < 3; tap++) {
        SCOPED_TRACE(stringf("tap %d", tap));
        auto [net, user] = conns.at(tap);
        TimingException exc;
        exc.multiplier = 2;
        exc.to.push_back(TimingPathPoint{TimingPathPoint::NET, net->name, IdString()});
        ctx->timing_exceptions = {exc};
        auto before = state();
        CellInfo *cell = impl->insert_delay_cell(net, user, free_cpe_near(user.cell, tap), tap);
        ASSERT_NE(cell, nullptr);
        IdString out = cell->getPort(impl->delay_cell_output(tap))->name;
        // The end point is on the cell's output net now: the exception names that too
        ASSERT_EQ(ctx->timing_exceptions.at(0).to.size(), 2u);
        EXPECT_EQ(ctx->timing_exceptions.at(0).to.at(1).name, out);
        EXPECT_EQ(user.cell->getPort(user.port)->name, out);
        EXPECT_TRUE(impl->remove_delay_cell(cell));
        EXPECT_EQ(differences(before, state()), 0);
        ASSERT_EQ(ctx->timing_exceptions.at(0).to.size(), 1u);
        EXPECT_EQ(ctx->timing_exceptions.at(0).to.at(0).name, net->name);
        EXPECT_TRUE(ctx->checkRoutedDesign());
    }
    // Removed after its connections were routed: the netlist and the old route come back, the cell's routing goes
    auto [net, user] = conns.at(0);
    auto before = state();
    CellInfo *cell = impl->insert_delay_cell(net, user, free_cpe_near(user.cell, 2), 2);
    ASSERT_NE(cell, nullptr);
    ASSERT_TRUE(impl->route_ripped({}));
    EXPECT_TRUE(impl->remove_delay_cell(cell));
    EXPECT_EQ(differences(before, state()), 0);
    EXPECT_TRUE(ctx->checkRoutedDesign());
}

// A delay cell keeps its CPE to itself: no other cell may go into it, the placer's check says so, and the CPE is
// marked used (no bridge through it)
TEST_F(GateMateDelayCellTest, delay_cell_takes_the_whole_cpe)
{
    init_context();
    build(3);
    ASSERT_TRUE(packs());
    ASSERT_TRUE(ctx->place());
    auto conns = register_connections();
    ASSERT_FALSE(conns.empty());
    auto [net, user] = conns.front();
    BelId bel = free_cpe_near(user.cell, 1);
    ASSERT_NE(bel, BelId());
    Loc loc = ctx->getBelLocation(bel);
    // (the FF bel of the CPE is valid for a register before)
    CellInfo *ff = ctx->createCell(ctx->id("extra_ff"), id_CPE_FF);
    BelId ff_bel = ctx->getBelByLocation(Loc(loc.x, loc.y, CPE_FF_L_Z));
    ctx->bindBel(ff_bel, ff, STRENGTH_WEAK);
    EXPECT_TRUE(ctx->isBelLocationValid(ff_bel));
    EXPECT_FALSE(impl->delay_cell_fits(bel, 1));
    ctx->unbindBel(ff_bel);
    CellInfo *cell = impl->insert_delay_cell(net, user, bel, 1);
    ASSERT_NE(cell, nullptr);
    EXPECT_EQ(ctx->getBelLocation(cell->bel).z, CPE_LT_FULL_Z);
    EXPECT_TRUE(ctx->isBelLocationValid(cell->bel));
    ctx->bindBel(ff_bel, ff, STRENGTH_WEAK);
    EXPECT_FALSE(ctx->isBelLocationValid(ff_bel));
    ctx->unbindBel(ff_bel);
    EXPECT_EQ(ctx->getBelBucketForCellType(impl->delay_cell_type), ctx->getBelBucketForBel(cell->bel));
    EXPECT_TRUE(ctx->isValidBelForCellType(impl->delay_cell_type, cell->bel));
    EXPECT_FALSE(ctx->isValidBelForCellType(impl->delay_cell_type, ctx->getBelByLocation(Loc(loc.x, loc.y, CPE_LT_U_Z))));
    // (the pip masks are recomputed when next needed)
    for (PipId pip : ctx->getPipsUphill(ctx->getWireByName(
                 IdStringList::concat(ctx->getBelName(bel)[0], ctx->id("CPE.MUXOUT_int")))))
        EXPECT_FALSE(impl->checkPipAvail(pip)) << ctx->nameOfPip(pip);
    EXPECT_TRUE(impl->used_cpes[bel.tile]);
    // The GUI shows it as a whole CPE: labels of its LUT2s, attributes, a drawing
    impl->update_gui_labels();
    EXPECT_FALSE(impl->gui_labels(cell->bel).empty());
    EXPECT_FALSE(impl->getBelAttrs(cell->bel).empty());
    std::vector<GraphicElement> g;
    impl->drawBel(g, GraphicElement::STYLE_ACTIVE, id_CPE_LT_FULL, ctx->getBelLocation(cell->bel));
    EXPECT_FALSE(g.empty());
}
