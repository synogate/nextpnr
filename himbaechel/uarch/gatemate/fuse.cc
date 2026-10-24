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

#include <algorithm>
#include <array>
#include <fstream>
#include <functional>
#include <numeric>

#include "pack.h"

#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

NEXTPNR_NAMESPACE_BEGIN

// Fusing LUTs before packing (--vopt fuse-luts).
//
// Synthesis leaves logic levels a CPE could do without: inverters in front of multiplexers, registers
// and adders, and LUTs whose function would fit into the LUT they feed. Each costs a LUT delay and a
// routing hop on every path through it, and a CPE half. Every rewrite here keeps the function
// exactly, by truth tables:
//  - an inverter goes into the tables of all the LUTs and 2:1 multiplexers it feeds (forward);
//  - or into the table of the LUT feeding it, when that feeds nothing else (backward);
//  - or behind the register it feeds, into the tables of all the register's users, with the
//    register's set/reset and initial values inverted (through a register);
//  - a LUT goes into the LUTs and 2:1 multiplexers it feeds, at most two, when each then still fits
//    one CPE half, L10(L00(I0, I1), L01(I2, I3)) (fuse). With more, each of its inputs would gain as
//    many sinks: one with 40 users made the routing much slower.
// Rewritten cells become CC_L2T4 (CC_L2T5 ones only change their tables). Cells left without users
// go in the cleanup after.
//
// After packing, the upper half of a CC_L2T5's CPE only passes its fifth input on to the lower half
// (COMBIN). Where that input comes from a LUT feeding nothing else, the upper half computes that LUT
// instead (fuse_l2t5_inputs).

namespace {
// A function of at most 5 distinct signals: bit i of the table is its value where signal k is
// (i >> k) & 1
struct Lut
{
    std::vector<NetInfo *> inputs;
    uint32_t table = 0;
    bool value(uint32_t index) const { return (table >> index) & 1; }
};

int lut2(int init, bool a, bool b) { return (init >> ((int(b) << 1) | int(a))) & 1; }

const std::array<IdString, 4> l2t4_inputs = {id_I0, id_I1, id_I2, id_I3};
} // namespace

namespace {
struct LutFuser
{
    Context *ctx;
    NetInfo *gnd, *vcc;
    int forward = 0, backward = 0, through = 0, fused = 0;

    bool keep(const CellInfo *ci) const { return ci->attrs.count(ctx->id("keep")); }

    // The ports and model of the LUT-like cells
    std::vector<IdString> lut_ports(const CellInfo *ci) const
    {
        if (ci->type == id_CC_LUT1)
            return {id_I0};
        if (ci->type == id_CC_LUT2)
            return {id_I0, id_I1};
        if (ci->type == id_CC_L2T4)
            return {id_I0, id_I1, id_I2, id_I3};
        if (ci->type == id_CC_L2T5)
            return {id_I0, id_I1, id_I2, id_I3, id_I4};
        if (ci->type == id_CC_MX2)
            return {id_D0, id_D1, id_S0};
        if (ci->type == id_CPE_L2T4)
            return {id_D0_00, id_D1_00, id_D0_01, id_D1_01, id_D0_10, id_D1_10};
        return {};
    }
    bool evaluate(const CellInfo *ci, const std::vector<bool> &v) const
    {
        auto init = [&](IdString name) { return int_or_default(ci->params, name, 0); };
        if (ci->type == id_CC_LUT1)
            return (init(id_INIT) >> int(v[0])) & 1;
        if (ci->type == id_CC_LUT2)
            return lut2(init(id_INIT), v[0], v[1]);
        if (ci->type == id_CC_L2T4)
            return lut2(init(id_INIT_L10), lut2(init(id_INIT_L00), v[0], v[1]), lut2(init(id_INIT_L01), v[2], v[3]));
        if (ci->type == id_CC_L2T5)
            return lut2(
                    init(id_INIT_L20), v[4],
                    lut2(init(id_INIT_L11), lut2(init(id_INIT_L02), v[0], v[1]), lut2(init(id_INIT_L03), v[2], v[3])));
        if (ci->type == id_CPE_L2T4) {
            // Packed: D0_10/D1_10 feed L10 directly where connected
            bool a = ci->getPort(id_D0_10) ? v[4] : lut2(init(id_INIT_L00), v[0], v[1]);
            bool b = ci->getPort(id_D1_10) ? v[5] : lut2(init(id_INIT_L01), v[2], v[3]);
            return lut2(init(id_INIT_L10), a, b);
        }
        return v[2] ? v[1] : v[0]; // CC_MX2
    }
    IdString output(const CellInfo *ci) const
    {
        return ci->type == id_CC_MX2 ? id_Y : ci->type == id_CPE_L2T4 ? id_OUT : id_O;
    }

