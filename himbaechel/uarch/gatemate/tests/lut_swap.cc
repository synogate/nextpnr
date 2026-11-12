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

// Swapping the input pairs of LUT trees after routing (--vopt lut-swap-rounds). That swaps keep
// the function is tested with the fuzzer's netlists (swapped_lut_inputs_match_reference).

#include <random>
#include <regex>
#include <sstream>
#include "json_frontend.h"
#include "jsonwrite.h"
#include "router2.h"
#include "testing.h"

#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

TEST_F(GateMateTest, undone_lut_swaps_restore_cells_and_routing)
{
    init_context("CCGM1A1", {"--allow-unconstrained", "--lut-swap-rounds=1"});
    // Registered LUT trees, each fed by earlier registers
    std::mt19937 rng(3);
    NetInfo *clk = global_clock("clk");
    std::vector<NetInfo *> signals;
    for (int i = 0; i < 4; i++)
        signals.push_back(input(stringf("i%d", i)));
    for (int n = 0; n < 40; n++) {
        auto pick = [&]() { return signals.at(rng() % signals.size()); };
        NetInfo *y = l2t4(stringf("lut%d", n), rng() & 0xf, rng() & 0xf, rng() & 0xf, pick(), pick(), pick(), pick());
        signals.push_back(dff(stringf("ff%d", n), y, clk)->getPort(id_Q));
    }
    output("o", signals.back());
    ASSERT_TRUE(packs());
    ASSERT_TRUE(ctx->place());
    impl->preRoute();
    Router2Cfg cfg(ctx);
    impl->configureRouter2(cfg);
    ASSERT_TRUE(router2(ctx, cfg));

    auto state = [&]() {
        std::map<std::string, std::string> s;
        for (auto &net : ctx->nets)
            for (auto &w : net.second->wires)
                s[net.first.str(ctx) + " " + ctx->nameOfWire(w.first)] = stringf(
                        "%s %d", w.second.pip == PipId() ? "-" : ctx->nameOfPip(w.second.pip), int(w.second.strength));
        for (auto &cell : ctx->cells) {
            for (auto &port : cell.second->ports)
                if (port.second.net)
                    s[cell.first.str(ctx) + "." + port.first.str(ctx)] = port.second.net->name.str(ctx);
            for (auto &param : cell.second->params)
                s[cell.first.str(ctx) + "#" + param.first.str(ctx)] = param.second.to_string();
        }
        return s;
    };
    auto before = state();
    EXPECT_GT(impl->improve_lut_inputs(3, true), 0);
    auto after = state();
    EXPECT_EQ(before.size(), after.size());
    int differences = 0;
    for (auto &entry : before)
        if (!after.count(entry.first) || after.at(entry.first) != entry.second)
            if (differences++ < 10)
                ADD_FAILURE() << entry.first << ": " << entry.second << " -> "
                              << (after.count(entry.first) ? after.at(entry.first) : "(gone)");
    EXPECT_EQ(differences, 0);
}

namespace {
// The number the log reports for `pattern`'s first group, summed over its lines
int logged_count(const std::string &log, const std::string &pattern)
{
    std::regex re(pattern);
    int sum = 0;
    for (auto it = std::sregex_iterator(log.begin(), log.end(), re); it != std::sregex_iterator(); ++it)
        sum += std::stoi((*it)[1]);
    return sum;
}
} // namespace

// The router may swap the two subtrees of a half (the virtual LUT2_10X/LUT2_11X selects): postRoute
// moves the first-level tables and swaps L10's inputs. Registered random LUT trees and LUT2s (alone in
// a half, through the first level to L10), routed, must compute the same, with some trees swapped, and
// none with --no-lut-tree-swap or a chip database without the swap.
TEST_F(GateMateTest, routed_subtree_swaps_match_reference)
{
    auto build = [&] {
        std::mt19937 rng(5);
        NetInfo *clk = global_clock("clk");
        std::vector<NetInfo *> signals;
        for (int i = 0; i < 12; i++)
            signals.push_back(dff(stringf("in_ff%d", i), input(stringf("in%d", i)), clk)->getPort(id_Q));
        for (int n = 0; n < 60; n++) {
            auto pick = [&]() { return signals.at(rng() % signals.size()); };
            NetInfo *y = n % 3 == 2 ? lut2(stringf("l%d", n), rng() & 0xf, pick(), pick())
                                    : l2t4(stringf("t%d", n), rng() & 0xf, rng() & 0xf, rng() & 0xf, pick(), pick(),
                                           pick(), pick());
            signals.push_back(dff(stringf("ff%d", n), y, clk)->getPort(id_Q));
        }
        for (int i = 0; i < 12; i++)
            output(stringf("out%d", i), signals.at(signals.size() - 1 - i));
    };
    for (bool swap : {true, false}) {
        SCOPED_TRACE(swap ? "subtree swaps" : "--no-lut-tree-swap");
        // (no bridges and CP lines: the reference model has none)
        std::vector<std::string> opts = {"--allow-unconstrained", "--no-bridges", "--no-cpe-cp"};
        if (!swap)
            opts.push_back("--no-lut-tree-swap");
        init_context("CCGM1A1", opts);
        LogCapture info(LogLevel::INFO_MSG);
        EXPECT_TRUE(implementation_matches_reference(build, 100, 1, nullptr, [&] { ctx->route(); }));
        int swapped = logged_count(info.str(), "(\\d+) LUT trees routed with their subtrees swapped");
        // (a chip database from before the virtual subtree swap has none: the function must still match)
        bool in_chipdb = false;
        for (BelId bel : ctx->getBels())
            if (ctx->getBelType(bel) == id_CPE_LT_U) {
                in_chipdb = ctx->getWireByName(IdStringList::concat(ctx->getBelName(bel)[0],
                                                                    ctx->id("CPE.P0_00_int"))) != WireId();
                break;
            }
        if (swap && in_chipdb)
            EXPECT_GT(swapped, 0);
        else
            EXPECT_EQ(swapped, 0);
    }
}

