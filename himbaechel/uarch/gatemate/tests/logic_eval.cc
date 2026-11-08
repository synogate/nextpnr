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

// The logic of the cells for the logic check (GateMateImpl::evaluateCell, logic_eval.cc): truth tables of single
// cells, the same function of every cell before and after the passes that change cells (repack after placement, the
// passes after routing, prepareCellForMove), and agreement with the simulator of the tests (netlist_eval.cc).

#include <algorithm>
#include <map>
#include <memory>
#include <random>
#include <set>
#include "netlist_eval.h"
#include "testing.h"
#include "uarch/gatemate/extra_data.h"
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"
#include "cell_compare.h"
#include "logic_check.h"

USING_NEXTPNR_NAMESPACE

class GateMateLogicEvalTest : public GateMateTest
{
  protected:
    CellInfo *make_cell(const std::string &name, IdString type, std::initializer_list<IdString> inputs,
                        std::initializer_list<IdString> outputs)
    {
        CellInfo *cell = ctx->createCell(ctx->id(name), type);
        for (IdString p : inputs)
            cell->addInput(p);
        for (IdString p : outputs)
            cell->addOutput(p);
        return cell;
    }
    CellEvalResult eval(const CellInfo *cell, const PortInputs &in, bool expect_known = true)
    {
        CellEvalResult r;
        EXPECT_EQ(ctx->evaluateCell(cell, in, r), expect_known) << cell->name.c_str(ctx);
        return r;
    }

    // A random netlist of all the logic the packer makes CPEs from: LUTs, LUT trees (also five-input ones),
    // multiplexers (2:1, 4:1, 8:1, with constant selects), carry chains, registers with enables, set and reset,
    // latches, constants and fan-out
    void build(int seed)
    {
        std::mt19937 rng(seed);
        auto pick = [&](int n) { return int(rng() % unsigned(n)); };
        NetInfo *clk = global_clock("clk");
        std::vector<NetInfo *> nets;
        for (int i = 0; i < 10; i++)
            nets.push_back(input(stringf("in%d", i)));
        auto operand = [&]() -> NetInfo * {
            int r = pick(24);
            if (r == 0)
                return gnd();
            if (r == 1)
                return vcc();
            return nets.at(pick(int(nets.size())));
        };
        // The data of an 8:1 multiplexer from the logic, not from constants or input pads, whose ways to the
        // multiplexer often all invert ("can only be routed inverted", as tests/mx8.cc avoids it too)
        auto signal = [&]() {
            return nets.size() > 10 ? nets.at(10 + pick(int(nets.size()) - 10)) : nets.at(pick(int(nets.size())));
        };
        auto operands = [&](int n) {
            std::vector<NetInfo *> v;
            for (int i = 0; i < n; i++)
                v.push_back(operand());
            return v;
        };
        for (int n = 0; n < 70; n++) {
            std::string name = stringf("c%d", n);
            switch (pick(11)) {
            case 0: {
                int init = pick(4);
                nets.push_back(lut1(name, init, operand()));
                break;
            }
            case 1: {
                int init = pick(16);
                auto in = operands(2);
                nets.push_back(lut2(name, init, in[0], in[1]));
                break;
            }
            case 2: {
                int l00 = pick(16), l01 = pick(16), l10 = pick(16);
                auto in = operands(4);
                nets.push_back(l2t4(name, l00, l01, l10, in[0], in[1], in[2], in[3]));
                break;
            }
            case 3: {
                CellInfo *lut = create_cell_ptr(id_CC_L2T5, name);
                for (auto p : {id_INIT_L02, id_INIT_L03, id_INIT_L11, id_INIT_L20})
                    lut->params[p] = Property(pick(16), 4);
                auto in = operands(5);
                for (int i = 0; i < 5; i++)
                    lut->connectPort(ctx->idf("I%d", i), in[i]);
                NetInfo *o = create_net(name + "$o");
                lut->connectPort(id_O, o);
                nets.push_back(o);
                break;
            }
            case 4: {
                auto in = operands(3);
                nets.push_back(mx2(name, in[0], in[1], in[2]));
                break;
            }
            case 5: {
                auto in = operands(6);
                nets.push_back(mx4(name, in[0], in[1], in[2], in[3], in[4], in[5]));
                break;
            }
            case 6: {
                std::vector<NetInfo *> d;
                for (int i = 0; i < 8; i++)
                    d.push_back(signal());
                auto s = operands(3);
                nets.push_back(mx8(name, d, s));
                break;
            }
            case 7: {
                // The carry in from a primary input or a constant: a carry out continuing into another chain can
                // fail to route ("Failed to route arc ... CPOUT2_int to ... CINY1")
                int r = pick(12);
                NetInfo *carry = r == 0 ? gnd() : r == 1 ? vcc() : nets.at(pick(10));
                int width = 1 + pick(4);
                for (int i = 0; i < width; i++) {
                    auto in = operands(2);
                    auto [s, co] = addf(stringf("%s_%d", name.c_str(), i), in[0], in[1], carry);
                    nets.push_back(s);
                    carry = co;
                }
                nets.push_back(carry);
                break;
            }
            case 8: {
                NetInfo *d = operand();
                NetInfo *g = operand();
                NetInfo *sr = pick(2) ? operand() : nullptr;
                CellInfo *latch = dlt(name, d, g, sr);
                latch->params[id_G_INV] = Property(pick(4) == 0, 1);
                latch->params[id_SR_VAL] = Property(pick(2), 1);
                nets.push_back(latch->getPort(id_Q));
                break;
            }
            default: {
                NetInfo *d = operand();
                NetInfo *en = pick(2) ? operand() : nullptr;
                NetInfo *sr = pick(2) ? operand() : nullptr;
                // Sometimes a clock from the fabric
                NetInfo *c = pick(4) == 0 ? operand() : clk;
                CellInfo *ff = dff(name, d, c, en, sr);
                ff->params[id_CLK_INV] = Property(pick(4) == 0, 1);
                ff->params[id_EN_INV] = Property(pick(4) == 0, 1);
                ff->params[id_SR_INV] = Property(pick(4) == 0, 1);
                ff->params[id_SR_VAL] = Property(pick(2), 1);
                if (pick(2))
                    ff->params[id_INIT] = Property(pick(2), 1);
                nets.push_back(ff->getPort(id_Q));
                break;
            }
            }
        }
        for (int i = 0; i < 20; i++)
            output(stringf("o%d", i), nets.at(nets.size() - 1 - i));
        for (int i = 0; i < 8; i++)
            output(stringf("r%d", i), nets.at(10 + pick(int(nets.size()) - 10)));
    }
};

