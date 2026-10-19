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

// IO registers (FF_IBF/FF_OBF), double data rate registers and tristate/bidirectional pads,
// compared against the reference models after packing and placement.

#include "known_bugs.h"
#include "netlist_eval.h"
#include "testing.h"
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

namespace {
struct IoClockCase
{
    bool global_clock;
    bool clk_inv;
};

std::string io_clock_name(const ::testing::TestParamInfo<IoClockCase> &info)
{
    return stringf("%s_%s", info.param.global_clock ? "global" : "fabric", info.param.clk_inv ? "negedge" : "posedge");
}

const auto io_clock_cases = ::testing::Values(IoClockCase{false, false}, IoClockCase{false, true},
                                              IoClockCase{true, false}, IoClockCase{true, true});
} // namespace

class GateMateIoRegisterTest : public GateMateTest, public ::testing::WithParamInterface<IoClockCase>
{
  protected:
    NetInfo *clock()
    {
        return GetParam().global_clock ? global_clock("clk") : input("clk");
    }
};

// A register directly behind an input buffer with FF_IBF is moved into the IOSEL.
TEST_P(GateMateIoRegisterTest, input_register)
{
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                NetInfo *clk = clock();
                NetInfo *d = input("d");
                ctx->cells.at(ctx->id("d"))->params[id_FF_IBF] = Property(1, 1);
                CellInfo *ff = dff("ff", d, clk);
                ff->params[id_CLK_INV] = Property(GetParam().clk_inv, 1);
                output("q", lut1("inv", 0b01, ff->getPort(id_Q)));
            },
            300));
}

// A register directly in front of an output buffer with FF_OBF is moved into the IOSEL.
TEST_P(GateMateIoRegisterTest, output_register)
{
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                NetInfo *clk = clock();
                CellInfo *ff = dff("ff", lut2("xor", 0b0110, input("a"), input("b")), clk);
                ff->params[id_CLK_INV] = Property(GetParam().clk_inv, 1);
                CellInfo *obuf = output("q", ff->getPort(id_Q));
                obuf->params[id_FF_OBF] = Property(1, 1);
            },
            300));
}

TEST_P(GateMateIoRegisterTest, ddr_input)
{
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                NetInfo *clk = clock();
                auto [q0, q1] = iddr("iddr", input("d"), clk, GetParam().clk_inv);
                output("q0", lut1("inv0", 0b01, q0));
                output("q1", lut1("inv1", 0b01, q1));
                output("x", lut2("xor", 0b0110, q0, q1));
            },
            300));
}

TEST_P(GateMateIoRegisterTest, ddr_output)
{
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                NetInfo *clk = clock();
                NetInfo *a = input("a"), *b = input("b");
                output("q", oddr("oddr", lut2("and", 0b1000, a, b), lut2("or", 0b1110, a, b), clk, input("sel"),
                                 GetParam().clk_inv));
            },
            300));
}

TEST_P(GateMateIoRegisterTest, ddr_output_through_inverter)
{
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                NetInfo *clk = clock();
                NetInfo *q = oddr("oddr", input("d0"), input("d1"), clk, input("sel"), GetParam().clk_inv);
                output("q", lut1("inv", 0b01, q));
                // An unrelated output next to it
                output("other", input("p"));
            },
            300));
}

// Several double data rate outputs share the DDR select of their bank.
TEST_P(GateMateIoRegisterTest, ddr_outputs_sharing_select)
{
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                NetInfo *clk = clock();
                NetInfo *sel = input("sel");
                for (int i = 0; i < 3; i++)
                    output(stringf("q%d", i), oddr(stringf("oddr%d", i), input(stringf("d0_%d", i)),
                                                   input(stringf("d1_%d", i)), clk, sel, GetParam().clk_inv));
            },
            300));
}

INSTANTIATE_TEST_SUITE_P(Clocks, GateMateIoRegisterTest, io_clock_cases, io_clock_name);

TEST_F(GateMateTest, tristate_output)
{
    EXPECT_TRUE(implementation_matches_reference([&] {
        NetInfo *a = input("a"), *t = input("t");
        tristate_output("o", lut1("inv", 0b01, a), t);
        tristate_output("o_t_inv", a, lut1("tinv", 0b01, t));
        tristate_output("o_on", a, gnd());
        tristate_output("o_off", a, vcc());
    }));
}

TEST_F(GateMateTest, bidirectional_pad)
{
    EXPECT_TRUE(implementation_matches_reference([&] {
        NetInfo *a = input("a"), *t = input("t");
        NetInfo *y = bidirectional("io", lut2("xor", 0b0110, a, t), t);
        // The pad value is used by the fabric: driven value while driving, external otherwise
        output("y", lut1("inv", 0b01, y));
        output("y_and_a", lut2("and", 0b1000, y, a));
    }));
}

TEST_F(GateMateTest, bidirectional_pad_with_registers)
{
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                NetInfo *clk = global_clock("clk");
                NetInfo *t = input("t");
                CellInfo *out_ff = dff("out_ff", input("d"), clk);
                CellInfo *io = create_cell_ptr(id_CC_IOBUF, "io");
                io->params[id_FF_OBF] = Property(1, 1);
                io->params[id_FF_IBF] = Property(1, 1);
                io->connectPort(id_A, out_ff->getPort(id_Q));
                io->connectPort(id_T, t);
                NetInfo *y = create_net("io$y");
                io->connectPort(id_Y, y);
                CellInfo *in_ff = dff("in_ff", y, clk);
                output("q", lut1("inv", 0b01, in_ff->getPort(id_Q)));
            },
            300));
}

