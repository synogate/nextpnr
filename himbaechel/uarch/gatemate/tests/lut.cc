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

#include <algorithm>
#include <random>
#include <regex>
#include <vector>
#include "command.h"
#include "testing.h"
#include "uarch/gatemate/pack.h"
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

namespace {
// The sum over all matches in log of group `group` of `pattern` (a repair logs once per pass)
int sum_of_matches(const std::string &log, const std::string &pattern, int group)
{
    std::regex re(pattern);
    int sum = 0;
    for (auto it = std::sregex_iterator(log.begin(), log.end(), re); it != std::sregex_iterator(); ++it)
        sum += std::stoi((*it)[group]);
    return sum;
}
} // namespace

TEST_F(GateMateTest, pack_constants)
{
    GateMatePacker packer(ctx, impl);
    packer.pack_constants();
    ASSERT_EQ(ctx->cells.size(), 2LU);
    packer.remove_constants();
    ASSERT_EQ(ctx->cells.size(), 0LU);
}

// LUT[1:0]	Function
// ==============================
// 00       Constant 0
// 01       NOT A (inverts input)
// 10       A (passes input)
// 11       Constant 1

TEST_F(GateMateTest, remove_lut1_zero)
{
    CellInfo *lut1 = create_cell_ptr(id_CC_LUT1, "lut");
    lut1->params[id_INIT] = Property(0b00, 2);

    CellInfo *obuf = create_cell_ptr(id_CC_OBUF, "obuf");

    direct_connect(lut1, id_O, obuf, id_A);

    ASSERT_EQ(ctx->cells.size(), 2LU);
    ctx->uarch->pack();
    ASSERT_EQ(ctx->cells.size(), 4LU);
}

TEST_F(GateMateTest, remove_lut1_one)
{
    CellInfo *lut1 = create_cell_ptr(id_CC_LUT1, "lut");
    lut1->params[id_INIT] = Property(0b11, 2);

    CellInfo *obuf = create_cell_ptr(id_CC_OBUF, "obuf");

    direct_connect(lut1, id_O, obuf, id_A);

    ASSERT_EQ(ctx->cells.size(), 2LU);
    ctx->uarch->pack();
    ASSERT_EQ(ctx->cells.size(), 4LU);
}

TEST_F(GateMateTest, remove_lut1_pass)
{
    CellInfo *lut1 = create_cell_ptr(id_CC_LUT1, "lut");
    lut1->params[id_INIT] = Property(0b10, 2);

    CellInfo *obuf = create_cell_ptr(id_CC_OBUF, "obuf");
    CellInfo *ibuf = create_cell_ptr(id_CC_IBUF, "ibuf");

    direct_connect(ibuf, id_Y, lut1, id_I0);
    direct_connect(lut1, id_O, obuf, id_A);

    ASSERT_EQ(ctx->cells.size(), 3LU);
    ctx->uarch->pack();
    // Expect IBUF -> CPE -> OBUF
    // LUT removed, but CPE for driving OBUF added
    ASSERT_EQ(ctx->cells.size(), 8LU);
}

TEST_F(GateMateTest, remove_lut1_inv)
{
    CellInfo *lut1 = create_cell_ptr(id_CC_LUT1, "lut");
    lut1->params[id_INIT] = Property(0b01, 2);

    CellInfo *obuf = create_cell_ptr(id_CC_OBUF, "obuf");
    CellInfo *ibuf = create_cell_ptr(id_CC_IBUF, "ibuf");

    direct_connect(ibuf, id_Y, lut1, id_I0);
    direct_connect(lut1, id_O, obuf, id_A);

    ASSERT_EQ(ctx->cells.size(), 3LU);
    ctx->uarch->pack();
    // Expect IBUF -> CPE -> OBUF
    // LUT merged, but CPE for driving OBUF added
    ASSERT_EQ(ctx->cells.size(), 8LU);
}

TEST_F(GateMateTest, remove_lut1_not_driven)
{
    CellInfo *lut1 = create_cell_ptr(id_CC_LUT1, "lut");
    lut1->params[id_INIT] = Property(0b01, 2);

    CellInfo *obuf = create_cell_ptr(id_CC_OBUF, "obuf");
    CellInfo *ibuf = create_cell_ptr(id_CC_IBUF, "ibuf");

    NetInfo *net_in = ctx->createNet(ctx->id("in"));
    ibuf->connectPort(id_Y, net_in);
    lut1->connectPort(id_I0, net_in);
    obuf->connectPort(id_A, net_in);

    ASSERT_EQ(ctx->cells.size(), 3LU);
    ctx->uarch->pack();
    // Expect IBUF -> CPE -> OBUF
    // LUT1 removed as not used, but CPE for driving OBUF added
    ASSERT_EQ(ctx->cells.size(), 8LU);
}