// The comparator: COMPOUT is the inverse of L30, whose first input (D0) is COMB1 and second (D1) COMB2
TEST_F(GateMateLogicEvalTest, comparator_output_inverted)
{
    CellInfo *comp = make_cell("comp", id_CPE_COMP, {id_COMB1, id_COMB2}, {id_COMPOUT});
    PortInputs in;
    in.values[id_COMB1] = 0xcccccccccccccccc;
    in.values[id_COMB2] = 0xaaaaaaaaaaaaaaaa;
    comp->params[id_INIT_L30] = Property(0b1000, 4); // AND
    EXPECT_EQ(eval(comp, in).outputs.at(id_COMPOUT), ~(in.values[id_COMB1] & in.values[id_COMB2]));
    comp->params[id_INIT_L30] = Property(0b0101, 4); // ~D0: passes COMB1 (pack_mult: "OUT1 -> COMP_OUT")
    EXPECT_EQ(eval(comp, in).outputs.at(id_COMPOUT), in.values[id_COMB1]);
    comp->params[id_INIT_L30] = Property(0b0011, 4); // ~D1: passes COMB2 (pack_mult: "L10 -> COMP_OUT")
    EXPECT_EQ(eval(comp, in).outputs.at(id_COMPOUT), in.values[id_COMB2]);
    comp->params[id_INIT_L30] = Property(0b1111, 4); // constant 0 out
    EXPECT_EQ(eval(comp, in).outputs.at(id_COMPOUT), Word(0));
}

