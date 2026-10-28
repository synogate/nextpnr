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

#include "netlist_eval.h"
#include <boost/algorithm/string.hpp>
#include <random>
#include "uarch/gatemate/extra_data.h"
#include "util.h"
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

NEXTPNR_NAMESPACE_BEGIN

const char *tri_str(Tri v)
{
    switch (v) {
    case Tri::F:
        return "0";
    case Tri::T:
        return "1";
    default:
        return "x";
    }
}

namespace {

Tri tri(bool v) { return v ? Tri::T : Tri::F; }

Tri tri_not(Tri a) { return a == Tri::X ? Tri::X : tri(a == Tri::F); }

Tri tri_and(Tri a, Tri b)
{
    if (a == Tri::F || b == Tri::F)
        return Tri::F;
    if (a == Tri::T && b == Tri::T)
        return Tri::T;
    return Tri::X;
}

Tri tri_or(Tri a, Tri b) { return tri_not(tri_and(tri_not(a), tri_not(b))); }

Tri tri_xor(Tri a, Tri b)
{
    if (a == Tri::X || b == Tri::X)
        return Tri::X;
    return tri(a != b);
}

// sel ? d1 : d0
Tri tri_mux(Tri sel, Tri d0, Tri d1)
{
    if (sel == Tri::T)
        return d1;
    if (sel == Tri::F)
        return d0;
    return d0 == d1 ? d0 : Tri::X;
}

// Value of a register that may or may not take the new value
Tri tri_merge(Tri a, Tri b) { return a == b ? a : Tri::X; }

// The 2-bit configurable inverters of the CPE register controls (C_CPE_CLK, C_CPE_EN,
// C_CPE_RES, C_CPE_SET): 0b00 constant 0, 0b11 constant 1, 0b10 pass, 0b01 invert.
Tri lut1(int cfg, Tri in) { return tri_mux(in, tri(cfg & 1), tri(cfg & 2)); }

// A signal inside a CPE, computed on first use. Inputs that a LUT does not depend on are never
// evaluated: they may be connected (through other logic) to another output of the same CPE,
// which is not a combinational loop in the hardware.
struct Lazy
{
    explicit Lazy(std::function<Tri()> fn) : fn(std::move(fn)) {};
    Tri operator()()
    {
        if (!done) {
            value = fn();
            done = true;
        }
        return value;
    }

  private:
    std::function<Tri()> fn;
    bool done = false;
    Tri value = Tri::X;
};

Tri lut2_lazy(int init, Lazy &d0, Lazy &d1)
{
    bool uses_d0 = ((init >> 1) & 0b0101) != (init & 0b0101);
    bool uses_d1 = ((init >> 2) & 0b0011) != (init & 0b0011);
    return NetlistEvaluator::lut2(init, uses_d0 ? d0() : Tri::F, uses_d1 ? d1() : Tri::F);
}

// sel ? d1 : d0, only evaluating the selected input
Tri mux_lazy(Tri sel, Lazy &d0, Lazy &d1)
{
    if (sel == Tri::T)
        return d1();
    if (sel == Tri::F)
        return d0();
    return tri_mux(sel, d0(), d1());
}

} // namespace

NetlistEvaluator::NetlistEvaluator(Context *ctx) : ctx(ctx)
{
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        Tri init = Tri::X;
        if (ci->type.in(id_CC_DFF, id_CC_DLT)) {
            if (ci->params.count(id_INIT) && ci->params.at(id_INIT).is_fully_def())
                init = tri(bool_or_default(ci->params, id_INIT, false));
        } else if (ci->type.in(id_CPE_FF, id_CPE_LATCH)) {
            // FF_INIT: bit 1 marks a defined initial value, bit 0 is the value
            int ff_init = int_or_default(ci->params, id_FF_INIT, 0);
            if (ff_init & 2)
                init = tri(ff_init & 1);
        }
        for (int index : seq_elements(ci))
            seq[SeqKey(ci, index)] = SeqState{init, Tri::X};
        if (ci->type.in(id_CC_BRAM_20K, id_CC_BRAM_40K))
            add_reference_ram(ci);
        else if (ci->type == id_CC_FIFO_40K)
            add_reference_fifo(ci);
        else if (ci->type == id_RAM && (ci->params.count(id_RAM_cfg_fifo_sync_enable) ||
                                        ci->params.count(id_RAM_cfg_fifo_async_enable)))
            add_placed_fifo(ci);
        else if (ci->type == id_RAM)
            add_placed_ram(ci);
    }
    link_ram_cascades();
}

std::string NetlistEvaluator::seq_name(const SeqKey &key) const
{
    return key.second == 0 ? key.first->name.str(ctx) : stringf("%s#%d", key.first->name.c_str(ctx), key.second);
}

std::vector<std::pair<const CellInfo *, int>> NetlistEvaluator::state_bits() const
{
    std::vector<std::pair<const CellInfo *, int>> bits;
    for (auto &entry : seq)
        bits.push_back(entry.first);
    return bits;
}

Tri NetlistEvaluator::state(const CellInfo *cell, int index) const { return seq.at(SeqKey(cell, index)).q; }

void NetlistEvaluator::set_state(const CellInfo *cell, int index, Tri value)
{
    seq.at(SeqKey(cell, index)).q = value;
    memo.clear();
}

std::set<std::string> NetlistEvaluator::register_names() const
{
    std::set<std::string> names;
    for (auto &entry : seq)
        names.insert(seq_name(entry.first));
    return names;
}

void NetlistEvaluator::resolve_unknown_init(uint64_t seed, const std::set<std::string> &names)
{
    for (auto &entry : seq) {
        std::string name = seq_name(entry.first);
        if (entry.second.q != Tri::X || !names.count(name))
            continue;
        uint64_t h = std::hash<std::string>()(name) ^ (seed * 0x9e3779b97f4a7c15ULL);
        h ^= h >> 31;
        h *= 0xbf58476d1ce4e5b9ULL;
        h ^= h >> 29;
        // A register --vopt fuse-luts moved an inverter behind holds the inverse of the original
        bool inverted = bool_or_default(entry.first.first->attrs, ctx->id("FUSE_INVERTED"), false);
        entry.second.q = tri((h & 1) ^ inverted);
    }
    memo.clear();
}

void NetlistEvaluator::set_input(IdString ibuf, bool value) { set_input(ibuf, tri(value)); }

void NetlistEvaluator::set_input(IdString ibuf, Tri value)
{
    inputs[ibuf] = value;
    memo.clear();
}

Tri NetlistEvaluator::lut2(int init, Tri d0, Tri d1)
{
    // Bit index is {D1, D0}, as in CC_LUT2
    bool seen[2] = {false, false};
    for (int v1 = 0; v1 < 2; v1++) {
        if (d1 != Tri::X && v1 != int(d1))
            continue;
        for (int v0 = 0; v0 < 2; v0++) {
            if (d0 != Tri::X && v0 != int(d0))
                continue;
            seen[(init >> ((v1 << 1) | v0)) & 1] = true;
        }
    }
    if (seen[0] && seen[1])
        return Tri::X;
    return seen[1] ? Tri::T : Tri::F;
}

Tri NetlistEvaluator::eval(const NetInfo *net)
{
    if (net == nullptr || net->driver.cell == nullptr)
        return Tri::X;
    auto found = memo.find(net);
    if (found != memo.end())
        return found->second;
    if (visiting.count(net))
        throw EvalUnsupported(stringf("combinational loop through net '%s'", net->name.c_str(ctx)));
    visiting.insert(net);
    Tri value;
    try {
        value = eval_port(net->driver.cell, net->driver.port);
    } catch (...) {
        visiting.erase(net);
        throw;
    }
    visiting.erase(net);
    memo[net] = value;
    return value;
}

Tri NetlistEvaluator::eval_cell(const CellInfo *cell, IdString port,
                                 const std::unordered_map<const NetInfo *, Tri> &inputs)
{
    auto saved = std::move(memo);
    memo = inputs;
    Tri value;
    try {
        value = eval_port(cell, port);
    } catch (...) {
        memo = std::move(saved);
        throw;
    }
    memo = std::move(saved);
    return value;
}

Tri NetlistEvaluator::eval_output(IdString buffer)
{
    if (!ctx->cells.count(buffer))
        throw EvalUnsupported(stringf("no output buffer cell '%s'", buffer.c_str(ctx)));
    return input(ctx->cells.at(buffer).get(), id_A);
}

Tri NetlistEvaluator::eval_output_enable(IdString buffer)
{
    if (!ctx->cells.count(buffer))
        throw EvalUnsupported(stringf("no output buffer cell '%s'", buffer.c_str(ctx)));
    const CellInfo *cell = ctx->cells.at(buffer).get();
    if (cell->type.in(id_CC_OBUF, id_CC_LVDS_OBUF))
        return Tri::T;
    if (cell->type.in(id_CC_TOBUF, id_CC_IOBUF, id_CC_LVDS_TOBUF, id_CC_LVDS_IOBUF))
        return tri_not(input(cell, id_T)); // cells_sim.v: high impedance while T is set
    if (cell->type.in(id_CPE_OBUF, id_CPE_TOBUF, id_CPE_IOBUF, id_CPE_LVDS_OBUF, id_CPE_LVDS_TOBUF,
                      id_CPE_LVDS_IOBUF)) {
        // The pad drives while the enable from the IOSEL (GPIO_EN) is set
        if (connected(cell, id_T))
            return input(cell, id_T);
        IdString iosel_name = ctx->idf("%s$iosel", buffer.c_str(ctx));
        if (!ctx->cells.count(iosel_name))
            throw EvalUnsupported(stringf("no IOSEL for output '%s'", buffer.c_str(ctx)));
        return eval_iosel(ctx->cells.at(iosel_name).get(), id_GPIO_EN);
    }
    throw EvalUnsupported(stringf("cell '%s' is not an output buffer", buffer.c_str(ctx)));
}

Tri NetlistEvaluator::external(const CellInfo *buffer)
{
    auto found = inputs.find(buffer->name);
    if (found == inputs.end())
        throw EvalUnsupported(stringf("no value assigned to input '%s'", buffer->name.c_str(ctx)));
    return found->second;
}

Tri NetlistEvaluator::input(const CellInfo *cell, IdString port) { return eval(cell->getPort(port)); }

