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

// The hold fix (hold_fix.cc): delay cells on the connections below the hold margin in any hold corner, within the
// setup slack of the routed grade; nothing changed where nothing is below the margin; undone where it fails.

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include "json_frontend.h"
#include "jsonwrite.h"
#include "logic_check.h"
#include "testing.h"
#include "timing.h"

#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

class GateMateHoldFixTest : public GateMateTest
{
  protected:
    NetInfo *clk = nullptr;

    void sdc(const std::string &text)
    {
        std::istringstream in(text);
        ctx->read_sdc(in);
    }

    // A chain of n inverters
    NetInfo *chain(const std::string &name, int n, NetInfo *src)
    {
        for (int i = 0; i < n; i++)
            src = lut1(name + std::to_string(i), 0b01, src);
        return src;
    }

    // A shift register of n registers (ff0..), one clock at 100 MHz named sys
    void shift_register(int n)
    {
        clk = global_clock("clk");
        NetInfo *q = input("d");
        for (int i = 0; i < n; i++)
            q = dff(stringf("ff%d", i), q, clk)->getPort(id_Q);
        output("q", q);
        sdc("create_clock -name sys -period 10 [get_nets {clk$y}]\n");
    }

    // Packed, placed and routed with the finish deferred (the routing repairs and the hold fix run in repairRoute)
    void route_deferred()
    {
        ASSERT_TRUE(packs());
        ASSERT_TRUE(ctx->place());
        ctx->defer_route_finish = true;
        ASSERT_TRUE(ctx->route());
    }

    // The corners the hold fix covers: the hold corners and the routed grade
    std::vector<std::pair<int, int>> corners()
    {
        auto list = impl->hold_corners;
        std::pair<int, int> routed(impl->timing_mode, impl->fpga_mode);
        if (std::find(list.begin(), list.end(), routed) == list.end())
            list.push_back(routed);
        return list;
    }

    struct CornerHold
    {
        std::string grade;
        delay_t worst = std::numeric_limits<delay_t>::max();
        std::set<std::string> below; // end points below the margin
    };

    // Per corner the worst hold slack over the end points and those below `margin_ns`
    std::vector<CornerHold> hold_by_corner(float margin_ns)
    {
        int tm = impl->timing_mode, fm = impl->fpga_mode;
        std::vector<CornerHold> result;
        for (auto [t, f] : corners()) {
            impl->select_speed_grade(t, f);
            TimingAnalyser tmg(ctx);
            tmg.setup_only = false;
            tmg.with_clock_skew = true;
            tmg.setup();
            CornerHold ch;
            ch.grade = IdString(ctx->speed_grade->name).str(ctx);
            for (auto &ep : tmg.get_endpoint_timings()) {
                if (ep.hold_slack == std::numeric_limits<delay_t>::max())
                    continue;
                ch.worst = std::min(ch.worst, ep.hold_slack);
                if (ep.hold_slack < ctx->getDelayFromNS(margin_ns))
                    ch.below.insert(ep.endpoint.cell.str(ctx) + "." + ep.endpoint.port.str(ctx));
            }
            result.push_back(ch);
        }
        impl->select_speed_grade(tm, fm);
        return result;
    }

    // The setup timing of the routed grade: per end point (and launch clock) the setup slack, Fmax per clock
    std::map<std::string, delay_t> setup_slacks()
    {
        TimingAnalyser tmg(ctx);
        tmg.setup_only = false;
        tmg.with_clock_skew = true;
        tmg.setup();
        std::map<std::string, delay_t> slacks;
        for (auto &ep : tmg.get_endpoint_timings()) {
            std::string key = ep.endpoint.cell.str(ctx) + "." + ep.endpoint.port.str(ctx) + " " +
                              ep.clock_pair.start.clock.str(ctx) + std::to_string(int(ep.clock_pair.start.edge));
            slacks[key] = slacks.count(key) ? std::min(slacks.at(key), ep.setup_slack) : ep.setup_slack;
        }
        return slacks;
    }
    // Every end point's setup slack kept, or at least half the guard (what a lengthened path keeps)
    void expect_setup_kept(const std::map<std::string, delay_t> &before)
    {
        for (auto &s : setup_slacks())
            EXPECT_GE(s.second, std::min(before.at(s.first), ctx->getDelayFromNS(0.05))) << s.first;
    }
    std::map<std::string, float> fmax()
    {
        TimingAnalyser tmg(ctx);
        tmg.with_clock_skew = true;
        tmg.setup(false, false, true);
        std::map<std::string, float> f;
        for (auto &c : tmg.get_timing_result().clock_fmax)
            f[c.first.str(ctx)] = c.second.achieved;
        return f;
    }