// The 8:1 multiplexer selects IN<1 + S0 + 2 S1 + 4 S2>, the selects coming from CLK, EN and SR through their LUT1s,
// and after routing S0 / S1 possibly from the carry lines CINY2 / PINY2 (C_CLKSEL / C_ENSEL, the LUT1 a constant)
TEST_F(GateMateLogicEvalTest, mx8_select_order)
{
    CellInfo *mux = make_cell("mux", id_CPE_MX8,
                              {id_IN1, id_IN2, id_IN3, id_IN4, id_IN5, id_IN6, id_IN7, id_IN8, id_CLK, id_EN, id_SR},
                              {id_MUXOUT});
    mux->params[id_C_CPE_CLK] = Property(0b10, 2);
    mux->params[id_C_CPE_EN] = Property(0b10, 2);
    mux->params[id_C_CPE_RES] = Property(0b10, 2);
    std::mt19937_64 rng(3);
    PortInputs in;
    Word d[8];
    for (int i = 0; i < 8; i++)
        in.values[ctx->idf("IN%d", i + 1)] = d[i] = rng();
    Word s0 = rng(), s1 = rng(), s2 = rng();
    in.values[id_CLK] = s0;
    in.values[id_EN] = s1;
    in.values[id_SR] = s2;
    auto expected = [&](Word a, Word b, Word c) {
        Word r = 0;
        for (int bit = 0; bit < 64; bit++) {
            int sel = ((a >> bit) & 1) | (((b >> bit) & 1) << 1) | (((c >> bit) & 1) << 2);
            r |= ((d[sel] >> bit) & 1) << bit;
        }
        return r;
    };
    EXPECT_EQ(eval(mux, in).outputs.at(id_MUXOUT), expected(s0, s1, s2));
    // An inverting LUT1 inverts its select
    mux->params[id_C_CPE_CLK] = Property(0b01, 2);
    EXPECT_EQ(eval(mux, in).outputs.at(id_MUXOUT), expected(~s0, s1, s2));
    // After routing through the carry lines: the clock select from CINY2 (C_CPE_CLK 0b11 inverts it), the enable
    // select from PINY2 (0b00 passes it)
    mux->renamePort(id_CLK, id_CINY2);
    mux->renamePort(id_EN, id_PINY2);
    in.values.erase(id_CLK);
    in.values.erase(id_EN);
    in.values[id_CINY2] = s0;
    in.values[id_PINY2] = s1;
    mux->params[id_C_CLKSEL] = Property(1, 1);
    mux->params[id_C_ENSEL] = Property(1, 1);
    mux->params[id_C_CPE_CLK] = Property(0b11, 2);
    mux->params[id_C_CPE_EN] = Property(0b00, 2);
    EXPECT_EQ(eval(mux, in).outputs.at(id_MUXOUT), expected(~s0, s1, s2));
    // A constant select (the packer disconnects the select then)
    mux->disconnectPort(id_SR);
    in.values.erase(id_SR);
    mux->params[id_C_CPE_RES] = Property(0b11, 2);
    EXPECT_EQ(eval(mux, in).outputs.at(id_MUXOUT), expected(~s0, s1, ALL));
    // The select adder has no model
    mux->params[id_C_SN] = Property(1, 3);
    eval(mux, in, false);
}

// A lower half passes its LUT tree through L20, which combines it with COMBIN (from the upper half) as D0; an upper
// half (after placement: L2T4_UPPER) has no L20
TEST_F(GateMateLogicEvalTest, l2t4_combin_through_l20)
{
    CellInfo *lt = make_cell("lt", id_CPE_L2T4, {id_D0_10, id_D1_10, id_COMBIN}, {id_OUT});
    lt->params[id_INIT_L10] = Property(0b1000, 4); // AND
    PortInputs in;
    Word a = 0xf0f0f0f0f0f0f0f0, b = 0xff00ff00ff00ff00, combin = 0xcccccccccccccccc;
    in.values[id_D0_10] = a;
    in.values[id_D1_10] = b;
    in.values[id_COMBIN] = combin;
    lt->params[id_INIT_L20] = Property(0b0110, 4); // XOR
    EXPECT_EQ(eval(lt, in).outputs.at(id_OUT), combin ^ (a & b));
    lt->params[id_INIT_L20] = Property(0b0010, 4); // D0 & ~D1
    EXPECT_EQ(eval(lt, in).outputs.at(id_OUT), combin & ~(a & b));
    lt->params[id_INIT_L20] = Property(0b1100, 4); // passes L10
    EXPECT_EQ(eval(lt, in).outputs.at(id_OUT), a & b);
    lt->params[id_INIT_L20] = Property(0b1010, 4); // passes COMBIN
    EXPECT_EQ(eval(lt, in).outputs.at(id_OUT), combin);
    // An upper half ignores L20 and COMBIN... which it then must not have connected
    lt->params[id_L2T4_UPPER] = Property(1, 1);
    eval(lt, in, false);
    lt->disconnectPort(id_COMBIN);
    in.values.erase(id_COMBIN);
    EXPECT_EQ(eval(lt, in).outputs.at(id_OUT), a & b);
}