std::string NetlistEvaluator::explain_unknown(const NetInfo *net)
{
    std::unordered_set<const CellInfo *> seen;
    std::string path;
    while (true) {
        if (net == nullptr)
            return path + "unconnected";
        const CellInfo *cell = net->driver.cell;
        if (cell == nullptr)
            return path + stringf("net '%s' has no driver", net->name.c_str(ctx));
        if (!seen.insert(cell).second)
            return path + stringf("cell '%s' (%s), in a loop", cell->name.c_str(ctx), cell->type.c_str(ctx));
        path += stringf("%s.%s (%s) <- ", cell->name.c_str(ctx), net->driver.port.c_str(ctx), cell->type.c_str(ctx));
        if (fifo_index.count(cell)) {
            const FifoBlock &fifo = fifos.at(fifo_index.at(cell));
            return path + stringf("FIFO output (state %s, rd_clk=%s wr_clk=%s rst_n=%s)",
                                  fifo.known ? "known" : "unknown", tri_str(pin_value(cell, fifo.rd_clk)),
                                  tri_str(pin_value(cell, fifo.wr_clk)), tri_str(pin_value(cell, fifo.rst_n)));
        }
        if (ram_memory.count(cell)) {
            auto found = ram_outputs.find(std::make_pair(cell, net->driver.port));
            if (found == ram_outputs.end())
                return path + "block RAM output";
            const RamBlock &block = rams.at(std::get<0>(found->second));
            const RamPort &port = block.port[std::get<1>(found->second)];
            std::string addr;
            IdString unknown;
            for (int i = 15; i >= 0; i--) {
                Tri v = input(cell, port.addr[i]);
                addr += tri_str(v);
                if (v == Tri::X && unknown == IdString())
                    unknown = port.addr[i];
            }
            for (const RamPin *pin : {&port.clk, &port.en, &port.we})
                if (ram_pin(block, *pin) == Tri::X)
                    unknown = pin->port;
            std::string state =
                    stringf("block RAM output (half %d, port %c: clk=%s en=%s we=%s addr=%s)", block.half,
                            "AB"[std::get<1>(found->second)], tri_str(ram_pin(block, port.clk)),
                            tri_str(ram_pin(block, port.en)), tri_str(ram_pin(block, port.we)), addr.c_str());
            if (unknown == IdString())
                return path + state;
            return path + state + "; " + unknown.str(ctx) + " <- " + explain_unknown(cell->getPort(unknown));
        }
        if (is_sequential(cell)) {
            std::string params;
            for (auto &p : cell->params)
                params += stringf(" %s=%s", p.first.c_str(ctx), p.second.to_string().c_str());
            std::string state;
            for (int index : seq_elements(cell)) {
                SeqInputs s = seq_inputs(cell, index, true);
                state += stringf(" [%d] holding %s with clk=%s en=%s d=%s rst=%s set=%s;", index,
                                 tri_str(seq.at(SeqKey(cell, index)).q), tri_str(s.clk), tri_str(s.en),
                                 tri_str(s.d), tri_str(s.rst), tri_str(s.set));
            }
            return path + "register" + state + params;
        }
        const NetInfo *unknown_input = nullptr;
        std::string unconnected;
        for (auto &port : cell->ports) {
            if (port.second.type != PORT_IN)
                continue;
            if (port.second.net == nullptr)
                unconnected += " " + port.first.str(ctx);
            else if (unknown_input == nullptr && eval(port.second.net) == Tri::X)
                unknown_input = port.second.net;
        }
        if (unknown_input == nullptr)
            return path + stringf("x from known inputs; unconnected inputs:%s",
                                  unconnected.empty() ? " none" : unconnected.c_str());
        net = unknown_input;
    }
}

// The state bits of a cell:
//  - CC_DFF, CC_DLT, CPE_FF, CPE_LATCH: the register or latch (0);
//  - CC_IDDR, CC_ODDR: the rising edge (0) and falling edge (1) register;
//  - IOSEL: the input registers (0, 1) and output registers (2, 3) that are in use.
std::vector<int> NetlistEvaluator::seq_elements(const CellInfo *cell) const
{
    if (cell->type.in(id_CC_DFF, id_CC_DLT, id_CPE_FF, id_CPE_LATCH))
        return {0};
    if (cell->type.in(id_CC_IDDR, id_CC_ODDR))
        return {0, 1};
    if (cell->type == id_IOSEL) {
        std::vector<int> elements;
        IdString flags[4] = {id_IN1_FF, id_IN2_FF, id_OUT1_FF, id_OUT2_FF};
        for (int i = 0; i < 4; i++)
            if (bool_or_default(cell->params, flags[i], false))
                elements.push_back(i);
        return elements;
    }
    return {};
}

bool NetlistEvaluator::is_latch(const CellInfo *cell) const { return cell->type.in(id_CC_DLT, id_CPE_LATCH); }

// Only the asynchronous controls are evaluated when `sync` is false, so that the output of a
// register can be computed without evaluating its data input (which may depend on that output).
NetlistEvaluator::SeqInputs NetlistEvaluator::seq_inputs(const CellInfo *cell, int index, bool sync)
{
    SeqInputs s{Tri::X, Tri::X, Tri::X, Tri::X, Tri::X, is_latch(cell)};
    auto flag = [&](IdString name) { return bool_or_default(cell->params, name, false); };
    if (cell->type.in(id_CC_IDDR, id_CC_ODDR, id_IOSEL)) {
        // Registers without set/reset or enable
        s.rst = Tri::F;
        s.set = Tri::F;
        s.en = Tri::T;
        if (!sync)
            return s;
        if (cell->type.in(id_CC_IDDR, id_CC_ODDR)) {
            // cells_sim.v: the rising edge register takes D (IDDR) or D0 (ODDR), the falling edge
            // register D (IDDR) or D1 (ODDR)
            Tri clk = input(cell, id_CLK);
            if (flag(id_CLK_INV))
                clk = tri_not(clk);
            s.clk = index == 0 ? clk : tri_not(clk);
            s.d = cell->type == id_CC_IDDR ? input(cell, id_D) : input(cell, index == 0 ? id_D0 : id_D1);
            return s;
        }
        // IOSEL: the input registers sample the pad, the output registers OUT1/OUT2. Each register
        // uses the input or output clock (a global clock CLOCKn selected by IN_CLOCK/OUT_CLOCK, or
        // the fabric clock on OUT4 with SEL_IN_CLOCK/SEL_OUT_CLOCK), on the falling edge when its
        // INV_*_CLOCK is set. (No independent reference for the IOSEL exists; this follows what
        // the configuration bits are named after.)
        bool out = index >= 2;
        Tri clk = flag(out ? id_SEL_OUT_CLOCK : id_SEL_IN_CLOCK)
                          ? input(cell, id_OUT4)
                          : input(cell, ctx->idf("CLOCK%d",
                                                 int_or_default(cell->params, out ? id_OUT_CLOCK : id_IN_CLOCK, 0) + 1));
        static const IdString inv[4] = {id_INV_IN1_CLOCK, id_INV_IN2_CLOCK, id_INV_OUT1_CLOCK, id_INV_OUT2_CLOCK};
        s.clk = flag(inv[index]) ? tri_not(clk) : clk;
        s.d = out ? input(cell, index == 2 ? id_OUT1 : id_OUT2) : input(cell, id_GPIO_IN);
        return s;
    }
    if (cell->type.in(id_CC_DFF, id_CC_DLT)) {
        // cells_sim.v: CC_DFF / CC_DLT
        Tri sr = input(cell, id_SR);
        if (flag(id_SR_INV))
            sr = tri_not(sr);
        bool sr_val = flag(id_SR_VAL);
        s.rst = sr_val ? Tri::F : sr;
        s.set = sr_val ? sr : Tri::F;
        if (!sync)
            return s;
        s.d = input(cell, id_D);
        if (cell->type == id_CC_DFF) {
            s.clk = input(cell, id_CLK);
            if (flag(id_CLK_INV))
                s.clk = tri_not(s.clk);
            s.en = input(cell, id_EN);
            if (flag(id_EN_INV))
                s.en = tri_not(s.en);
        } else {
            s.clk = input(cell, id_G);
            if (flag(id_G_INV))
                s.clk = tri_not(s.clk);
            s.en = Tri::T;
        }
        return s;
    }
    // CPE sequential logic: C_CPE_CLK/C_CPE_EN select the clock/enable polarity (or a constant);
    // reset and set are active low after their configurable inverters C_CPE_RES/C_CPE_SET. The
    // set signal comes from SR when C_EN_SR is set and from EN otherwise.
    auto cfg = [&](IdString name) { return int_or_default(cell->params, name, 0); };
    if (cell->params.count(id_C_CLKSEL) || cell->params.count(id_C_ENSEL))
        throw EvalUnsupported(stringf("register '%s' uses a CP line clock or enable", cell->name.c_str(ctx)));
    // After routing, CLK_INT and EN_INT are named after the CPE inputs they were routed to (CLK, EN)
    auto routed = [&](IdString port, IdString after_routing) {
        return !connected(cell, port) && connected(cell, after_routing) ? input(cell, after_routing)
                                                                        : input(cell, port);
    };
    s.rst = tri_not(lut1(cfg(id_C_CPE_RES), input(cell, id_SR)));
    s.set = tri_not(
            lut1(cfg(id_C_CPE_SET), flag(id_C_EN_SR) ? input(cell, id_SR) : routed(id_EN_INT, id_EN)));
    if (!sync)
        return s;
    s.clk = lut1(cfg(id_C_CPE_CLK), routed(id_CLK_INT, id_CLK));
    s.en = lut1(cfg(id_C_CPE_EN), routed(id_EN_INT, id_EN));
    s.d = input(cell, id_DIN);
    return s;
}

Tri NetlistEvaluator::seq_output(const CellInfo *cell, int index)
{
    SeqInputs s = seq_inputs(cell, index, is_latch(cell));
    Tri q = seq.at(SeqKey(cell, index)).q;
    if (s.latch)
        q = tri_mux(tri_and(s.clk, s.en), q, s.d);
    // Asynchronous set/reset override the stored value
    if (s.rst == Tri::T)
        return s.set == Tri::F ? Tri::F : Tri::X;
    if (s.set == Tri::T)
        return s.rst == Tri::F ? Tri::T : Tri::X;
    if (s.rst == Tri::X)
        q = tri_merge(q, Tri::F);
    if (s.set == Tri::X)
        q = tri_merge(q, Tri::T);
    return q;
}

void NetlistEvaluator::settle(bool power_up)
{
    // All registers sample their inputs at the same time
    memo.clear();
    std::map<SeqKey, Tri> next;
    std::map<SeqKey, Tri> clocks;
    for (auto &entry : seq) {
        const SeqKey &key = entry.first;
        SeqInputs s = seq_inputs(key.first, key.second, true);
        clocks[key] = s.clk;
        if (s.latch || power_up)
            continue;
        Tri q = entry.second.q;
        Tri last = entry.second.last_clk;
        bool no_edge = s.clk == Tri::F || last == Tri::T;
        bool edge = last == Tri::F && s.clk == Tri::T;
        if (no_edge || s.en == Tri::F)
            next[key] = q;
        else if (edge && s.en == Tri::T)
            next[key] = s.d;
        else
            next[key] = tri_merge(q, s.d);
    }
    std::vector<RamUpdate> ram_updates;
    for (auto &block : rams)
        ram_updates.push_back(ram_sample(block, power_up));
    std::vector<FifoBlock> fifo_updates;
    std::vector<std::vector<std::pair<int, Tri>>> fifo_writes(fifos.size());
    for (size_t i = 0; i < fifos.size(); i++)
        fifo_updates.push_back(fifo_sample(fifos[i], power_up, fifo_writes[i]));
    for (auto &entry : next)
        seq.at(entry.first).q = entry.second;
    for (auto &entry : clocks)
        seq.at(entry.first).last_clk = entry.second;
    for (size_t i = 0; i < rams.size(); i++)
        ram_commit(rams[i], ram_updates[i]);
    for (size_t i = 0; i < fifos.size(); i++) {
        fifos[i] = std::move(fifo_updates[i]);
        std::vector<Tri> &memory = ram_memory.at(fifos[i].cell);
        for (auto &w : fifo_writes[i])
            memory.at(w.first) = w.second;
    }
    memo.clear();
    // Then let latches and asynchronous set/reset settle
    for (int iter = 0; iter < 64; iter++) {
        memo.clear();
        bool changed = false;
        std::vector<std::pair<SeqKey, Tri>> updates;
        for (auto &entry : seq) {
            Tri q = seq_output(entry.first.first, entry.first.second);
            if (q != entry.second.q)
                updates.emplace_back(entry.first, q);
        }
        for (auto &u : updates) {
            seq.at(u.first).q = u.second;
            changed = true;
        }
        if (!changed) {
            memo.clear();
            return;
        }
    }
    throw EvalUnsupported("registers and latches do not settle");
}

