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

// Properties the GUI shows for a selected bel, wire or pip: the configuration of the cell on a bel
// in words (LUT functions, register modes, carry line sources, IO settings) and what a routing
// wire or mux is.

#include <functional>
#include <map>

#include "gatemate.h"
#include "nextpnr.h"

#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {

using Attrs = std::vector<std::pair<IdString, std::string>>;

// LUT2 function of inputs A (D0) and B (D1); INIT bit i is the output for (B << 1) | A
const char *lut2_function(int init)
{
    static const char *names[16] = {"0",     "~(A | B)", "A & ~B", "~B",     "~A & B", "~A",     "A ^ B", "~(A & B)",
                                    "A & B", "~(A ^ B)", "A",      "A | ~B", "B",      "~A | B", "A | B", "1"};
    return names[init & 15];
}

std::string binary(int value, int width)
{
    std::string s;
    for (int i = width - 1; i >= 0; i--)
        s += ((value >> i) & 1) ? '1' : '0';
    return s;
}

struct AttrWriter
{
    const Context *ctx;
    Attrs &out;
    void add(const std::string &key, const std::string &value)
    {
        if (!value.empty())
            out.emplace_back(ctx->id(key), value);
    }
};

// The node of a tile wire, looked up by its tile-local name
WireId tile_wire(const Context *ctx, int tile, const std::string &name)
{
    IdString id = ctx->id(name);
    const auto &tt = chip_tile_info(ctx->chip_info, tile);
    for (int i = 0; i < tt.wires.ssize(); i++)
        if (tt.wires[i].name == id.index)
            return ctx->normalise_wire(tile, i);
    return WireId();
}

std::string net_on(const Context *ctx, int tile, const std::string &wire_name)
{
    WireId w = tile_wire(ctx, tile, wire_name);
    if (w == WireId())
        return "";
    const NetInfo *net = ctx->getBoundWireNet(w);
    return net ? net->name.str(ctx) : "";
}

// Tile-local name of the source of the used pip into a wire (the input a mux selects), or ""
std::string selected_source(const Context *ctx, int tile, const std::string &wire_name)
{
    WireId w = tile_wire(ctx, tile, wire_name);
    if (w == WireId())
        return "";
    for (PipId pip : ctx->getPipsUphill(w)) {
        if (pip.tile != tile || ctx->getBoundPipNet(pip) == nullptr)
            continue;
        const auto &pd = chip_pip_info(ctx->chip_info, pip);
        return IdString(chip_tile_info(ctx->chip_info, tile).wires[pd.src_wire].name).str(ctx);
    }
    return "";
}

int param(const CellInfo *cell, IdString name, int def = -1) { return int_or_default(cell->params, name, def); }

std::string with_net(const std::string &what, const std::string &net)
{
    return net.empty() ? what : what + " (" + net + ")";
}