    // The function of a LUT-like cell over its distinct signals; false if it depends on an
    // unconnected input or has more than 5 signals
    bool function(CellInfo *ci, Lut &f) const
    {
        auto ports = lut_ports(ci);
        if (ports.empty() || keep(ci))
            return false;
        // A packed LUT tree fed otherwise (carry, COMBIN, ...) has no model here
        for (auto &port : ci->ports)
            if (port.second.type == PORT_IN && port.second.net &&
                std::find(ports.begin(), ports.end(), port.first) == ports.end())
                return false;
        for (IdString param : {id_C_I1, id_C_I2, id_C_I3, id_C_I4})
            if (int_or_default(ci->params, param, 0))
                return false;
        f = Lut();
        std::vector<int> source(ports.size()); // signal index, or -1 constant 0, -2 constant 1, -3 open
        std::vector<int> open;
        for (size_t i = 0; i < ports.size(); i++) {
            NetInfo *net = ci->getPort(ports[i]);
            if (!net) {
                source[i] = -3;
                open.push_back(i);
            } else if (net == gnd || net == vcc) {
                source[i] = net == gnd ? -1 : -2;
            } else {
                auto found = std::find(f.inputs.begin(), f.inputs.end(), net);
                source[i] = found - f.inputs.begin();
                if (found == f.inputs.end())
                    f.inputs.push_back(net);
            }
        }
        if (f.inputs.size() > 5)
            return false;
        for (uint32_t index = 0; index < (1u << f.inputs.size()); index++) {
            int result = -1;
            for (uint32_t o = 0; o < (1u << open.size()); o++) {
                std::vector<bool> v(ports.size());
                for (size_t i = 0; i < ports.size(); i++)
                    v[i] = source[i] >= 0 ? (index >> source[i]) & 1 : source[i] == -2;
                for (size_t k = 0; k < open.size(); k++)
                    v[open[k]] = (o >> k) & 1;
                int r = evaluate(ci, v);
                if (result != -1 && r != result)
                    return false;
                result = r;
            }
            f.table |= uint32_t(result) << index;
        }
        return true;
    }

    // f with `signal` replaced by g
    bool compose(const Lut &f, NetInfo *signal, const Lut &g, Lut &out) const
    {
        out = Lut();
        for (NetInfo *net : f.inputs)
            if (net != signal)
                out.inputs.push_back(net);
        for (NetInfo *net : g.inputs)
            if (std::find(out.inputs.begin(), out.inputs.end(), net) == out.inputs.end())
                out.inputs.push_back(net);
        if (out.inputs.size() > 5)
            return false;
        auto index_of = [&](const Lut &l, uint32_t index) {
            uint32_t sub = 0;
            for (size_t k = 0; k < l.inputs.size(); k++) {
                auto pos = std::find(out.inputs.begin(), out.inputs.end(), l.inputs[k]) - out.inputs.begin();
                if (pos < int(out.inputs.size()))
                    sub |= ((index >> pos) & 1) << k;
            }
            return sub;
        };
        for (uint32_t index = 0; index < (1u << out.inputs.size()); index++) {
            bool inner = g.value(index_of(g, index));
            uint32_t sub = index_of(f, index);
            auto pos = std::find(f.inputs.begin(), f.inputs.end(), signal) - f.inputs.begin();
            if (pos < int(f.inputs.size()))
                sub = (sub & ~(1u << pos)) | (uint32_t(inner) << pos);
            out.table |= uint32_t(f.value(sub)) << index;
        }
        return true;
    }