Tri NetlistEvaluator::eval_l2t4(const CellInfo *cell)
{
    // PINX has a model only in a lower half selecting it with C_I4 (below)
    bool lower_pinx = cell->bel != BelId() && ctx->getBelLocation(cell->bel).z != CPE_LT_U_Z &&
                      bool_or_default(cell->params, id_C_I4, false);
    for (auto port : {id_PINX, id_CINY2, id_PINY2})
        if (connected(cell, port) && !(port == id_PINX && lower_pinx))
            throw EvalUnsupported(stringf("cell '%s' uses port %s, which has no model", cell->name.c_str(ctx),
                                          port.c_str(ctx)));
    int func = int_or_default(cell->params, id_C_FUNCTION, 0);
    if (func != 0 && func != C_EN_CIN)
        throw EvalUnsupported(stringf("cell '%s' uses CPE function %d, which has no half-CPE model",
                                      cell->name.c_str(ctx), func));
    auto init = [&](IdString name) { return int_or_default(cell->params, name, 0); };
    auto flag = [&](IdString name) { return bool_or_default(cell->params, name, false); };
    // Before routing a half CPE uses the logical inputs D0_xx/D1_xx; IN1..IN4 are equivalent
    // (as in the delay model). Upper halves select PINY1/CINX with C_I1/C_I2, lower halves
    // PINY1/PINX with C_I3/C_I4 (as written to the bitstream).
    auto pin = [&](IdString d_port, IdString in_port) {
        return connected(cell, d_port) ? input(cell, d_port) : input(cell, in_port);
    };
    bool upper = true;
    if (cell->bel != BelId())
        upper = ctx->getBelLocation(cell->bel).z == CPE_LT_U_Z;
    else if (cell->params.count(id_L2T4_UPPER))
        upper = bool_or_default(cell->params, id_L2T4_UPPER, true);
    bool sel_d1_00 = upper ? flag(id_C_I1) : flag(id_C_I3);
    bool sel_d1_01 = upper ? flag(id_C_I2) : flag(id_C_I4);
    if ((connected(cell, id_PINY1) && !sel_d1_00) ||
        (connected(cell, id_CINX) && !(upper && sel_d1_01)) ||
        (connected(cell, id_CINY1) && func != C_EN_CIN))
        throw EvalUnsupported(stringf("cell '%s' has a CP line input without a model", cell->name.c_str(ctx)));
    if (func == C_EN_CIN && upper)
        throw EvalUnsupported(stringf("cell '%s' uses C_EN_CIN in an upper half", cell->name.c_str(ctx)));
    Lazy d0_00([&] { return pin(id_D0_00, id_IN1); });
    Lazy d1_00([&] { return sel_d1_00 ? input(cell, id_PINY1) : pin(id_D1_00, id_IN2); });
    Lazy d0_01([&] { return pin(id_D0_01, id_IN3); });
    Lazy d1_01([&] {
        return sel_d1_01 ? (upper ? input(cell, id_CINX) : input(cell, id_PINX)) : pin(id_D1_01, id_IN4);
    });
    // CC_L2T4 structure: L10 combines the outputs of L00 and L01. D0_10/D1_10 feed L10 directly
    // (the LUT1/LUT2 mapping); they are routed through L00/L01 after placement.
    Lazy l00([&] { return lut2_lazy(init(id_INIT_L00), d0_00, d1_00); });
    Lazy l01([&] {
        Tri v = lut2_lazy(init(id_INIT_L01), d0_01, d1_01);
        // C_EN_CIN ORs the carry input into the second input of the lower half's LUT (L11)
        return func == C_EN_CIN ? tri_or(v, input(cell, id_CINY1)) : v;
    });
    Lazy a([&] { return connected(cell, id_D0_10) ? input(cell, id_D0_10) : l00(); });
    Lazy b([&] { return connected(cell, id_D1_10) ? input(cell, id_D1_10) : l01(); });
    Lazy l10([&] { return lut2_lazy(init(id_INIT_L10), a, b); });
    // CC_L2T5 structure: L20 combines COMBIN from the upper half (as D0) with L10 (as D1).
    if (cell->params.count(id_INIT_L20)) {
        Lazy combin([&] { return input(cell, id_COMBIN); });
        return lut2_lazy(init(id_INIT_L20), combin, l10);
    }
    if (connected(cell, id_COMBIN))
        throw EvalUnsupported(stringf("cell '%s' has COMBIN connected but no INIT_L20", cell->name.c_str(ctx)));
    // After placement a lower half's result always passes L20, which then has no configuration
    // (repack() sets INIT_L20 for every LUT tree placed there, and L2T4_UPPER for every one)
    if (!upper && cell->bel != BelId() && cell->params.count(id_L2T4_UPPER))
        throw EvalUnsupported(stringf("cell '%s' is in a lower half but has no INIT_L20", cell->name.c_str(ctx)));
    return l10();
}

// A full CPE after placement (both halves merged): CPE_ADDF, CPE_ADDF2, CPE_MX4.
Tri NetlistEvaluator::eval_cpe(const CellInfo *cell, IdString port)
{
    auto init = [&](IdString name) { return int_or_default(cell->params, name, 0); };
    auto flag = [&](IdString name) { return bool_or_default(cell->params, name, false); };
    auto pin = [&](IdString d_port, IdString in_port) {
        return connected(cell, d_port) ? input(cell, d_port) : input(cell, in_port);
    };
    int func = int_or_default(cell->params, id_C_FUNCTION, 0);
    // C_FUNCTION decode from the CPE sheet
    bool c_addf = func == C_ADDF || func == C_ADDF2 || func == C_MULT || func == C_CONCAT;
    bool c_addf2 = func == C_ADDF2 || func == C_MULT;
    bool c_addcin = func == C_ADDF || func == C_ADDF2 || func == C_MULT || func == C_ADDCIN;
    bool c_mx4 = func == C_MX4;
    if (func == C_MULT || func == C_CONCAT || func == C_ADDCIN || func == C_EN_CIN)
        throw EvalUnsupported(stringf("cell '%s' uses CPE function %d, which has no model", cell->name.c_str(ctx), func));

    // First level LUTs; the second input can come from a CP line instead (C_I1..C_I4)
    Lazy in1([&] { return pin(id_D0_00, id_IN1); });
    Lazy in2([&] { return flag(id_C_I1) ? input(cell, id_PINY1) : pin(id_D1_00, id_IN2); });
    Lazy in3([&] { return pin(id_D0_01, id_IN3); });
    Lazy in4([&] { return flag(id_C_I2) ? input(cell, id_CINX) : pin(id_D1_01, id_IN4); });
    Lazy in5([&] { return pin(id_D0_02, id_IN5); });
    Lazy in6([&] { return flag(id_C_I3) ? input(cell, id_PINY1) : pin(id_D1_02, id_IN6); });
    Lazy in7([&] { return pin(id_D0_03, id_IN7); });
    Lazy in8([&] { return flag(id_C_I4) ? input(cell, id_PINX) : pin(id_D1_03, id_IN8); });
    Lazy l00([&] { return lut2_lazy(init(id_INIT_L00), in1, in2); });
    Lazy l01([&] { return lut2_lazy(init(id_INIT_L01), in3, in4); });
    Lazy l02([&] { return lut2_lazy(init(id_INIT_L02), in5, in6); });
    Lazy l03([&] { return lut2_lazy(init(id_INIT_L03), in7, in8); });
    Lazy cin([&] { return flag(id_C_HORIZ) ? input(cell, id_CINX) : input(cell, id_CINY1); });
    Lazy l10_d0([&] { return connected(cell, id_D0_10) ? input(cell, id_D0_10) : l00(); });
    Lazy l10_d1([&] { return connected(cell, id_D1_10) ? input(cell, id_D1_10) : l01(); });
    Lazy l11_d0([&] { return connected(cell, id_D0_11) ? input(cell, id_D0_11) : l02(); });
    Lazy l11_d1([&] { return connected(cell, id_D1_11) ? input(cell, id_D1_11) : l03(); });
    Lazy l10([&] { return lut2_lazy(init(id_INIT_L10), l10_d0, l10_d1); });
    Lazy l11([&] { return lut2_lazy(init(id_INIT_L11), l11_d0, l11_d1); });

    // Inputs of L20 (and the COMB2OUT path). In MX4 mode they come from the two 4-input muxes,
    // whose data inputs are masked by the X bits (cleared bit: constant 0) and inverted by the
    // C bits.
    //
    // NOTE: the Peppercorn CPE schematic puts the IN1..IN4 multiplexer (selected by L02/L03, C =
    // L10, X = L11) on L20.D0 and the IN5..IN8 multiplexer (selected by L00/L01, C = L11, X = L10)
    // on L20.D1. nextpnr (pack_cpe.cc, bitstream.cc update_cpe_mux) uses IN1..IN4 with selects on
    // L02/L03, C = L11, X = L10 and takes the result from L20.D1. The two disagree on which data
    // inputs and selects belong to which multiplexer; this model follows nextpnr, which has
    // been used on hardware, until this is confirmed on a device.
    auto mux4 = [&](int first_in, int mask, int inv, Lazy &s0, Lazy &s1) {
        Tri data[4];
        for (int i = 0; i < 4; i++) {
            Tri v = (mask >> i) & 1 ? input(cell, ctx->idf("IN%d", first_in + i)) : Tri::F;
            data[i] = tri_xor(v, tri((inv >> i) & 1));
        }
        Tri sel1 = s1();
        Tri lo = tri_mux(s0(), data[0], data[1]), hi = tri_mux(s0(), data[2], data[3]);
        return tri_mux(sel1, lo, hi);
    };
    Lazy d0([&] { return c_mx4 ? mux4(5, init(id_INIT_L11), init(id_INIT_L10), l00, l01) : l10(); });
    Lazy d1([&] { return c_mx4 ? mux4(1, init(id_INIT_L10), init(id_INIT_L11), l02, l03) : l11(); });
    // ADDF2: carry between the two bits of the CPE (bit 0 is L02/L03/L11, bit 1 is L00/L01/L10)
    Lazy carry_mid([&] { return mux_lazy(l11(), l02, cin); });

    if (port == id_OUT1)
        return tri_xor(lut2_lazy(init(id_INIT_L20), d0, d1), c_addcin ? cin() : Tri::F);
    if (port == id_OUT2)
        return tri_xor(d0(), c_addf2 ? carry_mid() : Tri::F);
    if (port.in(id_COUTY1, id_CPOUT1, id_CPOUT2)) {
        if (!c_addf || flag(id_C_HORIZ))
            throw EvalUnsupported(stringf("cell '%s' port %s has no model outside vertical adder mode",
                                          cell->name.c_str(ctx), port.c_str(ctx)));
        // Propagate (CADD_S, active low) and generate (CADD_A) of the adder. The carry lines
        // pass CINY1 when propagating and the generate signal otherwise; the CP outputs carry the
        // same vertical carry.
        Tri cadd_s = c_addf2 ? tri_not(tri_and(l10(), l11())) : tri_not(tri_xor(l10(), l11()));
        Lazy cadd_a([&] { return c_addf2 ? tri_mux(l10(), tri_or(l00(), l01()), l02()) : tri_or(l00(), l01()); });
        Lazy ciny1([&] { return input(cell, id_CINY1); });
        return mux_lazy(tri_not(cadd_s), cadd_a, ciny1);
    }
    throw EvalUnsupported(stringf("cell '%s' port %s has no model", cell->name.c_str(ctx), port.c_str(ctx)));
}

Tri NetlistEvaluator::eval_cplines(const CellInfo *cell, IdString port)
{
    auto flag = [&](IdString name) { return bool_or_default(cell->params, name, false); };
    if (port == id_COUTY1) {
        // CY1_VAL selects OUT2, OUT1, COMP_OUT or CINX with {C_SEL_C, C_SELY1}; C_CY1_I passes it
        // to COUTY1, otherwise CINY1 passes vertically.
        if (!flag(id_C_CY1_I))
            return input(cell, id_CINY1);
        Tri sources[4] = {input(cell, id_OUT2), input(cell, id_OUT1), input(cell, id_COMPOUT), input(cell, id_CINX)};
        return sources[(flag(id_C_SEL_C) ? 2 : 0) | (flag(id_C_SELY1) ? 1 : 0)];
    }
    throw EvalUnsupported(stringf("CP lines '%s' port %s has no model", cell->name.c_str(ctx), port.c_str(ctx)));
}