void describe_cpe_logic(const Context *ctx, const CellInfo *cell, IdString bel_type, int tile, AttrWriter &w)
{
    static const char *functions[] = {"LUT tree",
                                      "adder (ADDF)",
                                      "two-bit adder (ADDF2)",
                                      "multiplier (MULT)",
                                      "4:1 multiplexer (MX4)",
                                      "LUT tree with carry enable (EN_CIN)",
                                      "concatenated LUT trees (CONCAT)",
                                      "adder with carry input (ADDCIN)"};
    int function = param(cell, id_C_FUNCTION, 0);
    w.add("Function", function >= 0 && function < 8 ? functions[function] : std::to_string(function));

    // The cell's LUT2s and their inputs in the CPE. After routing, nextpnr gives the LUT2 contents in
    // terms of the physical inputs (postRoute resolves the input swaps the router may use): A of
    // L00 is IN1, B is IN2 or the carry line selected by C_I1, and so on. A cell on a half bel names
    // its LUT2s as the upper half does (L00, L01, L10, and L20 for the combining LUT2).
    struct Lut
    {
        const char *param;
        const char *physical;
        const char *a, *b; // a wire of the CPE ("IN1_int"), or what drives the input ("=L00 output")
    };
    static const Lut upper[] = {{"INIT_L00", "L00", "IN1_int", "TI2_int"},
                                {"INIT_L01", "L01", "IN3_int", "TI4_int"},
                                {"INIT_L10", "L10", "=L00 output", "=L01 output"}};
    static const Lut lower[] = {{"INIT_L00", "L02", "IN5_int", "TI6_int"},
                                {"INIT_L01", "L03", "IN7_int", "TI8_int"},
                                {"INIT_L10", "L11", "=L02 output", "=L03 output"},
                                {"INIT_L20", "L20", "=upper half (COMBIN)", "=L11 output"}};
    static const Lut full[] = {{"INIT_L00", "L00", "IN1_int", "TI2_int"},
                               {"INIT_L01", "L01", "IN3_int", "TI4_int"},
                               {"INIT_L02", "L02", "IN5_int", "TI6_int"},
                               {"INIT_L03", "L03", "IN7_int", "TI8_int"},
                               {"INIT_L10", "L10", "=L00 output", "=L01 output"},
                               {"INIT_L11", "L11", "=L02 output", "=L03 output"},
                               {"INIT_L20", "L20", "=upper half (COMBIN)", "=L11 output"}};
    struct
    {
        const Lut *b, *e;
        const Lut *begin() const { return b; }
        const Lut *end() const { return e; }
    } luts{std::begin(full), std::end(full)};
    if (bel_type == id_CPE_LT_U)
        luts = {std::begin(upper), std::end(upper)};
    else if (bel_type == id_CPE_LT_L)
        luts = {std::begin(lower), std::end(lower)};
    std::map<std::string, std::pair<int, const Lut *>> used; // physical name -> INIT, LUT2
    for (auto &lut : luts) {
        IdString id = ctx->id(lut.param);
        if (cell->params.count(id))
            used[lut.physical] = {param(cell, id, 0), &lut};
    }
    // What drives an input: a net, or the output of another LUT2; through a LUT2 that only passes
    // on (or inverts) one of its inputs, what drives that input.
    std::function<std::string(const char *)> input = [&](const char *src) -> std::string {
        if (*src != '=') {
            std::string net = net_on(ctx, tile, std::string("CPE.") + src);
            return net.empty() ? "unused" : net;
        }
        std::string text = src + 1;
        std::string from = text.substr(0, text.find(' '));
        auto it = used.find(from);
        if (it == used.end() || text.find(" output") == std::string::npos)
            return text;
        int init = it->second.first;
        const Lut *l = it->second.second;
        std::string through = " (through " + from + ")";
        if (init == 0b1010 || init == 0b0101)
            return (init == 0b0101 ? "~" : "") + input(l->a) + through;
        if (init == 0b1100 || init == 0b0011)
            return (init == 0b0011 ? "~" : "") + input(l->b) + through;
        return text;
    };
    for (auto &lut : luts) {
        if (!used.count(lut.physical))
            continue;
        int init = used.at(lut.physical).first;
        w.add(std::string(lut.physical) + " function",
              std::string(lut2_function(init)) + "  (INIT " + binary(init, 4) + ")");
        w.add(std::string(lut.physical) + " A", input(lut.a));
        w.add(std::string(lut.physical) + " B", input(lut.b));
    }

    // Inputs replaced by carry/propagate lines
    static const std::pair<const char *, const char *> carry_inputs[] = {{"C_I1", "IN2 taken from PINY1"},
                                                                         {"C_I2", "IN4 taken from CINX"},
                                                                         {"C_I3", "IN6 taken from PINY1"},
                                                                         {"C_I4", "IN8 taken from PINX"}};
    for (auto &ci : carry_inputs)
        if (param(cell, ctx->id(ci.first), 0) == 1)
            w.add(ci.first, ci.second);

    // Outputs: what the output mux selects
    auto output = [&](int n) {
        std::string src = selected_source(ctx, tile, "CPE.OUT" + std::to_string(n) + "_int");
        std::string what;
        if (src.empty())
            return;
        if (src.find("DOUT") != std::string::npos)
            what = "register";
        else if (src.find("MUXOUT") != std::string::npos)
            what = "8:1 multiplexer (MX8)";
        else if (src.find("CPOUT") != std::string::npos)
            what = "carry/propagate output";
        else if (src.find("COMBOUT") != std::string::npos)
            what = "LUT tree";
        else
            what = src;
        w.add("OUT" + std::to_string(n), with_net(what, net_on(ctx, tile, "CPE.OUT" + std::to_string(n))));
    };
    if (bel_type != id_CPE_LT_L)
        output(2);
    if (bel_type != id_CPE_LT_U)
        output(1);
}

