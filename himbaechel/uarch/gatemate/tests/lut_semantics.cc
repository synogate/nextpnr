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

// Functional checks of LUT and multiplexer packing: after packing, every output pad must
// compute the same function as the CC_* netlist, including when the packer folds constant or
// repeated inputs.

#include "netlist_eval.h"
#include "testing.h"
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

namespace {

// CC_LUT2 semantics: INIT bit index is {I1, I0}
bool lut2_value(int init, bool i0, bool i1) { return (init >> ((int(i1) << 1) | int(i0))) & 1; }

bool lut1_value(int init, bool i0) { return (init >> int(i0)) & 1; }

// CC_L2T4 semantics: L10 combines L00(I0, I1) and L01(I2, I3)
bool l2t4_value(int l00, int l01, int l10, bool i0, bool i1, bool i2, bool i3)
{
    return lut2_value(l10, lut2_value(l00, i0, i1), lut2_value(l01, i2, i3));
}

std::string init_name(const ::testing::TestParamInfo<int> &info) { return stringf("init_%x", info.param); }

} // namespace

class GateMateLut1Test : public GateMateTest, public ::testing::WithParamInterface<int>
{
};

TEST_P(GateMateLut1Test, signal_input)
{
    int init = GetParam();
    output("o", lut1("lut", init, input("x")));
    pack();
    EXPECT_TRUE(output_computes(ctx, "o", {"x"}, [&](const std::vector<bool> &v) { return lut1_value(init, v[0]); }));
}

TEST_P(GateMateLut1Test, constant_inputs)
{
    int init = GetParam();
    output("o0", lut1("lut0", init, gnd()));
    output("o1", lut1("lut1", init, vcc()));
    pack();
    EXPECT_TRUE(output_computes(ctx, "o0", {}, [&](const std::vector<bool> &) { return lut1_value(init, false); }));
    EXPECT_TRUE(output_computes(ctx, "o1", {}, [&](const std::vector<bool> &) { return lut1_value(init, true); }));
}

INSTANTIATE_TEST_SUITE_P(AllInits, GateMateLut1Test, ::testing::Range(0, 4), init_name);

class GateMateLut2Test : public GateMateTest, public ::testing::WithParamInterface<int>
{
};

TEST_P(GateMateLut2Test, distinct_inputs)
{
    int init = GetParam();
    output("o", lut2("lut", init, input("x"), input("y")));
    pack();
    EXPECT_TRUE(output_computes(ctx, "o", {"x", "y"},
                                [&](const std::vector<bool> &v) { return lut2_value(init, v[0], v[1]); }));
}

TEST_P(GateMateLut2Test, same_net_on_both_inputs)
{
    int init = GetParam();
    NetInfo *x = input("x");
    output("o", lut2("lut", init, x, x));
    pack();
    EXPECT_TRUE(
            output_computes(ctx, "o", {"x"}, [&](const std::vector<bool> &v) { return lut2_value(init, v[0], v[0]); }));
}

TEST_P(GateMateLut2Test, constant_i0)
{
    int init = GetParam();
    NetInfo *y = input("y");
    output("o0", lut2("lut0", init, gnd(), y));
    output("o1", lut2("lut1", init, vcc(), y));
    pack();
    EXPECT_TRUE(output_computes(ctx, "o0", {"y"},
                                [&](const std::vector<bool> &v) { return lut2_value(init, false, v[0]); }));
    EXPECT_TRUE(
            output_computes(ctx, "o1", {"y"}, [&](const std::vector<bool> &v) { return lut2_value(init, true, v[0]); }));
}

TEST_P(GateMateLut2Test, constant_i1)
{
    int init = GetParam();
    NetInfo *x = input("x");
    output("o0", lut2("lut0", init, x, gnd()));
    output("o1", lut2("lut1", init, x, vcc()));
    pack();
    EXPECT_TRUE(output_computes(ctx, "o0", {"x"},
                                [&](const std::vector<bool> &v) { return lut2_value(init, v[0], false); }));
    EXPECT_TRUE(
            output_computes(ctx, "o1", {"x"}, [&](const std::vector<bool> &v) { return lut2_value(init, v[0], true); }));
}

