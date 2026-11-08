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

// The logic of the GateMate cells for the logic check (logic_check.h, ArchAPI::evaluateCell): 64 input patterns at
// once, one bit per pattern.
//
// The models follow the simulator of the tests (tests/netlist_eval.cc, after the CPE schematic of Project
// Peppercorn) and the post-route CPE models of prjpeppercorn (cpesim.v). A cell evaluates the same before and after
// routing, as the passes after routing change it (finishRoute) and as prepareCellForMove changes it back:
//
//  - The LUT inputs of a LUT tree are the logical inputs D0_00 .. D1_03 (and D0_10 .. D1_11, which feed the second
//    level directly) before routing; finishRoute renames them to the CPE inputs the routing reached (IN1 .. IN4 of a
//    half, IN1 .. IN8 of a whole CPE) and rewrites the LUT tables for the inputs in that order. Every input of a
//    first-level LUT is therefore taken from its logical input if that is connected, else from the CPE input at the
//    same place (D0_00 / IN1, D1_00 / IN2, ...): before routing the CPE inputs are the logical ones, after routing the
//    tables are those of the CPE inputs.
//  - The second input of a first-level LUT can come from a carry line instead (C_I1 .. C_I4: PINY1, CINX, PINX).
//    Each half has its own two of these selects (the upper half C_I1 / C_I2, the lower half C_I3 / C_I4), so the
//    select tells where the input comes from without the location of the cell.
//  - A register's clock and enable are CLK_INT / EN_INT before routing, CLK / EN after, or the carry lines CINY2 /
//    PINY2 with C_CLKSEL / C_ENSEL, which are XORed with the clock / enable LUT1 (then a constant: 0b00 passes the
//    line, 0b11 inverts it). The sinks of a register are named after what they are ("CLK", "EN", ...), whichever pin
//    they arrive on.
//
// An unconnected input is 0 (CellEvalInputs gives 0 for it). A cell with an input connected that its model does not
// use (e.g. a carry line without the select that takes it in) is not one the model knows: evaluateCell returns false
// for it, so that the check compares its inputs instead of a function that leaves one out.

#include "gatemate.h"
#include "log.h"
#include "util.h"

#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {

using Word = uint64_t;

constexpr Word ALL = ~Word(0);

Word word(bool value) { return value ? ALL : Word(0); }

// sel ? d1 : d0, per pattern
Word mux(Word sel, Word d0, Word d1) { return (sel & d1) | (~sel & d0); }

// A LUT2: the bit index of `init` is {D1, D0}, as in CC_LUT2
Word lut2(int init, Word d0, Word d1)
{
    return (word(init & 1) & ~d1 & ~d0) | (word(init & 2) & ~d1 & d0) | (word(init & 4) & d1 & ~d0) |
           (word(init & 8) & d1 & d0);
}

// The 2-bit configurable inverters of the CPE register controls and of the multiplexer selects (C_CPE_CLK, C_CPE_EN,
// C_CPE_RES, C_CPE_SET): 0b00 constant 0, 0b11 constant 1, 0b10 passes, 0b01 inverts.
Word lut1(int cfg, Word in) { return mux(in, word(cfg & 1), word(cfg & 2)); }

const IdString cpe_inputs[8] = {id_IN1, id_IN2, id_IN3, id_IN4, id_IN5, id_IN6, id_IN7, id_IN8};

bool lut1_constant(int cfg) { return cfg == 0b00 || cfg == 0b11; }

// Reads the parameters and inputs of one cell, and remembers which inputs the model read
struct Cell
{
    Cell(const CellInfo *cell, const CellEvalInputs &inputs) : cell(cell), inputs(inputs) {}

    const CellInfo *cell;
    const CellEvalInputs &inputs;
    pool<IdString> read;

    int param(IdString name) const { return int_or_default(cell->params, name, 0); }
    bool flag(IdString name) const { return param(name) != 0; }
    bool has_param(IdString name) const { return cell->params.count(name) != 0; }