void describe_ff(const Context *ctx, const CellInfo *cell, bool upper, int tile, AttrWriter &w)
{
    bool latch = param(cell, id_C_L_D, 0) == 1;
    w.add("Type", latch ? "latch" : "flip-flop");
    std::string clk_net = net_on(ctx, tile, "CPE.CLK_int");
    switch (param(cell, id_C_CPE_CLK, 0)) {
    case 0b10:
        w.add(latch ? "Gate" : "Clock", with_net(latch ? "transparent when high" : "rising edge", clk_net));
        break;
    case 0b01:
        w.add(latch ? "Gate" : "Clock", with_net(latch ? "transparent when low" : "falling edge", clk_net));
        break;
    default:
        w.add(latch ? "Gate" : "Clock", "constant (never clocked)");
        break;
    }
    if (param(cell, id_C_CLKSEL, 0) == 1)
        w.add("Clock source", "CINY2 (carry line) instead of CLK");
    std::string en_net = net_on(ctx, tile, "CPE.EN_int");
    switch (param(cell, id_C_CPE_EN, 0b11)) {
    case 0b11:
        w.add("Enable", "always enabled");
        break;
    case 0b00:
        w.add("Enable", "never enabled");
        break;
    case 0b10:
        w.add("Enable", with_net("active high", en_net));
        break;
    case 0b01:
        w.add("Enable", with_net("active low", en_net));
        break;
    }
    if (param(cell, id_C_ENSEL, 0) == 1)
        w.add("Enable source", "PINY2 (carry line) instead of EN");
    std::string sr_net = net_on(ctx, tile, "CPE.SR");
    auto set_reset = [&](const char *what, int v) {
        switch (v) {
        case 0b11:
            w.add(what, "unused");
            break;
        case 0b00:
            w.add(what, "always active");
            break;
        case 0b01:
            w.add(what, with_net("from SR, active high", sr_net));
            break;
        case 0b10:
            w.add(what, with_net("from SR, active low", sr_net));
            break;
        }
    };
    set_reset("Reset", param(cell, id_C_CPE_RES, 0b11));
    set_reset("Set", param(cell, id_C_CPE_SET, 0b11));
    if (param(cell, id_C_EN_SR, 0) == 1)
        w.add("C_EN_SR", "1");
    int init = param(cell, id_FF_INIT, -1);
    w.add("Initial value", init == 0b11 ? "1" : (init == 0b10 ? "0" : "not set"));

    std::string din = upper ? "CPE.DIN2_int" : "CPE.DIN1_int";
    std::string src = selected_source(ctx, tile, din);
    std::string what = src.empty() ? "LUT tree of this half" : src;
    if (src.find("COMBOUT1") != std::string::npos)
        what = upper ? "lower LUT tree" : "LUT tree of this half";
    else if (src.find("COMBOUT2") != std::string::npos)
        what = "upper LUT tree";
    else if (src.find("MUXOUT") != std::string::npos)
        what = "8:1 multiplexer (MX8)";
    w.add("Data input", with_net(what, net_on(ctx, tile, din)));
    w.add("Output", net_on(ctx, tile, upper ? "CPE.DOUT2_int" : "CPE.DOUT1_int"));
}

