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

// Comparing the cells of a design with a snapshot of them, as the logic check holds it (GateMateImpl::evaluateCell,
// logic_eval.cc): the same outputs and sinks for every assignment of the input nets.

#ifndef GATEMATE_CELL_COMPARE_H
#define GATEMATE_CELL_COMPARE_H

#include <algorithm>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>
#include "nextpnr.h"
#include "log.h"
#include "util.h"
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {

using Word = uint64_t;
constexpr Word ALL = ~Word(0);

// Inputs given per port (a port not given is unconnected)
struct PortInputs : CellEvalInputs
{
    dict<IdString, Word> values;
    Word value(IdString port, bool *connected = nullptr) const override
    {
        auto found = values.find(port);
        if (connected)
            *connected = found != values.end();
        return found == values.end() ? 0 : found->second;
    }
};

// Inputs given per net name, with the net name of each connected port
struct NetInputs : CellEvalInputs
{
    NetInputs(const dict<IdString, IdString> &port_nets, const dict<IdString, Word> &values)
            : port_nets(port_nets), values(values)
    {
    }
    const dict<IdString, IdString> &port_nets;
    const dict<IdString, Word> &values;
    Word value(IdString port, bool *connected = nullptr) const override
    {
        auto found = port_nets.find(port);
        if (connected)
            *connected = found != port_nets.end();
        if (found == port_nets.end())
            return 0;
        auto v = values.find(found->second);
        return v == values.end() ? 0 : v->second;
    }
};

// A cell as the logic check's snapshot holds it: type, parameters, attributes and ports without nets, and the net
// name of each connected port
struct CellCopy
{
    std::unique_ptr<CellInfo> info;
    dict<IdString, IdString> port_nets;
};

struct Snapshot
{
    std::map<std::string, CellCopy> cells;
    pool<IdString> nets;
};

Snapshot take_snapshot(Context *ctx)
{
    Snapshot snap;
    for (auto &cell : ctx->cells) {
        CellCopy copy;
        copy.info = std::make_unique<CellInfo>(ctx, cell.first, cell.second->type);
        copy.info->params = cell.second->params;
        copy.info->attrs = cell.second->attrs;
        for (auto &port : cell.second->ports) {
            PortInfo info;
            info.name = port.first;
            info.net = nullptr;
            info.type = port.second.type;
            copy.info->ports[port.first] = info;
            if (port.second.net)
                copy.port_nets[port.first] = port.second.net->name;
        }
        snap.cells.emplace(cell.first.str(ctx), std::move(copy));
    }
    for (auto &net : ctx->nets)
        snap.nets.insert(net.first);
    return snap;
}

std::string params_str(Context *ctx, const CellInfo *cell)
{
    std::string s;
    for (auto &p : cell->params)
        s += stringf(" %s=%s", p.first.c_str(ctx), p.second.to_string().c_str());
    return s;
}

std::string ports_str(Context *ctx, const dict<IdString, IdString> &port_nets)
{
    std::string s;
    for (auto &p : port_nets)
        s += stringf(" %s:%s", p.first.c_str(ctx), p.second.c_str(ctx));
    return s;
}

struct CompareStats
{
    std::map<std::string, int> compared;     // per cell type
    std::map<std::string, int> other_inputs; // cells whose input nets changed
    int unmodelled = 0;
    std::vector<std::string> failures;
    // Cells that have fewer ports on a net than the net has users at them: two inputs renamed to the same port
    std::vector<std::string> lost_ports;
    // Of the cells compared: those taking an input from a carry line (C_I1 .. C_I4), a register control from one
    // (C_CLKSEL / C_ENSEL), and an input through a bridge or CP lines made after the snapshot
    int carry_line_inputs = 0, carry_line_controls = 0, followed = 0;

    int count(const std::string &type) const { return compared.count(type) ? compared.at(type) : 0; }
    std::string summary() const
    {
        std::string s = "compared:";
        for (auto &c : compared)
            s += stringf(" %s %d", c.first.c_str(), c.second);
        s += "; other input nets:";
        for (auto &c : other_inputs)
            s += stringf(" %s %d", c.first.c_str(), c.second);
        s += stringf("; unmodelled %d; carry line inputs %d, controls %d; through new bridges / CP lines %d; lost "
                     "ports %d",
                     unmodelled, carry_line_inputs, carry_line_controls, followed, int(lost_ports.size()));
        for (auto &l : lost_ports)
            s += "\n    lost ports: " + l;
        return s;
    }
};

// Compares the cells of a snapshot with the cells of the same name in the design now: for every assignment of the
// input nets (exhaustive up to 12 nets, else random), the same outputs and sinks. Nets the routing made after the
// snapshot through bridges and CP lines (cells that pass one of their inputs on) are followed back to the net they
// pass on. Cells whose (followed) input nets changed are only counted.
struct Comparer
{
    Comparer(Context *ctx, const Snapshot &snap) : ctx(ctx), snap(snap) {}
    Context *ctx;
    const Snapshot &snap;
    std::mt19937_64 rng{1};
    CompareStats stats;
    bool same_type_only = false;

    // The net a bridge or CP lines cell made after the snapshot passes on to `net` (nullptr if that cannot be told)
    const NetInfo *source(const NetInfo *net)
    {
        for (int depth = 0; depth < 64; depth++) {
            if (!net || snap.nets.count(net->name) || !net->driver.cell ||
                !net->driver.cell->type.in(id_CPE_BRIDGE, id_CPE_CPLINES))
                return net;
            const CellInfo *drv = net->driver.cell;
            // Random values on its inputs: the output is one of them
            PortInputs in;
            for (auto &port : drv->ports)
                if (port.second.type == PORT_IN && port.second.net)
                    in.values[port.first] = rng();
            CellEvalResult r;
            if (!ctx->evaluateCell(drv, in, r) || !r.outputs.count(net->driver.port))
                return nullptr;
            Word v = r.outputs.at(net->driver.port);
            const NetInfo *from = nullptr;
            int found = 0;
            for (auto &kv : in.values)
                if (kv.second == v) {
                    from = drv->getPort(kv.first);
                    found++;
                }
            if (found != 1)
                return nullptr;
            net = from;
        }
        return nullptr;
    }

    void compare(const std::string &name, const CellCopy &old, const CellInfo *now)
    {
        std::string type = old.info->type.str(ctx);
        if (same_type_only && old.info->type != now->type)
            return;
        // Inputs renamed onto each other (the net keeps a user for each, the cell has one port)
        for (auto &port : now->ports) {
            const NetInfo *net = port.second.net;
            if (!net || port.second.type == PORT_OUT)
                continue;
            int users = 0, ports = 0;
            for (auto &user : net->users)
                users += user.cell == now;
            for (auto &p : now->ports)
                ports += p.second.net == net && p.second.type != PORT_OUT;
            if (users > ports) {
                // (finishRoute did this when two inputs reached the same CPE input: a net user without a port)
                stats.lost_ports.push_back(stringf("%s (%s):%s |%s", name.c_str(), now->type.c_str(ctx),
                                                   ports_str(ctx, old.port_nets).c_str(),
                                                   params_str(ctx, now).c_str()));
                stats.failures.push_back(name + ": the net " + net->name.str(ctx) +
                                         " has more users at the cell than it has ports on it");
                return;
            }
        }
        // The input nets of the cell now, followed back through new pass-through cells
        dict<IdString, IdString> now_nets;
        for (auto &port : now->ports) {
            if (!port.second.net)
                continue;
            if (port.second.type == PORT_OUT) {
                now_nets[port.first] = port.second.net->name;
                continue;
            }
            const NetInfo *src = source(port.second.net);
            if (!src) {
                stats.other_inputs[type]++;
                return;
            }
            now_nets[port.first] = src->name;
        }
        std::set<IdString> old_in, now_in;
        for (auto &p : old.port_nets)
            if (old.info->ports.at(p.first).type != PORT_OUT)
                old_in.insert(p.second);
        for (auto &p : now_nets)
            if (now->ports.at(p.first).type != PORT_OUT)
                now_in.insert(p.second);
        CellEvalResult probe_old, probe_now;
        dict<IdString, Word> none;
        bool known_old = ctx->evaluateCell(old.info.get(), NetInputs(old.port_nets, none), probe_old);
        bool known_now = ctx->evaluateCell(now, NetInputs(now_nets, none), probe_now);
        if (!known_old && !known_now) {
            stats.unmodelled++;
            return;
        }
        if (old_in != now_in) {
            stats.other_inputs[type]++;
            return;
        }
        auto fail = [&](const std::string &what) {
            stats.failures.push_back(stringf("%s (%s -> %s): %s\n    before:%s |%s\n    now:   %s |%s", name.c_str(),
                                             type.c_str(), now->type.c_str(ctx), what.c_str(),
                                             params_str(ctx, old.info.get()).c_str(),
                                             ports_str(ctx, old.port_nets).c_str(), params_str(ctx, now).c_str(),
                                             ports_str(ctx, now_nets).c_str()));
        };
        if (known_old != known_now) {
            fail(stringf("modelled %s before, %s now", known_old ? "yes" : "no", known_now ? "yes" : "no"));
            return;
        }
        // An output drives the same net under the same port name
        for (auto &out : probe_old.outputs) {
            auto o = old.port_nets.find(out.first);
            auto n = now_nets.find(out.first);
            if ((o == old.port_nets.end()) != (n == now_nets.end()) ||
                (o != old.port_nets.end() && o->second != n->second))
                fail("output " + out.first.str(ctx) + " drives another net");
        }
        std::vector<IdString> nets(old_in.begin(), old_in.end());
        int k = int(nets.size());
        int words = k <= 12 ? std::max(1, (1 << k) / 64) : 64;
        Word valid = k < 6 ? (Word(1) << (1 << k)) - 1 : ALL;
        for (int w = 0; w < words; w++) {
            dict<IdString, Word> values;
            for (int j = 0; j < k; j++) {
                Word v = 0;
                if (k <= 12) {
                    for (int b = 0; b < 64; b++)
                        if (((w * 64 + b) >> j) & 1)
                            v |= Word(1) << b;
                } else {
                    v = rng();
                }
                values[nets[j]] = v;
            }
            CellEvalResult r_old, r_now;
            ctx->evaluateCell(old.info.get(), NetInputs(old.port_nets, values), r_old);
            ctx->evaluateCell(now, NetInputs(now_nets, values), r_now);
            auto same = [&](const dict<IdString, Word> &a, const dict<IdString, Word> &b, const char *what) {
                for (auto &kv : a) {
                    if (!b.count(kv.first)) {
                        fail(stringf("%s %s is gone", what, kv.first.c_str(ctx)));
                        return false;
                    }
                    if ((kv.second ^ b.at(kv.first)) & valid) {
                        fail(stringf("%s %s differs: %016llx before, %016llx now", what, kv.first.c_str(ctx),
                                     (unsigned long long)(kv.second & valid),
                                     (unsigned long long)(b.at(kv.first) & valid)));
                        return false;
                    }
                }
                for (auto &kv : b)
                    if (!a.count(kv.first)) {
                        fail(stringf("%s %s is new", what, kv.first.c_str(ctx)));
                        return false;
                    }
                return true;
            };
            if (!same(r_old.outputs, r_now.outputs, "output") || !same(r_old.sinks, r_now.sinks, "sink"))
                return;
        }
        stats.compared[type]++;
        auto set = [&](IdString p) { return int_or_default(now->params, p, 0) != 0; };
        if (set(id_C_I1) || set(id_C_I2) || set(id_C_I3) || set(id_C_I4))
            stats.carry_line_inputs++;
        if (set(id_C_CLKSEL) || set(id_C_ENSEL))
            stats.carry_line_controls++;
        for (auto &port : now->ports)
            if (port.second.type != PORT_OUT && port.second.net && now_nets.at(port.first) != port.second.net->name) {
                stats.followed++;
                break;
            }
    }

    void run()
    {
        for (auto &cell : snap.cells) {
            auto found = ctx->cells.find(ctx->id(cell.first));
            if (found == ctx->cells.end())
                continue;
            compare(cell.first, cell.second, found->second.get());
        }
    }
};

std::string failures_str(const CompareStats &stats)
{
    std::string s;
    for (size_t i = 0; i < stats.failures.size() && i < 10; i++)
        s += "\n  " + stats.failures[i];
    if (stats.failures.size() > 10)
        s += stringf("\n  ... and %d more", int(stats.failures.size()) - 10);
    return s;
}

} // namespace

NEXTPNR_NAMESPACE_END

#endif