    bool is_input(IdString port) const
    {
        auto found = cell->ports.find(port);
        return found != cell->ports.end() && found->second.type != PORT_OUT;
    }
    bool connected(IdString port) const
    {
        if (!is_input(port))
            return false;
        bool is_connected = false;
        inputs.value(port, &is_connected);
        return is_connected;
    }
    Word in(IdString port)
    {
        read.insert(port);
        return is_input(port) ? inputs.value(port) : Word(0);
    }
    // The logical LUT input if it is connected, else the CPE input at its place
    Word pin(IdString logical, IdString cpe_input) { return connected(logical) ? in(logical) : in(cpe_input); }

    void out(CellEvalResult &result, IdString port, Word value) const
    {
        auto found = cell->ports.find(port);
        if (found != cell->ports.end() && found->second.type != PORT_IN)
            result.outputs[port] = value;
    }

    // Whether every connected input was read by the model
    bool all_inputs_used() const
    {
        for (auto &port : cell->ports)
            if (port.second.type != PORT_OUT && !read.count(port.first) && connected(port.first))
                return false;
        return true;
    }
};

// A half CPE (CPE_L2T4): L10 combines L00 and L01 (or D0_10 / D1_10 directly); a lower half passes the result
// through L20, which combines it with COMBIN, the result of the upper half (CC_L2T5). After placement repack()
// marks the upper halves with L2T4_UPPER, and gives every lower half an INIT_L20 (passing L10 where it had none).
// Upper halves have no L20 (cpesim.v). The carry functions of a lower half (C_FUNCTION, C_HORIZ selecting CINX
// instead of CINY1 as the carry in): C_EN_CIN ORs the carry into L01's result, C_ADDCIN XORs it into the output.
bool eval_l2t4(Cell &c, CellEvalResult &result)
{
    int func = c.param(id_C_FUNCTION);
    bool upper = c.flag(id_L2T4_UPPER);
    if (func != 0 && func != C_EN_CIN && func != C_ADDCIN)
        return false;
    if ((c.flag(id_C_I1) && c.flag(id_C_I3)) || (c.flag(id_C_I2) && c.flag(id_C_I4)))
        return false;
    bool carry = !upper && func != 0;
    Word d0_00 = c.pin(id_D0_00, id_IN1);
    Word d1_00 = (c.flag(id_C_I1) || c.flag(id_C_I3)) ? c.in(id_PINY1) : c.pin(id_D1_00, id_IN2);
    Word d0_01 = c.pin(id_D0_01, id_IN3);
    Word d1_01 = c.flag(id_C_I2) ? c.in(id_CINX) : c.flag(id_C_I4) ? c.in(id_PINX) : c.pin(id_D1_01, id_IN4);
    Word cin = 0;
    if (carry)
        cin = c.flag(id_C_HORIZ) ? c.in(id_CINX) : c.in(id_CINY1);
    Word l00 = lut2(c.param(id_INIT_L00), d0_00, d1_00);
    Word l01 = lut2(c.param(id_INIT_L01), d0_01, d1_01);
    if (carry && func == C_EN_CIN)
        l01 |= cin;
    Word a = c.connected(id_D0_10) ? c.in(id_D0_10) : l00;
    Word b = c.connected(id_D1_10) ? c.in(id_D1_10) : l01;
    Word value = lut2(c.param(id_INIT_L10), a, b);
    // (INIT_L20 = 0b1100 passes L10 on, which cpesim.v takes as a bypass: the same function)
    if (!upper && c.has_param(id_INIT_L20))
        value = lut2(c.param(id_INIT_L20), c.in(id_COMBIN), value);
    if (carry && func == C_ADDCIN)
        value ^= cin;
    c.out(result, id_OUT, value);
    return true;
}