    // The tables of one CPE half for f, with its signals on I0..I3 (nullptr: unused, and the tables
    // do not depend on it); false if f does not decompose so
    bool fit_half(const Lut &f, std::array<NetInfo *, 4> &on, int &l00, int &l01, int &l10) const
    {
        int n = f.inputs.size();
        if (n > 4)
            return false;
        std::array<int, 4> slots = {0, 1, 2, 3};
        do {
            // signal k on slot slots[k]
            auto value = [&](int row, int col) {
                int bits = row | (col << 2);
                uint32_t index = 0;
                for (int k = 0; k < n; k++)
                    index |= uint32_t((bits >> slots[k]) & 1) << k;
                return f.value(index);
            };
            std::array<int, 4> row_class, col_class;
            std::vector<int> row_rep, col_rep;
            auto classify = [&](bool rows, std::array<int, 4> &cls, std::vector<int> &rep) {
                for (int a = 0; a < 4; a++) {
                    cls[a] = -1;
                    for (size_t c = 0; c < rep.size() && cls[a] == -1; c++) {
                        bool same = true;
                        for (int b = 0; b < 4; b++)
                            same &= rows ? value(a, b) == value(rep[c], b) : value(b, a) == value(b, rep[c]);
                        if (same)
                            cls[a] = c;
                    }
                    if (cls[a] == -1) {
                        cls[a] = rep.size();
                        rep.push_back(a);
                    }
                }
                return rep.size() <= 2;
            };
            if (classify(true, row_class, row_rep) && classify(false, col_class, col_rep)) {
                on.fill(nullptr);
                for (int k = 0; k < n; k++)
                    on[slots[k]] = f.inputs[k];
                l00 = l01 = l10 = 0;
                for (int a = 0; a < 4; a++) {
                    l00 |= row_class[a] << a;
                    l01 |= col_class[a] << a;
                }
                for (int g = 0; g < 2; g++)
                    for (int h = 0; h < 2; h++) {
                        int row = row_rep[std::min<int>(g, row_rep.size() - 1)];
                        int col = col_rep[std::min<int>(h, col_rep.size() - 1)];
                        l10 |= int(value(row, col)) << ((h << 1) | g);
                    }
                return true;
            }
        } while (std::next_permutation(slots.begin(), slots.end()));
        // One signal on an input of both first-level LUTs (as the select of a 2:1 multiplexer):
        // then only the input combinations where the two agree occur, and the tables come from a
        // search over the ones of L00 and L01
        if (n > 3)
            return false;
        for (int dup = 0; dup < n; dup++)
            for (int a : {0, 1})
                for (int b : {2, 3}) {
                    std::vector<int> rest;
                    for (int k = 0; k < n; k++)
                        if (k != dup)
                            rest.push_back(k);
                    std::array<int, 2> free = {1 - a, 5 - b};
                    for (int swap = 0; swap < 2; swap++) {
                        std::array<int, 4> slot_of = {-1, -1, -1, -1}; // slot -> signal
                        slot_of[a] = slot_of[b] = dup;
                        for (size_t r = 0; r < rest.size(); r++)
                            slot_of[free[(r + swap) % 2]] = rest[r];
                        auto consistent = [&](int bits) { return ((bits >> a) & 1) == ((bits >> b) & 1); };
                        auto value = [&](int bits) {
                            uint32_t index = 0;
                            for (int slot = 0; slot < 4; slot++)
                                if (slot_of[slot] >= 0)
                                    index |= uint32_t((bits >> slot) & 1) << slot_of[slot];
                            return f.value(index);
                        };
                        // The first-level tables must not depend on an unused input
                        auto ignores_unused = [&](int table, int first) {
                            for (int x = 0; x < 4; x++)
                                for (int bit = 0; bit < 2; bit++)
                                    if (slot_of[first + bit] < 0 &&
                                        ((table >> x) & 1) != ((table >> (x ^ (1 << bit))) & 1))
                                        return false;
                            return true;
                        };
                        for (int g = 0; g < 16; g++) {
                            if (!ignores_unused(g, 0))
                                continue;
                            for (int h = 0; h < 16; h++) {
                                if (!ignores_unused(h, 2))
                                    continue;
                                int table = 0, known = 0;
                                bool ok = true;
                                for (int bits = 0; bits < 16 && ok; bits++) {
                                    if (!consistent(bits))
                                        continue;
                                    int at = (((h >> (bits >> 2)) & 1) << 1) | ((g >> (bits & 3)) & 1);
                                    int v = value(bits);
                                    if ((known >> at) & 1)
                                        ok = ((table >> at) & 1) == v;
                                    table |= v << at;
                                    known |= 1 << at;
                                }
                                if (!ok)
                                    continue;
                                on.fill(nullptr);
                                for (int slot = 0; slot < 4; slot++)
                                    if (slot_of[slot] >= 0)
                                        on[slot] = f.inputs[slot_of[slot]];
                                l00 = g;
                                l01 = h;
                                l10 = table;
                                return true;
                            }
                        }
                    }
                }
        return false;
    }

    // Make ci a CC_L2T4 computing f (which must fit)
    void write_half(CellInfo *ci, const Lut &f)
    {
        std::array<NetInfo *, 4> on;
        int l00, l01, l10;
        bool fits = fit_half(f, on, l00, l01, l10);
        NPNR_ASSERT(fits);
        IdString out = output(ci);
        for (IdString port : lut_ports(ci)) {
            ci->disconnectPort(port);
            ci->ports.erase(port);
        }
        if (out != id_O)
            ci->renamePort(out, id_O);
        for (IdString param :
             {id_INIT, id_INIT_L00, id_INIT_L01, id_INIT_L02, id_INIT_L03, id_INIT_L10, id_INIT_L11, id_INIT_L20})
            ci->params.erase(param);
        ci->type = id_CC_L2T4;
        for (int i = 0; i < 4; i++) {
            ci->addInput(l2t4_inputs[i]);
            if (on[i])
                ci->connectPort(l2t4_inputs[i], on[i]);
        }
        ci->params[id_INIT_L00] = Property(l00, 4);
        ci->params[id_INIT_L01] = Property(l01, 4);
        ci->params[id_INIT_L10] = Property(l10, 4);
    }

    // Make the packed LUT tree ci compute f (which must fit) through its first level
    void write_packed_half(CellInfo *ci, const Lut &f)
    {
        std::array<NetInfo *, 4> on;
        int l00, l01, l10;
        bool fits = fit_half(f, on, l00, l01, l10);
        NPNR_ASSERT(fits);
        for (IdString port : lut_ports(ci))
            if (ci->ports.count(port)) {
                ci->disconnectPort(port);
                ci->ports.erase(port);
            }
        const std::array<IdString, 4> first = {id_D0_00, id_D1_00, id_D0_01, id_D1_01};
        for (int i = 0; i < 4; i++) {
            ci->addInput(first[i]);
            if (on[i])
                ci->connectPort(first[i], on[i]);
        }
        ci->params[id_INIT_L00] = Property(l00, 4);
        ci->params[id_INIT_L01] = Property(l01, 4);
        ci->params[id_INIT_L10] = Property(l10, 4);
    }