// Each carry/propagate output line of the CPE that a net uses, and where its signal comes from:
// follow the used pips back through the internal selects to a CPE output or an input line.
void describe_cplines(const Context *ctx, int tile, AttrWriter &w)
{
    for (const char *out : {"COUTX", "COUTY1", "COUTY2", "POUTX", "POUTY1", "POUTY2"}) {
        std::string wire = std::string("CPE.") + out;
        std::string net = net_on(ctx, tile, wire);
        if (net.empty())
            continue;
        std::string src = wire;
        for (int i = 0; i < 4; i++) {
            std::string prev = selected_source(ctx, tile, src);
            if (prev.empty())
                break;
            src = prev;
            if (src.find("_VAL") == std::string::npos && src.find("_OUT") == std::string::npos &&
                src.find("IY12") == std::string::npos)
                break;
        }
        std::string what = src.substr(src.find('.') + 1);
        if (what == "OUT1_IN_int" || what == "OUT2_IN_int")
            what = what.substr(0, 4);
        else if (what == "COMPOUT_IN_int")
            what = "comparator (COMPOUT)";
        else if (what.compare(0, 2, "CI") == 0 || what.compare(0, 2, "PI") == 0)
            what = (src == wire ? "" : "passed on from ") + what;
        w.add(out, with_net(what, net));
    }
}

const char *drive_strength(int v)
{
    static const char *ma[4] = {"3 mA", "6 mA", "9 mA", "12 mA"};
    return ma[v & 3];
}

void describe_iosel(const Context *ctx, const CellInfo *cell, AttrWriter &w)
{
    auto has = [&](IdString id) { return cell->params.count(id) > 0; };
    auto p = [&](IdString id) { return param(cell, id, 0); };
    bool in_reg = p(id_IN1_FF) || p(id_IN2_FF), out_reg = p(id_OUT1_FF) || p(id_OUT2_FF);
    if (p(id_INPUT_ENABLE))
        w.add("Input",
              std::string("enabled") + (in_reg ? ", registered" : "") + (p(id_IN1_FF) && p(id_IN2_FF) ? " (DDR)" : ""));
    if (p(id_OE_ENABLE) || p(id_OUT_SIGNAL))
        w.add("Output", std::string(p(id_OE_SIGNAL) ? "tristate" : "enabled") + (out_reg ? ", registered" : "") +
                                (p(id_USE_DDR) ? " (DDR)" : ""));
    // The IO registers' clock: a global clock (its index), or from the fabric (SEL_*_CLOCK)
    auto clock = [&](bool inverted, IdString global, IdString fabric) {
        std::string edge = inverted ? "falling edge of " : "rising edge of ";
        if (p(fabric))
            return edge + "a fabric clock";
        if (has(global))
            return edge + "global clock " + std::to_string(p(global));
        return std::string("constant (never clocked)");
    };
    if (in_reg)
        w.add("Input clock", clock(p(id_INV_IN1_CLOCK), id_IN_CLOCK, id_SEL_IN_CLOCK));
    if (out_reg)
        w.add("Output clock", clock(p(id_INV_OUT1_CLOCK), id_OUT_CLOCK, id_SEL_OUT_CLOCK));
    if (has(id_DRIVE))
        w.add("Drive strength", drive_strength(p(id_DRIVE)));
    if (has(id_SLEW))
        w.add("Slew rate", p(id_SLEW) ? "slow" : "fast");
    bool up = p(id_PULLUP), down = p(id_PULLDOWN);
    if (up || down)
        w.add("Pull", up && down ? "keeper" : (up ? "pull-up" : "pull-down"));
    if (p(id_LVDS_EN))
        w.add("LVDS", "enabled");
    for (auto &kv : cell->params)
        if (kv.first.in(id_DELAY_IBF, id_DELAY_OBF))
            w.add(kv.first.str(ctx), kv.second.to_string());
}