Tri NetlistEvaluator::eval_iosel(const CellInfo *cell, IdString port)
{
    auto flag = [&](IdString name) { return bool_or_default(cell->params, name, false); };
    if (port.in(id_IN1, id_IN2)) {
        bool in1 = port == id_IN1;
        if (flag(in1 ? id_IN1_FF : id_IN2_FF))
            return seq_output(cell, in1 ? 0 : 1);
        return input(cell, id_GPIO_IN);
    }
    if (port == id_GPIO_OUT) {
        if (flag(id_USE_DDR)) {
            // Double data rate: the DDR signal selects the OUT2 register (assumption, as in
            // nextpnr's mapping of CC_ODDR, whose rising edge data goes to OUT2)
            if (!flag(id_OUT1_FF) || !flag(id_OUT2_FF))
                throw EvalUnsupported(stringf("IOSEL '%s' uses DDR without both registers", cell->name.c_str(ctx)));
            return tri_mux(input(cell, id_DDR), seq_output(cell, 2), seq_output(cell, 3));
        }
        if (flag(id_OUT_SIGNAL)) {
            // Same output source selection as the timing model
            IdString src = flag(id_OUT23_14_SEL) ? (flag(id_OUT2_3) ? id_OUT3 : id_OUT2)
                                                 : (flag(id_OUT1_4) ? id_OUT4 : id_OUT1);
            if (src == id_OUT1 && flag(id_OUT1_FF))
                return seq_output(cell, 2);
            if (src == id_OUT2 && flag(id_OUT2_FF))
                return seq_output(cell, 3);
            return input(cell, src);
        }
        // Constant outputs are encoded by the packer in OUT23_14_SEL without OUT_SIGNAL
        if (cell->params.count(id_OUT23_14_SEL))
            return flag(id_OUT23_14_SEL) ? Tri::T : Tri::F;
        return Tri::X;
    }
    if (port == id_GPIO_EN) {
        // Output enable: from the fabric (OE_SIGNAL selects the source, as in the timing model),
        // or always on for outputs (OE_ENABLE)
        if (cell->params.count(id_OE_SIGNAL)) {
            int oe = int_or_default(cell->params, id_OE_SIGNAL, 0);
            IdString src = (oe & 2) ? ((oe & 1) ? id_OUT4 : id_OUT3) : ((oe & 1) ? id_OUT2 : id_OUT1);
            return input(cell, src);
        }
        return tri(flag(id_OE_ENABLE));
    }
    throw EvalUnsupported(stringf("IOSEL '%s' port %s has no model", cell->name.c_str(ctx), port.c_str(ctx)));
}

// Global clock path: CLKIN selects a clock input per reference clock (REFn, optionally
// inverted), GLBOUT drives the global clocks from either the reference clocks or the fabric.
Tri NetlistEvaluator::eval_clocking(const CellInfo *cell, IdString port)
{
    auto flag = [&](IdString name) { return bool_or_default(cell->params, name, false); };
    const std::string name = port.str(ctx);
    if (cell->type == id_CLKIN && name.rfind("CLK_REF", 0) == 0) {
        int i = name.back() - '0';
        IdString ref = ctx->idf("REF%d", i);
        if (!cell->params.count(ref))
            return Tri::X;
        int sel = int_or_default(cell->params, ref, 0);
        Tri v = sel == 4 ? input(cell, id_SER_CLK) : (sel < 4 ? input(cell, ctx->idf("CLK%d", sel)) : Tri::X);
        return flag(ctx->idf("REF%d_INV", i)) ? tri_not(v) : v;
    }
    if (cell->type == id_GLBOUT && name.rfind("GLB", 0) == 0 && name.size() == 4) {
        int i = name.back() - '0';
        if (flag(ctx->idf("USR_GLB%d_EN", i)))
            return input(cell, ctx->idf("USR_GLB%d", i));
        if (int_or_default(cell->params, ctx->idf("GLB%d_CFG", i), 0) != 0)
            throw EvalUnsupported(stringf("global clock %d from a PLL has no model", i));
        return input(cell, ctx->idf("CLK_REF_OUT%d", i));
    }
    throw EvalUnsupported(stringf("clocking cell '%s' port %s has no model", cell->name.c_str(ctx), port.c_str(ctx)));
}

// Unpacked CC_* cells, after yosys techlibs/gatemate/cells_sim.v
Tri NetlistEvaluator::eval_reference(const CellInfo *cell, IdString port)
{
    auto init = [&](IdString name) { return int_or_default(cell->params, name, 0); };
    if (cell->type == id_CC_LUT1)
        return lut2(init(id_INIT) * 5, input(cell, id_I0), Tri::F);
    if (cell->type == id_CC_LUT2)
        return lut2(init(id_INIT), input(cell, id_I0), input(cell, id_I1));
    if (cell->type == id_CC_L2T4) {
        Tri l00 = lut2(init(id_INIT_L00), input(cell, id_I0), input(cell, id_I1));
        Tri l01 = lut2(init(id_INIT_L01), input(cell, id_I2), input(cell, id_I3));
        return lut2(init(id_INIT_L10), l00, l01);
    }
    if (cell->type == id_CC_L2T5) {
        Tri l02 = lut2(init(id_INIT_L02), input(cell, id_I0), input(cell, id_I1));
        Tri l03 = lut2(init(id_INIT_L03), input(cell, id_I2), input(cell, id_I3));
        Tri l11 = lut2(init(id_INIT_L11), l02, l03);
        return lut2(init(id_INIT_L20), input(cell, id_I4), l11);
    }
    if (cell->type == id_CC_MX2)
        return tri_mux(input(cell, id_S0), input(cell, id_D0), input(cell, id_D1));
    if (cell->type == id_CC_MX4) {
        Tri s0 = input(cell, id_S0), s1 = input(cell, id_S1);
        return tri_mux(s1, tri_mux(s0, input(cell, id_D0), input(cell, id_D1)),
                       tri_mux(s0, input(cell, id_D2), input(cell, id_D3)));
    }
    if (cell->type == id_CC_ADDF) {
        Tri a = input(cell, id_A), b = input(cell, id_B), ci = input(cell, id_CI);
        if (port == id_S)
            return tri_xor(tri_xor(a, b), ci);
        return tri_or(tri_and(a, b), tri_and(ci, tri_or(a, b)));
    }
    if (cell->type == id_CC_BUFG)
        return input(cell, id_I);
    if (cell->type == ctx->id("VCC"))
        return Tri::T;
    if (cell->type == ctx->id("GND"))
        return Tri::F;
    throw EvalUnsupported(stringf("cell '%s' of type %s port %s has no model", cell->name.c_str(ctx),
                                  cell->type.c_str(ctx), port.c_str(ctx)));
}

Tri NetlistEvaluator::eval_port(const CellInfo *cell, IdString port)
{
    if (cell->type.in(id_CC_DFF, id_CC_DLT, id_CPE_FF, id_CPE_LATCH)) {
        if (port.in(id_Q, id_DOUT))
            return seq_output(cell, 0);
        throw EvalUnsupported(stringf("register '%s' port %s has no model", cell->name.c_str(ctx), port.c_str(ctx)));
    }
    if (cell->type == id_CC_IDDR)
        return seq_output(cell, port == id_Q0 ? 0 : 1);
    if (cell->type == id_CC_ODDR) // cells_sim.v: Q = DDR ? q0 : q1
        return tri_mux(input(cell, id_DDR), seq_output(cell, 1), seq_output(cell, 0));
    // Pads: the value driven into the device, or the value the device drives on a bidirectional
    // pad while it drives (cells_sim.v CC_IOBUF; for the packed GPIO cell, A and T come from
    // the IOSEL output and output enable)
    if (cell->type.in(id_CC_IBUF, id_CPE_IBUF, id_CPE_LVDS_IBUF) && port == id_Y)
        return external(cell);
    if (cell->type.in(id_CC_IOBUF, id_CPE_IOBUF, id_CC_LVDS_IOBUF, id_CPE_LVDS_IOBUF) && port == id_Y) {
        Tri drives = cell->type.in(id_CC_IOBUF, id_CC_LVDS_IOBUF) ? tri_not(input(cell, id_T)) : input(cell, id_T);
        return tri_mux(drives, external(cell), input(cell, id_A));
    }
    // cells_sim.v CC_LVDS_IBUF: Y = I_P (the value driven into the device, per buffer)
    if (cell->type == id_CC_LVDS_IBUF && port == id_Y)
        return external(cell);
    if (cell->type == id_IOSEL)
        return eval_iosel(cell, port);
    if (fifo_index.count(cell))
        return eval_fifo(fifos.at(fifo_index.at(cell)), port);
    if (cell->type.in(id_CC_BRAM_20K, id_CC_BRAM_40K, id_RAM))
        return eval_ram(cell, port);
    if (cell->type.in(id_CPE_RAMO, id_CPE_RAMI, id_CPE_RAMIO)) {
        if (port == id_RAM_O)
            return bool_or_default(cell->params, id_C_RAM_O, false) ? input(cell, id_I) : Tri::X;
        if (port == id_OUT)
            return bool_or_default(cell->params, id_C_RAM_I, false) ? input(cell, id_RAM_I) : Tri::X;
    }
    if (cell->type == id_CPE_L2T4 && port == id_OUT)
        return eval_l2t4(cell);
    if (cell->type.in(id_CPE_ADDF, id_CPE_ADDF2, id_CPE_MX4))
        return eval_cpe(cell, port);
    if (cell->type == id_CPE_CPLINES)
        return eval_cplines(cell, port);
    if (cell->type.in(id_CLKIN, id_GLBOUT))
        return eval_clocking(cell, port);
    if (cell->type.c_str(ctx)[0] == 'C' && cell->type.c_str(ctx)[1] == 'C')
        return eval_reference(cell, port);
    if (cell->type.in(ctx->id("VCC"), ctx->id("GND")))
        return eval_reference(cell, port);
    throw EvalUnsupported(stringf("cell '%s' of type %s port %s has no model", cell->name.c_str(ctx),
                                  cell->type.c_str(ctx), port.c_str(ctx)));
}

// Block RAM
//
// The behaviour is that of the yosys simulation models CC_BRAM_20K / CC_BRAM_40K (cells_sim.v),
// including their memory bit order. A placed RAM cell is mapped back onto these models:
//  - the RAM_cfg_* settings are read with the meaning nextpnr gives them (pack_bram.cc, and the
//    clock selection in delay.cc), as no independent description of them exists;
//  - the 40K memory holds 512 rows of 80 bits; when split, the half at the RAM's own location
//    (half 0) has the lower 40 bits of each row, the other half the upper 40 bits. A 20K half
//    holds 512 rows of 40 bits in the 20K model, so this places each 40-bit row of a half in the
//    same row of the 40K block.
//  - the configurable inverters act after the selection of a clock, enable or write enable, so
//    an inverted constant is the opposite constant (as with the *_INV parameters of the models).
//    The output registers and outputs start as 0, as in the models.

void NetlistEvaluator::add_ram(RamBlock block, int memory_bits)
{
    for (int p = 0; p < 2; p++) {
        RamPort &port = block.port[p];
        port.out.assign(port.dout.size(), Tri::F);
        port.reg.assign(port.dout.size(), Tri::F);
        for (size_t i = 0; i < port.dout.size(); i++)
            ram_outputs[std::make_pair(block.cell, port.dout[i])] = std::make_tuple(int(rams.size()), p, int(i));
    }
    if (!ram_memory.count(block.cell)) {
        // INIT_xx hold 320 bits each, from bit 0 of the memory up
        std::vector<Tri> &memory = ram_memory[block.cell];
        memory.assign(memory_bits, Tri::F);
        for (int i = 0; i < memory_bits / 320; i++) {
            Property init = get_or_default(block.cell->params, ctx->idf("INIT_%02X", i), Property());
            std::vector<bool> bits = init.extract(0, 320).as_bits();
            for (int j = 0; j < 320; j++)
                memory[i * 320 + j] = tri(bits[j]);
        }
    }
    rams.push_back(std::move(block));
}