// The carry functions of a lower half: C_ADDCIN XORs the carry in (CINY1, or CINX with C_HORIZ) into the output,
// C_EN_CIN ORs it into L01's result
TEST_F(GateMateLogicEvalTest, l2t4_carry_in)
{
    CellInfo *lt = make_cell("lt", id_CPE_L2T4, {id_D0_00, id_D1_00, id_D0_01, id_D1_01, id_CINY1, id_CINX}, {id_OUT});
    PortInputs in;
    std::mt19937_64 rng(7);
    for (IdString p : {id_D0_00, id_D1_00, id_D0_01, id_D1_01, id_CINY1, id_CINX})
        in.values[p] = rng();
    Word l00 = in.values[id_D0_00] & in.values[id_D1_00];
    Word l01 = in.values[id_D0_01] | in.values[id_D1_01];
    lt->params[id_INIT_L00] = Property(0b1000, 4);
    lt->params[id_INIT_L01] = Property(0b1110, 4);
    lt->params[id_INIT_L10] = Property(0b0110, 4);
    lt->params[id_INIT_L20] = Property(0b1100, 4);
    lt->params[id_C_FUNCTION] = Property(C_ADDCIN, 3);
    PortInputs no_cinx = in;
    no_cinx.values.erase(id_CINX);
    EXPECT_EQ(eval(lt, no_cinx).outputs.at(id_OUT), l00 ^ l01 ^ in.values[id_CINY1]);
    PortInputs no_ciny1 = in;
    no_ciny1.values.erase(id_CINY1);
    lt->params[id_C_HORIZ] = Property(1, 1);
    EXPECT_EQ(eval(lt, no_ciny1).outputs.at(id_OUT), l00 ^ l01 ^ in.values[id_CINX]);
    lt->params[id_C_HORIZ] = Property(0, 1);
    lt->params[id_C_FUNCTION] = Property(C_EN_CIN, 3);
    lt->params[id_INIT_L10] = Property(0b1100, 4); // passes L01
    EXPECT_EQ(eval(lt, no_cinx).outputs.at(id_OUT), l01 | in.values[id_CINY1]);
    // Not in an upper half (cpesim.v), where the carry line then is an input without a use
    lt->params[id_L2T4_UPPER] = Property(1, 1);
    eval(lt, no_cinx, false);
    lt->params[id_L2T4_UPPER] = Property(0, 1);
    // A function a half CPE does not have
    lt->params[id_C_FUNCTION] = Property(C_MX4, 3);
    eval(lt, no_cinx, false);
}

// The inputs of a half CPE before routing (the logical LUT inputs) and after (the CPE inputs, or a carry line with
// C_I1 .. C_I4) are taken from the same place; a carry line without its select is not a known configuration
TEST_F(GateMateLogicEvalTest, l2t4_inputs_before_and_after_routing)
{
    std::mt19937_64 rng(9);
    Word x[4] = {rng(), rng(), rng(), rng()};
    auto tree = [&](Word a, Word b, Word c, Word d) { return (a ^ b) & ~(c | d); };
    CellInfo *before = make_cell("before", id_CPE_L2T4, {id_D0_00, id_D1_00, id_D0_01, id_D1_01}, {id_OUT});
    for (CellInfo *ci : {before}) {
        ci->params[id_INIT_L00] = Property(0b0110, 4);
        ci->params[id_INIT_L01] = Property(0b1110, 4);
        ci->params[id_INIT_L10] = Property(0b0010, 4); // D0 & ~D1
    }
    PortInputs in_before;
    in_before.values[id_D0_00] = x[0];
    in_before.values[id_D1_00] = x[1];
    in_before.values[id_D0_01] = x[2];
    in_before.values[id_D1_01] = x[3];
    EXPECT_EQ(eval(before, in_before).outputs.at(id_OUT), tree(x[0], x[1], x[2], x[3]));
    // Upper half after routing: IN1, PINY1 (C_I1), IN3, CINX (C_I2)
    CellInfo *upper = make_cell("upper", id_CPE_L2T4, {id_IN1, id_PINY1, id_IN3, id_CINX}, {id_OUT});
    upper->params = before->params;
    upper->params[id_C_I1] = Property(1, 1);
    upper->params[id_C_I2] = Property(1, 1);
    upper->params[id_L2T4_UPPER] = Property(1, 1);
    PortInputs in_upper;
    in_upper.values[id_IN1] = x[0];
    in_upper.values[id_PINY1] = x[1];
    in_upper.values[id_IN3] = x[2];
    in_upper.values[id_CINX] = x[3];
    EXPECT_EQ(eval(upper, in_upper).outputs.at(id_OUT), tree(x[0], x[1], x[2], x[3]));
    // Lower half after routing: IN1, PINY1 (C_I3), IN3, PINX (C_I4), passing L20
    CellInfo *lower = make_cell("lower", id_CPE_L2T4, {id_IN1, id_PINY1, id_IN3, id_PINX}, {id_OUT});
    lower->params = before->params;
    lower->params[id_C_I3] = Property(1, 1);
    lower->params[id_C_I4] = Property(1, 1);
    lower->params[id_L2T4_UPPER] = Property(0, 1);
    lower->params[id_INIT_L20] = Property(0b1100, 4);
    PortInputs in_lower;
    in_lower.values[id_IN1] = x[0];
    in_lower.values[id_PINY1] = x[1];
    in_lower.values[id_IN3] = x[2];
    in_lower.values[id_PINX] = x[3];
    EXPECT_EQ(eval(lower, in_lower).outputs.at(id_OUT), tree(x[0], x[1], x[2], x[3]));
    // Without C_I4, PINX is connected but not used
    lower->params.erase(id_C_I4);
    eval(lower, in_lower, false);
}