    // Whether user can take g in place of its input `signal`, and the result
    bool absorbs(CellInfo *user, NetInfo *signal, const Lut &g, Lut &result) const
    {
        Lut f;
        std::array<NetInfo *, 4> on;
        int l00, l01, l10;
        if (user->type == id_CC_L2T5)
            return false;
        return function(user, f) && compose(f, signal, g, result) && fit_half(result, on, l00, l01, l10);
    }
    // An inverted input of a CC_L2T5 is a permutation of its tables
    bool invert_l2t5_input(CellInfo *ci, NetInfo *signal)
    {
        if (ci->type != id_CC_L2T5 || keep(ci))
            return false;
        const std::array<std::pair<IdString, int>, 5> where = {
                {{id_INIT_L02, 1}, {id_INIT_L02, 2}, {id_INIT_L03, 1}, {id_INIT_L03, 2}, {id_INIT_L20, 1}}};
        for (int i = 0; i < 5; i++) {
            if (ci->getPort(lut_ports(ci)[i]) != signal)
                continue;
            auto [param, flip] = where[i];
            int init = int_or_default(ci->params, param, 0), inverted = 0;
            for (int b = 0; b < 4; b++)
                inverted |= ((init >> (b ^ flip)) & 1) << b;
            ci->params[param] = Property(inverted, 4);
        }
        return true;
    }

    // The users of net, all of which could take g in place of it
    bool all_absorb(NetInfo *net, const Lut &g, std::vector<std::pair<CellInfo *, Lut>> &rewrites,
                    bool l2t5_inversion) const
    {
        rewrites.clear();
        pool<IdString> seen;
        for (auto &user : net->users) {
            if (!user.cell || seen.count(user.cell->name))
                continue;
            seen.insert(user.cell->name);
            Lut result;
            if (l2t5_inversion && user.cell->type == id_CC_L2T5 && !keep(user.cell))
                rewrites.emplace_back(user.cell, Lut());
            else if (absorbs(user.cell, net, g, result))
                rewrites.emplace_back(user.cell, result);
            else
                return false;
        }
        return !rewrites.empty();
    }
    // The rewrites all_absorb() found; a CC_L2T5 user takes `replacement` inverted in place of net
    void apply(NetInfo *net, const std::vector<std::pair<CellInfo *, Lut>> &rewrites, NetInfo *replacement)
    {
        for (auto &[cell, result] : rewrites) {
            if (cell->type != id_CC_L2T5) {
                write_half(cell, result);
                continue;
            }
            invert_l2t5_input(cell, net);
            if (replacement != net)
                for (IdString port : lut_ports(cell))
                    if (cell->getPort(port) == net) {
                        cell->disconnectPort(port);
                        cell->connectPort(port, replacement);
                    }
        }
    }

    bool inverter(CellInfo *ci, Lut &f) const
    {
        return ci->type.in(id_CC_LUT1, id_CC_LUT2, id_CC_L2T4) && function(ci, f) && f.inputs.size() == 1 &&
               f.table == 0b01;
    }

    // The rules for one inverter; true if it changed anything
    bool fold_inverter(CellInfo *inv, const Lut &f)
    {
        NetInfo *out = inv->getPort(output(inv)), *in = f.inputs.at(0);
        if (!out || out->users.empty())
            return false;
        std::vector<std::pair<CellInfo *, Lut>> rewrites;
        // Forward, into all its users (f is NOT in)
        if (all_absorb(out, f, rewrites, true)) {
            apply(out, rewrites, in);
            forward++;
            return true;
        }
        // Backward, into the cell feeding it alone
        CellInfo *driver = in->driver.cell;
        Lut df;
        if (driver && in->users.entries() == 1 && function(driver, df)) {
            Lut complement = df;
            complement.table = ~df.table & uint32_t((1ull << (1u << df.inputs.size())) - 1);
            std::array<NetInfo *, 4> on;
            int l00, l01, l10;
            bool fits = driver->type == id_CC_L2T5 || fit_half(complement, on, l00, l01, l10);
            if (fits) {
                if (driver->type == id_CC_L2T5)
                    driver->params[id_INIT_L20] = Property(~int_or_default(driver->params, id_INIT_L20, 0) & 0xf, 4);
                else
                    write_half(driver, complement);
                driver->disconnectPort(id_O);
                inv->disconnectPort(output(inv));
                driver->connectPort(id_O, out);
                backward++;
                return true;
            }
        }
        // Through the register it feeds, into all the register's users
        if (out->users.entries() == 1) {
            const PortRef &user = *out->users.begin();
            CellInfo *ff = user.cell;
            NetInfo *q = ff ? ff->getPort(id_Q) : nullptr;
            // (Not where the register's output is the inverter's input: its meaning changes)
            if (ff && ff->type == id_CC_DFF && user.port == id_D && !keep(ff) && q && q != out && q != in &&
                all_absorb(q, Lut{{q}, 0b01}, rewrites, true)) {
                apply(q, rewrites, q);
                ff->disconnectPort(id_D);
                ff->connectPort(id_D, in);
                ff->params[id_SR_VAL] = Property(!bool_or_default(ff->params, id_SR_VAL, false), 1);
                if (ff->params.count(id_INIT) && ff->params.at(id_INIT).is_fully_def())
                    ff->params[id_INIT] = Property(!bool_or_default(ff->params, id_INIT, false), 1);
                // It holds the inverse of what it held: for comparing its undefined initial value
                IdString inverted = ctx->id("FUSE_INVERTED");
                ff->attrs[inverted] = Property(!bool_or_default(ff->attrs, inverted, false), 1);
                through++;
                return true;
            }
        }
        return false;
    }