// place-repair moves L20 pairs (a 5-input LUT: the lower half with its L20, the upper half feeding its
// COMBIN) as a whole: a registered network of 5-input LUTs under a clock it cannot meet (so the
// repairs run until they find nothing more), with its L20 pairs moved far away after placement;
// place-repair must bring some back, and the routed design compute the same.
TEST_F(GateMateTest, place_repair_moves_l20_pairs)
{
    init_context("CCGM1A1", {"--allow-unconstrained", "--no-bridges", "--no-cpe-cp", "--lookahead=table",
                             "--place-delay=table"});
    LogCapture info(LogLevel::INFO_MSG);
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                std::mt19937 rng(3);
                NetInfo *clk = global_clock("clk");
                ctx->addClock(clk->name, 400);
                std::vector<NetInfo *> layer;
                for (int i = 0; i < 16; i++)
                    layer.push_back(dff(stringf("in_ff%d", i), input(stringf("in%d", i)), clk)->getPort(id_Q));
                int n = 0;
                for (int level = 0; level < 3; level++) {
                    std::vector<NetInfo *> next;
                    for (int i = 0; i < 16; i++) {
                        CellInfo *lut = create_cell_ptr(id_CC_L2T5, stringf("l5_%d", n));
                        for (IdString param : {id_INIT_L02, id_INIT_L03, id_INIT_L11, id_INIT_L20})
                            lut->params[param] = Property(int(rng() % 14) + 1, 4);
                        // five different signals, as synthesis makes them
                        std::vector<NetInfo *> pick = layer;
                        std::shuffle(pick.begin(), pick.end(), rng);
                        for (int k = 0; k < 5; k++)
                            lut->connectPort(ctx->idf("I%d", k), pick[k]);
                        NetInfo *o = create_net(stringf("l5_%d$o", n++));
                        lut->connectPort(id_O, o);
                        next.push_back(o);
                    }
                    layer = next;
                }
                for (int i = 0; i < 16; i++)
                    output(stringf("out%d", i), dff(stringf("out_ff%d", i), layer[i], clk)->getPort(id_Q));
            },
            100, 1, nullptr, [&] {
                // Banish every L20 pair without a register far away, for place-repair to bring back
                int banished = 0;
                for (auto &c : ctx->cells) {
                    CellInfo *lower = c.second.get();
                    NetInfo *combin = lower->getPort(id_COMBIN);
                    if (lower->type != id_CPE_L2T4 || !combin || lower->constr_children.size() != 1)
                        continue;
                    CellInfo *upper = combin->driver.cell;
                    Loc at = ctx->getBelLocation(lower->bel);
                    for (int x = ctx->getGridDimX() - 8; x > at.x + 40 && upper; x--) {
                        BelId lo = ctx->getBelByLocation(Loc(x, at.y, CPE_LT_L_Z)),
                              up = ctx->getBelByLocation(Loc(x, at.y, CPE_LT_U_Z));
                        if (lo == BelId() || up == BelId() || !ctx->checkBelAvail(lo) || !ctx->checkBelAvail(up) ||
                            !ctx->checkBelAvail(ctx->getBelByLocation(Loc(x, at.y, CPE_FF_U_Z))) ||
                            !ctx->checkBelAvail(ctx->getBelByLocation(Loc(x, at.y, CPE_FF_L_Z))))
                            continue;
                        PlaceStrength strength = lower->belStrength;
                        ctx->unbindBel(lower->bel);
                        ctx->unbindBel(upper->bel);
                        ctx->bindBel(lo, lower, strength);
                        ctx->bindBel(up, upper, strength);
                        banished++;
                        break;
                    }
                }
                EXPECT_GT(banished, 0);
                ctx->route();
            }));
    std::string log = info.str();
    EXPECT_GT(sum_of_matches(log, "cell moves \\((\\d+) of L20 pairs", 1), 0);
}