TEST_P(GateMateLut2Test, both_inputs_constant)
{
    int init = GetParam();
    for (int i = 0; i < 4; i++)
        output(stringf("o%d", i), lut2(stringf("lut%d", i), init, (i & 1) ? vcc() : gnd(), (i & 2) ? vcc() : gnd()));
    pack();
    for (int i = 0; i < 4; i++)
        EXPECT_TRUE(output_computes(ctx, stringf("o%d", i), {},
                                    [&](const std::vector<bool> &) { return lut2_value(init, i & 1, i & 2); }))
                << "inputs " << (i & 1) << ", " << ((i >> 1) & 1);
}

// The packer only removes LUTs when the output becomes a constant or a copy of an input, so
// also check a LUT whose input is driven by another LUT that becomes a copy of the same net.
TEST_P(GateMateLut2Test, input_equal_after_buffer_removal)
{
    int init = GetParam();
    NetInfo *x = input("x");
    NetInfo *buffered = lut1("buf", 0b10, x);
    output("o", lut2("lut", init, x, buffered));
    pack();
    EXPECT_TRUE(
            output_computes(ctx, "o", {"x"}, [&](const std::vector<bool> &v) { return lut2_value(init, v[0], v[0]); }));
}

INSTANTIATE_TEST_SUITE_P(AllInits, GateMateLut2Test, ::testing::Range(0, 16), init_name);

struct L2T4Case
{
    int l00, l01, l10;
};

class GateMateL2T4Test : public GateMateTest, public ::testing::WithParamInterface<L2T4Case>
{
};

TEST_P(GateMateL2T4Test, distinct_inputs)
{
    auto c = GetParam();
    output("o", l2t4("lut", c.l00, c.l01, c.l10, input("a"), input("b"), input("c"), input("d")));
    pack();
    EXPECT_TRUE(output_computes(ctx, "o", {"a", "b", "c", "d"}, [&](const std::vector<bool> &v) {
        return l2t4_value(c.l00, c.l01, c.l10, v[0], v[1], v[2], v[3]);
    }));
}

TEST_P(GateMateL2T4Test, repeated_inputs)
{
    auto c = GetParam();
    NetInfo *a = input("a"), *b = input("b");
    output("o", l2t4("lut", c.l00, c.l01, c.l10, a, a, b, b));
    pack();
    EXPECT_TRUE(output_computes(ctx, "o", {"a", "b"}, [&](const std::vector<bool> &v) {
        return l2t4_value(c.l00, c.l01, c.l10, v[0], v[0], v[1], v[1]);
    }));
}

TEST_P(GateMateL2T4Test, constant_inputs)
{
    auto c = GetParam();
    // Tie each input pin to a constant in turn
    for (int pin = 0; pin < 4; pin++) {
        for (int value = 0; value < 2; value++) {
            SCOPED_TRACE(stringf("I%d = %d", pin, value));
            init_context();
            std::vector<NetInfo *> ins;
            std::vector<std::string> names;
            for (int i = 0; i < 4; i++) {
                if (i == pin) {
                    ins.push_back(value ? vcc() : gnd());
                } else {
                    names.push_back(stringf("in%d", i));
                    ins.push_back(input(names.back()));
                }
            }
            output("o", l2t4("lut", c.l00, c.l01, c.l10, ins[0], ins[1], ins[2], ins[3]));
            pack();
            EXPECT_TRUE(output_computes(ctx, "o", names, [&](const std::vector<bool> &v) {
                bool in[4];
                for (int i = 0, j = 0; i < 4; i++)
                    in[i] = (i == pin) ? bool(value) : v[j++];
                return l2t4_value(c.l00, c.l01, c.l10, in[0], in[1], in[2], in[3]);
            }));
        }
    }
}

// A fixed pseudo-random selection of LUT tree configurations, plus the configurations yosys
// produces for plain 3- and 4-input gates.
INSTANTIATE_TEST_SUITE_P(Configs, GateMateL2T4Test,
                         ::testing::Values(L2T4Case{0b1000, 0b1000, 0b1000}, // AND4
                                           L2T4Case{0b1110, 0b1110, 0b1110}, // OR4
                                           L2T4Case{0b0110, 0b0110, 0b0110}, // XOR4
                                           L2T4Case{0b1000, 0b1010, 0b0111}, // NAND(AND(a,b), c)
                                           L2T4Case{0b0010, 0b0100, 0b1001}, L2T4Case{0b1101, 0b0011, 0b0100},
                                           L2T4Case{0b0101, 0b1100, 0b1110}, L2T4Case{0b1011, 0b0110, 0b0010},
                                           L2T4Case{0b0001, 0b1111, 0b1000}, L2T4Case{0b1010, 0b1010, 0b0110}),
                         [](const ::testing::TestParamInfo<L2T4Case> &info) {
                             return stringf("l00_%x_l01_%x_l10_%x", info.param.l00, info.param.l01, info.param.l10);
                         });