// From packing on, a RAM's configuration is in its RAM_cfg_* parameters (pack_bram.cc), per port
// (a, b) and per 20K half (0, 1); a RAM block that holds two 20K RAMs has all four ports.
void describe_ram(const Context *ctx, const CellInfo *cell, AttrWriter &w)
{
    auto cfg = [&](const std::string &name) { return param(cell, ctx->id("RAM_cfg_" + name), 0); };
    int sram_mode = param(cell, id_RAM_cfg_sram_mode, -1);
    if (sram_mode >= 0)
        w.add("Mode", std::string((sram_mode & 2) ? "SDP" : "TDP") +
                              ((sram_mode & 1) ? ", two 20K RAMs (ports A0/B0 and A1/B1)" : ", 40K"));
    static const int widths[8] = {0, 1, 2, 5, 10, 20, 40, 80};
    for (const char *half : {"0", "1"}) {
        for (const char *port : {"a", "b"}) {
            std::string p = std::string(port) + half;
            int rd = cfg("output_config_" + p) & 7, wr = cfg("input_config_" + p) & 7;
            if (!rd && !wr)
                continue;
            std::string desc =
                    "read " + std::to_string(widths[rd]) + " bit, write " + std::to_string(widths[wr]) + " bit";
            if (cfg(p + "_set_outputreg"))
                desc += ", output register";
            if (wr && cfg(p + "_writemode"))
                desc += ", write-through";
            w.add(std::string("Port ") + char(toupper(port[0])) + half, desc);
        }
    }
    for (auto &kv : cell->params)
        if (kv.first.str(ctx).compare(0, 8, "RAM_cfg_") == 0)
            w.add(kv.first.str(ctx), kv.second.to_string());
}

void describe_params(const Context *ctx, const CellInfo *cell, AttrWriter &w)
{
    for (auto &kv : cell->params)
        w.add(kv.first.str(ctx), kv.second.is_string ? kv.second.as_string() : kv.second.to_string());
}

} // namespace

Attrs GateMateImpl::getBelAttrs(BelId bel) const
{
    Attrs attrs;
    AttrWriter w{ctx, attrs};
    IdString type = ctx->getBelType(bel);
    const PadInfoPOD *pad = type.in(id_GPIO, id_IOSEL) ? tile_pad(ctx->getBelLocation(bel)) : nullptr;
    if (pad != nullptr) {
        w.add("Package pin", IdString(pad->package_pin).str(ctx));
        w.add("Pad", IdString(pad->pad_function).str(ctx));
        w.add("Bank", std::to_string(pad->pad_bank));
    }
    const CellInfo *cell = ctx->getBoundBelCell(bel);
    if (cell == nullptr)
        return attrs;
    int tile = bel.tile;
    if (type.in(id_CPE_LT_U, id_CPE_LT_L, id_CPE_LT_FULL))
        describe_cpe_logic(ctx, cell, type, tile, w);
    else if (type.in(id_CPE_FF_U, id_CPE_FF_L))
        describe_ff(ctx, cell, type == id_CPE_FF_U, tile, w);
    else if (type == id_CPE_CPLINES)
        describe_cplines(ctx, tile, w);
    else if (type == id_CPE_COMP) {
        // The comparator is a LUT2 (L30) behind L20, whose output is inverted: INIT_L30 is the
        // inverse of what it computes. Its output goes only into the carry logic.
        int init = param(cell, id_INIT_L30, 0);
        w.add("Function", std::string(lut2_function(~init & 15)) + "  (INIT_L30 " + binary(init, 4) +
                                  ", output inverted)");
        w.add("A", with_net("COMB1, from L20", net_on(ctx, tile, "CPE.COMBIN1_int")));
        w.add("B", with_net("COMB2, from L10", net_on(ctx, tile, "CPE.COMBIN2_int")));
        w.add("Output", with_net("COMPOUT, into the carry logic", net_on(ctx, tile, "CPE.COMPOUT_int")));
    }
    else if (type.in(id_CPE_RAMIO_U, id_CPE_RAMIO_L)) {
        bool upper = type == id_CPE_RAMIO_U;
        if (param(cell, id_C_RAM_O, 0))
            w.add("RAM input", with_net(upper ? "driven through RAM_O2" : "driven through RAM_O1",
                                        net_on(ctx, tile, upper ? "CPE.RAM_O2" : "CPE.RAM_O1")));
        if (param(cell, id_C_RAM_I, 0))
            w.add("RAM output", with_net(upper ? "taken in through RAM_I2" : "taken in through RAM_I1",
                                         net_on(ctx, tile, upper ? "CPE.RAM_I2" : "CPE.RAM_I1")));
    } else if (type == id_IOSEL)
        describe_iosel(ctx, cell, w);
    else if (type.in(id_RAM, id_RAM_HALF))
        describe_ram(ctx, cell, w);
    else
        describe_params(ctx, cell, w);
    return attrs;
}

