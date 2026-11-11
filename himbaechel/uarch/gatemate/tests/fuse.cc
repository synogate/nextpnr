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

// --vopt fuse-luts, one rule at a time: each netlist gives the pass one thing to do, which it must
// do, and the implementation must compute what the netlist does (simulated over clock cycles). The
// fuzzer's netlists test the rules together (fused_luts_match_reference).

#include <algorithm>
#include <array>
#include <random>
#include <regex>
#include "known_bugs.h"
#include "testing.h"
#include "uarch/gatemate/pack.h"

#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

namespace {
// Inverters folded forward, backward, through registers, and LUTs fused, as the pass logs them
std::array<int, 4> fuse_counts(const std::string &log)
{
    std::smatch m;
    if (!std::regex_search(log, m,
                           std::regex("(\\d+) inverters folded forward, (\\d+) backward, (\\d+) through registers, "
                                      "(\\d+) LUTs fused")))
        return {-1, -1, -1, -1};
    return {std::stoi(m[1]), std::stoi(m[2]), std::stoi(m[3]), std::stoi(m[4])};
}

// After packing, the CPEs of 5-input LUTs (a lower half taking COMBIN from the upper half of its
// cluster): how many upper halves compute a LUT, and how many only pass the fifth input on
// (L10 = D0, nothing else connected)
std::pair<int, int> l2t5_upper_halves(Context *ctx)
{
    int computing = 0, passing = 0;
    for (auto &cell : ctx->cells) {
        CellInfo *lower = cell.second.get();
        NetInfo *combin = lower->getPort(id_COMBIN);
        if (lower->type != id_CPE_L2T4 || lower->cluster != lower->name || lower->constr_z != CPE_LT_L_Z || !combin)
            continue;
        CellInfo *upper = combin->driver.cell;
        if (!upper || upper->type != id_CPE_L2T4 || upper->cluster != lower->name || upper->constr_z != CPE_LT_U_Z)
            continue;
        bool only_d0 = true;
        for (auto &port : upper->ports)
            only_d0 &= port.second.type != PORT_IN || !port.second.net || port.first == id_D0_10;
        if (only_d0 && int_or_default(upper->params, id_INIT_L10, 0) == LUT_D0)
            passing++;
        else
            computing++;
    }
    return {computing, passing};
}
} // namespace

class GateMateFuseTest : public GateMateTest
{
  protected:
    void SetUp() override
    {
        GateMateTest::SetUp();
        init_context("CCGM1A1", {"--allow-unconstrained", "--fuse-luts"});
    }
    // Implements build's netlist, compares it with the netlist, and returns what the pass did
    std::array<int, 4> fuse(const std::function<void()> &build)
    {
        LogCapture info(LogLevel::INFO_MSG);
        EXPECT_TRUE(implementation_matches_reference(build, 300, 1));
        return fuse_counts(info.str());
    }
};

TEST_F(GateMateFuseTest, inverter_folds_into_the_multiplexer_it_feeds)
{
    auto counts = fuse([&] {
        NetInfo *inverted = lut1("inv", 0b01, input("x"));
        output("y", mx2("mux", inverted, input("d1"), input("s")));
    });
    EXPECT_EQ(counts, (std::array<int, 4>{1, 0, 0, 0}));
}

TEST_F(GateMateFuseTest, inverter_folds_into_the_lut_feeding_it)
{
    auto counts = fuse([&] {
        NetInfo *both = lut2("and", 0b1000, input("a"), input("b"));
        NetInfo *inverted = lut1("inv", 0b01, both);
        output("q", dff("ff", inverted, global_clock("clk"))->getPort(id_Q));
    });
    EXPECT_EQ(counts, (std::array<int, 4>{0, 1, 0, 0}));
}

// Behind the register, its set/reset and initial values are inverted
class GateMateFuseRegisterTest : public GateMateFuseTest,
                                 public ::testing::WithParamInterface<std::tuple<int, int, bool>>
{
};

TEST_P(GateMateFuseRegisterTest, inverter_moves_behind_the_register_it_feeds)
{
    auto [sr_val, init, sr] = GetParam();
    auto counts = fuse([&, sr_val = sr_val, init = init, sr = sr] {
        // From an input: no LUT to fold it into backward
        NetInfo *inverted = lut1("inv", 0b01, input("d"));
        CellInfo *ff = dff("ff", inverted, global_clock("clk"), input("en"), sr ? input("sr") : nullptr);
        ff->params[id_SR_VAL] = Property(sr_val, 1);
        if (init != 2)
            ff->params[id_INIT] = Property(init, 1);
        NetInfo *q = ff->getPort(id_Q);
        output("x", lut2("xor", 0b0110, q, input("b")));
        output("m", mx2("mux", input("d0"), input("d1"), q));
    });
    EXPECT_EQ(counts, (std::array<int, 4>{0, 0, 1, 0}));
}