// A register's controls, before routing (CLK_INT, EN_INT) and after (CLK / EN, or the carry lines CINY2 / PINY2 with
// C_CLKSEL / C_ENSEL and the polarity moved into a constant LUT1), are the same sinks; reset and set are active high
TEST_F(GateMateLogicEvalTest, register_controls)
{
    std::mt19937_64 rng(11);
    Word din = rng(), clk = rng(), en = rng(), sr = rng();
    CellInfo *before = make_cell("before", id_CPE_FF, {id_DIN, id_CLK_INT, id_EN_INT, id_SR}, {id_DOUT});
    before->params[id_C_CPE_CLK] = Property(0b01, 2); // inverted
    before->params[id_C_CPE_EN] = Property(0b10, 2);
    before->params[id_C_CPE_RES] = Property(0b01, 2); // reset on SR high (CC_DFF SR without SR_INV)
    before->params[id_C_CPE_SET] = Property(0b11, 2); // no set
    PortInputs in_before;
    in_before.values[id_DIN] = din;
    in_before.values[id_CLK_INT] = clk;
    in_before.values[id_EN_INT] = en;
    in_before.values[id_SR] = sr;
    CellEvalResult r = eval(before, in_before);
    EXPECT_TRUE(r.outputs.empty());
    EXPECT_EQ(r.sinks.at(id_D), din);
    EXPECT_EQ(r.sinks.at(id_CLK), ~clk);
    EXPECT_EQ(r.sinks.at(id_EN), en);
    EXPECT_EQ(r.sinks.at(ctx->id("RESET")), sr);
    EXPECT_EQ(r.sinks.at(ctx->id("SET")), Word(0));

    CellInfo *after = make_cell("after", id_CPE_FF, {id_DIN, id_CINY2, id_PINY2, id_SR}, {id_DOUT});
    after->params = before->params;
    after->params[id_C_CLKSEL] = Property(1, 1);
    after->params[id_C_CPE_CLK] = Property(0b11, 2);
    after->params[id_C_ENSEL] = Property(1, 1);
    after->params[id_C_CPE_EN] = Property(0b00, 2);
    PortInputs in_after;
    in_after.values[id_DIN] = din;
    in_after.values[id_CINY2] = clk;
    in_after.values[id_PINY2] = en;
    in_after.values[id_SR] = sr;
    CellEvalResult r2 = eval(after, in_after);
    EXPECT_EQ(r2.sinks, r.sinks);

    // Set from SR (C_EN_SR), active low on SR (C_CPE_SET 0b10)
    before->params[id_C_CPE_RES] = Property(0b11, 2);
    before->params[id_C_CPE_SET] = Property(0b10, 2);
    before->params[id_C_EN_SR] = Property(1, 1);
    r = eval(before, in_before);
    EXPECT_EQ(r.sinks.at(ctx->id("RESET")), Word(0));
    EXPECT_EQ(r.sinks.at(ctx->id("SET")), ~sr);
}