void NetlistEvaluator::add_reference_ram(const CellInfo *cell)
{
    RamBlock block;
    block.cell = cell;
    block.is_40k = cell->type == id_CC_BRAM_40K;
    block.sdp = str_or_default(cell->params, id_RAM_MODE, "SDP") == "SDP";
    std::string cas = str_or_default(cell->params, id_CAS, "NONE");
    block.cascade = cas == "LOWER" ? 1 : cas == "UPPER" ? 2 : 0;
    for (IdString ecc : {id_ECC_EN, id_A_ECC_EN, id_B_ECC_EN})
        if (int_or_default(cell->params, ecc, 0))
            block.unsupported = "block RAM with ECC";
    int width = block.is_40k ? 40 : 20;
    for (int p = 0; p < 2; p++) {
        char name = "AB"[p];
        auto param = [&](const char *suffix) { return int_or_default(cell->params, ctx->idf("%c_%s", name, suffix), 0); };
        RamPort &port = block.port[p];
        port.clk = RamPin{ctx->idf("%c_CLK", name), Tri::X, bool(param("CLK_INV"))};
        port.en = RamPin{ctx->idf("%c_EN", name), Tri::X, bool(param("EN_INV"))};
        port.we = RamPin{ctx->idf("%c_WE", name), Tri::X, bool(param("WE_INV"))};
        port.rd_width = param("RD_WIDTH");
        port.wr_width = param("WR_WIDTH");
        port.write_through =
                str_or_default(cell->params, ctx->idf("%c_WR_MODE", name), "NO_CHANGE") == "WRITE_THROUGH";
        port.do_reg = param("DO_REG");
        for (int i = 0; i < 16; i++)
            port.addr.push_back(ctx->idf("%c_ADDR[%d]", name, i));
        for (int i = 0; i < width; i++) {
            port.di.push_back(ctx->idf("%c_DI[%d]", name, i));
            port.bm.push_back(ctx->idf("%c_BM[%d]", name, i));
            port.dout.push_back(ctx->idf("%c_DO[%d]", name, i));
        }
    }
    int memory_bits = block.is_40k ? 40960 : 20480;
    add_ram(std::move(block), memory_bits);
}

void NetlistEvaluator::add_placed_ram(const CellInfo *cell)
{
    auto cfg = [&](const std::string &name) { return int_or_default(cell->params, ctx->id("RAM_cfg_" + name), 0); };
    int mode = cfg("sram_mode");
    bool split = mode & 1;
    static const int widths[8] = {0, 1, 2, 5, 10, 20, 40, 80};
    for (int half = 0; half < (split ? 2 : 1); half++) {
        RamBlock block;
        block.cell = cell;
        block.half = split ? half : -1;
        block.is_40k = !split;
        block.sdp = mode & 2;
        if (cfg("ecc_enable"))
            block.unsupported = "block RAM with ECC";
        block.cascade = cfg("cascade_enable");
        if (cfg("fifo_sync_enable") || cfg("fifo_async_enable"))
            block.unsupported = "FIFO";
        int width = split ? 20 : 40;
        for (int p = 0; p < 2; p++) {
            char name = "AB"[p], other = "BA"[p];
            std::string port_half = stringf("%c%d", "ab"[p], half);
            RamPort &port = block.port[p];
            int inv = cfg("inversion_" + port_half);
            // Clock selection (as in delay.cc): one of the two clock pins of the port, of the
            // other port, or a global clock
            int idx = 2 * half;
            switch (cfg("forward_" + port_half + "_clk")) {
            case 0b00000000:
                port.clk.port = ctx->idf("CLK%c[%d]", name, idx);
                break;
            case 0b00000100:
                port.clk.port = ctx->idf("CLK%c[%d]", name, idx + 1);
                break;
            case 0b00001000:
                port.clk.port = ctx->idf("CLK%c[%d]", other, idx);
                break;
            case 0b00001100:
                port.clk.port = ctx->idf("CLK%c[%d]", other, idx + 1);
                break;
            case 0b00100011:
                port.clk.port = id_CLOCK1;
                break;
            case 0b00110011:
                port.clk.port = id_CLOCK2;
                break;
            case 0b00000011:
                port.clk.port = id_CLOCK3;
                break;
            case 0b00010011:
                port.clk.port = id_CLOCK4;
                break;
            default:
                block.unsupported = "block RAM clock selection " + port_half;
            }
            port.clk.invert = inv & 0b100;
            // Enable and write enable: the pin of the port, or a constant (pack_bram.cc)
            auto ctrl = [&](const char *what, const char *pin, bool invert) {
                RamPin result;
                result.invert = invert;
                switch (cfg(stringf("forward_%s_%s", port_half.c_str(), what))) {
                case 0b00000000:
                    result.port = ctx->idf("%s%c[%d]", pin, name, idx);
                    break;
                case 0b00000011:
                    result.constant = Tri::F;
                    break;
                case 0b00010011:
                    result.constant = Tri::T;
                    break;
                default:
                    block.unsupported = stringf("block RAM %s selection %s", what, port_half.c_str());
                }
                return result;
            };
            port.en = ctrl("en", "EN", inv & 0b001);
            port.we = ctrl("we", "GLWE", inv & 0b010);
            port.rd_width = widths[cfg("output_config_" + port_half) & 7];
            port.wr_width = widths[cfg("input_config_" + port_half) & 7];
            port.write_through = cfg(port_half + "_writemode");
            port.do_reg = cfg(port_half + "_set_outputreg");
            for (int i = 0; i < 16; i++)
                port.addr.push_back(ctx->idf("ADDR%c%d[%d]", name, half, i));
            for (int i = 0; i < width; i++) {
                int bit = i + 20 * half;
                port.di.push_back(ctx->idf("DI%c[%d]", name, bit));
                port.bm.push_back(ctx->idf("WE%c[%d]", name, bit));
                port.dout.push_back(ctx->idf("DO%c[%d]", name, bit));
            }
        }
        add_ram(std::move(block), 40960);
    }
}

// The two blocks of a cascade: in the reference netlist the upper block's A_CI is driven by the
// lower block's A_CO; in the placed netlist (where these are not connected) the lower block is
// the RAM 16 rows below the upper one (pack_bram.cc).
void NetlistEvaluator::link_ram_cascades()
{
    std::map<const CellInfo *, int> index;
    for (size_t i = 0; i < rams.size(); i++)
        index[rams[i].cell] = int(i);
    for (auto &block : rams) {
        if (block.cascade != 2)
            continue;
        const CellInfo *lower = nullptr;
        if (block.cell->type == id_RAM) {
            if (block.cell->bel != BelId()) {
                Loc loc = ctx->getBelLocation(block.cell->bel);
                lower = ctx->getBoundBelCell(ctx->getBelByLocation(Loc(loc.x, loc.y - 16, loc.z)));
            }
        } else {
            const NetInfo *ci = block.cell->getPort(id_A_CI);
            lower = ci ? ci->driver.cell : nullptr;
        }
        if (lower == nullptr || !index.count(lower) || rams[index.at(lower)].cascade != 1) {
            block.unsupported = "upper block of a cascade without its lower block";
            continue;
        }
        block.partner = index.at(lower);
        rams[block.partner].partner = int(&block - rams.data());
    }
}

Tri NetlistEvaluator::ram_read(const RamBlock &block, const std::vector<int> &firsts, int bit) const
{
    const std::vector<Tri> &memory = ram_memory.at(block.cell);
    int memory_bits = block.half < 0 ? int(memory.size()) : int(memory.size()) / 2;
    Tri value = Tri::X;
    bool any = false;
    for (int f : firsts) {
        if (f < 0 || f + bit >= memory_bits)
            return Tri::X;
        Tri v = memory.at(ram_bit(block, f + bit));
        value = any ? tri_merge(value, v) : v;
        any = true;
    }
    return value;
}

// The cascade output (A_CO/B_CO) of the lower block of a cascade: while it is selected
// (address bit 0 low), the memory bit at its address, or with WRITE_THROUGH the data being
// written. Otherwise it holds its value in the yosys model; here it is unknown then.
Tri NetlistEvaluator::ram_cascade_out(const RamBlock &lower, int p)
{
    const RamPort &port = lower.port[p];
    if (input(lower.cell, port.addr[0]) != Tri::F)
        return Tri::X;
    Tri value = ram_read(lower, ram_addresses(lower, p, 1), 0);
    if (port.write_through)
        value = tri_mux(tri_and(ram_pin(lower, port.we), input(lower.cell, port.bm[0])), value,
                        input(lower.cell, port.di[0]));
    return value;
}

Tri NetlistEvaluator::pin_value(const CellInfo *cell, const RamPin &pin)
{
    Tri value = pin.port == IdString() ? pin.constant : input(cell, pin.port);
    return pin.invert ? tri_not(value) : value;
}

int NetlistEvaluator::ram_bit(const RamBlock &block, int bit) const
{
    if (block.half < 0)
        return bit;
    return (bit / 40) * 80 + 40 * block.half + bit % 40;
}

// The first memory bit accessed by port `p` with the given width, for every address the address
// pins may have (several if some are unknown); {-1} if too many to enumerate.
std::vector<int> NetlistEvaluator::ram_addresses(const RamBlock &block, int p, int width)
{
    // Address bits used, and the first bit as in cells_sim.v
    std::function<int(int)> first;
    int lsb;
    if (block.sdp) {
        if (width != (block.is_40k ? 80 : 40))
            throw EvalUnsupported(stringf("block RAM '%s': width %d in SDP mode", block.cell->name.c_str(ctx), width));
        lsb = 7;
        first = [&](int a) { return (a >> 7) * width; };
    } else if (block.is_40k) {
        static const int lsbs[] = {1, 2, 3, 4, 5, 6};
        static const int limits[] = {1, 2, 5, 10, 20, 40};
        int k = 0;
        while (k < 5 && width > limits[k])
            k++;
        lsb = lsbs[k];
        first = [k](int a) {
            int tmp = a >> lsbs[k];
            switch (k) {
            case 0:
                return tmp + tmp / 4;
            case 1:
                return tmp * 2 + tmp / 2;
            default:
                return tmp * limits[k];
            }
        };
    } else {
        // 20K: address bits [15:7] and the low bits [5:lsb]; bit 6 is not used
        static const int lsbs[] = {1, 2, 3, 4, 5};
        static const int limits[] = {1, 2, 5, 10, 20};
        int k = 0;
        while (k < 4 && width > limits[k])
            k++;
        lsb = lsbs[k];
        first = [k](int a) {
            int low = (a >> lsbs[k]) & ((1 << (6 - lsbs[k])) - 1);
            int tmp = ((a >> 7) << (6 - lsbs[k])) | low;
            switch (k) {
            case 0:
                return tmp + tmp / 4;
            case 1:
                return tmp * 2 + tmp / 2;
            default:
                return tmp * limits[k];
            }
        };
    }
    int known = 0;
    std::vector<int> unknown;
    for (int i = lsb; i < 16; i++) {
        if (!block.is_40k && !block.sdp && i == 6)
            continue;
        if (block.sdp && i < 7)
            continue;
        Tri v = input(block.cell, block.port[p].addr[i]);
        if (v == Tri::X)
            unknown.push_back(i);
        else if (v == Tri::T)
            known |= 1 << i;
    }
    if (unknown.size() > 6)
        return {-1};
    std::vector<int> result;
    for (int m = 0; m < (1 << unknown.size()); m++) {
        int a = known;
        for (size_t j = 0; j < unknown.size(); j++)
            if (m & (1 << j))
                a |= 1 << unknown[j];
        result.push_back(first(a));
    }
    return result;
}