INSTANTIATE_TEST_SUITE_P(SetResetAndInit, GateMateFuseRegisterTest,
                         ::testing::Combine(::testing::Values(0, 1), ::testing::Values(0, 1, 2), ::testing::Bool()));

TEST_F(GateMateFuseTest, lut_fuses_into_the_lut_it_feeds)
{
    auto counts = fuse([&] {
        NetInfo *both = lut2("and", 0b1000, input("a"), input("b"));
        output("y", lut2("or", 0b1110, both, input("c")));
    });
    EXPECT_EQ(counts, (std::array<int, 4>{0, 0, 0, 1}));
}

// A 4:1 multiplexer and an adder take no inverted inputs here, and an input pad feeds the
// inverter: it stays
TEST_F(GateMateFuseTest, inverter_into_a_multiplexer4_and_an_adder_stays)
{
    auto counts = fuse([&] {
        NetInfo *inverted = lut1("inv", 0b01, input("x"));
        output("m", mx4("mux", input("d0"), input("d1"), inverted, input("d3"), input("s0"), input("s1")));
        auto [s, co] = addf("add", input("a"), inverted, input("ci"));
        output("s", s);
        output("co", co);
    });
    EXPECT_EQ(counts, (std::array<int, 4>{0, 0, 0, 0}));
}

// The upper half of a 5-input LUT's CPE, otherwise passing the fifth input on, computes the LUT
// feeding that input (pack_cpe puts it there, or else fuse_l2t5_inputs)
TEST_F(GateMateFuseTest, lut_feeding_a_5_input_lut_goes_into_its_upper_half)
{
    std::pair<int, int> halves;
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                std::mt19937 rng(7);
                std::vector<NetInfo *> in;
                for (int i = 0; i < 8; i++)
                    in.push_back(input(stringf("i%d", i)));
                auto any = [&] { return in[rng() % in.size()]; };
                // Tables using both inputs: a constant one fuses into nothing, which the count below
                // leaves out
                const int both[] = {0b1000, 0b1110, 0b0110, 0b1001, 0b0111, 0b0001, 0b0100, 0b0010};
                auto table = [&] { return both[rng() % 8]; };
                for (int n = 0; n < 16; n++) {
                    NetInfo *fifth;
                    if (n % 3 == 0)
                        fifth = lut2(stringf("f%d", n), table(), any(), any());
                    else if (n % 3 == 1)
                        fifth = l2t4(stringf("f%d", n), table(), table(), table(), any(), any(), any(), any());
                    else
                        fifth = mx2(stringf("f%d", n), any(), any(), any());
                    CellInfo *lut = create_cell_ptr(id_CC_L2T5, stringf("lut%d", n));
                    for (auto p : {id_INIT_L02, id_INIT_L03, id_INIT_L11, id_INIT_L20})
                        lut->params[p] = Property(int(rng() & 0xf), 4);
                    for (auto p : {id_I0, id_I1, id_I2, id_I3})
                        lut->connectPort(p, any());
                    lut->connectPort(id_I4, fifth);
                    lut->connectPort(id_O, create_net(stringf("lut%d$o", n)));
                    output(stringf("o%d", n), lut->getPort(id_O));
                }
            },
            300, 1, [&] { halves = l2t5_upper_halves(ctx); }));
    EXPECT_EQ(halves.first, 16);
    EXPECT_EQ(halves.second, 0);
}

// --vopt enable-to-lut: a register's enable goes into its LUT (D = EN ? D : Q) where that LUT would
// only pass its data through; the register keeps what it does, enable inverted or not, with
// set/reset and initial values
class GateMateEnableTest : public GateMateTest, public ::testing::WithParamInterface<std::tuple<bool, int, int, bool>>
{
};