// One net on both inputs of a first-level LUT: the LUT2 input swap being virtual, the router can take both from the
// same CPE input. finishRoute then keeps one port (the net has no user without a port) and folds the table onto the
// input it reaches, the other input not being routed. Likewise both carry-line selects of an adder taking the one
// PINY1. Every cell computes what it did before routing, and the logic check proves the routed design.
TEST_F(GateMateLogicEvalTest, same_net_on_both_inputs_of_a_lut)
{
    std::mt19937 rng(21);
    std::vector<NetInfo *> in;
    for (int i = 0; i < 8; i++)
        in.push_back(input(stringf("in%d", i)));
    auto pick = [&] { return in.at(rng() % in.size()); };
    int n = 0;
    for (int i = 0; i < 24; i++) {
        NetInfo *a = pick(), *b = pick();
        int init = rng() & 15, l00 = rng() & 15, l01 = rng() & 15, l10 = rng() & 15;
        output(stringf("o%d", n++), lut2(stringf("lut%d", i), init, a, a));
        output(stringf("o%d", n++), l2t4(stringf("tree%d", i), l00, l01, l10, a, a, b, b));
        output(stringf("o%d", n++), l2t4(stringf("tree3_%d", i), l01, l00, l10, a, a, a, b));
        output(stringf("o%d", n++), mx2(stringf("mux%d", i), a, b, a));
        if (i % 3 == 0) {
            auto [s0, c0] = addf(stringf("add%d_0", i), a, a, b);
            auto [s1, c1] = addf(stringf("add%d_1", i), a, b, c0);
            output(stringf("o%d", n++), s0);
            output(stringf("o%d", n++), s1);
            output(stringf("o%d", n++), c1);
        }
    }
    pack();
    ctx->place();
    Snapshot placed = take_snapshot(ctx);
    LogicSnapshot ref = LogicSnapshot::take(ctx);
    ctx->route();
    Comparer cmp(ctx, placed);
    cmp.run();
    EXPECT_TRUE(cmp.stats.failures.empty()) << failures_str(cmp.stats);
    // The case came up: cells with fewer input ports than before routing
    int merged_inputs = 0;
    for (auto &c : placed.cells) {
        auto now = ctx->cells.find(ctx->id(c.first));
        if (now == ctx->cells.end() || !now->second->type.in(id_CPE_L2T4, id_CPE_ADDF, id_CPE_ADDF2))
            continue;
        int before = 0, after = 0;
        for (auto &p : c.second.port_nets)
            before += c.second.info->ports.at(p.first).type == PORT_IN;
        for (auto &p : now->second->ports)
            after += p.second.type == PORT_IN && p.second.net;
        merged_inputs += after < before;
    }
    printf("cells with inputs merged by the routing: %d; %s\n", merged_inputs, cmp.stats.summary().c_str());
    EXPECT_GT(merged_inputs, 0);
    LogicCheckResult res = checkLogic(ctx, ref);
    EXPECT_EQ(res.mismatched, 0) << res.summary();
    EXPECT_EQ(res.proven, res.sinks) << res.summary();
}

// The packer's constant drivers (CPE_L2T4 without inputs, INIT_L10 0b1111 or 0b0000: those of the $PACKER_VCC /
// $PACKER_GND nets, and those feeding a register a constant) drive constants
TEST_F(GateMateLogicEvalTest, constant_drivers)
{
    NetInfo *x = input("x");
    NetInfo *clk = global_clock("clk");
    output("o0", lut2("and", 0b1000, x, vcc()));
    output("o1", vcc());
    output("o2", gnd());
    output("o3", dff("ff1", vcc(), clk, input("en"), gnd())->getPort(id_Q));
    output("o4", dff("ff0", gnd(), clk)->getPort(id_Q));
    output("o5", mx4("mux", x, gnd(), vcc(), input("y"), input("s0"), input("s1")));
    pack();
    int ones = 0, zeros = 0;
    for (auto &cell : ctx->cells) {
        const CellInfo *ci = cell.second.get();
        if (ci->type != id_CPE_L2T4 || !ci->getPort(id_OUT))
            continue;
        bool any_input = false;
        for (auto &port : ci->ports)
            any_input |= port.second.type == PORT_IN && port.second.net;
        if (any_input)
            continue;
        int init = int_or_default(ci->params, id_INIT_L10, 0);
        PortInputs none;
        CellEvalResult r = eval(ci, none);
        ASSERT_TRUE(r.outputs.count(id_OUT)) << cell.first.c_str(ctx);
        EXPECT_TRUE(init == 0 || init == 15) << cell.first.c_str(ctx);
        EXPECT_EQ(r.outputs.at(id_OUT), init == 15 ? ALL : Word(0)) << cell.first.c_str(ctx);
        (init == 15 ? ones : zeros)++;
    }
    EXPECT_GT(ones, 0);
    EXPECT_GT(zeros, 0);
    // VCC / GND cells (as the frontend or a partition makes them)
    CellInfo *vcc_cell = make_cell("vcc_cell", ctx->id("VCC"), {}, {id_Y});
    CellInfo *gnd_cell = make_cell("gnd_cell", ctx->id("GND"), {}, {id_Y});
    PortInputs none;
    EXPECT_EQ(eval(vcc_cell, none).outputs.at(id_Y), ALL);
    EXPECT_EQ(eval(gnd_cell, none).outputs.at(id_Y), Word(0));
}