NetlistEvaluator::RamUpdate NetlistEvaluator::ram_sample(const RamBlock &block, bool power_up)
{
    if (!block.unsupported.empty())
        throw EvalUnsupported(stringf("'%s': %s has no model", block.cell->name.c_str(ctx), block.unsupported.c_str()));
    RamUpdate u;
    Tri edge[2];
    for (int p = 0; p < 2; p++) {
        u.clk[p] = ram_pin(block, block.port[p].clk);
        u.out[p] = block.port[p].out;
        u.reg[p] = block.port[p].reg;
        Tri last = block.port[p].last_clk;
        if (power_up || u.clk[p] == Tri::F || last == Tri::T)
            edge[p] = Tri::F;
        else if (last == Tri::F && u.clk[p] == Tri::T)
            edge[p] = Tri::T;
        else
            edge[p] = Tri::X;
    }
    if (edge[0] == Tri::F && edge[1] == Tri::F)
        return u;
    const std::vector<Tri> &memory = ram_memory.at(block.cell);
    int memory_bits = block.half < 0 ? int(memory.size()) : int(memory.size()) / 2;
    // Value of `bit` (from the first bit at each possible address) before the clock edge
    auto read = [&](const std::vector<int> &firsts, int bit) { return ram_read(block, firsts, bit); };
    auto write = [&](const std::vector<int> &firsts, int bit, Tri value, Tri cond) {
        if (cond == Tri::F)
            return;
        if (firsts.size() != 1)
            cond = Tri::X;
        for (int f : firsts) {
            if (f < 0) {
                // Anything may be written
                for (int i = 0; i < memory_bits; i++)
                    u.writes.emplace_back(ram_bit(block, i), Tri::X, Tri::X);
                return;
            }
            if (f + bit < memory_bits)
                u.writes.emplace_back(ram_bit(block, f + bit), value, cond);
        }
    };
    // An update of `old` that happens when `cond` holds
    auto update = [](Tri cond, Tri old, Tri value) { return tri_mux(cond, old, value); };
    // Output registers take the data output from before the edge
    for (int p = 0; p < 2; p++)
        if (block.port[p].do_reg)
            for (size_t i = 0; i < u.reg[p].size(); i++)
                u.reg[p][i] = update(edge[p], u.reg[p][i], block.port[p].out[i]);
    const RamPort &a = block.port[0], &b = block.port[1];
    if (block.sdp) {
        // Port A writes, with the data and bit mask of port B for the upper bits; port B reads,
        // into the data outputs of port A for the lower bits and of port B for the upper bits.
        int half_width = int(a.di.size());
        if (a.wr_width > 0 && edge[0] != Tri::F) {
            Tri cond = tri_and(edge[0], tri_and(ram_pin(block, a.en), ram_pin(block, a.we)));
            std::vector<int> firsts = ram_addresses(block, 0, a.wr_width);
            for (int k = 0; k < a.wr_width; k++) {
                const RamPort &src = k < half_width ? a : b;
                int i = k % half_width;
                write(firsts, k, input(block.cell, src.di[i]), tri_and(cond, input(block.cell, src.bm[i])));
            }
        }
        if (b.rd_width > 0 && edge[1] != Tri::F) {
            Tri cond = tri_and(edge[1], ram_pin(block, b.en));
            std::vector<int> firsts = ram_addresses(block, 1, b.rd_width);
            for (int k = 0; k < b.rd_width; k++) {
                int dst = k < half_width ? 0 : 1, i = k % half_width;
                u.out[dst][i] = update(cond, u.out[dst][i], read(firsts, k));
            }
        }
        return u;
    }
    for (int p = 0; p < 2; p++) {
        const RamPort &port = block.port[p];
        int width = std::max(port.rd_width, port.wr_width);
        if (width == 0 || edge[p] == Tri::F)
            continue;
        Tri en = tri_and(edge[p], ram_pin(block, port.en));
        Tri we = ram_pin(block, port.we);
        std::vector<int> firsts = ram_addresses(block, p, width);
        // In a cascade, address bit 0 selects the block: it writes only when selected; the upper
        // block outputs its own data or the lower block's (the lower one's outputs stay)
        Tri selected = Tri::T, own = Tri::T, cascade_in = Tri::X;
        if (block.cascade != 0) {
            Tri a0 = input(block.cell, port.addr[0]);
            selected = block.cascade == 1 ? tri_not(a0) : a0;
            own = a0;
            if (block.cascade == 2)
                cascade_in = ram_cascade_out(rams.at(block.partner), p);
        }
        for (int i = 0; i < width; i++) {
            Tri di = input(block.cell, port.di[i]);
            Tri wr = tri_and(we, input(block.cell, port.bm[i]));
            write(firsts, i, di, tri_and(selected, tri_and(en, wr)));
            if (block.cascade == 1)
                continue;
            Tri data = port.write_through ? tri_mux(wr, read(firsts, i), di) : read(firsts, i);
            if (block.cascade == 2)
                data = tri_mux(own, cascade_in, data);
            if (port.write_through)
                u.out[p][i] = update(en, u.out[p][i], data);
            else
                u.out[p][i] = update(tri_and(en, tri_not(we)), u.out[p][i], data);
        }
    }
    return u;
}

void NetlistEvaluator::ram_commit(RamBlock &block, const RamUpdate &u)
{
    for (int p = 0; p < 2; p++) {
        block.port[p].last_clk = u.clk[p];
        block.port[p].out = u.out[p];
        block.port[p].reg = u.reg[p];
    }
    // Writes of the same edge all see the memory from before it; two definite writes of different
    // values to the same bit (both ports at the same address) leave it unknown.
    std::vector<Tri> &memory = ram_memory.at(block.cell);
    std::unordered_map<int, Tri> definite;
    std::unordered_map<int, Tri> value;
    for (auto &w : u.writes) {
        int bit = std::get<0>(w);
        Tri v = std::get<1>(w), cond = std::get<2>(w);
        Tri cur = value.count(bit) ? value.at(bit) : memory.at(bit);
        if (cond == Tri::T) {
            if (definite.count(bit) && definite.at(bit) != v)
                v = Tri::X;
            definite[bit] = v;
            value[bit] = v;
        } else {
            value[bit] = tri_merge(cur, v);
        }
    }
    for (auto &entry : value)
        memory.at(entry.first) = entry.second;
    memo.clear();
}

Tri NetlistEvaluator::eval_ram(const CellInfo *cell, IdString port)
{
    auto found = ram_outputs.find(std::make_pair(cell, port));
    if (found == ram_outputs.end() && port.in(id_A_CO, id_B_CO)) {
        for (auto &block : rams)
            if (block.cell == cell && block.cascade == 1)
                return ram_cascade_out(block, port == id_A_CO ? 0 : 1);
        return Tri::X;
    }
    if (found == ram_outputs.end()) {
        if (boost::starts_with(port.str(ctx), "ECC") || boost::contains(port.str(ctx), "ECC_"))
            return Tri::X; // not modelled: unknown in the yosys models as well
        throw EvalUnsupported(stringf("block RAM '%s' port %s has no model", cell->name.c_str(ctx), port.c_str(ctx)));
    }
    const RamBlock &block = rams.at(std::get<0>(found->second));
    if (!block.unsupported.empty())
        throw EvalUnsupported(stringf("'%s': %s has no model", cell->name.c_str(ctx), block.unsupported.c_str()));
    const RamPort &p = block.port[std::get<1>(found->second)];
    int bit = std::get<2>(found->second);
    return p.do_reg ? p.reg.at(bit) : p.out.at(bit);
}

// FIFO
//
// The behaviour of the yosys simulation model CC_FIFO_40K: pointers, synchronisers and status
// flags as there, with one deviation. That model stores word n at bit n * 2^(15 - tp), its
// F_WR_PTR value, which for widths of 5 bits and more is less than the width: consecutive words
// overlap and corrupt each other (a word of 40 bits loses its upper 8 bits to the next word).
// Here word n starts at bit n * width, as in the block RAM models. A placed RAM configured as
// FIFO is mapped back with the meaning nextpnr gives its settings (pack_bram.cc).

void NetlistEvaluator::add_fifo(FifoBlock block)
{
    int w = block.width;
    if (w != 1 && w != 2 && w != 5 && w != 10 && w != 20 && w != 40 && w != 80)
        block.unsupported = stringf("FIFO width %d", w);
    block.tp = w == 1 ? 15 : w == 2 ? 14 : w == 5 ? 13 : w == 10 ? 12 : w == 20 ? 11 : w == 40 ? 10 : 9;
    block.depth = 1 << block.tp;
    block.counter_max = 2 * block.depth - 1;
    for (int p = 0; p < 2; p++) {
        block.out[p].assign(40, Tri::F);
        block.reg[p].assign(40, Tri::F);
    }
    ram_memory[block.cell].assign(40960, Tri::F);
    fifo_index[block.cell] = int(fifos.size());
    fifos.push_back(std::move(block));
}

void NetlistEvaluator::add_reference_fifo(const CellInfo *cell)
{
    FifoBlock block;
    block.cell = cell;
    auto param = [&](IdString name) { return int_or_default(cell->params, name, 0); };
    block.sdp = str_or_default(cell->params, id_RAM_MODE, "TDP") == "SDP";
    block.async = str_or_default(cell->params, id_FIFO_MODE, "SYNC") == "ASYNC";
    block.width = param(id_A_WIDTH);
    if (param(id_B_WIDTH) != block.width)
        block.unsupported = "FIFO with different port widths";
    if (param(id_A_ECC_EN) || param(id_B_ECC_EN))
        block.unsupported = "FIFO with ECC";
    block.dyn_offsets = param(id_DYN_STAT_SELECT);
    block.almost_full_offset = param(id_ALMOST_FULL_OFFSET);
    block.almost_empty_offset = param(id_ALMOST_EMPTY_OFFSET);
    block.do_reg[0] = param(id_A_DO_REG);
    block.do_reg[1] = param(id_B_DO_REG);
    block.rd_clk = RamPin{id_A_CLK, Tri::X, bool(param(id_A_CLK_INV))};
    block.wr_clk = block.async ? RamPin{id_B_CLK, Tri::X, bool(param(id_B_CLK_INV))} : block.rd_clk;
    block.a_en = RamPin{id_A_EN, Tri::X, bool(param(id_A_EN_INV))};
    block.b_en = RamPin{id_B_EN, Tri::X, bool(param(id_B_EN_INV))};
    block.b_we = RamPin{id_B_WE, Tri::X, bool(param(id_B_WE_INV))};
    block.rst_n = RamPin{id_F_RST_N};
    for (int i = 0; i < 15; i++) {
        block.almost_full_pins.push_back(ctx->idf("F_ALMOST_FULL_OFFSET[%d]", i));
        block.almost_empty_pins.push_back(ctx->idf("F_ALMOST_EMPTY_OFFSET[%d]", i));
    }
    for (int p = 0; p < 2; p++) {
        char name = "AB"[p];
        for (int i = 0; i < 40; i++) {
            block.di[p].push_back(ctx->idf("%c_DI[%d]", name, i));
            block.bm[p].push_back(ctx->idf("%c_BM[%d]", name, i));
            block.outputs[ctx->idf("%c_DO[%d]", name, i)] = {p ? FIFO_DO_B : FIFO_DO_A, i};
        }
    }
    block.outputs[id_F_FULL] = {FIFO_FULL, 0};
    block.outputs[id_F_EMPTY] = {FIFO_EMPTY, 0};
    block.outputs[id_F_ALMOST_FULL] = {FIFO_ALMOST_FULL, 0};
    block.outputs[id_F_ALMOST_EMPTY] = {FIFO_ALMOST_EMPTY, 0};
    block.outputs[id_F_RD_ERROR] = {FIFO_RD_ERROR, 0};
    block.outputs[id_F_WR_ERROR] = {FIFO_WR_ERROR, 0};
    for (int i = 0; i < 16; i++) {
        block.outputs[ctx->idf("F_RD_PTR[%d]", i)] = {FIFO_RD_PTR, i};
        block.outputs[ctx->idf("F_WR_PTR[%d]", i)] = {FIFO_WR_PTR, i};
    }
    add_fifo(std::move(block));
}

