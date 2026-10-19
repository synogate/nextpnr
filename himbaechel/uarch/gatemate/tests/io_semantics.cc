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

// Functional checks of IO packing: values must reach the output pads unchanged (or inverted
// exactly where the netlist says so), independent of what other IOs exist.

#include "netlist_eval.h"
#include "testing.h"
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

namespace {
bool identity(const std::vector<bool> &v) { return v[0]; }
bool inverted(const std::vector<bool> &v) { return !v[0]; }
} // namespace

TEST_F(GateMateTest, output_follows_input)
{
    output("o", input("x"));
    pack();
    EXPECT_TRUE(output_computes(ctx, "o", {"x"}, identity));
}

TEST_F(GateMateTest, output_through_inverter)
{
    output("o", lut1("inv", 0b01, input("x")));
    pack();
    EXPECT_TRUE(output_computes(ctx, "o", {"x"}, inverted));
}

TEST_F(GateMateTest, constant_outputs)
{
    output("o0", gnd());
    output("o1", vcc());
    pack();
    EXPECT_TRUE(output_computes(ctx, "o0", {}, [](const std::vector<bool> &) { return false; }));
    EXPECT_TRUE(output_computes(ctx, "o1", {}, [](const std::vector<bool> &) { return true; }));
}

TEST_F(GateMateTest, input_fanout_to_several_outputs)
{
    NetInfo *x = input("x");
    for (int i = 0; i < 4; i++)
        output(stringf("o%d", i), x);
    output("n", lut1("inv", 0b01, x));
    pack();
    for (int i = 0; i < 4; i++)
        EXPECT_TRUE(output_computes(ctx, stringf("o%d", i), {"x"}, identity));
    EXPECT_TRUE(output_computes(ctx, "n", {"x"}, inverted));
}

class GateMateOddrInverterTest : public GateMateTest, public ::testing::WithParamInterface<bool>
{
  protected:
    // An ODDR output with an inverter in front of the output buffer; the packer folds the
    // inverter into the DDR register inputs.
    void add_inverted_ddr_output()
    {
        CellInfo *oddr = create_cell_ptr(id_CC_ODDR, "oddr");
        oddr->connectPort(id_D0, input("d0"));
        oddr->connectPort(id_D1, input("d1"));
        oddr->connectPort(id_CLK, input("clk"));
        oddr->connectPort(id_DDR, input("sel"));
        NetInfo *q = create_net("oddr$q");
        oddr->connectPort(id_Q, q);
        output("ddr_out", lut1("ddr_inv", 0b01, q));
    }
};

// The other outputs must not be affected by the inverter folded into the DDR output,
// whichever order the IOs are packed in.
TEST_P(GateMateOddrInverterTest, other_outputs_not_inverted)
{
    bool ddr_first = GetParam();
    if (ddr_first)
        add_inverted_ddr_output();
    NetInfo *p = input("p");
    for (int i = 0; i < 4; i++)
        output(stringf("plain%d", i), p);
    if (!ddr_first)
        add_inverted_ddr_output();
    pack();
    for (int i = 0; i < 4; i++)
        EXPECT_TRUE(output_computes(ctx, stringf("plain%d", i), {"p"}, identity)) << "output plain" << i;
}

TEST_F(GateMateOddrInverterTest, ddr_inputs_inverted)
{
    add_inverted_ddr_output();
    pack();
    // Both DDR data inputs of the output register get an inverting CPE in front of them
    CellInfo *iosel = ctx->cells.at(ctx->id("ddr_out$iosel")).get();
    EXPECT_TRUE(bool_or_default(iosel->params, id_USE_DDR, false));
    NetlistEvaluator ev(ctx);
    for (int value = 0; value < 2; value++) {
        ev.set_input(ctx->id("d0"), value);
        ev.set_input(ctx->id("d1"), value);
        EXPECT_EQ(ev.eval(iosel->getPort(id_OUT1)), value ? Tri::F : Tri::T) << "OUT1 for D1=" << value;
        EXPECT_EQ(ev.eval(iosel->getPort(id_OUT2)), value ? Tri::F : Tri::T) << "OUT2 for D0=" << value;
    }
}

INSTANTIATE_TEST_SUITE_P(Order, GateMateOddrInverterTest, ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool> &info) {
                             return info.param ? "ddr_created_first" : "ddr_created_last";
                         });
