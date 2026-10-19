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

// Registers and latches: every combination of CC_DFF / CC_DLT parameters and constant or signal
// control inputs must behave as in the reference model after packing and placement.

#include "netlist_eval.h"
#include "testing.h"
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

namespace {

enum class Control
{
    SIGNAL,
    ZERO,
    ONE,
};

const char *control_name(Control c) { return c == Control::SIGNAL ? "sig" : c == Control::ZERO ? "0" : "1"; }

// INIT: 0, 1 or undefined
void set_init(CellInfo *cell, int init)
{
    if (init < 2)
        cell->params[id_INIT] = Property(init, 1);
}

struct DffMatrixCase
{
    bool global_clock;
    bool clk_inv;
    int init;
};

} // namespace

class GateMateDffMatrixTest : public GateMateTest, public ::testing::WithParamInterface<DffMatrixCase>
{
};

// One design per clock configuration and INIT value, holding a register for every combination
// of enable and set/reset configuration, each with its own output.
TEST_P(GateMateDffMatrixTest, all_control_combinations)
{
    auto c = GetParam();
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                NetInfo *clk = c.global_clock ? global_clock("clk") : input("clk");
                NetInfo *d = input("d"), *en = input("en"), *sr = input("sr");
                auto control = [&](Control kind, NetInfo *sig) {
                    return kind == Control::SIGNAL ? sig : kind == Control::ZERO ? gnd() : vcc();
                };
                for (Control en_kind : {Control::SIGNAL, Control::ZERO, Control::ONE})
                    for (bool en_inv : {false, true})
                        for (Control sr_kind : {Control::SIGNAL, Control::ZERO, Control::ONE})
                            for (bool sr_inv : {false, true})
                                for (bool sr_val : {false, true}) {
                                    std::string name = stringf("ff_en%s%s_sr%s%s_v%d", control_name(en_kind),
                                                               en_inv ? "n" : "", control_name(sr_kind),
                                                               sr_inv ? "n" : "", int(sr_val));
                                    CellInfo *ff = dff(name, d, clk, control(en_kind, en), control(sr_kind, sr));
                                    ff->params[id_CLK_INV] = Property(c.clk_inv, 1);
                                    ff->params[id_EN_INV] = Property(en_inv, 1);
                                    ff->params[id_SR_INV] = Property(sr_inv, 1);
                                    ff->params[id_SR_VAL] = Property(sr_val, 1);
                                    set_init(ff, c.init);
                                    output("q_" + name, ff->getPort(id_Q));
                                }
            },
            300));
}

INSTANTIATE_TEST_SUITE_P(Clocks, GateMateDffMatrixTest,
                         ::testing::Values(DffMatrixCase{false, false, 0}, DffMatrixCase{false, false, 1},
                                           DffMatrixCase{false, false, 2}, DffMatrixCase{false, true, 0},
                                           DffMatrixCase{false, true, 1}, DffMatrixCase{false, true, 2},
                                           DffMatrixCase{true, false, 0}, DffMatrixCase{true, false, 1},
                                           DffMatrixCase{true, false, 2}, DffMatrixCase{true, true, 0},
                                           DffMatrixCase{true, true, 1}, DffMatrixCase{true, true, 2}),
                         [](const ::testing::TestParamInfo<DffMatrixCase> &info) {
                             return stringf("%s_%s_init%s", info.param.global_clock ? "global" : "fabric",
                                            info.param.clk_inv ? "negedge" : "posedge",
                                            info.param.init == 2 ? "x" : std::to_string(info.param.init).c_str());
                         });

class GateMateLatchMatrixTest : public GateMateTest, public ::testing::WithParamInterface<int>
{
};