    std::vector<CellInfo *> delay_cells()
    {
        std::vector<CellInfo *> cells;
        for (auto &cell : ctx->cells)
            if (impl->is_delay_cell(cell.second.get()))
                cells.push_back(cell.second.get());
        return cells;
    }

    // The non-delay cell at the start of the chain of delay cells a delay cell is in
    const CellInfo *chain_driver(const CellInfo *cell)
    {
        while (impl->is_delay_cell(cell)) {
            const NetInfo *in = cell->getPort(id_IN6);
            cell = in ? in->driver.cell : nullptr;
        }
        return cell;
    }

    // Everything placement and routing decided (bels, routing with strengths) and the netlist's connections
    std::string implementation()
    {
        std::map<std::string, std::string> lines;
        for (auto &cell : ctx->cells) {
            lines["cell " + cell.first.str(ctx)] = cell.second->bel == BelId() ? "-" : ctx->nameOfBel(cell.second->bel);
            for (auto &port : cell.second->ports)
                lines["port " + cell.first.str(ctx) + "." + port.first.str(ctx)] =
                        port.second.net ? port.second.net->name.str(ctx) : "-";
        }
        for (auto &net : ctx->nets) {
            std::map<std::string, std::string> wires;
            for (auto &w : net.second->wires)
                wires[ctx->nameOfWire(w.first)] =
                        (w.second.pip == PipId() ? std::string("-") : std::string(ctx->nameOfPip(w.second.pip))) + "/" +
                        std::to_string(int(w.second.strength));
            std::string text;
            for (auto &w : wires)
                text += w.first + "<" + w.second + " ";
            lines["net " + net.first.str(ctx)] = text;
        }
        std::string all;
        for (auto &l : lines)
            all += l.first + ": " + l.second + "\n";
        return all;
    }

    std::set<std::string> leftovers(GateMateImpl::HoldFixReason reason)
    {
        std::set<std::string> found;
        for (auto &l : impl->hold_fix_report.leftovers)
            if (l.reason == reason)
                found.insert(l.cell.str(ctx) + "." + l.port.str(ctx));
        return found;
    }
    std::set<std::string> all_leftovers()
    {
        std::set<std::string> found;
        for (auto &l : impl->hold_fix_report.leftovers)
            found.insert(l.cell.str(ctx) + "." + l.port.str(ctx));
        return found;
    }

    // The clock net of the registers after packing, whose constraint the analysis takes
    NetInfo *register_clock()
    {
        for (auto &cell : ctx->cells)
            if (cell.second->type.in(id_CPE_FF, id_CPE_FF_U, id_CPE_FF_L))
                for (IdString port : {id_CLK, id_CLK_INT})
                    if (cell.second->getPort(port) && cell.second->getPort(port)->clkconstr)
                        return cell.second->getPort(port);
        return nullptr;
    }
    // A new period for the clock of the registers (after routing: placement and routing stay)
    void set_period(delay_t period)
    {
        NetInfo *net = register_clock();
        ASSERT_NE(net, nullptr);
        net->clkconstr->period = DelayPair(period);
        net->clkconstr->high = DelayPair(period / 2);
        net->clkconstr->low = DelayPair(period - period / 2);
    }

    // Every end point at or above the margin in every corner, or among the reported leftovers
    void expect_margin_met(float margin_ns)
    {
        auto left = all_leftovers();
        for (auto &ch : hold_by_corner(margin_ns))
            for (auto &ep : ch.below)
                EXPECT_TRUE(left.count(ep)) << ep << " below the margin in " << ch.grade << " and not reported";
    }