// A whole CPE after placement (repack() merges the halves): CPE_ADDF, CPE_ADDF2 (adders, C_FUNCTION C_ADDF /
// C_ADDF2) and CPE_MX4 (C_MX4), as tests/netlist_eval.cc eval_cpe() models them.
bool eval_cpe(Cell &c, CellEvalResult &result)
{
    int func = c.param(id_C_FUNCTION);
    if (func != 0 && func != C_ADDF && func != C_ADDF2 && func != C_MX4)
        return false;
    bool c_addf = func == C_ADDF || func == C_ADDF2;
    bool c_addf2 = func == C_ADDF2;
    bool c_mx4 = func == C_MX4;
    // First level LUTs; the second input can come from a carry line instead (C_I1 .. C_I4). C_I1 and C_I3 both take
    // the one PINY1 (into L00 and into L02), which the routing does for a net feeding both.
    Word in1 = c.pin(id_D0_00, id_IN1);
    Word in2 = c.flag(id_C_I1) ? c.in(id_PINY1) : c.pin(id_D1_00, id_IN2);
    Word in3 = c.pin(id_D0_01, id_IN3);
    Word in4 = c.flag(id_C_I2) ? c.in(id_CINX) : c.pin(id_D1_01, id_IN4);
    Word in5 = c.pin(id_D0_02, id_IN5);
    Word in6 = c.flag(id_C_I3) ? c.in(id_PINY1) : c.pin(id_D1_02, id_IN6);
    Word in7 = c.pin(id_D0_03, id_IN7);
    Word in8 = c.flag(id_C_I4) ? c.in(id_PINX) : c.pin(id_D1_03, id_IN8);
    Word l00 = lut2(c.param(id_INIT_L00), in1, in2);
    Word l01 = lut2(c.param(id_INIT_L01), in3, in4);
    Word l02 = lut2(c.param(id_INIT_L02), in5, in6);
    Word l03 = lut2(c.param(id_INIT_L03), in7, in8);
    Word cin = c.flag(id_C_HORIZ) ? c.in(id_CINX) : c.in(id_CINY1);
    Word l10 = lut2(c.param(id_INIT_L10), c.connected(id_D0_10) ? c.in(id_D0_10) : l00,
                    c.connected(id_D1_10) ? c.in(id_D1_10) : l01);
    Word l11 = lut2(c.param(id_INIT_L11), c.connected(id_D0_11) ? c.in(id_D0_11) : l02,
                    c.connected(id_D1_11) ? c.in(id_D1_11) : l03);
    // In MX4 mode the inputs of L20 come from the two 4:1 multiplexers, whose data inputs (the CPE inputs, also
    // the inputs of the first-level LUTs at the same place) are masked by the X bits (a cleared bit gives a constant
    // 0) and inverted by the C bits: IN1 .. IN4 selected by L02 / L03 (X = INIT_L10, C = INIT_L11) into L20's D1,
    // IN5 .. IN8 selected by L00 / L01 (X = INIT_L11, C = INIT_L10) into D0 (cpesim.v CPE_MX4).
    auto mux4 = [&](int first, int mask, int invert, Word s0, Word s1) {
        static const IdString logical[8] = {id_D0_00, id_D1_00, id_D0_01, id_D1_01,
                                            id_D0_02, id_D1_02, id_D0_03, id_D1_03};
        Word data[4];
        for (int i = 0; i < 4; i++) {
            int k = first + i - 1;
            Word v = ((mask >> i) & 1) ? c.pin(logical[k], cpe_inputs[k]) : Word(0);
            data[i] = v ^ word((invert >> i) & 1);
        }
        return mux(s1, mux(s0, data[0], data[1]), mux(s0, data[2], data[3]));
    };
    Word d0 = c_mx4 ? mux4(5, c.param(id_INIT_L11), c.param(id_INIT_L10), l00, l01) : l10;
    Word d1 = c_mx4 ? mux4(1, c.param(id_INIT_L10), c.param(id_INIT_L11), l02, l03) : l11;
    // ADDF2: the carry between the two bits of the CPE (bit 0 is L02 / L03 / L11, bit 1 is L00 / L01 / L10)
    Word carry_mid = mux(l11, l02, cin);
    c.out(result, id_OUT1, lut2(c.param(id_INIT_L20), d0, d1) ^ (c_addf ? cin : Word(0)));
    // The packer's multiplexer uses L20's D1 only: the routing puts the selects on whichever inputs of L02 / L03 it
    // likes, which also feed the other multiplexer's data, so OUT2 (that multiplexer) is no function of the cell
    // before routing; it is left a free variable. (The same holds for OUT1 where INIT_L20 takes D0.)
    if (!c_mx4)
        c.out(result, id_OUT2, d0 ^ (c_addf2 ? carry_mid : Word(0)));
    if (c_addf && !c.flag(id_C_HORIZ)) {
        // The carry out of a vertical adder: CINY1 where the CPE propagates (CADD_S low), its generate signal
        // (CADD_A) otherwise; the CP outputs carry the same
        Word propagate = c_addf2 ? (l10 & l11) : (l10 ^ l11);
        Word generate = c_addf2 ? mux(l10, l00 | l01, l02) : (l00 | l01);
        Word carry_out = mux(propagate, generate, c.in(id_CINY1));
        c.out(result, id_COUTY1, carry_out);
        c.out(result, id_CPOUT1, carry_out);
        c.out(result, id_CPOUT2, carry_out);
    }
    return true;
}