TEST_P(GateMateEnableTest, enable_moves_into_the_registers_lut)
{
    auto [en_inv, sr_val, init, sr] = GetParam();
    init_context("CCGM1A1", {"--allow-unconstrained", "--enable-to-lut=1"});
    LogCapture info(LogLevel::INFO_MSG);
    EXPECT_TRUE(implementation_matches_reference(
            [&, en_inv = en_inv, sr_val = sr_val, init = init, sr = sr] {
                // The data also feeds an output, so the packer cannot merge its LUT with the register
                NetInfo *data = lut2("data", 0b0110, input("a"), input("b"));
                output("x", data);
                CellInfo *ff = dff("ff", data, global_clock("clk"), input("en"), sr ? input("sr") : nullptr);
                ff->params[id_EN_INV] = Property(en_inv, 1);
                ff->params[id_SR_VAL] = Property(sr_val, 1);
                if (init != 2)
                    ff->params[id_INIT] = Property(init, 1);
                // Not straight to an output pad: that would make it an IO register
                output("q", lut2("out", 0b0110, ff->getPort(id_Q), input("c")));
            },
            300, 1));
    std::string log = info.str();
    EXPECT_NE(log.find("Moved 1 register enables into their LUT"), std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(EnableSetResetInit, GateMateEnableTest,
                         ::testing::Combine(::testing::Bool(), ::testing::Values(0, 1), ::testing::Values(0, 1, 2),
                                            ::testing::Bool()));

// --vopt mux-chains: a 2:1 multiplexer feeding only a data input of another becomes one 4:1
// multiplexer with it, on either side, also with a select shared between the two
TEST_F(GateMateTest, chained_multiplexers_become_one_4_to_1_multiplexer)
{
    init_context("CCGM1A1", {"--allow-unconstrained", "--mux-chains"});
    LogCapture info(LogLevel::INFO_MSG);
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                std::mt19937 rng(11);
                std::vector<NetInfo *> in;
                for (int i = 0; i < 8; i++)
                    in.push_back(input(stringf("i%d", i)));
                for (int n = 0; n < 16; n++) {
                    // Distinct signals within each multiplexer
                    std::vector<NetInfo *> pick = in;
                    std::shuffle(pick.begin(), pick.end(), rng);
                    NetInfo *inner = mx2(stringf("in%d", n), pick[0], pick[1], pick[2]);
                    NetInfo *select = n % 4 == 3 ? pick[2] : pick[3];
                    NetInfo *y = n % 2 ? mx2(stringf("out%d", n), inner, pick[4], select)
                                       : mx2(stringf("out%d", n), pick[4], inner, select);
                    output(stringf("o%d", n), y);
                }
            },
            300, 1));
    std::string log = info.str();
    EXPECT_NE(log.find("Merged 16 chains of 2:1 multiplexers"), std::string::npos);
}

// A LUT feeding a LUT goes with it into one CPE (the first in the upper half, the second's other
// inputs in the lower one, the second's own function in L20) where the second splits so, also with a
// copy of the first where it feeds more. The default (--vopt cpe-merge=2): over 18 designs Fmax +13%,
// docs/timing_experiments.md
TEST_F(GateMateTest, lut_pairs_merge_into_one_cpe)
{
    LogCapture info(LogLevel::INFO_MSG);
    std::pair<int, int> halves;
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                std::mt19937 rng(13);
                std::vector<NetInfo *> in;
                for (int i = 0; i < 10; i++)
                    in.push_back(input(stringf("i%d", i)));
                auto any = [&] { return in[rng() % in.size()]; };
                // Tables using their inputs: a constant one merges into nothing, which the count below
                // leaves out
                const int used[] = {0b1000, 0b1110, 0b0110, 0b1001, 0b0111, 0b0001, 0b0100, 0b0010};
                auto table = [&] { return used[rng() % 8]; };
                auto lut = [&](const std::string &name, int kind, NetInfo *x) {
                    // x on a random input
                    std::vector<NetInfo *> ins = {any(), any(), any(), any()};
                    if (x)
                        ins[rng() % (kind == 0 ? 2 : kind == 1 ? 4 : 3)] = x;
                    if (kind == 0)
                        return lut2(name, table(), ins[0], ins[1]);
                    if (kind == 1)
                        return l2t4(name, table(), table(), table(), ins[0], ins[1], ins[2], ins[3]);
                    return mx2(name, ins[0], ins[1], ins[2]);
                };
                for (int n = 0; n < 24; n++) {
                    NetInfo *first = lut(stringf("a%d", n), rng() % 3, nullptr);
                    output(stringf("o%d", n), lut(stringf("b%d", n), rng() % 3, first));
                    // Every other first LUT also feeds an output of its own
                    if (n % 2)
                        output(stringf("f%d", n), first);
                }
            },
            300, 1, [&] { halves = l2t5_upper_halves(ctx); }));
    std::smatch m;
    std::string log = info.str();
    ASSERT_TRUE(std::regex_search(log, m, std::regex("Merged (\\d+) pairs of LUT levels")));
    int merged = std::stoi(m[1]);
    EXPECT_GT(merged, 8);
    // each with the first LUT in the upper half
    EXPECT_EQ(halves.first, merged);
    EXPECT_EQ(halves.second, 0);
}