    // A LUT into all its users
    bool fuse(CellInfo *ci)
    {
        if (ci->type == id_CC_L2T5)
            return false;
        Lut f;
        NetInfo *out = ci->getPort(output(ci));
        if (!out || out->users.empty() || !function(ci, f))
            return false;
        pool<IdString> users;
        for (auto &user : out->users) {
            if (user.cell == ci)
                return false;
            users.insert(user.cell->name);
        }
        if (users.size() > 2)
            return false;
        std::vector<std::pair<CellInfo *, Lut>> rewrites;
        if (!all_absorb(out, f, rewrites, false))
            return false;
        apply(out, rewrites, out);
        fused++;
        return true;
    }
};
} // namespace

void GateMatePacker::fuse_luts()
{
    log_info("Fusing LUTs..\n");
    LutFuser fuser{ctx, net_PACKER_GND, net_PACKER_VCC};
    for (int pass = 0; pass < 10; pass++) {
        bool changed = false;
        std::vector<IdString> names;
        for (auto &cell : ctx->cells)
            names.push_back(cell.first);
        for (IdString name : names) {
            if (!ctx->cells.count(name) || !deep(name))
                continue;
            CellInfo *ci = ctx->cells.at(name).get();
            Lut f;
            if (fuser.inverter(ci, f))
                changed |= fuser.fold_inverter(ci, f);
            else
                changed |= fuser.fuse(ci);
        }
        if (!changed)
            break;
    }
    // The cells left without users go
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        NetInfo *out = fuser.lut_ports(ci).empty() ? nullptr : ci->getPort(fuser.output(ci));
        if (!fuser.lut_ports(ci).empty() && !fuser.keep(ci) && (!out || out->users.empty()))
            packed_cells.insert(ci->name);
    }
    int removed = packed_cells.size();
    flush_cells();
    log_info("    %d inverters folded forward, %d backward, %d through registers, %d LUTs fused; %d cells removed\n",
             fuser.forward, fuser.backward, fuser.through, fuser.fused, removed);
}

void GateMatePacker::fuse_l2t5_inputs()
{
    LutFuser fuser{ctx, net_PACKER_GND, net_PACKER_VCC};
    int folded = 0;
    for (auto &cell : ctx->cells) {
        CellInfo *upper = cell.second.get();
        // The upper half of a CC_L2T5: its fifth input passed through L10 to the lower half's COMBIN
        NetInfo *fifth = upper->getPort(id_D0_10), *out = upper->getPort(id_OUT);
        if (upper->type != id_CPE_L2T4 || upper->cluster == ClusterId() || !fifth || !out ||
            int_or_default(upper->params, id_INIT_L10, 0) != LUT_D0 || out->users.entries() != 1 ||
            (*out->users.begin()).port != id_COMBIN)
            continue;
        bool alone = true;
        for (auto &port : upper->ports)
            alone &= port.second.type != PORT_IN || !port.second.net || port.first == id_D0_10;
        CellInfo *driver = fifth->driver.cell;
        Lut f;
        std::array<NetInfo *, 4> on;
        int l00, l01, l10;
        if (!alone || !driver || !(deep(driver->name) || into_upper_half.count(driver->name)) ||
            driver->type != id_CPE_L2T4 || driver->cluster != ClusterId() || fifth->users.entries() != 1 ||
            !fuser.function(driver, f) || !fuser.fit_half(f, on, l00, l01, l10))
            continue;
        fuser.write_packed_half(upper, f);
        packed_cells.insert(driver->name);
        folded++;
    }
    flush_cells();
    log_info("    %d 5-input LUTs took the LUT feeding their fifth input into their upper half\n", folded);
}

NEXTPNR_NAMESPACE_END

NEXTPNR_NAMESPACE_BEGIN