Attrs GateMateImpl::getPipAttrs(PipId pip) const
{
    Attrs attrs;
    AttrWriter w{ctx, attrs};
    const auto *extra_ptr = pip_extra_data(pip);
    if (extra_ptr == nullptr)
        return attrs;
    const auto &extra = *extra_ptr;
    std::string name = IdString(extra.name).str(ctx);
    if (extra.type == PIP_EXTRA_MUX && !name.empty()) {
        w.add("Mux", name);
        w.add("Selects input", std::to_string(extra.value) + " of " + std::to_string(1 << extra.bits));
        w.add("Config bits", binary(extra.value, extra.bits));
        w.add("Inverting", (extra.flags & MUX_INVERT) ? "yes" : "no");
    } else {
        w.add("Mux", "none (fixed connection)");
    }
    if (extra.plane)
        w.add("Plane", std::to_string(extra.plane));
    const NetInfo *net = ctx->getBoundPipNet(pip);
    if (net)
        w.add("Used by", net->name.str(ctx));
    return attrs;
}

Attrs GateMateImpl::getWireAttrs(WireId wire) const
{
    Attrs attrs;
    AttrWriter w{ctx, attrs};
    // Describe the switch box and input mux wires by what they connect to
    std::string name = IdString(chip_wire_info(ctx->chip_info, wire).name).str(ctx);
    size_t p = name.find(".P");
    if (p != std::string::npos &&
        (name.compare(0, 2, "SB") == 0 || name.compare(0, 2, "IM") == 0 || name.compare(0, 2, "OM") == 0))
        w.add("Plane", std::to_string(atoi(name.c_str() + p + 2)));
    static const char *directions[] = {"", "west", "south", "east", "north"};
    static const int distances_big[] = {0, 0, 2, 4, 8, 12, 16, 20};
    size_t d = name.find(".D");
    if (name.compare(0, 2, "SB") == 0 && d != std::string::npos && name.size() > d + 4 && name[d + 3] == '_') {
        int k = name[d + 2] - '0', dir = name[d + 4] - '0';
        if (k >= 2 && k <= 7 && dir >= 1 && dir <= 4)
            w.add("Connects to", "switch box " + std::to_string(distances_big[k]) + " tiles " + directions[dir]);
    }
    size_t y = name.find(".Y");
    if (name.compare(0, 2, "SB") == 0 && y != std::string::npos && isdigit(name[y + 2])) {
        static const char *drives[] = {"", "east", "north", "west", "south"};
        w.add("Drives", std::string("switch boxes and input muxes to the ") + drives[name[y + 2] - '0']);
    }
    return attrs;
}

namespace {

// A LUT2 function in a few characters, for labels in the drawing
const char *lut2_short(int init)
{
    static const char *names[16] = {"0",   "NOR",  "A&~B", "~B",   "~A&B", "~A",   "XOR", "NAND",
                                    "AND", "XNOR", "A",    "A|~B", "B",    "~A|B", "OR",  "1"};
    return names[init & 15];
}

} // namespace

const PadInfoPOD *GateMateImpl::tile_pad(Loc loc) const
{
    // The package data refers to the IOSEL bel of a pad
    for (BelId bel : ctx->getBelsByTile(loc.x, loc.y))
        if (const PadInfoPOD *pad = ctx->get_bel_package_pin(bel))
            return pad;
    return nullptr;
}