// The 8:1 multiplexer of a CPE: data on IN1 .. IN8, the selects S0, S1, S2 from CLK, EN and SR through their LUT1s
// (C_CPE_CLK, C_CPE_EN, C_CPE_RES; a constant LUT1 takes no input), S0 / S1 XORed with CINY2 / PINY2 with C_CLKSEL /
// C_ENSEL (after routing through the carry lines, as for a register's clock and enable). The adder that adds C_SN and
// the carry in C_2D_IN to the select (C_BR selects C_SN alone, for bridges) has no model: the packer leaves them 0.
bool eval_mx8(Cell &c, CellEvalResult &result)
{
    for (IdString p : {id_C_BR, id_C_SN, id_C_2D_IN})
        if (c.param(p) != 0)
            return false;
    auto select = [&](IdString port, IdString cfg_param, IdString line_select, IdString line) {
        int cfg = c.param(cfg_param);
        Word s = lut1_constant(cfg) ? word(cfg & 1) : lut1(cfg, c.in(port));
        if (line_select != IdString() && c.flag(line_select))
            s ^= c.in(line);
        return s;
    };
    Word s0 = select(id_CLK, id_C_CPE_CLK, id_C_CLKSEL, id_CINY2);
    Word s1 = select(id_EN, id_C_CPE_EN, id_C_ENSEL, id_PINY2);
    Word s2 = select(id_SR, id_C_CPE_RES, IdString(), IdString());
    Word d[8];
    for (int i = 0; i < 8; i++)
        d[i] = c.in(cpe_inputs[i]);
    Word lower = mux(s1, mux(s0, d[0], d[1]), mux(s0, d[2], d[3]));
    Word upper = mux(s1, mux(s0, d[4], d[5]), mux(s0, d[6], d[7]));
    c.out(result, id_MUXOUT, mux(s2, lower, upper));
    return true;
}

// A register or latch of a CPE: nothing combinational (DOUT is a free variable of the check); its sinks are the
// effective controls, as cpesim.v CPE_FF / CPE_LATCH compute them:
//  - "D": the data input DIN;
//  - "CLK": the clock (of a latch: the gate), C_CPE_CLK applied to CLK_INT (before routing) or CLK, XORed with CINY2
//    with C_CLKSEL;
//  - "EN": the enable, likewise from EN_INT / EN, C_CPE_EN and PINY2 with C_ENSEL (latches take it too, as in the
//    test simulator: transparent while gate and enable are set);
//  - "RESET" / "SET": the asynchronous reset and set, active high: C_CPE_RES / C_CPE_SET inverted on SR, the set
//    taken from SR with C_EN_SR and from the enable input (EN_INT / EN, or PINY2 with C_ENSEL) otherwise.
// A constant LUT1 takes no input.
bool eval_register(const Context *ctx, Cell &c, CellEvalResult &result)
{
    auto routed = [&](IdString before, IdString after) {
        return c.connected(before) ? c.in(before) : c.in(after);
    };
    auto control = [&](IdString cfg_param, IdString before, IdString after, IdString line_select, IdString line) {
        int cfg = c.param(cfg_param);
        Word v = lut1_constant(cfg) ? word(cfg & 1) : lut1(cfg, routed(before, after));
        if (c.flag(line_select))
            v ^= c.in(line);
        return v;
    };
    result.sinks[id_D] = c.in(id_DIN);
    result.sinks[id_CLK] = control(id_C_CPE_CLK, id_CLK_INT, id_CLK, id_C_CLKSEL, id_CINY2);
    result.sinks[id_EN] = control(id_C_CPE_EN, id_EN_INT, id_EN, id_C_ENSEL, id_PINY2);
    int res = c.param(id_C_CPE_RES);
    result.sinks[ctx->id("RESET")] = ~(lut1_constant(res) ? word(res & 1) : lut1(res, c.in(id_SR)));
    int set = c.param(id_C_CPE_SET);
    Word set_source = 0;
    if (!lut1_constant(set))
        set_source = c.flag(id_C_EN_SR) ? c.in(id_SR) : c.flag(id_C_ENSEL) ? c.in(id_PINY2) : routed(id_EN_INT, id_EN);
    result.sinks[ctx->id("SET")] = ~(lut1_constant(set) ? word(set & 1) : lut1(set, set_source));
    return true;
}