// Register enables into the register's own LUT (--vopt enable-to-lut=N). TEMPORARY until measured
// (docs/timing_experiments.md).
//
// A register enable reaches a CPE only on its enable plane, and a write enable feeding many
// registers is hard to route fast there. Where a register's half would only pass its data through
// its LUT (the data comes from something the packer cannot merge with it), that LUT can take the
// enable instead: D' = EN ? D : Q, with the enable tied high. The enable then arrives on the LUT
// inputs' planes, for the LUT's delay and a connection from the register's output back into its
// own CPE. Set/reset keep their priority. For enables feeding at least min_fanout registers.
void GateMatePacker::enables_into_luts(int min_fanout)
{
    auto io = [&](const CellInfo *ci) {
        return ci && ci->type.in(id_CC_IBUF, id_CC_OBUF, id_CC_TOBUF, id_CC_IOBUF, id_CC_LVDS_IBUF, id_CC_LVDS_OBUF,
                                 id_CC_LVDS_TOBUF, id_CC_LVDS_IOBUF, id_CC_IDDR, id_CC_ODDR);
    };
    std::vector<CellInfo *> registers;
    for (auto &cell : ctx->cells)
        if (cell.second->type == id_CC_DFF && !cell.second->attrs.count(ctx->id("keep")))
            registers.push_back(cell.second.get());
    int moved = 0;
    for (CellInfo *ff : registers) {
        NetInfo *en = ff->getPort(id_EN), *d = ff->getPort(id_D), *q = ff->getPort(id_Q);
        if (!en || en == net_PACKER_GND || en == net_PACKER_VCC || !d || !q || int(en->users.entries()) < min_fanout)
            continue;
        // A LUT feeding only the register merges into its half (pack_cpe); IO registers go into the IO
        CellInfo *driver = d->driver.cell;
        bool merges = driver && driver->type.in(id_CC_LUT1, id_CC_LUT2, id_CC_L2T4, id_CC_L2T5, id_CC_MX2) &&
                      d->users.entries() == 1;
        bool io_register = io(driver);
        for (auto &user : q->users)
            io_register |= io(user.cell);
        // With rewrite-depth, only enables at the end of deep paths
        CellInfo *enable = en->driver.cell;
        if (merges || io_register || (only_deep && (!enable || !deep(enable->name))))
            continue;
        bool inverted = bool_or_default(ff->params, id_EN_INV, false);
        CellInfo *mux = ctx->createCell(ctx->idf("%s$enable", ff->name.c_str(ctx)), id_CC_MX2);
        for (IdString port : {id_D0, id_D1, id_S0})
            mux->addInput(port);
        mux->addOutput(id_Y);
        mux->connectPort(id_D0, inverted ? d : q);
        mux->connectPort(id_D1, inverted ? q : d);
        mux->connectPort(id_S0, en);
        NetInfo *data = ctx->createNet(ctx->idf("%s$enabled", ff->name.c_str(ctx)));
        mux->connectPort(id_Y, data);
        ff->disconnectPort(id_D);
        ff->connectPort(id_D, data);
        ff->disconnectPort(id_EN);
        ff->connectPort(id_EN, net_PACKER_VCC);
        ff->params[id_EN_INV] = Property(0, 1);
        moved++;
    }
    log_info("Moved %d register enables into their LUT.\n", moved);
}

NEXTPNR_NAMESPACE_END

NEXTPNR_NAMESPACE_BEGIN

// Chains of two 2:1 multiplexers into one 4:1 multiplexer (--vopt mux-chains). TEMPORARY until
// measured (docs/timing_experiments.md).
//
// A 2:1 multiplexer feeding only a data input of another is half of a 4:1 multiplexer
// S1 ? (S0 ? D3 : D2) : (S0 ? D1 : D0): with the outer select as S1 and the inner one's as S0, the
// inner data on one side and the outer's other data on both inputs of the other side. The 4:1
// multiplexer takes a whole CPE, as the two halves did, but one level and one routing hop less.
void GateMatePacker::merge_mux_chains()
{
    std::vector<IdString> names;
    for (auto &cell : ctx->cells)
        if (cell.second->type == id_CC_MX2)
            names.push_back(cell.first);
    auto keep = [&](const CellInfo *ci) { return ci->attrs.count(ctx->id("keep")) > 0; };
    int merged = 0;
    for (IdString name : names) {
        if (packed_cells.count(name))
            continue;
        CellInfo *outer = ctx->cells.at(name).get();
        if (outer->type != id_CC_MX2 || keep(outer))
            continue;
        for (IdString side : {id_D0, id_D1}) {
            NetInfo *chained = outer->getPort(side);
            CellInfo *inner = chained ? chained->driver.cell : nullptr;
            if (!inner || inner == outer || inner->type != id_CC_MX2 || keep(inner) ||
                (!deep(outer->name) && !deep(inner->name)) || chained->users.entries() != 1 ||
                packed_cells.count(inner->name))
                continue;
            NetInfo *a0 = inner->getPort(id_D0), *a1 = inner->getPort(id_D1), *sa = inner->getPort(id_S0);
            NetInfo *other = outer->getPort(side == id_D0 ? id_D1 : id_D0), *sb = outer->getPort(id_S0);
            if (!a0 || !a1 || !sa || !other || !sb)
                continue;
            for (IdString port : {id_D0, id_D1, id_S0}) {
                outer->disconnectPort(port);
                outer->ports.erase(port);
            }
            outer->type = id_CC_MX4;
            for (IdString port : {id_D0, id_D1, id_D2, id_D3, id_S0, id_S1})
                outer->addInput(port);
            outer->connectPort(id_S1, sb);
            outer->connectPort(id_S0, sa);
            bool low = side == id_D0; // the inner multiplexer where S1 is 0
            outer->connectPort(id_D0, low ? a0 : other);
            outer->connectPort(id_D1, low ? a1 : other);
            outer->connectPort(id_D2, low ? other : a0);
            outer->connectPort(id_D3, low ? other : a1);
            for (auto &port : inner->ports)
                inner->disconnectPort(port.first);
            packed_cells.insert(inner->name);
            merged++;
            break;
        }
    }
    flush_cells();
    log_info("Merged %d chains of 2:1 multiplexers into 4:1 multiplexers.\n", merged);
}

