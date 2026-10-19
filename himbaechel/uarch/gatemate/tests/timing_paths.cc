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
#include "timing.h"
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

class GateMateTimingTest : public GateMateTest
{
  protected:
    // Run timing analysis (as the command line flow does) and collect the critical path of every
    // domain pair.
    std::vector<CriticalPath> critical_paths()
    {
        timing_analysis(ctx, false, true, true, false, true);
        auto &result = ctx->timing_result;
        std::vector<CriticalPath> paths;
        for (auto &p : result.clock_paths)
            paths.push_back(p.second);
        for (auto &p : result.xclock_paths)
            paths.push_back(p);
        return paths;
    }

    static std::string describe(Context *ctx, const std::vector<CriticalPath> &paths)
    {
        std::string s = stringf(" %d", int(paths.size()));
        for (auto &p : paths) {
            if (p.segments.empty()) {
                s += "\n    (empty path)";
                continue;
            }
            s += stringf("\n    %s.%s -> %s.%s", p.segments.front().from.first.c_str(ctx),
                         p.segments.front().from.second.c_str(ctx), p.segments.back().to.first.c_str(ctx),
                         p.segments.back().to.second.c_str(ctx));
        }
        return s;
    }

    static bool path_ends_at(const std::vector<CriticalPath> &paths, IdString cell)
    {
        for (auto &p : paths)
            if (!p.segments.empty() && p.segments.back().to.first == cell)
                return true;
        return false;
    }

    static bool path_starts_at(const std::vector<CriticalPath> &paths, IdString cell)
    {
        for (auto &p : paths)
            if (!p.segments.empty() && p.segments.front().from.first == cell)
                return true;
        return false;
    }

    // Value of a named entry in the timing database of the current speed grade
    DelayQuad timing_db(IdString name)
    {
        const auto *sp = reinterpret_cast<const GateMateSpeedGradeExtraDataPOD *>(ctx->speed_grade->extra_data.get());
        for (const auto &t : sp->timings)
            if (IdString(t.name) == name)
                return DelayQuad(t.delay.fast_min, t.delay.fast_max, t.delay.slow_min, t.delay.slow_max);
        ADD_FAILURE() << "timing database has no entry " << name.c_str(ctx);
        return DelayQuad(0);
    }
};

// Path checks are run on the placed design and again on the routed design, since routing
// rewrites the CPE ports to the physical pins that were used.
enum class Stage
{
    PLACED,
    ROUTED,
};

class GateMateTimingPathTest : public GateMateTimingTest, public ::testing::WithParamInterface<Stage>
{
  protected:
    void implement()
    {
        ctx->pack();
        ctx->place();
        if (GetParam() == Stage::ROUTED)
            ctx->route();
    }
};

TEST_P(GateMateTimingPathTest, input_pad_to_register)
{
    NetInfo *clk = global_clock("clk");
    CellInfo *ff = dff("ff", input("d"), clk);
    output("q", ff->getPort(id_Q));
    implement();
    auto paths = critical_paths();
    EXPECT_TRUE(path_starts_at(paths, ctx->id("d"))) << "analysed paths:" << describe(ctx, paths);
}

TEST_P(GateMateTimingPathTest, register_to_output_pad)
{
    NetInfo *clk = global_clock("clk");
    CellInfo *ff = dff("ff", input("d"), clk);
    output("q", ff->getPort(id_Q));
    implement();
    auto paths = critical_paths();
    EXPECT_TRUE(path_ends_at(paths, ctx->id("q"))) << "analysed paths:" << describe(ctx, paths);
}

TEST_P(GateMateTimingPathTest, input_pad_to_output_pad)
{
    output("y", lut1("inv", 0b01, input("x")));
    implement();
    auto paths = critical_paths();
    EXPECT_TRUE(path_ends_at(paths, ctx->id("y"))) << "analysed paths:" << describe(ctx, paths);
}