    std::vector<std::string> margin_opts(const std::string &margin, std::vector<std::string> more = {})
    {
        more.insert(more.begin(), {"--allow-unconstrained", "--hold-margin=" + margin});
        return more;
    }
};

TEST_F(GateMateHoldFixTest, options)
{
    EXPECT_TRUE(impl->hold_fix);
    EXPECT_FLOAT_EQ(impl->hold_fix_margin, 0.1f);
    EXPECT_EQ(impl->hold_fix_max_cells, 8);
    init_context("CCGM1A1", {"--allow-unconstrained", "--hold-fix=0", "--hold-margin=0.4", "--hold-fix-max-cells=3",
                             "--hold-fix-radius=2", "--hold-fix-setup-guard=0.2"});
    EXPECT_FALSE(impl->hold_fix);
    EXPECT_FLOAT_EQ(impl->hold_fix_margin, 0.4f);
    EXPECT_EQ(impl->hold_fix_max_cells, 3);
    EXPECT_EQ(impl->hold_fix_radius, 2);
    EXPECT_FLOAT_EQ(impl->hold_fix_setup_guard, 0.2f);
    EXPECT_LOG_ERROR(init_context("CCGM1A1", {"--allow-unconstrained", "--hold-fix-max-cells=0"}),
                     "hold-fix-max-cells must be at least 1");
}

// Nothing below the margin: no cell, and the same routing as without the pass
TEST_F(GateMateHoldFixTest, nothing_below_the_margin_changes_nothing)
{
    init_context("CCGM1A1", {"--allow-unconstrained", "--hold-fix=0"});
    shift_register(6);
    place_and_route();
    std::string without = implementation();

    init_context();
    shift_register(6);
    std::string log;
    {
        LogCapture capture;
        place_and_route();
        log = capture.str();
    }
    auto &report = impl->hold_fix_report;
    ASSERT_TRUE(report.ran);
    for (size_t c = 0; c < report.below_before.size(); c++)
        ASSERT_EQ(report.below_before[c], 0) << "precondition: below the margin in " << report.corners[c];
    EXPECT_NE(log.find("Hold fix: nothing below the margin"), std::string::npos);
    EXPECT_TRUE(delay_cells().empty());
    EXPECT_EQ(implementation(), without);
}

// A shift register with a margin above its hold slacks: delay cells, every end point meets the margin in every corner,
// setup stays met, finishRoute, the logic check against the design before, the bitstream and a JSON round trip
TEST_F(GateMateHoldFixTest, shift_register_meets_a_large_margin)
{
    std::string cfg = write_temp_file("shift.cfg", "");
    std::vector<std::string> opts = margin_opts("1.5", {"--out=" + cfg});
    init_context("CCGM1A1", opts);
    shift_register(8);
    ASSERT_TRUE(packs());
    ASSERT_TRUE(ctx->place());
    LogicSnapshot ref = LogicSnapshot::take(ctx);
    ctx->defer_route_finish = true;
    ASSERT_TRUE(ctx->route());
    auto before = hold_by_corner(1.5);
    ASSERT_FALSE(before.front().below.empty()) << "precondition: nothing below the margin";
    ctx->repairRoute();
    auto &report = impl->hold_fix_report;
    int cells = int(delay_cells().size());
    EXPECT_GT(cells, 0);
    int by_tap = 0;
    for (int n : report.cells_by_tap)
        by_tap += n;
    EXPECT_EQ(by_tap, cells);
    EXPECT_TRUE(ctx->checkRoutedDesign());
    expect_margin_met(1.5);
    for (auto &f : fmax())
        EXPECT_GE(f.second, 100.0f) << f.first;
    for (auto &[ep, slack] : setup_slacks())
        EXPECT_GT(slack, 0) << ep;

    std::vector<std::string> names;
    for (CellInfo *cell : delay_cells())
        names.push_back(cell->name.str(ctx));
    ctx->finishRoute();
    EXPECT_TRUE(ctx->checkRoutedDesign());
    LogicCheckResult res = checkLogic(ctx, ref);
    EXPECT_EQ(res.mismatched, 0) << res.summary();
    EXPECT_EQ(res.unproven, 0) << res.summary();
    EXPECT_GT(res.proven, 0);
    std::ifstream written(cfg);
    std::string text((std::istreambuf_iterator<char>(written)), std::istreambuf_iterator<char>());
    EXPECT_GT(text.size(), 100u);

    // JSON round trip: the cells come back, routing again (a design routed before) skips the pass
    std::ostringstream out;
    std::string filename = "shift.json";
    ASSERT_TRUE(write_json_file(out, filename, ctx));
    init_context("CCGM1A1", opts);
    std::istringstream in(out.str());
    ASSERT_TRUE(parse_json(in, filename, ctx));
    for (auto &name : names)
        EXPECT_TRUE(impl->is_delay_cell(ctx->cells.count(ctx->id(name)) ? ctx->cells.at(ctx->id(name)).get()
                                                                         : nullptr))
                << name;
    EXPECT_TRUE(ctx->checkRoutedDesign());
    std::string log;
    {
        LogCapture capture;
        ctx->route();
        log = capture.str();
    }
    EXPECT_NE(log.find("Hold fix skipped: the design was routed before"), std::string::npos);
    EXPECT_EQ(int(delay_cells().size()), cells);
    EXPECT_TRUE(ctx->checkRoutedDesign());
}