void NetlistEvaluator::add_placed_fifo(const CellInfo *cell)
{
    FifoBlock block;
    block.cell = cell;
    auto cfg = [&](const std::string &name) { return int_or_default(cell->params, ctx->id("RAM_cfg_" + name), 0); };
    static const int widths[8] = {0, 1, 2, 5, 10, 20, 40, 80};
    block.sdp = cfg("sram_mode") & 2;
    block.async = cfg("fifo_async_enable");
    block.width = widths[cfg("output_config_a0") & 7];
    if (widths[cfg("input_config_b0") & 7] != block.width)
        block.unsupported = "FIFO with different port widths";
    if (cfg("ecc_enable"))
        block.unsupported = "FIFO with ECC";
    block.dyn_offsets = cfg("dyn_stat_select") & 0b10;
    block.almost_full_offset = cfg("almost_full_offset");
    block.almost_empty_offset = cfg("almost_empty_offset");
    block.do_reg[0] = cfg("a0_set_outputreg");
    block.do_reg[1] = cfg("b0_set_outputreg");
    // Clock, enable and write enable selection as for block RAM (port A pops, port B pushes)
    auto clock = [&](int p) {
        RamPin pin;
        char name = "AB"[p], other = "BA"[p];
        switch (cfg(stringf("forward_%c0_clk", "ab"[p]))) {
        case 0b00000000:
            pin.port = ctx->idf("CLK%c[0]", name);
            break;
        case 0b00001000:
            pin.port = ctx->idf("CLK%c[0]", other);
            break;
        case 0b00100011:
            pin.port = id_CLOCK1;
            break;
        case 0b00110011:
            pin.port = id_CLOCK2;
            break;
        case 0b00000011:
            pin.port = id_CLOCK3;
            break;
        case 0b00010011:
            pin.port = id_CLOCK4;
            break;
        default:
            block.unsupported = "FIFO clock selection";
        }
        pin.invert = cfg(stringf("inversion_%c0", "ab"[p])) & 0b100;
        return pin;
    };
    auto ctrl = [&](int p, const char *what, const char *pin_name, int inv_bit) {
        RamPin pin;
        switch (cfg(stringf("forward_%c0_%s", "ab"[p], what))) {
        case 0b00000000:
            pin.port = ctx->idf("%s%c[0]", pin_name, "AB"[p]);
            break;
        case 0b00000011:
            pin.constant = Tri::F;
            break;
        case 0b00010011:
            pin.constant = Tri::T;
            break;
        default:
            block.unsupported = stringf("FIFO %s selection", what);
        }
        pin.invert = cfg(stringf("inversion_%c0", "ab"[p])) & inv_bit;
        return pin;
    };
    block.rd_clk = clock(0);
    block.wr_clk = block.async ? clock(1) : block.rd_clk;
    block.a_en = ctrl(0, "en", "EN", 0b001);
    block.b_en = ctrl(1, "en", "EN", 0b001);
    block.b_we = ctrl(1, "we", "GLWE", 0b010);
    block.rst_n = RamPin{id_F_RSTN};
    // Dynamic offsets come in on the bit mask pins of port A (pack_bram.cc)
    for (int i = 0; i < 15; i++) {
        block.almost_empty_pins.push_back(ctx->idf("WEA[%d]", i));
        block.almost_full_pins.push_back(ctx->idf("WEA[%d]", 20 + i));
    }
    for (int p = 0; p < 2; p++) {
        char name = "AB"[p];
        for (int i = 0; i < 40; i++) {
            block.di[p].push_back(ctx->idf("DI%c[%d]", name, i));
            block.bm[p].push_back(ctx->idf("WE%c[%d]", name, i));
            block.outputs[ctx->idf("DO%c[%d]", name, i)] = {p ? FIFO_DO_B : FIFO_DO_A, i};
        }
    }
    block.outputs[ctx->id("F_FULL[0]")] = {FIFO_FULL, 0};
    block.outputs[ctx->id("F_EMPTY[0]")] = {FIFO_EMPTY, 0};
    block.outputs[ctx->id("F_AL_FULL[0]")] = {FIFO_ALMOST_FULL, 0};
    block.outputs[ctx->id("F_AL_EMPTY[0]")] = {FIFO_ALMOST_EMPTY, 0};
    block.outputs[ctx->id("FRD_ERR[0]")] = {FIFO_RD_ERROR, 0};
    block.outputs[ctx->id("FWR_ERR[0]")] = {FIFO_WR_ERROR, 0};
    for (int i = 0; i < 16; i++) {
        block.outputs[ctx->idf("FRD_ADDR[%d]", i)] = {FIFO_RD_PTR, i};
        block.outputs[ctx->idf("FWR_ADDR[%d]", i)] = {FIFO_WR_PTR, i};
    }
    add_fifo(std::move(block));
}

// The almost full/empty offsets in use; X if dynamic offsets have unknown bits
Tri NetlistEvaluator::fifo_offsets_known(const FifoBlock &block, int &almost_full, int &almost_empty)
{
    almost_full = block.almost_full_offset;
    almost_empty = block.almost_empty_offset;
    if (!block.dyn_offsets)
        return Tri::T;
    almost_full = almost_empty = 0;
    for (int i = 0; i < 15; i++) {
        Tri f = input(block.cell, block.almost_full_pins[i]);
        Tri e = input(block.cell, block.almost_empty_pins[i]);
        if (f == Tri::X || e == Tri::X)
            return Tri::X;
        almost_full |= (f == Tri::T) << i;
        almost_empty |= (e == Tri::T) << i;
    }
    return Tri::T;
}

// The combinational part of cells_sim.v CC_FIFO_40K, for a known state
NetlistEvaluator::FifoFlags NetlistEvaluator::fifo_flags(const FifoBlock &b, int almost_full_offset,
                                                         int almost_empty_offset) const
{
    int tp = b.tp;
    auto bits = [](int v, int hi, int lo) { return (v >> lo) & ((1 << (hi - lo + 1)) - 1); };
    auto bit = [](int v, int i) { return (v >> i) & 1; };
    FifoFlags f;
    f.rdaddr = (bit(b.rd_pointer, tp) << tp) | bits(b.rd_pointer_int, tp - 1, 0);
    f.wraddr = ((bit(b.wr_pointer, tp) << (tp + 1)) | bits(b.wr_pointer_int, tp, 0)) & 0xffff;
    // Gray code decoding of the synchronised pointers
    auto decode = [&](int sync) {
        int v = 0;
        for (int i = tp; i >= 0; i--)
            v |= (i == tp ? bit(sync, i) : (bit(sync, i) ^ bit(v, i + 1))) << i;
        return v;
    };
    int rd_pointer_sync = decode(b.rd_sync_1), wr_pointer_sync = decode(b.wr_sync_1);
    if (b.async)
        f.full = bits(f.wraddr, tp - 2, 0) == bits(b.rd_sync_1, tp - 2, 0) &&
                 bit(f.wraddr, tp) != bit(b.rd_sync_1, tp) && bit(f.wraddr, tp - 1) != bit(b.rd_sync_1, tp - 1);
    else
        f.full = bits(b.wr_pointer, tp - 1, 0) == bits(b.rd_pointer, tp - 1, 0) &&
                 bit(b.wr_pointer, tp) != bit(b.rd_pointer, tp);
    if (b.async)
        f.empty = bits(b.wr_sync_1, tp, 0) == bits(f.rdaddr, tp, 0);
    else
        f.empty = bits(b.wr_pointer, tp, 0) == bits(b.rd_pointer, tp, 0);
    // 16-bit unsigned arithmetic, as in the model
    int rd_cmp = b.async ? rd_pointer_sync : b.rd_pointer;
    int wr_low = bits(b.wr_pointer, tp - 1, 0), rd_cmp_low = bits(rd_cmp, tp - 1, 0);
    if (bit(b.wr_pointer, tp) == bit(rd_cmp, tp))
        f.almost_full = ((wr_low - rd_cmp_low) & 0xffff) >= ((b.depth - almost_full_offset) & 0xffff);
    else
        f.almost_full = ((rd_cmp_low - wr_low) & 0xffff) <= almost_full_offset;
    int wr_cmp = b.async ? wr_pointer_sync : b.wr_pointer;
    int wr_cmp_low = bits(wr_cmp, tp - 1, 0), rd_low = bits(b.rd_pointer, tp - 1, 0);
    if (bit(wr_cmp, tp) == bit(b.rd_pointer, tp))
        f.almost_empty = ((wr_cmp_low - rd_low) & 0xffff) <= almost_empty_offset;
    else
        f.almost_empty = ((rd_low - wr_cmp_low) & 0xffff) >= ((b.depth - almost_empty_offset) & 0xffff);
    return f;
}