// The same for 4:1 multiplexers, which take a whole CPE: moved far away after placement, place-repair
// must bring some back.
TEST_F(GateMateTest, place_repair_moves_multiplexers)
{
    init_context("CCGM1A1", {"--allow-unconstrained", "--no-bridges", "--no-cpe-cp", "--lookahead=table",
                             "--place-delay=table"});
    LogCapture info(LogLevel::INFO_MSG);
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                std::mt19937 rng(9);
                NetInfo *clk = global_clock("clk");
                ctx->addClock(clk->name, 400);
                std::vector<NetInfo *> layer;
                for (int i = 0; i < 16; i++)
                    layer.push_back(dff(stringf("in_ff%d", i), input(stringf("in%d", i)), clk)->getPort(id_Q));
                for (int level = 0; level < 3; level++) {
                    std::vector<NetInfo *> next;
                    for (int i = 0; i < 16; i++) {
                        NetInfo *x[6];
                        for (auto &v : x)
                            v = layer[rng() % layer.size()];
                        next.push_back(mx4(stringf("m%d_%d", level, i), x[0], x[1], x[2], x[3], x[4], x[5]));
                    }
                    layer = next;
                }
                for (int i = 0; i < 16; i++)
                    output(stringf("out%d", i), dff(stringf("out_ff%d", i), layer[i], clk)->getPort(id_Q));
            },
            100, 1, nullptr, [&] {
                // Banish every multiplexer alone in its CPE far away, for place-repair to bring back
                int banished = 0;
                for (auto &c : ctx->cells) {
                    CellInfo *mux = c.second.get();
                    if (mux->type != id_CPE_MX4 || mux->bel == BelId())
                        continue;
                    Loc at = ctx->getBelLocation(mux->bel);
                    bool alone = true;
                    for (int z : {CPE_FF_U_Z, CPE_FF_L_Z})
                        alone &= ctx->checkBelAvail(ctx->getBelByLocation(Loc(at.x, at.y, z)));
                    if (!alone)
                        continue;
                    for (int x = ctx->getGridDimX() - 8; x > at.x + 40; x--) {
                        bool free = true;
                        for (int z : {CPE_LT_U_Z, CPE_LT_L_Z, CPE_FF_U_Z, CPE_FF_L_Z, CPE_LT_FULL_Z}) {
                            BelId bel = ctx->getBelByLocation(Loc(x, at.y, z));
                            free &= bel != BelId() && ctx->checkBelAvail(bel);
                        }
                        if (!free)
                            continue;
                        PlaceStrength strength = mux->belStrength;
                        ctx->unbindBel(mux->bel);
                        ctx->bindBel(ctx->getBelByLocation(Loc(x, at.y, CPE_LT_FULL_Z)), mux, strength);
                        banished++;
                        break;
                    }
                }
                EXPECT_GT(banished, 0);
                ctx->route();
            }));
    std::string log = info.str();
    EXPECT_GT(sum_of_matches(log, "of L20 pairs, (\\d+) of multiplexers", 1), 0);
}

// place-repair also fuses: after routing, a critical LUT2 (or a copy of it, if it feeds others too)
// computes in the first level of the LUT2 alone in a half it feeds. A registered random LUT2 network
// under a clock it cannot meet, routed, must compute the same, with some fusions kept, also of copies.
TEST_F(GateMateTest, place_repair_fuses_critical_lut2s)
{
    init_context("CCGM1A1", {"--allow-unconstrained", "--no-bridges", "--no-cpe-cp", "--lookahead=table",
                             "--place-delay=table"});
    LogCapture info(LogLevel::INFO_MSG);
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                std::mt19937 rng(13);
                NetInfo *clk = global_clock("clk");
                ctx->addClock(clk->name, 400);
                std::vector<NetInfo *> layer;
                for (int i = 0; i < 16; i++)
                    layer.push_back(dff(stringf("in_ff%d", i), input(stringf("in%d", i)), clk)->getPort(id_Q));
                for (int level = 0; level < 5; level++) {
                    std::vector<NetInfo *> next;
                    for (int i = 0; i < 24; i++) {
                        NetInfo *a = layer[rng() % layer.size()], *b;
                        do
                            b = layer[rng() % layer.size()];
                        while (b == a);
                        static const int tables[] = {0b1000, 0b0110, 0b1110, 0b0100, 0b0010, 0b1001, 0b0111, 0b1011};
                        next.push_back(lut2(stringf("l%d_%d", level, i), tables[rng() % 8], a, b));
                    }
                    layer = next;
                }
                for (int i = 0; i < 16; i++)
                    output(stringf("out%d", i), dff(stringf("out_ff%d", i), layer[i], clk)->getPort(id_Q));
            },
            100, 1, nullptr, [&] { ctx->route(); }));
    std::string log = info.str();
    EXPECT_GT(sum_of_matches(log, "Kept (\\d+) fused LUT2 levels \\((\\d+) with a copy", 1), 0);
    EXPECT_GT(sum_of_matches(log, "Kept (\\d+) fused LUT2 levels \\((\\d+) with a copy", 2), 0);
}