class GateMateCpeMergeTest : public GateMateTest
{
  protected:
    CellInfo *first = nullptr, *second = nullptr;

    // b = a & i2 & (i3 | i4), a = i0 & i1: b is a function of a and of one LUT tree of the rest. With
    // `shared`, a feeds an output too
    void build(bool shared)
    {
        NetInfo *a = lut2("a", 0b1000, input("i0"), input("i1"));
        NetInfo *b = l2t4("b", 0b1000, 0b1110, 0b1000, a, input("i2"), input("i3"), input("i4"));
        output("o", b);
        if (shared)
            output("p", a);
        first = a->driver.cell;
        second = b->driver.cell;
    }

    // 5-input LUTs with a LUT in the upper half after packing
    int merged()
    {
        ctx->pack();
        return l2t5_upper_halves(ctx).first;
    }
};

// The first LUT is the upper half of the second one's CPE, named as the upper half of a 5-input LUT is
TEST_F(GateMateCpeMergeTest, pair_merges_by_default)
{
    build(false);
    EXPECT_EQ(merged(), 1);
    EXPECT_FALSE(ctx->cells.count(ctx->id("a")));
    ASSERT_TRUE(ctx->cells.count(ctx->id("b$upper")));
    const CellInfo *upper = ctx->cells.at(ctx->id("b$upper")).get();
    EXPECT_EQ(upper->cluster, ctx->id("b"));
    EXPECT_EQ(upper->constr_z, CPE_LT_U_Z);
}

// With a copy in the upper half, the first LUT stays for its other users
TEST_F(GateMateCpeMergeTest, pair_merges_with_a_copy_by_default)
{
    build(true);
    EXPECT_EQ(merged(), 1);
    ASSERT_TRUE(ctx->cells.count(ctx->id("b$upper")));
    EXPECT_EQ(ctx->cells.at(ctx->id("b$upper"))->cluster, ctx->id("b"));
    ASSERT_TRUE(ctx->cells.count(ctx->id("a")));
    EXPECT_EQ(ctx->cells.at(ctx->id("a"))->cluster, ClusterId());
}

TEST_F(GateMateCpeMergeTest, level_1_merges_without_copies_only)
{
    init_context("CCGM1A1", {"--allow-unconstrained", "--cpe-merge=1"});
    build(true);
    EXPECT_EQ(merged(), 0);
    init_context("CCGM1A1", {"--allow-unconstrained", "--cpe-merge=1"});
    build(false);
    EXPECT_EQ(merged(), 1);
}

TEST_F(GateMateCpeMergeTest, nothing_merges_with_the_option_at_0)
{
    for (bool shared : {false, true}) {
        init_context("CCGM1A1", {"--allow-unconstrained", "--cpe-merge=0"});
        build(shared);
        EXPECT_EQ(merged(), 0) << shared;
    }
}

// The first LUT (or its copy) is placed in the second's CPE: one of another region would leave its region
TEST_F(GateMateCpeMergeTest, lut_of_another_region_is_not_merged)
{
    for (bool shared : {false, true})
        for (int regions = 0; regions < 3; regions++) {
            init_context("CCGM1A1", {"--allow-unconstrained"});
            build(shared);
            ctx->createRectangularRegion(ctx->id("r1"), 40, 40, 60, 60);
            ctx->createRectangularRegion(ctx->id("r2"), 80, 80, 100, 100);
            // 0: the first in a region, the second in none; 1: in different regions; 2: in the same one
            ctx->constrainCellToRegion(first->name, ctx->id("r1"));
            if (regions)
                ctx->constrainCellToRegion(second->name, ctx->id(regions == 1 ? "r2" : "r1"));
            EXPECT_EQ(merged(), regions == 2 ? 1 : 0) << shared << " " << regions;
        }
}

TEST_F(GateMateCpeMergeTest, merged_pair_matches_reference)
{
    for (bool shared : {false, true}) {
        init_context("CCGM1A1", {"--allow-unconstrained"});
        EXPECT_TRUE(implementation_matches_reference([&] { build(shared); }, 64, 1)) << shared;
    }
}