// The corners: a margin between the worst hold slack of the routed grade and that of the best corner is nothing to
// do with hold-corners=routed, and gives cells with all corners, after which all corners meet it
TEST_F(GateMateHoldFixTest, margin_between_corners)
{
    init_context("CCGM1A1", {"--allow-unconstrained", "--hold-fix=0"});
    shift_register(8);
    route_deferred();
    auto measured = hold_by_corner(0);
    // (corners: best, typ, worst; the routed one is worst_spd)
    delay_t routed = measured.back().worst, least = routed;
    for (auto &ch : measured)
        least = std::min(least, ch.worst);
    ASSERT_LT(least + ctx->getDelayFromNS(0.05), routed) << "precondition: the corners' hold slacks differ";
    float margin = ctx->getDelayNS((least + routed) / 2);
    std::string m = stringf("%.3f", margin);

    init_context("CCGM1A1", margin_opts(m, {"--hold-corners=routed"}));
    shift_register(8);
    route_deferred();
    ctx->repairRoute();
    EXPECT_TRUE(delay_cells().empty());
    EXPECT_EQ(impl->hold_fix_report.corners.size(), 1u);

    init_context("CCGM1A1", margin_opts(m));
    shift_register(8);
    route_deferred();
    ctx->repairRoute();
    EXPECT_FALSE(delay_cells().empty());
    EXPECT_EQ(impl->hold_fix_report.corners.size(), 3u);
    EXPECT_TRUE(ctx->checkRoutedDesign());
    expect_margin_met(margin);
}

// Setup budget: with the clock made so tight after routing that the worst path keeps less than the guard, its end
// point gets no cell and is reported; the worst setup slack and Fmax stay as they were
TEST_F(GateMateHoldFixTest, no_setup_budget_is_reported_and_setup_kept)
{
    init_context("CCGM1A1", margin_opts("3", {"--route-repair=0", "--place-repair=0", "--hold-corners=routed"}));
    shift_register(8);
    ctx->settings[ctx->id("timing/allowFail")] = true;
    route_deferred();
    // The period: the longest path's delay and 0.03 ns
    TimingAnalyser tmg(ctx);
    tmg.setup_only = false;
    tmg.with_clock_skew = true;
    tmg.setup();
    delay_t longest = 0;
    std::string worst;
    for (auto &ep : tmg.get_endpoint_timings())
        if (ep.clock_pair.start.clock == ep.clock_pair.end.clock && ep.clock_pair.start.clock != IdString() &&
            ep.delay > longest) {
            longest = ep.delay;
            worst = ep.endpoint.cell.str(ctx) + "." + ep.endpoint.port.str(ctx);
        }
    set_period(longest + ctx->getDelayFromNS(0.03));
    auto setup_before = setup_slacks();
    auto fmax_before = fmax();
    auto below = hold_by_corner(3).back().below;
    ASSERT_TRUE(below.count(worst)) << "precondition: the worst setup path needs hold delay";
    ctx->repairRoute();
    EXPECT_TRUE(leftovers(GateMateImpl::HoldFixReason::NO_BUDGET).count(worst)) << worst;
    auto setup_after = setup_slacks();
    delay_t worst_before = std::numeric_limits<delay_t>::max(), worst_after = worst_before;
    for (auto &s : setup_before)
        worst_before = std::min(worst_before, s.second);
    for (auto &s : setup_after)
        worst_after = std::min(worst_after, s.second);
    EXPECT_EQ(worst_after, worst_before);
    EXPECT_EQ(fmax(), fmax_before);
    // every path lengthened keeps the guard (less what the routing estimate can be off by)
    expect_setup_kept(setup_before);
    EXPECT_TRUE(ctx->checkRoutedDesign());
}