TEST_P(GateMateTimingPathTest, register_to_register)
{
    NetInfo *clk = global_clock("clk");
    CellInfo *ff1 = dff("ff1", input("d"), clk);
    CellInfo *ff2 = dff("ff2", lut1("inv", 0b01, ff1->getPort(id_Q)), clk);
    output("q", ff2->getPort(id_Q));
    implement();
    auto paths = critical_paths();
    bool found = false;
    for (auto &p : paths)
        if (!p.segments.empty() && p.segments.front().from.first == ctx->id("ff1") &&
            p.segments.back().to.first == ctx->id("ff2") && p.clock_pair.start.clock == ctx->id("clk$glb") &&
            p.clock_pair.end.clock == ctx->id("clk$glb"))
            found = true;
    EXPECT_TRUE(found) << "analysed paths:" << describe(ctx, paths);
}

// Every flip-flop must keep a clock input that the timing model recognises as such.
TEST_P(GateMateTimingPathTest, register_clock_pins_are_clock_inputs)
{
    NetInfo *clk = global_clock("clk");
    CellInfo *ff1 = dff("ff1", input("d"), clk);
    CellInfo *ff2 = dff("ff2", lut1("inv", 0b01, ff1->getPort(id_Q)), clk);
    output("q", ff2->getPort(id_Q));
    implement();
    NetInfo *glb = ctx->nets.at(ctx->id("clk$glb")).get();
    int checked = 0;
    for (auto &usr : glb->users) {
        if (!usr.cell->type.in(id_CPE_FF, id_CPE_LATCH))
            continue;
        int count = 0;
        EXPECT_EQ(impl->getPortTimingClass(usr.cell, usr.port, count), TMG_CLOCK_INPUT)
                << usr.cell->name.c_str(ctx) << "." << usr.port.c_str(ctx);
        auto info = impl->getPortClockingInfo(usr.cell, id_DIN, 0);
        EXPECT_EQ(info.clock_port.str(ctx), usr.port.str(ctx)) << "clock of " << usr.cell->name.c_str(ctx) << ".DIN";
        checked++;
    }
    EXPECT_EQ(checked, 2);
}

INSTANTIATE_TEST_SUITE_P(Stages, GateMateTimingPathTest, ::testing::Values(Stage::PLACED, Stage::ROUTED),
                         [](const ::testing::TestParamInfo<Stage> &info) {
                             return info.param == Stage::PLACED ? "placed" : "routed";
                         });

class GateMateFfTimingTest : public GateMateTimingTest, public ::testing::WithParamInterface<int>
{
  protected:
    // A CPE flip-flop bound to the given half (CPE_FF_U_Z or CPE_FF_L_Z) of the `index`-th CPE.
    CellInfo *placed_ff(int z, int clk_cfg, int index = 0)
    {
        Loc loc;
        int found = 0;
        for (auto bel : ctx->getBels()) {
            if (ctx->getBelType(bel) == id_CPE_FF_U && found++ == index) {
                loc = ctx->getBelLocation(bel);
                break;
            }
        }
        loc.z = z;
        CellInfo *ff = ctx->createCell(ctx->idf("ff_z%d_clk%d_%d", z, clk_cfg, index), id_CPE_FF);
        ff->addInput(id_CLK_INT);
        ff->addInput(id_DIN);
        ff->addOutput(id_DOUT);
        ff->params[id_C_CPE_CLK] = Property(clk_cfg, 2);
        ctx->bindBel(ctx->getBelByLocation(loc), ff, STRENGTH_USER);
        return ff;
    }
};