// The halves of a whole CPE before repack() are no functions of their own ports, nor are cells kept by port name
TEST_F(GateMateLogicEvalTest, unmodelled_cells)
{
    PortInputs none;
    eval(make_cell("lt_l", id_CPE_LT_L, {id_D0_00}, {id_OUT}), none, false);
    eval(make_cell("lt_u", id_CPE_LT_U, {id_D0_00}, {id_OUT}), none, false);
    eval(make_cell("iosel", id_IOSEL, {id_OUT1}, {id_GPIO_OUT}), none, false);
    eval(make_cell("dummy", id_CPE_DUMMY, {}, {}), none, true);
}

struct LogicEvalCase
{
    int seed;
    std::vector<std::string> vopts;
    std::string label;
};

class GateMateLogicEvalFlowTest : public GateMateLogicEvalTest, public ::testing::WithParamInterface<LogicEvalCase>
{
};

// Every cell computes the same, as evaluateCell sees it, after packing and after placement (repack(), the moves of
// place-repair), after routing (the LUT inputs renamed to the CPE inputs, the LUT tables rewritten, the subtrees
// swapped, carry lines, the register controls through the carry lines, bridges and CP lines inserted) and after
// prepareCellForMove on every cell
TEST_P(GateMateLogicEvalFlowTest, same_function_through_the_flow)
{
    std::vector<std::string> opts = {"--allow-unconstrained"};
    for (auto &o : GetParam().vopts)
        opts.push_back(o);
    init_context("CCGM1A1", opts);
    build(GetParam().seed);
    pack();
    Snapshot packed = take_snapshot(ctx);
    ctx->place();
    {
        Comparer cmp(ctx, packed);
        cmp.same_type_only = true;
        cmp.run();
        EXPECT_TRUE(cmp.stats.failures.empty()) << "packed -> placed:" << failures_str(cmp.stats);
        EXPECT_GT(cmp.stats.count("CPE_L2T4"), 0);
        printf("packed -> placed: %s\n", cmp.stats.summary().c_str());
    }
    Snapshot placed = take_snapshot(ctx);
    try {
        ctx->route();
    } catch (const log_execution_error_exception &) {
        GTEST_SKIP() << "the design does not route";
    }
    {
        Comparer cmp(ctx, placed);
        cmp.run();
        EXPECT_TRUE(cmp.stats.failures.empty()) << "placed -> routed:" << failures_str(cmp.stats);
        for (const char *type : {"CPE_L2T4", "CPE_FF", "CPE_ADDF2", "CPE_MX4", "CPE_MX8"})
            EXPECT_GT(cmp.stats.count(type), 0) << type << " " << cmp.stats.summary();
        printf("placed -> routed: %s\n", cmp.stats.summary().c_str());
    }
    int prepared = 0;
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (!ci->type.in(id_CPE_L2T4, id_CPE_MX4, id_CPE_ADDF, id_CPE_ADDF2, id_CPE_FF, id_CPE_FF_U, id_CPE_FF_L,
                         id_CPE_LATCH, id_CPE_MX8))
            continue;
        if (!impl->prepareCellForMove(ci, true).empty())
            continue;
        EXPECT_EQ(impl->prepareCellForMove(ci, false), "");
        prepared++;
    }
    {
        Comparer cmp(ctx, placed);
        cmp.run();
        EXPECT_TRUE(cmp.stats.failures.empty()) << "placed -> prepared for moving:" << failures_str(cmp.stats);
        printf("placed -> prepared for moving (%d cells): %s\n", prepared, cmp.stats.summary().c_str());
    }
}