class GateMateHoldFixPairTest : public GateMateHoldFixTest
{
  protected:
    CellInfo *from = nullptr, *to = nullptr;

    // One register to register path through one inverter
    void pair(float period_ns)
    {
        clk = global_clock("clk");
        from = dff("from", input("a"), clk);
        to = dff("to", lut1("x", 0b01, from->getPort(id_Q)), clk);
        output("q", to->getPort(id_Q));
        sdc(stringf("create_clock -name sys -period %g [get_nets {clk$y}]\n", period_ns));
    }

    delay_t hold_slack_to()
    {
        TimingAnalyser tmg(ctx);
        tmg.setup_only = false;
        tmg.with_clock_skew = true;
        tmg.setup();
        delay_t slack = std::numeric_limits<delay_t>::max();
        for (auto &ep : tmg.get_endpoint_timings())
            if (ep.endpoint == CellPortKey(to->name, id_DIN))
                slack = std::min(slack, ep.hold_slack);
        return slack;
    }
};

// A setup multicycle without -hold on a short path at a fast clock: the hold check one period later needs a chain of
// cells (routed corner only: the cells' delay grows faster from best to worst than the two periods of setup do); with
// the matching -hold 1 there is nothing to do
TEST_F(GateMateHoldFixPairTest, multicycle_needs_a_chain)
{
    init_context("CCGM1A1", {"--allow-unconstrained", "--hold-corners=routed"});
    pair(6);
    sdc("set_multicycle_path 2 -setup -from [get_cells from] -to [get_cells to]\n"
        "set_multicycle_path 1 -hold -from [get_cells from] -to [get_cells to]\n");
    route_deferred();
    ctx->repairRoute();
    EXPECT_TRUE(delay_cells().empty());

    init_context("CCGM1A1", {"--allow-unconstrained", "--hold-corners=routed"});
    pair(6);
    sdc("set_multicycle_path 2 -setup -from [get_cells from] -to [get_cells to]\n");
    route_deferred();
    ASSERT_LT(hold_slack_to(), -ctx->getDelayFromNS(3)) << "precondition: a need of a few nanoseconds";
    auto setup_before = setup_slacks();
    ctx->repairRoute();
    auto cells = delay_cells();
    EXPECT_GE(cells.size(), 2u);
    for (CellInfo *cell : cells)
        EXPECT_EQ(chain_driver(cell), from) << cell->name.c_str(ctx);
    EXPECT_GE(hold_slack_to(), ctx->getDelayFromNS(0.1));
    expect_setup_kept(setup_before);
    EXPECT_TRUE(ctx->checkRoutedDesign());
    ctx->finishRoute();
    EXPECT_TRUE(ctx->checkRoutedDesign());
}