NEXTPNR_NAMESPACE_END

NEXTPNR_NAMESPACE_BEGIN

// Restricting the rewrites above to deep paths (--vopt rewrite-depth=F). TEMPORARY until measured
// (docs/timing_experiments.md).
//
// Before placement there is no timing, but the rewrites save logic levels, and a level costs a LUT
// and a routing hop. So: the logic levels of the longest path through each cell between registers,
// RAMs and pins, and the cells on paths at least F times as deep as the deepest. A LUT or
// multiplexer counts as a level, an adder as a quarter (carry chains are fast).
void GateMatePacker::find_deep_cells(float fraction)
{
    auto weight = [&](const CellInfo *ci) -> float {
        if (ci->type.in(id_CC_LUT1, id_CC_LUT2, id_CC_L2T4, id_CC_L2T5, id_CC_MX2, id_CC_MX4))
            return 1;
        if (ci->type == id_CC_ADDF)
            return 0.25f;
        return -1; // where paths start and end
    };
    dict<IdString, float> arrival, departure;
    // Levels up to and including the cell, and after it
    std::function<float(CellInfo *)> arrive = [&](CellInfo *ci) -> float {
        if (arrival.count(ci->name))
            return arrival.at(ci->name);
        arrival[ci->name] = 0; // against combinational loops
        float most = 0;
        for (auto &port : ci->ports) {
            NetInfo *net = port.second.net;
            if (port.second.type == PORT_IN && net && net->driver.cell && weight(net->driver.cell) >= 0)
                most = std::max(most, arrive(net->driver.cell));
        }
        return arrival[ci->name] = most + weight(ci);
    };
    std::function<float(CellInfo *)> depart = [&](CellInfo *ci) -> float {
        if (departure.count(ci->name))
            return departure.at(ci->name);
        departure[ci->name] = 0;
        float most = 0;
        for (auto &port : ci->ports) {
            NetInfo *net = port.second.net;
            if (port.second.type != PORT_OUT || !net)
                continue;
            for (auto &user : net->users)
                if (user.cell && weight(user.cell) >= 0)
                    most = std::max(most, depart(user.cell) + weight(user.cell));
        }
        return departure[ci->name] = most;
    };
    dict<IdString, float> through;
    float deepest = 0;
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (weight(ci) < 0)
            continue;
        float levels = arrive(ci) + depart(ci);
        through[ci->name] = levels;
        deepest = std::max(deepest, levels);
    }
    deep_cells.clear();
    for (auto &entry : through)
        if (entry.second >= fraction * deepest)
            deep_cells.insert(entry.first);
    only_deep = true;
    log_info("Rewriting only on deep paths: %d of %d logic cells on paths of at least %.1f of %.1f levels.\n",
             int(deep_cells.size()), int(through.size()), fraction * deepest, deepest);
}

NEXTPNR_NAMESPACE_END

NEXTPNR_NAMESPACE_BEGIN

// Restricting the rewrites to cells that were critical in an earlier run (--vopt
// rewrite-crit-in=file, rewrite-crit-min=F): the cells driving a net at least F critical after
// routing, as --vopt place-crit-out writes it. The critical nets are mostly the same from run to
// run, and net names survive packing. TEMPORARY until measured (docs/timing_experiments.md).
void GateMatePacker::find_critical_cells(const std::string &filename, float min_crit)
{
    std::ifstream in(filename);
    if (!in)
        log_error("Cannot read '%s'.\n", filename.c_str());
    pool<IdString> critical_nets;
    std::string name;
    float crit;
    while (in >> name >> crit)
        if (crit >= min_crit)
            critical_nets.insert(ctx->id(name));
    deep_cells.clear();
    for (auto &cell : ctx->cells)
        for (auto &port : cell.second->ports)
            if (port.second.type == PORT_OUT && port.second.net && critical_nets.count(port.second.net->name))
                deep_cells.insert(cell.first);
    only_deep = true;
    log_info("Rewriting only cells driving the %d nets at least %.2f critical in '%s': %d cells.\n",
             int(critical_nets.size()), min_crit, filename.c_str(), int(deep_cells.size()));
}

NEXTPNR_NAMESPACE_END

NEXTPNR_NAMESPACE_BEGIN