// The state after the current inputs (clock edges and reset); memory writes go to `writes`
NetlistEvaluator::FifoBlock NetlistEvaluator::fifo_sample(const FifoBlock &b, bool power_up,
                                                          std::vector<std::pair<int, Tri>> &writes)
{
    if (!b.unsupported.empty())
        throw EvalUnsupported(stringf("'%s': %s has no model", b.cell->name.c_str(ctx), b.unsupported.c_str()));
    FifoBlock n = b;
    Tri rd_clk = pin_value(b.cell, b.rd_clk), wr_clk = pin_value(b.cell, b.wr_clk);
    n.last_rd_clk = rd_clk;
    n.last_wr_clk = wr_clk;
    Tri rst_n = input(b.cell, b.rst_n.port);
    if (rst_n == Tri::F) {
        // Asynchronous reset of the pointers and synchronisers (not of the data outputs)
        n.known = true;
        n.rd_pointer = n.rd_pointer_int = n.wr_pointer = n.wr_pointer_int = 0;
        n.rd_sync_0 = n.rd_sync_1 = n.wr_sync_0 = n.wr_sync_1 = 0;
        return n;
    }
    auto edge_of = [&](Tri last, Tri now) {
        if (power_up || now == Tri::F || last == Tri::T)
            return Tri::F;
        if (last == Tri::F && now == Tri::T)
            return Tri::T;
        return Tri::X;
    };
    Tri rd_edge = edge_of(b.last_rd_clk, rd_clk), wr_edge = edge_of(b.last_wr_clk, wr_clk);
    if (rd_edge == Tri::F && wr_edge == Tri::F)
        return n;
    int almost_full, almost_empty;
    Tri offsets = fifo_offsets_known(b, almost_full, almost_empty);
    Tri a_en = pin_value(b.cell, b.a_en), b_en = pin_value(b.cell, b.b_en), b_we = pin_value(b.cell, b.b_we);
    if (!b.known || rst_n == Tri::X || rd_edge == Tri::X || wr_edge == Tri::X || offsets == Tri::X ||
        (rd_edge == Tri::T && a_en == Tri::X) || (wr_edge == Tri::T && (b_en == Tri::X || b_we == Tri::X))) {
        // The state (and what is popped) becomes unknown
        n.known = false;
        if (rd_edge != Tri::F)
            for (int p = 0; p < 2; p++)
                for (int i = 0; i < 40; i++) {
                    n.reg[p][i] = b.do_reg[p] ? tri_merge(b.reg[p][i], b.out[p][i]) : b.reg[p][i];
                    n.out[p][i] = Tri::X;
                }
        if (wr_edge != Tri::F)
            for (int i = 0; i < 40960; i++)
                writes.emplace_back(i, Tri::X);
        return n;
    }
    FifoFlags f = fifo_flags(b, almost_full, almost_empty);
    bool ram_en = !f.empty && a_en == Tri::T;
    bool ram_we = !f.full && b_en == Tri::T && b_we == Tri::T;
    auto next = [&](int p) { return p == b.counter_max ? 0 : p + 1; };
    const std::vector<Tri> &memory = ram_memory.at(b.cell);
    int rd_word = b.rd_pointer & (b.depth - 1), wr_word = b.wr_pointer & (b.depth - 1);
    if (rd_edge == Tri::T) {
        for (int p = 0; p < 2; p++)
            if (b.do_reg[p])
                n.reg[p] = b.out[p];
        if (ram_en) {
            int nxt = next(b.rd_pointer);
            n.rd_pointer = nxt;
            n.rd_pointer_int = ((nxt >> 1) ^ nxt) & 0x7fff;
            // TDP pops on port A; SDP on A for the lower 40 bits and B for the upper ones
            for (int k = 0; k < b.width; k++)
                n.out[k / 40][k % 40] = memory.at(rd_word * b.width + k);
        }
        n.wr_sync_0 = f.wraddr;
        n.wr_sync_1 = b.wr_sync_0;
    }
    if (wr_edge == Tri::T) {
        if (ram_we) {
            int nxt = next(b.wr_pointer);
            n.wr_pointer = nxt;
            n.wr_pointer_int = ((nxt >> 1) ^ nxt) & 0x7fff;
            // TDP pushes from port B; SDP from A for the lower 40 bits and B for the upper ones
            for (int k = 0; k < b.width; k++) {
                int p = b.sdp ? k / 40 : 1, i = b.sdp ? k % 40 : k;
                Tri bm = input(b.cell, b.bm[p][i]);
                Tri di = input(b.cell, b.di[p][i]);
                int bit = wr_word * b.width + k;
                if (bm == Tri::T)
                    writes.emplace_back(bit, di);
                else if (bm == Tri::X)
                    writes.emplace_back(bit, tri_merge(memory.at(bit), di));
            }
        }
        n.rd_sync_0 = f.rdaddr;
        n.rd_sync_1 = b.rd_sync_0;
    }
    return n;
}

Tri NetlistEvaluator::eval_fifo(const FifoBlock &b, IdString port)
{
    if (!b.unsupported.empty())
        throw EvalUnsupported(stringf("'%s': %s has no model", b.cell->name.c_str(ctx), b.unsupported.c_str()));
    auto found = b.outputs.find(port);
    if (found == b.outputs.end()) {
        if (boost::contains(port.str(ctx), "ECC"))
            return Tri::X;
        throw EvalUnsupported(stringf("FIFO '%s' port %s has no model", b.cell->name.c_str(ctx), port.c_str(ctx)));
    }
    FifoOutput what = found->second.first;
    int i = found->second.second;
    if (what == FIFO_DO_A || what == FIFO_DO_B) {
        int p = what == FIFO_DO_B;
        return b.do_reg[p] ? b.reg[p][i] : b.out[p][i];
    }
    int almost_full, almost_empty;
    if (!b.known || fifo_offsets_known(b, almost_full, almost_empty) == Tri::X)
        return Tri::X;
    FifoFlags f = fifo_flags(b, almost_full, almost_empty);
    auto ptr = [&](int pointer) { return ((pointer & ((1 << b.tp) - 1)) << (15 - b.tp)) & 0xffff; };
    switch (what) {
    case FIFO_FULL:
        return tri(f.full);
    case FIFO_EMPTY:
        return tri(f.empty);
    case FIFO_ALMOST_FULL:
        return tri(f.almost_full);
    case FIFO_ALMOST_EMPTY:
        return tri(f.almost_empty);
    case FIFO_RD_ERROR:
        return tri_and(tri(f.empty), pin_value(b.cell, b.a_en));
    case FIFO_WR_ERROR:
        return tri_and(tri(f.full), tri_and(pin_value(b.cell, b.b_en), pin_value(b.cell, b.b_we)));
    case FIFO_RD_PTR:
        return tri((ptr(b.rd_pointer) >> i) & 1);
    case FIFO_WR_PTR:
        return tri((ptr(b.wr_pointer) >> i) & 1);
    default:
        return Tri::X;
    }
}

::testing::AssertionResult output_computes(Context *ctx, const std::string &obuf, const std::vector<std::string> &inputs,
                                           const std::function<bool(const std::vector<bool> &)> &expected)
{
    NetlistEvaluator ev(ctx);
    std::vector<bool> values(inputs.size());
    try {
        for (uint64_t pattern = 0; pattern < (uint64_t(1) << inputs.size()); pattern++) {
            for (size_t i = 0; i < inputs.size(); i++) {
                values[i] = (pattern >> i) & 1;
                ev.set_input(ctx->id(inputs[i]), bool(values[i]));
            }
            Tri got = ev.eval_output(ctx->id(obuf));
            Tri want = expected(values) ? Tri::T : Tri::F;
            if (got != want) {
                std::string assignment;
                for (size_t i = 0; i < inputs.size(); i++)
                    assignment += stringf("%s%s=%d", i ? ", " : "", inputs[i].c_str(), int(values[i]));
                return ::testing::AssertionFailure() << "output '" << obuf << "' is " << tri_str(got) << ", expected "
                                                     << tri_str(want) << " for " << assignment;
            }
        }
    } catch (const EvalUnsupported &e) {
        return ::testing::AssertionFailure() << "cannot evaluate output '" << obuf << "': " << e.what();
    }
    return ::testing::AssertionSuccess();
}

::testing::AssertionResult outputs_match(Context *reference, Context *implementation, int steps, uint64_t seed)
{
    std::vector<std::string> ins, outs;
    for (auto &cell : reference->cells) {
        if (cell.second->type.in(id_CC_IBUF, id_CC_IOBUF, id_CC_LVDS_IBUF, id_CC_LVDS_IOBUF))
            ins.push_back(cell.first.str(reference));
        if (cell.second->type.in(id_CC_OBUF, id_CC_TOBUF, id_CC_IOBUF, id_CC_LVDS_OBUF, id_CC_LVDS_TOBUF,
                                 id_CC_LVDS_IOBUF))
            outs.push_back(cell.first.str(reference));
    }
    std::sort(ins.begin(), ins.end());
    std::sort(outs.begin(), outs.end());
    if (outs.empty())
        return ::testing::AssertionFailure() << "design has no outputs";
    NetlistEvaluator ref(reference), impl(implementation);
    // Registers removed by the packer (e.g. ones that can never change) may take any value
    std::set<std::string> regs, impl_regs = impl.register_names();
    for (auto &name : ref.register_names())
        if (impl_regs.count(name))
            regs.insert(name);
    ref.resolve_unknown_init(seed, regs);
    impl.resolve_unknown_init(seed, regs);
    std::mt19937_64 rng(seed);
    // Registers moved into an IOSEL lose their name; pair them with the original register through
    // the nets the packer keeps, and start both from the same value when the original has no
    // defined initial value.
    auto ref_bit = [&](const NetInfo *net) -> std::pair<const CellInfo *, int> {
        if (net == nullptr)
            return {nullptr, 0};
        IdString name = reference->id(net->name.str(implementation));
        if (!reference->nets.count(name))
            return {nullptr, 0};
        const PortRef &drv = reference->nets.at(name)->driver;
        if (drv.cell == nullptr)
            return {nullptr, 0};
        if (drv.cell->type.in(id_CC_DFF, id_CC_IDDR))
            return {drv.cell, drv.port == id_Q1 ? 1 : 0};
        return {nullptr, 0};
    };
    for (auto &bit : impl.state_bits()) {
        const CellInfo *iosel = bit.first;
        if (iosel->type != id_IOSEL)
            continue;
        std::pair<const CellInfo *, int> orig{nullptr, 0};
        if (bit.second < 2) {
            orig = ref_bit(iosel->getPort(bit.second == 0 ? id_IN1 : id_IN2));
        } else {
            // The output pad the IOSEL belongs to, and what drives it in the reference
            std::string pad = iosel->name.str(implementation);
            pad = pad.substr(0, pad.rfind("$iosel"));
            if (reference->cells.count(reference->id(pad))) {
                const NetInfo *a = reference->cells.at(reference->id(pad))->getPort(id_A);
                const CellInfo *drv = a ? a->driver.cell : nullptr;
                if (drv && drv->type == id_CC_DFF && bit.second == 2)
                    orig = {drv, 0};
                // As in nextpnr's CC_ODDR mapping: rising edge register to OUT2, falling to OUT1
                if (drv && drv->type == id_CC_ODDR)
                    orig = {drv, bit.second == 3 ? 0 : 1};
            }
        }
        if (orig.first == nullptr || ref.state(orig.first, orig.second) != Tri::X)
            continue;
        Tri v = (rng() & 1) ? Tri::T : Tri::F;
        ref.set_state(orig.first, orig.second, v);
        impl.set_state(iosel, bit.second, v);
    }
    std::string history;
    int compared = 0;
    try {
        // Power up with all inputs low
        for (auto &name : ins) {
            ref.set_input(reference->id(name), false);
            impl.set_input(implementation->id(name), false);
        }
        ref.settle(true);
        impl.settle(true);
        for (int step = 0; step < steps; step++) {
            std::string assignment;
            for (auto &name : ins) {
                bool v = rng() & 1;
                ref.set_input(reference->id(name), v);
                impl.set_input(implementation->id(name), v);
                assignment += stringf(" %s=%d", name.c_str(), int(v));
            }
            ref.settle();
            impl.settle();
            history += stringf("\n  step %d:%s", step, assignment.c_str());
            auto mismatch = [&](const std::string &what, const std::string &name, Tri got, Tri want, IdString port) {
                // Keep only the last few steps of the input history in the message
                size_t pos = history.size();
                for (int lines = 0; lines < 8 && pos != std::string::npos && pos > 0; lines++)
                    pos = history.rfind('\n', pos - 1);
                std::string why;
                CellInfo *buffer = implementation->cells.at(implementation->id(name)).get();
                if (got == Tri::X && buffer->getPort(port))
                    why = "; x comes from " + impl.explain_unknown(buffer->getPort(port));
                return ::testing::AssertionFailure()
                       << what << " of '" << name << "' is " << tri_str(got) << ", expected " << tri_str(want)
                       << " at step " << step << why
                       << "; inputs:" << history.substr(pos == std::string::npos ? 0 : pos);
            };
            for (auto &name : outs) {
                // Whether the pad drives, then the driven value
                Tri want_en = ref.eval_output_enable(reference->id(name));
                if (want_en != Tri::X) {
                    Tri got_en = impl.eval_output_enable(implementation->id(name));
                    compared++;
                    if (got_en != want_en)
                        return mismatch("output enable", name, got_en, want_en, id_T);
                }
                if (want_en != Tri::T)
                    continue;
                Tri want = ref.eval_output(reference->id(name));
                if (want == Tri::X)
                    continue;
                Tri got = impl.eval_output(implementation->id(name));
                compared++;
                if (got != want)
                    return mismatch("output", name, got, want, id_A);
            }
        }
    } catch (const EvalUnsupported &e) {
        return ::testing::AssertionFailure() << "cannot simulate: " << e.what();
    }
    if (compared == 0)
        return ::testing::AssertionFailure() << "no output had a known reference value";
    return ::testing::AssertionSuccess();
}

NEXTPNR_NAMESPACE_END