// Between the two edges of a clock with a setup multicycle: the hold check lies half a period after the launch edge;
// met, or reported with the end point's need
TEST_F(GateMateHoldFixPairTest, opposite_edges)
{
    for (bool falling_launch : {true, false}) {
        SCOPED_TRACE(falling_launch ? "falling to rising" : "rising to falling");
        init_context("CCGM1A1", {"--allow-unconstrained", "--hold-corners=routed"});
        pair(4);
        (falling_launch ? from : to)->params[id_CLK_INV] = Property(1, 1);
        sdc("set_multicycle_path 2 -setup -from [get_cells from] -to [get_cells to]\n");
        ctx->settings[ctx->id("timing/allowFail")] = true;
        route_deferred();
        delay_t before = hold_slack_to();
        ASSERT_LT(before, 0) << "precondition: a hold violation";
        auto setup_before = setup_slacks();
        ctx->repairRoute();
        delay_t after = hold_slack_to();
        auto left = all_leftovers();
        if (after < ctx->getDelayFromNS(0.1)) {
            ASSERT_TRUE(left.count("to.DIN"));
            for (auto &l : impl->hold_fix_report.leftovers)
                if (l.cell == to->name) {
                    EXPECT_EQ(l.slack, after);
                    EXPECT_EQ(l.need, ctx->getDelayFromNS(0.1) - after);
                }
        } else {
            EXPECT_FALSE(delay_cells().empty());
            EXPECT_FALSE(left.count("to.DIN"));
        }
        expect_setup_kept(setup_before);
        EXPECT_TRUE(ctx->checkRoutedDesign());
    }
}

// False paths are left alone: the path of multicycle_needs_a_chain (its hold check fails by nanoseconds) gets no cell
// and is no leftover under set_false_path or set_false_path -hold, and the finished routing has no hold violation.
// Under set_false_path -setup the hold check stays and is repaired, with no setup slack to keep.
TEST_F(GateMateHoldFixPairTest, false_paths_are_left_alone)
{
    const std::string multicycle = "set_multicycle_path 2 -setup -from [get_cells from] -to [get_cells to]\n";
    for (std::string option : {"", " -hold"}) {
        SCOPED_TRACE("set_false_path" + option);
        init_context("CCGM1A1", {"--allow-unconstrained", "--hold-corners=routed"});
        pair(6);
        sdc(multicycle + "set_false_path" + option + " -from [get_cells from] -to [get_cells to]\n");
        route_deferred();
        EXPECT_EQ(hold_slack_to(), std::numeric_limits<delay_t>::max());
        ctx->repairRoute();
        EXPECT_TRUE(delay_cells().empty());
        EXPECT_TRUE(all_leftovers().empty());
        ctx->finishRoute();
        EXPECT_TRUE(ctx->timing_result.min_delay_violations.empty());
        EXPECT_TRUE(ctx->checkRoutedDesign());
    }

    init_context("CCGM1A1", {"--allow-unconstrained", "--hold-corners=routed"});
    pair(6);
    sdc(multicycle + "set_false_path -setup -from [get_cells from] -to [get_cells to]\n");
    route_deferred();
    ASSERT_LT(hold_slack_to(), -ctx->getDelayFromNS(3)) << "precondition: a need of a few nanoseconds";
    ctx->repairRoute();
    EXPECT_GE(delay_cells().size(), 2u);
    EXPECT_GE(hold_slack_to(), ctx->getDelayFromNS(0.1));
    EXPECT_TRUE(all_leftovers().empty());
    EXPECT_TRUE(ctx->checkRoutedDesign());
}

// The chain cap: a need beyond hold-fix-max-cells cells is reported (as a probably missing -hold), not filled
TEST_F(GateMateHoldFixPairTest, chain_cap_reports_a_missing_hold_multicycle)
{
    init_context("CCGM1A1", {"--allow-unconstrained", "--hold-corners=routed", "--hold-fix-max-cells=1"});
    pair(5);
    sdc("set_multicycle_path 2 -setup -from [get_cells from] -to [get_cells to]\n");
    ctx->settings[ctx->id("timing/allowFail")] = true;
    route_deferred();
    std::string log;
    {
        LogCapture capture;
        ctx->repairRoute();
        log = capture.str();
    }
    EXPECT_TRUE(delay_cells().empty());
    ASSERT_TRUE(leftovers(GateMateImpl::HoldFixReason::CHAIN_CAP).count("to.DIN")) << log;
    for (auto &l : impl->hold_fix_report.leftovers)
        if (l.cell == to->name)
            EXPECT_TRUE(l.missing_hold_multicycle);
    EXPECT_NE(log.find("set_multicycle_path -hold' is probably missing"), std::string::npos) << log;
    EXPECT_NE(log.find("more than 1 delay cells give"), std::string::npos) << log;
}