void GateMateImpl::update_gui_labels()
{
    dict<BelId, std::vector<std::string>> labels;
    for (auto &cell : ctx->cells) {
        const CellInfo *ci = cell.second.get();
        if (ci->bel == BelId())
            continue;
        IdString type = ctx->getBelType(ci->bel);
        std::vector<std::string> l;
        auto lut = [&](const char *name) -> std::string {
            IdString id = ctx->id(name);
            return ci->params.count(id) ? lut2_short(param(ci, id, 0)) : "";
        };
        if (type.in(id_CPE_LT_U, id_CPE_LT_L, id_CPE_LT_FULL)) {
            // The LUT2s by position in the CPE: L00, L01, L02, L03, L10, L11, L20; then the mode
            l.assign(7, "");
            if (type == id_CPE_LT_U) {
                l[0] = lut("INIT_L00"), l[1] = lut("INIT_L01"), l[4] = lut("INIT_L10");
            } else if (type == id_CPE_LT_L) {
                l[2] = lut("INIT_L00"), l[3] = lut("INIT_L01"), l[5] = lut("INIT_L10"), l[6] = lut("INIT_L20");
            } else {
                const char *names[7] = {"INIT_L00", "INIT_L01", "INIT_L02", "INIT_L03",
                                        "INIT_L10", "INIT_L11", "INIT_L20"};
                for (int i = 0; i < 7; i++)
                    l[i] = lut(names[i]);
            }
            std::string mode = ci->type.str(ctx);
            if (mode.compare(0, 4, "CPE_") == 0)
                mode = mode.substr(4);
            l.push_back(mode == "L2T4" ? "LUT" : mode);
        } else if (type == id_CPE_COMP) {
            // what it computes: its output is inverted (see getBelAttrs)
            l.push_back(lut2_short(~param(ci, id_INIT_L30, 0) & 15));
        } else if (type == id_GPIO) {
            l.push_back(ci->name.str(ctx));
        } else if (type.in(id_CPE_RAMIO_U, id_CPE_RAMIO_L)) {
            // The directions in use: O to RAM_O, I from RAM_I
            l.push_back(std::string(param(ci, id_C_RAM_O, 0) ? "O" : "") + (param(ci, id_C_RAM_I, 0) ? "I" : ""));
        } else if (type == id_RAM) {
            int mode = param(ci, id_RAM_cfg_sram_mode, -1);
            l.push_back(mode < 0 ? "RAM"
                                 : std::string("RAM ") + ((mode & 2) ? "SDP" : "TDP") +
                                           ((mode & 1) ? " 2x20K" : " 40K"));
        } else if (type.in(id_CPE_FF_U, id_CPE_FF_L)) {
            // FF or LATCH, and the flags: ~ falling edge, E enable, R reset, S set
            l.push_back(param(ci, id_C_L_D, 0) == 1 ? "LATCH" : "FF");
            std::string flags;
            if (param(ci, id_C_CPE_CLK, 0) == 0b01)
                flags += "~";
            int en = param(ci, id_C_CPE_EN, 0b11);
            if (en == 0b10 || en == 0b01)
                flags += "E";
            if (param(ci, id_C_CPE_RES, 0b11) != 0b11)
                flags += "R";
            if (param(ci, id_C_CPE_SET, 0b11) != 0b11)
                flags += "S";
            l.push_back(flags);
        } else {
            continue;
        }
        labels[ci->bel] = l;
    }
    {
        std::lock_guard<std::mutex> lock(gui_labels_mutex_);
        gui_labels_ = std::move(labels);
    }
    update_gui_routing();
}

std::vector<std::string> GateMateImpl::gui_labels(BelId bel) const
{
    std::lock_guard<std::mutex> lock(gui_labels_mutex_);
    auto it = gui_labels_.find(bel);
    return it == gui_labels_.end() ? std::vector<std::string>() : it->second;
}

NEXTPNR_NAMESPACE_END