// Two levels of LUTs in one CPE (--vopt cpe-merge=N). TEMPORARY until measured
// (docs/timing_experiments.md).
//
// A LUT tree fills half a CPE, but a whole CPE has a third level, L20, combining the upper half's
// tree with the lower half's. Where a LUT B fed by a LUT A depends on its other inputs only through
// one function of them, B = G(A, V(others)), the pair fits one CPE: A in the upper half, V in the
// lower one, G in L20. That saves a LUT delay and a routing hop. B becomes a CC_L2T5 with A on its
// fifth input (I4), which fuse_l2t5_inputs() then moves into the upper half after packing. With
// N = 1 only where A feeds nothing else (two halves become one CPE); with N = 2 also with a copy of A
// (half a CPE more), for the cells marked by rewrite-crit-in / rewrite-depth if given.
void GateMatePacker::merge_into_cpes(int level)
{
    LutFuser fuser{ctx, net_PACKER_GND, net_PACKER_VCC};
    pool<IdString> uppers; // the A of a merge, to stay a LUT tree
    std::vector<IdString> names;
    for (auto &cell : ctx->cells)
        names.push_back(cell.first);
    int merged = 0, copied = 0;
    for (IdString name : names) {
        CellInfo *b = ctx->cells.at(name).get();
        Lut fb;
        if (uppers.count(name) || !b->type.in(id_CC_LUT1, id_CC_LUT2, id_CC_L2T4, id_CC_MX2) || !fuser.function(b, fb))
            continue;
        for (NetInfo *signal : fb.inputs) {
            CellInfo *a = signal->driver.cell;
            Lut fa;
            if (!a || a == b || !a->type.in(id_CC_LUT1, id_CC_LUT2, id_CC_L2T4, id_CC_MX2) || !fuser.function(a, fa))
                continue;
            bool alone = signal->users.entries() == 1;
            if (!alone && (level < 2 || !deep(name)))
                continue;
            // B = G(A, V(rest)): the pairs (B at A = 0, B at A = 1) over the rest take at most two values
            int k = std::find(fb.inputs.begin(), fb.inputs.end(), signal) - fb.inputs.begin();
            Lut v;
            for (NetInfo *net : fb.inputs)
                if (net != signal)
                    v.inputs.push_back(net);
            std::vector<int> classes; // of (B at A = 0) | (B at A = 1) << 1
            bool splits = true;
            for (uint32_t r = 0; r < (1u << v.inputs.size()) && splits; r++) {
                uint32_t low = (r & ((1u << k) - 1)) | ((r >> k) << (k + 1)); // A's bit 0
                int pair = int(fb.value(low)) | (int(fb.value(low | (1u << k))) << 1);
                auto found = std::find(classes.begin(), classes.end(), pair);
                if (found == classes.end()) {
                    if (classes.size() == 2)
                        splits = false;
                    classes.push_back(pair);
                    found = classes.end() - 1;
                }
                v.table |= uint32_t(found - classes.begin()) << r;
            }
            std::array<NetInfo *, 4> on;
            int l00, l01, l10;
            if (!splits || classes.size() < 2 || !fuser.fit_half(v, on, l00, l01, l10) ||
                !fuser.fit_half(fa, on, l00, l01, l10))
                continue;
            fuser.fit_half(v, on, l00, l01, l10);
            // L20 = G(I4 = A, L11 = V)
            int l20 = 0;
            for (int cls = 0; cls < 2; cls++)
                for (int x = 0; x < 2; x++)
                    l20 |= ((classes[cls] >> x) & 1) << ((cls << 1) | x);
            NetInfo *fifth = signal;
            if (!alone) {
                // A copy of A for B alone
                CellInfo *copy = ctx->createCell(ctx->idf("%s$cpe_copy%d", a->name.c_str(ctx), copied), a->type);
                copy->params = a->params;
                copy->attrs = a->attrs;
                for (auto &port : a->ports) {
                    if (port.second.type == PORT_IN) {
                        copy->addInput(port.first);
                        if (port.second.net)
                            copy->connectPort(port.first, port.second.net);
                    } else {
                        copy->addOutput(port.first);
                    }
                }
                fifth = ctx->createNet(ctx->idf("%s$cpe_copy%d", signal->name.c_str(ctx), copied));
                copy->connectPort(fuser.output(a), fifth);
                a = copy;
                copied++;
            }
            IdString out = fuser.output(b);
            for (IdString port : fuser.lut_ports(b)) {
                b->disconnectPort(port);
                b->ports.erase(port);
            }
            if (out != id_O)
                b->renamePort(out, id_O);
            for (IdString param : {id_INIT, id_INIT_L00, id_INIT_L01, id_INIT_L10})
                b->params.erase(param);
            b->type = id_CC_L2T5;
            const std::array<IdString, 4> first = {id_I0, id_I1, id_I2, id_I3};
            for (int i = 0; i < 4; i++) {
                b->addInput(first[i]);
                if (on[i])
                    b->connectPort(first[i], on[i]);
            }
            b->addInput(id_I4);
            b->connectPort(id_I4, fifth);
            b->params[id_INIT_L02] = Property(l00, 4);
            b->params[id_INIT_L03] = Property(l01, 4);
            b->params[id_INIT_L11] = Property(l10, 4);
            b->params[id_INIT_L20] = Property(l20, 4);
            uppers.insert(a->name);
            into_upper_half.insert(a->name);
            merged++;
            break;
        }
    }
    log_info("Merged %d pairs of LUT levels into one CPE each (%d with a copy of the first).\n", merged, copied);
}

NEXTPNR_NAMESPACE_END