// Constants produced while the packer simplifies the netlist (here a LUT with a constant
// function) must still be driven when the original constant users have been folded away (here
// the register enables and resets, which become configuration bits).
TEST_F(GateMateTest, constants_created_during_cleanup_stay_driven)
{
    for (bool value : {false, true}) {
        SCOPED_TRACE(value ? "constant 1" : "constant 0");
        init_context();
        NetInfo *x = input("x"), *y = input("y"), *s = input("s");
        // The only users of the constant nets in the original netlist
        output("q", dff("ff", x, input("clk"))->getPort(id_Q));
        NetInfo *constant = lut2("const", value ? 0b1111 : 0b0000, x, y);
        output("o", mx2("mux", x, constant, s));
        pack();
        EXPECT_TRUE(output_computes(ctx, "o", {"x", "y", "s"},
                                    [&](const std::vector<bool> &v) { return v[2] ? value : v[0]; }));
    }
}

TEST_F(GateMateTest, mx2_signal_select)
{
    output("o", mx2("mux", input("d0"), input("d1"), input("s")));
    pack();
    EXPECT_TRUE(output_computes(ctx, "o", {"d0", "d1", "s"},
                                [](const std::vector<bool> &v) { return v[2] ? v[1] : v[0]; }));
}

TEST_F(GateMateTest, mx2_constant_select)
{
    NetInfo *d0 = input("d0"), *d1 = input("d1");
    output("o0", mx2("mux0", d0, d1, gnd()));
    output("o1", mx2("mux1", d0, d1, vcc()));
    pack();
    EXPECT_TRUE(output_computes(ctx, "o0", {"d0", "d1"}, [](const std::vector<bool> &v) { return v[0]; }));
    EXPECT_TRUE(output_computes(ctx, "o1", {"d0", "d1"}, [](const std::vector<bool> &v) { return v[1]; }));
}

TEST_F(GateMateTest, mx2_constant_data)
{
    NetInfo *s = input("s"), *d = input("d");
    output("o_sel", mx2("mux_sel", gnd(), vcc(), s));
    output("o_inv", mx2("mux_inv", vcc(), gnd(), s));
    output("o_and", mx2("mux_and", gnd(), d, s));
    output("o_or", mx2("mux_or", d, vcc(), s));
    pack();
    EXPECT_TRUE(output_computes(ctx, "o_sel", {"s"}, [](const std::vector<bool> &v) { return v[0]; }));
    EXPECT_TRUE(output_computes(ctx, "o_inv", {"s"}, [](const std::vector<bool> &v) { return !v[0]; }));
    EXPECT_TRUE(output_computes(ctx, "o_and", {"s", "d"}, [](const std::vector<bool> &v) { return v[0] && v[1]; }));
    EXPECT_TRUE(output_computes(ctx, "o_or", {"s", "d"}, [](const std::vector<bool> &v) { return v[0] || v[1]; }));
}

TEST_F(GateMateTest, mx2_same_data_inputs)
{
    NetInfo *d = input("d");
    output("o", mx2("mux", d, d, input("s")));
    pack();
    EXPECT_TRUE(output_computes(ctx, "o", {"d", "s"}, [](const std::vector<bool> &v) { return v[0]; }));
}

TEST_F(GateMateTest, mx4_constant_selects)
{
    NetInfo *d[4];
    for (int i = 0; i < 4; i++)
        d[i] = input(stringf("d%d", i));
    for (int sel = 0; sel < 4; sel++)
        output(stringf("o%d", sel), mx4(stringf("mux%d", sel), d[0], d[1], d[2], d[3], (sel & 1) ? vcc() : gnd(),
                                        (sel & 2) ? vcc() : gnd()));
    pack();
    for (int sel = 0; sel < 4; sel++)
        EXPECT_TRUE(output_computes(ctx, stringf("o%d", sel), {"d0", "d1", "d2", "d3"},
                                    [&](const std::vector<bool> &v) { return v[sel]; }))
                << "select " << sel;
}
