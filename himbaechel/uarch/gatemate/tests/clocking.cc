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

#include "testing.h"
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

class GateMateClockingTest : public GateMateTest
{
  protected:
    // `count` clock inputs, each through a BUFG; clock i drives i + 1 registers so that the
    // BUFGs have distinct fan-outs.
    void clocks_with_fanout(int count)
    {
        NetInfo *d = input("d");
        for (int i = 0; i < count; i++) {
            CellInfo *bufg = create_cell_ptr(id_CC_BUFG, stringf("bufg%d", i));
            bufg->connectPort(id_I, input(stringf("clk%d", i)));
            NetInfo *clk = create_net(stringf("glb%d", i));
            bufg->connectPort(id_O, clk);
            for (int j = 0; j <= i; j++) {
                CellInfo *ff = dff(stringf("ff%d_%d", i, j), d, clk);
                output(stringf("q%d_%d", i, j), ff->getPort(id_Q));
            }
        }
    }

    std::set<std::string> global_nets()
    {
        std::set<std::string> names;
        for (auto &g : impl->global_signals)
            names.insert(g.first->name.str(ctx));
        return names;
    }
};

TEST_F(GateMateClockingTest, single_bufg_is_global)
{
    clocks_with_fanout(1);
    pack();
    EXPECT_EQ(global_nets(), (std::set<std::string>{"glb0"}));
}

TEST_F(GateMateClockingTest, four_bufgs_fit)
{
    clocks_with_fanout(4);
    pack();
    EXPECT_EQ(global_nets(), (std::set<std::string>{"glb0", "glb1", "glb2", "glb3"}));
}

// With more BUFGs than global clocks, the ones with the highest fan-out are kept.
TEST_F(GateMateClockingTest, excess_bufgs_keep_highest_fanout)
{
    clocks_with_fanout(5);
    pack();
    EXPECT_EQ(global_nets(), (std::set<std::string>{"glb1", "glb2", "glb3", "glb4"}));
}

// In the FULL_USE strategy every die of a multi-die device has its own four global clocks.
TEST_F(GateMateClockingTest, full_strategy_uses_globals_of_all_dies)
{
    init_context("CCGM1A2", {"--allow-unconstrained", "--strategy=full"});
    clocks_with_fanout(9);
    pack();
    EXPECT_EQ(global_nets().size(), 8u);
    EXPECT_FALSE(global_nets().count("glb0")) << "the BUFG with the lowest fan-out should be dropped";
}