TEST_P(GateMateLatchMatrixTest, all_control_combinations)
{
    int init = GetParam();
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                NetInfo *g = input("g"), *d = input("d"), *sr = input("sr");
                for (bool g_inv : {false, true})
                    for (Control sr_kind : {Control::SIGNAL, Control::ZERO, Control::ONE})
                        for (bool sr_inv : {false, true})
                            for (bool sr_val : {false, true}) {
                                std::string name = stringf("lat_g%s_sr%s%s_v%d", g_inv ? "n" : "",
                                                           control_name(sr_kind), sr_inv ? "n" : "", int(sr_val));
                                NetInfo *sr_net = sr_kind == Control::SIGNAL ? sr
                                                  : sr_kind == Control::ZERO ? gnd()
                                                                              : vcc();
                                CellInfo *latch = dlt(name, d, g, sr_net);
                                latch->params[id_G_INV] = Property(g_inv, 1);
                                latch->params[id_SR_INV] = Property(sr_inv, 1);
                                latch->params[id_SR_VAL] = Property(sr_val, 1);
                                set_init(latch, init);
                                output("q_" + name, latch->getPort(id_Q));
                            }
            },
            300));
}

INSTANTIATE_TEST_SUITE_P(Init, GateMateLatchMatrixTest, ::testing::Values(0, 1, 2),
                         [](const ::testing::TestParamInfo<int> &info) {
                             return info.param == 2 ? std::string("initx") : stringf("init%d", info.param);
                         });

// A register is packed into the CPE of the LUT driving it, also when the LUT drives other logic
// (the LUT then moves to the upper half and the lower half passes its output on).
TEST_F(GateMateTest, registers_merged_with_luts)
{
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                NetInfo *clk = global_clock("clk");
                NetInfo *a = input("a"), *b = input("b"), *c = input("c"), *e = input("e");
                // LUT only driving a register
                output("q1", dff("ff1", lut2("lut1", 0b0110, a, b), clk)->getPort(id_Q));
                // LUT driving a register and an output
                NetInfo *shared = lut2("lut2", 0b1000, b, c);
                output("q2", dff("ff2", shared, clk)->getPort(id_Q));
                output("comb2", shared);
                // LUT driving two compatible registers and logic
                NetInfo *shared3 = lut2("lut3", 0b1110, a, c);
                output("q3a", dff("ff3a", shared3, clk)->getPort(id_Q));
                output("q3b", dff("ff3b", shared3, clk)->getPort(id_Q));
                output("comb3", lut1("inv3", 0b01, shared3));
                // LUT tree with five inputs into a register
                CellInfo *l2t5 = create_cell_ptr(id_CC_L2T5, "l2t5");
                l2t5->params[id_INIT_L02] = Property(0b1000, 4);
                l2t5->params[id_INIT_L03] = Property(0b0110, 4);
                l2t5->params[id_INIT_L11] = Property(0b1110, 4);
                l2t5->params[id_INIT_L20] = Property(0b1001, 4);
                l2t5->connectPort(id_I0, a);
                l2t5->connectPort(id_I1, b);
                l2t5->connectPort(id_I2, c);
                l2t5->connectPort(id_I3, e);
                l2t5->connectPort(id_I4, b);
                l2t5->connectPort(id_O, create_net("l2t5$o"));
                output("q4", dff("ff4", ctx->nets.at(ctx->id("l2t5$o")).get(), clk)->getPort(id_Q));
            },
            300));
}

// Register chains and registers with different control sets (which must not share a CPE).
TEST_F(GateMateTest, registers_with_mixed_control_sets)
{
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                NetInfo *clk = global_clock("clk");
                NetInfo *d = input("d"), *en = input("en"), *rst = input("rst");
                CellInfo *ff1 = dff("ff1", d, clk);
                CellInfo *ff2 = dff("ff2", ff1->getPort(id_Q), clk, en);
                CellInfo *ff3 = dff("ff3", ff2->getPort(id_Q), clk, nullptr, rst);
                CellInfo *ff4 = dff("ff4", ff3->getPort(id_Q), clk, en, rst);
                ff4->params[id_CLK_INV] = Property(1, 1);
                ff4->params[id_SR_VAL] = Property(1, 1);
                for (CellInfo *ff : {ff1, ff2, ff3, ff4})
                    output("q_" + ff->name.str(ctx), ff->getPort(id_Q));
            },
            300));
}