// The CP lines of a CPE (cpesim.v CPE_CPLINES): each carry / propagate output passes its own line on, or with its
// C_*_I the value selected from the CPE outputs, the comparator and the other lines. A parameter not set is 0 (the
// passes after routing set every select the routing goes through).
bool eval_cplines(Cell &c, CellEvalResult &result)
{
    Word out1 = c.in(id_OUT1), out2 = c.in(id_OUT2), compout = c.in(id_COMPOUT);
    Word cinx = c.in(id_CINX), pinx = c.in(id_PINX);
    Word ciny1 = c.in(id_CINY1), piny1 = c.in(id_PINY1), ciny2 = c.in(id_CINY2), piny2 = c.in(id_PINY2);
    bool sel_c = c.flag(id_C_SEL_C), sel_p = c.flag(id_C_SEL_P);
    bool selx = c.flag(id_C_SELX), sely1 = c.flag(id_C_SELY1), sely2 = c.flag(id_C_SELY2);
    Word ciy12 = c.flag(id_C_Y12) ? ciny2 : ciny1;
    Word piy12 = c.flag(id_C_Y12) ? piny2 : piny1;
    Word cx_val = sel_c ? (selx ? ciy12 : compout) : (selx ? out2 : out1);
    Word px_val = sel_p ? (selx ? piy12 : compout) : (selx ? out1 : out2);
    Word cy1_val = sel_c ? (sely1 ? cinx : compout) : (sely1 ? out1 : out2);
    Word py1_val = sel_p ? (sely1 ? pinx : compout) : (sely1 ? out2 : out1);
    Word cy2_val = sel_c ? (sely2 ? cinx : compout) : (sely2 ? out2 : out1);
    Word py2_val = sel_p ? (sely2 ? pinx : compout) : (sely2 ? out1 : out2);
    c.out(result, id_COUTX, c.flag(id_C_CX_I) ? cx_val : cinx);
    c.out(result, id_COUTY1, c.flag(id_C_CY1_I) ? cy1_val : ciny1);
    c.out(result, id_COUTY2, c.flag(id_C_CY2_I) ? cy2_val : ciny2);
    c.out(result, id_POUTX, c.flag(id_C_PX_I) ? px_val : pinx);
    c.out(result, id_POUTY1, c.flag(id_C_PY1_I) ? py1_val : piny1);
    c.out(result, id_POUTY2, c.flag(id_C_PY2_I) ? py2_val : piny2);
    return true;
}

} // namespace