// One end point reached from two launch registers: from `a` with a setup multicycle (no -hold: it needs delay), from
// `b` through a chain made tight after routing (no setup slack). The connection they share has no budget: the cells
// go upstream on a's path, and b's path keeps its setup slack.
TEST_F(GateMateHoldFixTest, shared_connection_padded_upstream)
{
    init_context("CCGM1A1", {"--allow-unconstrained", "--hold-corners=routed", "--route-repair=0", "--place-repair=0"});
    clk = global_clock("clk");
    CellInfo *a = dff("a", input("x"), clk);
    CellInfo *b = dff("b", input("y"), clk);
    NetInfo *mix = lut2("mix", 0b0110, a->getPort(id_Q), chain("slow", 4, b->getPort(id_Q)));
    CellInfo *e = dff("e", mix, clk);
    output("q", e->getPort(id_Q));
    sdc("create_clock -name sys -period 10 [get_nets {clk$y}]\n"
        "set_multicycle_path 2 -setup -from [get_cells a] -to [get_cells e]\n");
    ctx->settings[ctx->id("timing/allowFail")] = true;
    route_deferred();
    // b's path (the longest into e) with 0.03 ns of setup slack
    TimingAnalyser tmg(ctx);
    tmg.setup_only = false;
    tmg.with_clock_skew = true;
    tmg.setup();
    delay_t b_delay = 0;
    for (auto &ep : tmg.get_endpoint_timings())
        if (ep.endpoint == CellPortKey(e->name, id_DIN) && ep.clock_pair.start.clock != IdString())
            b_delay = std::max(b_delay, ep.delay);
    ASSERT_GT(b_delay, 0);
    set_period(b_delay + ctx->getDelayFromNS(0.03));
    auto setup_before = setup_slacks();
    ctx->repairRoute();
    auto cells = delay_cells();
    // padded on a's path only (or reported)
    for (CellInfo *cell : cells)
        EXPECT_EQ(chain_driver(cell), a) << cell->name.c_str(ctx);
    if (cells.empty())
        EXPECT_TRUE(all_leftovers().count("e.DIN"));
    else
        EXPECT_FALSE(all_leftovers().count("e.DIN"));
    // setup of every end point: kept, or at least the guard
    for (auto &s : setup_slacks())
        EXPECT_GE(s.second, std::min(setup_before.at(s.first), ctx->getDelayFromNS(0.05))) << s.first;
    EXPECT_TRUE(ctx->checkRoutedDesign());
}

// The undo: with no free CPE in reach (radius 0: the sink's own CPE) and with the routing of every round failing
// (the test switch), netlist and routing are as without the pass, and the end points are reported
TEST_F(GateMateHoldFixTest, undo_leaves_the_design_as_it_was)
{
    init_context("CCGM1A1", margin_opts("1.5", {"--hold-fix=0"}));
    shift_register(6);
    route_deferred();
    ctx->repairRoute();
    std::string without = implementation();

    init_context("CCGM1A1", margin_opts("1.5", {"--hold-fix-radius=0"}));
    shift_register(6);
    route_deferred();
    ctx->repairRoute();
    EXPECT_TRUE(delay_cells().empty());
    EXPECT_FALSE(leftovers(GateMateImpl::HoldFixReason::NO_CPE).empty());
    EXPECT_EQ(implementation(), without);

    init_context("CCGM1A1", margin_opts("1.5"));
    shift_register(6);
    route_deferred();
    impl->hold_fix_fail_routing = true;
    ctx->repairRoute();
    EXPECT_TRUE(delay_cells().empty());
    EXPECT_GT(impl->hold_fix_report.removed, 0);
    EXPECT_FALSE(leftovers(GateMateImpl::HoldFixReason::NOT_ROUTABLE).empty());
    EXPECT_EQ(implementation(), without);
    EXPECT_TRUE(ctx->checkRoutedDesign());
}

// The same input, the same cells and routing
TEST_F(GateMateHoldFixTest, deterministic)
{
    std::string first;
    for (int run = 0; run < 2; run++) {
        init_context("CCGM1A1", margin_opts("1.5"));
        shift_register(8);
        route_deferred();
        ctx->repairRoute();
        EXPECT_FALSE(delay_cells().empty());
        if (run == 0)
            first = implementation();
        else
            EXPECT_EQ(implementation(), first);
    }
}
