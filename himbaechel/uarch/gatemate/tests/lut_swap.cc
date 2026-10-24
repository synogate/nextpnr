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