// After finishRoute the LUT inputs are the CPE inputs the routing reached and the tables are the physical ones:
// no routing goes on behind those pins, so no virtual select (LUT2 input swap, subtree swap, the passes into L10 /
// L11) is left set against the tables, and a JSON round trip (which drops nothing) and a second finish keep it so.
TEST_F(GateMateTest, finished_routing_has_no_virtual_selects_behind_the_lut_inputs)
{
    auto build = [&] {
        std::mt19937 rng(5);
        NetInfo *clk = global_clock("clk");
        std::vector<NetInfo *> signals;
        for (int i = 0; i < 12; i++)
            signals.push_back(dff(stringf("in_ff%d", i), input(stringf("in%d", i)), clk)->getPort(id_Q));
        for (int n = 0; n < 60; n++) {
            auto pick = [&]() { return signals.at(rng() % signals.size()); };
            NetInfo *y = n % 3 == 2 ? lut2(stringf("l%d", n), rng() & 0xf, pick(), pick())
                                    : l2t4(stringf("t%d", n), rng() & 0xf, rng() & 0xf, rng() & 0xf, pick(), pick(),
                                           pick(), pick());
            signals.push_back(dff(stringf("ff%d", n), y, clk)->getPort(id_Q));
        }
        for (int i = 0; i < 12; i++)
            output(stringf("out%d", i), signals.at(signals.size() - 1 - i));
    };
    // Bound virtual selects, and wires of a net that lead to none of its sinks
    auto check = [&](const char *when) {
        SCOPED_TRACE(when);
        int virtual_pips = 0;
        for (auto &n : ctx->nets) {
            NetInfo *net = n.second.get();
            pool<WireId> needed;
            for (auto &usr : net->users)
                for (WireId w : ctx->getNetinfoSinkWires(net, usr))
                    while (net->wires.count(w) && needed.insert(w).second && net->wires.at(w).pip != PipId())
                        w = ctx->getPipSrcWire(net->wires.at(w).pip);
            for (auto &w : net->wires) {
                if (w.second.pip != PipId()) {
                    const auto &extra = *impl->pip_extra_data(w.second.pip);
                    if (extra.type == PipExtra::PIP_EXTRA_MUX && extra.name != 0 &&
                        IdString(extra.name).str(ctx).rfind("LUT2_", 0) == 0) {
                        virtual_pips++;
                        ADD_FAILURE() << ctx->nameOfPip(w.second.pip) << " of " << net->name.c_str(ctx);
                    }
                }
                // (the LUT inputs' part of a CPE: the 2:1 muxes TI*, the physical P* and logical D* LUT2 inputs)
                std::string name = ctx->getWireName(w.first)[1].str(ctx);
                bool lut_inputs = name.rfind("CPE.TI", 0) == 0 ||
                                  ((name.rfind("CPE.P", 0) == 0 || name.rfind("CPE.D", 0) == 0) && name.size() > 6 &&
                                   isdigit(name[5]) && name[6] == '_');
                if (lut_inputs && !needed.count(w.first))
                    ADD_FAILURE() << ctx->nameOfWire(w.first) << " of " << net->name.c_str(ctx)
                                  << " leads to no sink";
            }
        }
        EXPECT_EQ(virtual_pips, 0);
    };
    init_context("CCGM1A1", {"--allow-unconstrained", "--no-bridges", "--no-cpe-cp"});
    LogCapture info(LogLevel::INFO_MSG);
    ASSERT_TRUE(implementation_matches_reference(build, 100, 1, nullptr, [&] { ctx->route(); }));
    // (the design must use the swaps for the test to say anything)
    EXPECT_GT(logged_count(info.str(), "(\\d+) LUT trees routed with their subtrees swapped"), 0);
    check("finished");
    std::map<std::string, std::string> tables;
    for (auto &c : ctx->cells)
        for (auto &p : c.second->params)
            if (p.first.str(ctx).rfind("INIT_L", 0) == 0)
                tables[c.first.str(ctx) + "." + p.first.str(ctx)] = p.second.to_string();
    std::ostringstream out;
    std::string filename = "finished.json";
    ASSERT_TRUE(write_json_file(out, filename, ctx));
    init_context("CCGM1A1", {"--allow-unconstrained", "--no-bridges", "--no-cpe-cp"});
    std::istringstream in(out.str());
    ASSERT_TRUE(parse_json(in, filename, ctx));
    check("loaded");
    ctx->route_finish_pending = true;
    ctx->finishRoute();
    check("finished again");
    for (auto &t : tables) {
        auto dot = t.first.rfind('.');
        CellInfo *ci = ctx->cells.at(ctx->id(t.first.substr(0, dot))).get();
        EXPECT_EQ(ci->params.at(ctx->id(t.first.substr(dot + 1))).to_string(), t.second) << t.first;
    }
}