INSTANTIATE_TEST_SUITE_P(Designs, GateMateLogicEvalFlowTest,
                         ::testing::Values(LogicEvalCase{1, {}, "default"}, LogicEvalCase{2, {}, "default"},
                                           LogicEvalCase{3, {}, "default"}, LogicEvalCase{4, {}, "default"},
                                           LogicEvalCase{8, {}, "default"}, LogicEvalCase{9, {}, "default"},
                                           LogicEvalCase{5, {"--lut-input-swap"}, "lut_input_swap"},
                                           LogicEvalCase{6, {"--lut-swap-rounds=2"}, "lut_swap_rounds"},
                                           LogicEvalCase{7, {"--net-repair=2"}, "net_repair"}),
                         [](const ::testing::TestParamInfo<LogicEvalCase> &info) {
                             return stringf("%s_seed%d", info.param.label.c_str(), info.param.seed);
                         });

// The combinational cells compute what the simulator of the tests computes (netlist_eval.cc eval_cell, given the
// values of their input nets), in a placed and in a routed design; outputs the simulator has no model for, and
// patterns where it gives an unknown value, are left out
TEST_F(GateMateLogicEvalTest, agrees_with_simulator)
{
    int routed = 0;
    for (int seed : {1, 2, 3}) {
        init_context();
        build(seed);
        pack();
        ctx->place();
        for (int stage = 0; stage < 2; stage++) {
            if (stage == 1) {
                try {
                    ctx->route();
                } catch (const log_execution_error_exception &) {
                    printf("seed %d does not route\n", seed);
                    break;
                }
                routed++;
            }
            NetlistEvaluator ev(ctx);
            std::mt19937_64 rng(seed);
            std::map<std::string, int> compared;
            int unknown = 0;
            for (auto &cell : ctx->cells) {
                const CellInfo *ci = cell.second.get();
                if (!ci->type.in(id_CPE_L2T4, id_CPE_ADDF, id_CPE_ADDF2, id_CPE_MX4, id_CPE_MX8, id_CPE_CPLINES,
                                 id_CPE_RAMI, id_CPE_RAMO, id_CPE_RAMIO))
                    continue;
                PortInputs in;
                std::map<const NetInfo *, Word> net_values;
                for (auto &port : ci->ports) {
                    if (port.second.type != PORT_IN || !port.second.net)
                        continue;
                    if (!net_values.count(port.second.net))
                        net_values[port.second.net] = rng();
                    in.values[port.first] = net_values.at(port.second.net);
                }
                CellEvalResult r;
                if (!ctx->evaluateCell(ci, in, r)) {
                    ADD_FAILURE() << ci->name.c_str(ctx) << " (" << ci->type.c_str(ctx) << ") not modelled"
                                  << params_str(ctx, ci);
                    continue;
                }
                for (auto &out : r.outputs) {
                    if (!ci->getPort(out.first))
                        continue;
                    bool ok = true, any = false;
                    for (int bit = 0; bit < 64 && ok; bit++) {
                        std::unordered_map<const NetInfo *, Tri> values;
                        for (auto &nv : net_values)
                            values[nv.first] = ((nv.second >> bit) & 1) ? Tri::T : Tri::F;
                        Tri t;
                        try {
                            t = ev.eval_cell(ci, out.first, values);
                        } catch (const EvalUnsupported &) {
                            break;
                        }
                        if (t == Tri::X) {
                            unknown++;
                            continue;
                        }
                        any = true;
                        if ((t == Tri::T) != bool((out.second >> bit) & 1)) {
                            ADD_FAILURE() << (stage ? "routed" : "placed") << " seed " << seed << ": "
                                          << ci->name.c_str(ctx) << " (" << ci->type.c_str(ctx) << ") "
                                          << out.first.c_str(ctx) << " pattern " << bit << ": simulator "
                                          << tri_str(t) << ", evaluateCell " << ((out.second >> bit) & 1)
                                          << params_str(ctx, ci);
                            ok = false;
                        }
                    }
                    if (any && ok)
                        compared[ci->type.str(ctx)]++;
                }
            }
            std::string s;
            for (auto &c : compared)
                s += stringf(" %s %d", c.first.c_str(), c.second);
            printf("seed %d %s: outputs compared:%s; unknown patterns %d\n", seed, stage ? "routed" : "placed",
                   s.c_str(), unknown);
            for (const char *type : {"CPE_L2T4", "CPE_ADDF2", "CPE_MX4", "CPE_MX8"})
                EXPECT_GT(compared.count(type) ? compared.at(type) : 0, 0) << type << ":" << s;
        }
    }
    // (8:1 multiplexers fail to route now and then, "can only be routed inverted")
    EXPECT_GT(routed, 0);
}