// A path from an input register to an output register, both in the IOSEL and clocked from the
// fabric with the same clock.
TEST_F(GateMateTest, fabric_clocked_io_register_paths)
{
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                NetInfo *clk = input("clk");
                NetInfo *d = input("d");
                ctx->cells.at(ctx->id("d"))->params[id_FF_IBF] = Property(1, 1);
                CellInfo *in_ff = dff("in_ff", d, clk);
                CellInfo *out_ff = dff("out_ff", lut1("inv", 0b01, in_ff->getPort(id_Q)), clk);
                output("q", out_ff->getPort(id_Q))->params[id_FF_OBF] = Property(1, 1);
            },
            300));
}

// Inputs and outputs with registers in both directions on separate pads, with different clocks.
TEST_F(GateMateTest, io_registers_with_two_clocks)
{
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                NetInfo *clk_a = global_clock("clk_a");
                NetInfo *clk_b = input("clk_b");
                NetInfo *d = input("d");
                ctx->cells.at(ctx->id("d"))->params[id_FF_IBF] = Property(1, 1);
                CellInfo *in_ff = dff("in_ff", d, clk_b);
                CellInfo *out_ff = dff("out_ff", lut1("inv", 0b01, in_ff->getPort(id_Q)), clk_a);
                output("q", out_ff->getPort(id_Q))->params[id_FF_OBF] = Property(1, 1);
            },
            300));
}

// LVDS buffers (pin pairs A<n>/B<n> of bank SA), plain and with IO registers, next to
// single-ended IOs.
class GateMateLvdsSemanticsTest : public GateMateTest
{
  protected:
    CellInfo *lvds(IdString type, const std::string &name, int pair)
    {
        CellInfo *buffer = create_cell_ptr(type, name);
        buffer->params[id_PIN_NAME_P] = Property(stringf("IO_SA_A%d", pair));
        buffer->params[id_PIN_NAME_N] = Property(stringf("IO_SA_B%d", pair));
        return buffer;
    }

    // The single-ended IOs get pins outside bank SA (unconstrained IOs could get the LVDS pins)
    void pin_single_ended()
    {
        if (!avoid(KnownBug::PIN_NAME_PAD_REUSED))
            return;
        std::vector<std::string> pins;
        for (auto &pin : package_pins())
            if (pin.rfind("IO_SA_", 0) != 0)
                pins.push_back(pin);
        size_t next = 0;
        for (auto &cell : ctx->cells)
            if (cell.second->type.in(id_CC_IBUF, id_CC_OBUF, id_CC_TOBUF, id_CC_IOBUF))
                cell.second->params[id_PIN_NAME] = Property(pins.at(next++));
    }

    // An LVDS input, output, tristate output and bidirectional pad
    void lvds_pads(bool registers)
    {
        NetInfo *clk = global_clock("clk");
        NetInfo *a = input("a");
        NetInfo *t = input("t");
        CellInfo *li = lvds(id_CC_LVDS_IBUF, "li", 0);
        NetInfo *y = create_net("li$y");
        li->connectPort(id_Y, y);
        if (registers) {
            li->params[id_FF_IBF] = Property(1, 1);
            y = dff("li_reg", y, clk)->getPort(id_Q);
        }
        output("o_li", lut1("li_inv", 0b01, y));
        CellInfo *lo = lvds(id_CC_LVDS_OBUF, "lo", 1);
        NetInfo *lo_a = lut2("lo_xor", 0b0110, y, a);
        if (registers) {
            lo->params[id_FF_OBF] = Property(1, 1);
            lo_a = dff("lo_reg", lo_a, clk)->getPort(id_Q);
        }
        lo->connectPort(id_A, lo_a);
        CellInfo *lt = lvds(id_CC_LVDS_TOBUF, "lt", 2);
        lt->connectPort(id_A, lut1("lt_inv", 0b01, y));
        lt->connectPort(id_T, t);
        CellInfo *lb = lvds(id_CC_LVDS_IOBUF, "lb", 3);
        lb->connectPort(id_A, a);
        lb->connectPort(id_T, lut2("lb_t", 0b1000, t, y));
        NetInfo *lb_y = create_net("lb$y");
        lb->connectPort(id_Y, lb_y);
        output("o_lb", lut1("lb_inv", 0b01, lb_y));
        pin_single_ended();
    }
};

TEST_F(GateMateLvdsSemanticsTest, pads)
{
    EXPECT_TRUE(implementation_matches_reference([&] { lvds_pads(false); }, 300));
}

TEST_F(GateMateLvdsSemanticsTest, pads_with_registers)
{
    EXPECT_TRUE(implementation_matches_reference([&] { lvds_pads(true); }, 300));
    EXPECT_TRUE(bool_or_default(ctx->cells.at(ctx->id("li$iosel"))->params, id_IN1_FF));
    EXPECT_TRUE(bool_or_default(ctx->cells.at(ctx->id("lo$iosel"))->params, id_OUT1_FF));
}

TEST_F(GateMateLvdsSemanticsTest, double_data_rate)
{
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                NetInfo *clk = global_clock("clk");
                NetInfo *sel = input("sel");
                CellInfo *li = lvds(id_CC_LVDS_IBUF, "li", 0);
                NetInfo *y = create_net("li$y");
                li->connectPort(id_Y, y);
                auto [q0, q1] = iddr("li_ddr", y, clk);
                output("o_q0", q0);
                output("o_q1", q1);
                CellInfo *lo = lvds(id_CC_LVDS_OBUF, "lo", 1);
                lo->connectPort(id_A, oddr("lo_ddr", input("d0"), input("d1"), clk, sel));
                pin_single_ended();
            },
            300));
}