// Clock-to-Q of a CPE flip-flop includes the output path of the half it is placed in.
TEST_P(GateMateFfTimingTest, clock_to_q_uses_output_of_own_half)
{
    int z = GetParam();
    CellInfo *ff = placed_ff(z, 0b10);
    auto info = impl->getPortClockingInfo(ff, id_DOUT, 0);
    DelayQuad expected = timing_db(id_timing__SEQ_CLK_FF1_Q);
    expected += timing_db(z == CPE_FF_U_Z ? id_timing_Q2_OUT2 : id_timing_Q1_OUT1);
    expected += timing_db(id_timing_del_CPE_CP_Q);
    if (timing_db(id_timing_Q2_OUT2).maxDelay() == timing_db(id_timing_Q1_OUT1).maxDelay())
        GTEST_SKIP() << "timing database has the same delay for both halves";
    EXPECT_EQ(info.clockToQ.maxDelay(), expected.maxDelay());
    EXPECT_EQ(info.clockToQ.minDelay(), expected.minDelay());
}

TEST_P(GateMateFfTimingTest, clock_edge_follows_inversion)
{
    int z = GetParam();
    EXPECT_EQ(impl->getPortClockingInfo(placed_ff(z, 0b10, 0), id_DIN, 0).edge, RISING_EDGE);
    EXPECT_EQ(impl->getPortClockingInfo(placed_ff(z, 0b01, 1), id_DIN, 0).edge, FALLING_EDGE);
}

TEST_P(GateMateFfTimingTest, port_classes)
{
    CellInfo *ff = placed_ff(GetParam(), 0b10);
    int count = 0;
    EXPECT_EQ(impl->getPortTimingClass(ff, id_CLK_INT, count), TMG_CLOCK_INPUT);
    EXPECT_EQ(impl->getPortTimingClass(ff, id_DIN, count), TMG_REGISTER_INPUT);
    EXPECT_EQ(count, 1);
    EXPECT_EQ(impl->getPortTimingClass(ff, id_DOUT, count), TMG_REGISTER_OUTPUT);
    EXPECT_EQ(count, 1);
}

INSTANTIATE_TEST_SUITE_P(Halves, GateMateFfTimingTest, ::testing::Values(CPE_FF_U_Z, CPE_FF_L_Z),
                         [](const ::testing::TestParamInfo<int> &info) {
                             return info.param == CPE_FF_U_Z ? "upper" : "lower";
                         });

// common/kernel/timing_log.cc log_fmax: for a path between two related clocks of which neither
// has its own Fmax entry, clock_fmax.at() throws std::out_of_range (during placement, which
// reports timing). Here: a RAM and an output register in the IOSEL, both clocked from the fabric,
// each get the clock through a CPE of their own, two clocks related to the fabric clock. The RAM
// output goes to the IO register, and neither clock has paths of its own. (Another clock with
// register to register paths makes log_fmax report anything at all.)
TEST_F(GateMateTimingTest, related_clocks_without_own_paths)
{
    NetInfo *other = global_clock("other");
    CellInfo *ff1 = dff("ff1", input("d"), other);
    CellInfo *ff2 = dff("ff2", lut1("inv", 0b01, ff1->getPort(id_Q)), other);
    output("q2", ff2->getPort(id_Q));
    NetInfo *clk = input("clk");
    CellInfo *ram = ctx->createCell(ctx->id("ram"), id_CC_BRAM_20K);
    ram->params[id_RAM_MODE] = Property("TDP");
    for (auto p : {id_A_RD_WIDTH, id_A_WR_WIDTH})
        ram->params[p] = Property(1, 32);
    connect_input(ram, id_A_CLK, clk);
    connect_input(ram, id_A_EN, vcc());
    connect_input(ram, id_A_WE, gnd());
    connect_input(ram, ctx->id("A_ADDR[1]"), input("a"));
    CellInfo *ff = dff("ff", connect_output(ram, ctx->id("A_DO[0]")), clk);
    output("q", ff->getPort(id_Q))->params[id_FF_OBF] = Property(1, 1);
    pack();
    ASSERT_TRUE(bool_or_default(ctx->cells.at(ctx->id("q$iosel"))->params, id_OUT1_FF));
    EXPECT_NO_THROW(ctx->place());
}