// Cell types, and what the check gets from them:
//  - combinational, outputs only: CPE_L2T4 (OUT), CPE_ADDF / CPE_ADDF2 (OUT1, OUT2, and in vertical adder mode
//    COUTY1, CPOUT1, CPOUT2), CPE_MX4 (OUT1), CPE_MX8 (MUXOUT), CPE_COMP (COMPOUT = the inverse of L30 on COMB1
//    (D0) and COMB2 (D1)), CPE_CPLINES (the six carry / propagate outputs), CPE_BRIDGE (MUXOUT = IN<C_SN + 1>),
//    CPE_RAMI / RAMO / RAMIO (RAM_O = I with C_RAM_O, OUT = RAM_I with C_RAM_I: the paths between the fabric and a
//    RAM's pins), the constant drivers (the packer's are CPE_L2T4 without inputs, INIT_L10 0b1111 or 0b0000; VCC /
//    GND cells as the frontend or a partition makes them drive all their outputs);
//  - sinks only: CPE_FF, CPE_FF_U, CPE_FF_L, CPE_LATCH ("D", "CLK", "EN", "RESET", "SET", see eval_register), the
//    output buffers ("A", the tristate ones also "T", compared as they are);
//  - nothing: the input buffers (their output is a free variable), the cells holding a place free (CPE_DUMMY,
//    CPE_FF_DUMMY, RAM_HALF_DUMMY).
// Everything else returns false: the halves of a whole CPE before repack() merges them (CPE_LT_U / CPE_LT_L: the
// lower half's output depends on the upper half's L10, which is no net, so neither half is a function of its own
// ports), CPE_MULT and CPE_CONCAT, the CPE functions without a model, and the cells whose port names stay as they are
// anyway (IOSEL, RAM, RAM_HALF, PLL, CLKIN, GLBOUT, USR_RSTN, SERDES, ...): the check compares their inputs by port
// name and takes their outputs as free variables.
bool GateMateImpl::evaluateCell(const CellInfo *cell, const CellEvalInputs &inputs, CellEvalResult &result) const
{
    Cell c(cell, inputs);
    bool known = false;
    IdString type = cell->type;
    if (type == id_CPE_L2T4)
        known = eval_l2t4(c, result);
    else if (type.in(id_CPE_ADDF, id_CPE_ADDF2, id_CPE_MX4))
        known = eval_cpe(c, result);
    else if (type == id_CPE_MX8)
        known = eval_mx8(c, result);
    else if (type.in(id_CPE_FF, id_CPE_FF_U, id_CPE_FF_L, id_CPE_LATCH))
        known = eval_register(ctx, c, result);
    else if (type == id_CPE_CPLINES)
        known = eval_cplines(c, result);
    else if (type == id_CPE_COMP) {
        // (cpesim.v: COMPOUT = ~L30, with COMB1 as the LUT's first input)
        c.out(result, id_COMPOUT, ~lut2(c.param(id_INIT_L30), c.in(id_COMB1), c.in(id_COMB2)));
        known = true;
    } else if (type == id_CPE_BRIDGE) {
        int sel = c.param(id_C_SN) & 7;
        c.out(result, id_MUXOUT, c.in(cpe_inputs[sel]));
        known = true;
    } else if (type.in(id_CPE_RAMI, id_CPE_RAMO, id_CPE_RAMIO, id_CPE_RAMIO_U, id_CPE_RAMIO_L)) {
        // Without C_RAM_O / C_RAM_I the path is not configured: the output is a free variable
        if (c.flag(id_C_RAM_O))
            c.out(result, id_RAM_O, c.in(id_I));
        if (c.flag(id_C_RAM_I))
            c.out(result, id_OUT, c.in(id_RAM_I));
        known = true;
    } else if (type.in(id_CPE_IBUF, id_CPE_LVDS_IBUF, id_CPE_DUMMY, id_CPE_FF_DUMMY, id_RAM_HALF_DUMMY)) {
        return true;
    } else if (type.in(id_CPE_OBUF, id_CPE_LVDS_OBUF)) {
        result.sinks[id_A] = c.in(id_A);
        return true;
    } else if (type.in(id_CPE_TOBUF, id_CPE_IOBUF, id_CPE_LVDS_TOBUF, id_CPE_LVDS_IOBUF)) {
        result.sinks[id_A] = c.in(id_A);
        result.sinks[id_T] = c.in(id_T);
        return true;
    } else if (type.in(ctx->id("VCC"), ctx->id("GND"))) {
        Word value = word(type == ctx->id("VCC"));
        for (auto &port : cell->ports)
            if (port.second.type == PORT_OUT)
                result.outputs[port.first] = value;
        return true;
    }
    if (!known || !c.all_inputs_used()) {
        result.outputs.clear();
        result.sinks.clear();
        return false;
    }
    return true;
}

NEXTPNR_NAMESPACE_END
