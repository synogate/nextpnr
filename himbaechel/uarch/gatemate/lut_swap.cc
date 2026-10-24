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
#include <limits>
#include <set>

#include "gatemate.h"
#include "pack.h"
#include "router2.h"
#include "timing.h"

#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

NEXTPNR_NAMESPACE_BEGIN

// The order of the inputs of the LUT trees (--vopt lut-input-swap, lut-swap-rounds).
//
// A CPE half computes L10(L00(D0_00, D1_00), L01(D0_01, D1_01)). The router can feed either input
// of a first-level LUT2 from either of its two planes (IN1 or IN2 for L00, through TI2), but the
// pairs stay on planes 1-2 and 3-4 (5-6 and 7-8 in the lower half). Swapping the two subtrees,
// together with the inputs of L10, keeps the function and moves each pair to the other two planes.
// A CPE output reaches only two of planes 1 to 8 directly, the rest only through a plane change in
// a switch box's diagonal mux or an input mux, so the order matters for the delay up to the LUT's
// output: the routing plus the LUT's own delay from that input.

namespace {
const std::array<IdString, 4> tree_pins = {id_D0_00, id_D1_00, id_D0_01, id_D1_01};

// A placed LUT tree fed only through its first level: no input straight to L10, no carry or
// COMBIN input
bool plain_tree(const CellInfo *ci)
{
    if (ci->type != id_CPE_L2T4 || ci->bel == BelId())
        return false;
    bool used = false;
    for (auto &port : ci->ports) {
        if (port.second.type != PORT_IN || !port.second.net)
            continue;
        if (std::find(tree_pins.begin(), tree_pins.end(), port.first) == tree_pins.end())
            return false;
        used = true;
    }
    return used;
}

// Swap the subtrees; doing it twice restores the cell
void swap_tree(CellInfo *ci)
{
    std::array<NetInfo *, 4> nets;
    for (int i = 0; i < 4; i++) {
        nets[i] = ci->getPort(tree_pins[i]);
        if (nets[i])
            ci->disconnectPort(tree_pins[i]);
    }
    for (int i = 0; i < 4; i++) {
        if (!nets[i])
            continue;
        if (!ci->ports.count(tree_pins[i ^ 2])) {
            ci->addInput(tree_pins[i ^ 2]);
            ci->cell_bel_pins[tree_pins[i ^ 2]] = {tree_pins[i ^ 2]};
        }
        ci->connectPort(tree_pins[i ^ 2], nets[i]);
    }
    int l00 = int_or_default(ci->params, id_INIT_L00, 0), l01 = int_or_default(ci->params, id_INIT_L01, 0);
    int l10 = int_or_default(ci->params, id_INIT_L10, 0);
    ci->params[id_INIT_L00] = Property(l01, 4);
    ci->params[id_INIT_L01] = Property(l00, 4);
    // L10's address is {L01, L00}: swap its two address bits
    ci->params[id_INIT_L10] = Property((l10 & 0b1001) | ((l10 & 0b0010) << 1) | ((l10 & 0b0100) >> 1), 4);
}
} // namespace

std::pair<float, float> GateMateImpl::lut_tree_costs(CellInfo *ci, const TimingAnalyser &tmg, bool routed) const
{
    float cost[2] = {0, 0};
    for (int i = 0; i < 4; i++) {
        NetInfo *net = ci->getPort(tree_pins[i]);
        if (!net || !net->driver.cell || net->driver.cell->bel == BelId())
            continue;
        WireId src = ctx->getNetinfoSourceWire(net);
        delay_t delay[2];
        for (int s = 0; s < 2; s++) {
            IdString pin = tree_pins[s ? i ^ 2 : i];
            WireId dst = ctx->getBelPinWire(ci->bel, pin);
            if (routed && s == 0)
                delay[s] = ctx->getNetinfoRouteDelay(net, net->users.at(ci->ports.at(pin).user_idx));
            else
                delay[s] = src == WireId() || dst == WireId() ? -1 : lookahead.estimate(src, dst);
            DelayQuad lut;
            if (delay[s] >= 0 && getCellDelay(ci, pin, id_OUT, lut))
                delay[s] += lut.maxDelay();
        }
        if (delay[0] < 0 || delay[1] < 0)
            continue;
        float crit = tmg.get_criticality(CellPortKey(ci->name, tree_pins[i]));
        float weight = 1 + 9 * crit * crit;
        for (int s = 0; s < 2; s++)
            cost[s] += weight * delay[s];
    }
    return {cost[0], cost[1]};
}

int GateMateImpl::swap_lut_inputs(bool all)
{
    TimingAnalyser tmg(ctx);
    if (!all) {
        tmg.setup_only = true;
        tmg.setup();
        tmg.run();
    }
    int eligible = 0, swapped = 0;
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (!plain_tree(ci))
            continue;
        eligible++;
        if (!all) {
            auto costs = lut_tree_costs(ci, tmg, false);
            if (costs.second >= costs.first)
                continue;
        }
        swap_tree(ci);
        swapped++;
    }
    log_info("Swapped the input pairs of %d of %d LUT trees.\n", swapped, eligible);
    return swapped;
}

// After routing: on the most critical paths, take the moves that look faster (the tables against the
// real routing), make them and re-route what they touch. Keep them if Fmax improved; otherwise restore
// the cells and the routing, and try each half of them on its own. Each round re-routes once. The
// moves: swapping a LUT tree's input pairs, and swapping a CPE's halves (upper LUT tree and register
// for the lower ones), which also moves the outputs between OUT1 and OUT2, with their different
// planes. (`test_undo`: try any moves and undo every round, to test the undoing.)
int GateMateImpl::improve_lut_inputs(int rounds, bool test_undo)
{
    const int batch = 16;
    const float min_crit = 0.9f, min_gain = 20; // ps per unit of weight

    Router2Cfg cfg(ctx);
    configureRouter2(cfg);
    cfg.max_iterations = 100;
    // Fmax of the slowest clock, as timing_analysis reports it
    auto fmax = [&]() {
        TimingAnalyser tmg(ctx);
        tmg.with_clock_skew = true;
        tmg.setup(false, false, true);
        float worst = std::numeric_limits<float>::infinity();
        for (auto &clock : tmg.get_timing_result().clock_fmax)
            worst = std::min(worst, clock.second.achieved);
        return worst;
    };

    // --- Ripping up ---
    // Unbind wire and everything net reaches through it, and the wires leading only there
    auto rip_up_from = [&](NetInfo *net, WireId wire) {
        dict<WireId, std::vector<WireId>> children;
        for (auto &w : net->wires)
            if (w.second.pip != PipId())
                children[ctx->getPipSrcWire(w.second.pip)].push_back(w.first);
        PipId uphill = net->wires.at(wire).pip;
        std::vector<WireId> todo = {wire};
        while (!todo.empty()) {
            WireId w = todo.back();
            todo.pop_back();
            for (WireId child : children[w])
                todo.push_back(child);
            ctx->unbindWire(w);
        }
        while (uphill != PipId()) {
            WireId up = ctx->getPipSrcWire(uphill);
            int left = 0;
            for (WireId child : children[up])
                left += net->wires.count(child);
            if (left > 0 || net->wires.at(up).strength > STRENGTH_WEAK)
                break;
            uphill = net->wires.at(up).pip;
            if (uphill != PipId())
                ctx->unbindWire(up);
        }
    };
    // Free the CPE wires a move may need: those up to two pips before (`uphill`) or after the given
    // wires, in the same tile. Other nets can pass through a CPE (IN1 -> MUXOUT -> OUT1, say).
    auto rip_up_near = [&](const std::vector<WireId> &wires, bool uphill) {
        pool<WireId> near;
        std::vector<std::pair<WireId, int>> todo;
        for (WireId w : wires)
            todo.emplace_back(w, 0);
        while (!todo.empty()) {
            auto [w, depth] = todo.back();
            todo.pop_back();
            if (!near.insert(w).second || depth == 2)
                continue;
            if (uphill) {
                for (PipId pip : ctx->getPipsUphill(w))
                    if (ctx->getPipSrcWire(pip).tile == w.tile)
                        todo.emplace_back(ctx->getPipSrcWire(pip), depth + 1);
            } else {
                for (PipId pip : ctx->getPipsDownhill(w))
                    if (ctx->getPipDstWire(pip).tile == w.tile)
                        todo.emplace_back(ctx->getPipDstWire(pip), depth + 1);
            }
        }
        for (WireId w : near) {
            NetInfo *net = ctx->getBoundWireNet(w);
            if (net && net->wires.count(w) && net->wires.at(w).strength <= STRENGTH_WEAK)
                rip_up_from(net, w);
        }
    };
    auto weakly_routed = [&](NetInfo *net) {
        for (auto &w : net->wires)
            if (w.second.strength > STRENGTH_WEAK)
                return false;
        return true;
    };

    // --- The moves ---
    struct Move
    {
        CellInfo *tree = nullptr; // swap the input pairs of this LUT tree
        int tile = -1;            // or swap the halves of the CPE in this tile
    };
    auto tile_of = [](const Move &m) { return m.tree ? m.tree->bel.tile : m.tile; };
    auto cpe_bel = [&](int tile, int z) {
        int x, y;
        tile_xy(ctx->chip_info, tile, x, y);
        return ctx->getBelByLocation(Loc(x, y, z));
    };
    auto cpe_cell = [&](int tile, int z) {
        BelId bel = cpe_bel(tile, z);
        return bel == BelId() ? nullptr : ctx->getBoundBelCell(bel);
    };
    static const std::array<int, 4> half_z = {CPE_LT_U_Z, CPE_LT_L_Z, CPE_FF_U_Z, CPE_FF_L_Z};
    // Only plain LUT trees and the registers they feed, nothing else in the CPE, no fixed routing
    auto halves_swappable = [&](int tile) {
        for (int z : {CPE_RAMIO_U_Z, CPE_RAMIO_L_Z, CPE_COMP_Z, CPE_CPLINES_Z, CPE_LT_FULL_Z, CPE_BRIDGE_Z})
            if (cpe_cell(tile, z))
                return false;
        bool tree = false;
        for (int z : half_z) {
            CellInfo *ci = cpe_cell(tile, z);
            if (!ci)
                continue;
            if (ci->constr_abs_z)
                return false;
            if (z == CPE_LT_U_Z || z == CPE_LT_L_Z) {
                if (!plain_tree(ci))
                    return false;
                if (z == CPE_LT_L_Z && int_or_default(ci->params, id_INIT_L20, LUT_D1) != LUT_D1)
                    return false;
                tree = true;
            } else {
                NetInfo *din = ci->getPort(id_DIN);
                if (!ci->type.in(id_CPE_FF, id_CPE_LATCH) || (din && din->driver.cell != cpe_cell(tile, z - 2)))
                    return false;
            }
            BelId other = cpe_bel(tile, z ^ 1);
            for (auto &port : ci->ports)
                if (port.second.net &&
                    ctx->getBelPinWire(ci->bel, port.first) != ctx->getBelPinWire(other, port.first) &&
                    !weakly_routed(port.second.net))
                    return false;
        }
        return tree;
    };
    // Swapping halves twice restores them
    auto swap_cpe_halves = [&](int tile) {
        std::vector<std::tuple<CellInfo *, BelId, PlaceStrength>> moved;
        for (int z : half_z)
            if (CellInfo *ci = cpe_cell(tile, z))
                moved.emplace_back(ci, cpe_bel(tile, z ^ 1), ci->belStrength);
        for (auto &m : moved)
            ctx->unbindBel(std::get<0>(m)->bel);
        for (auto &[ci, bel, strength] : moved) {
            ctx->bindBel(bel, ci, strength);
            if (ci->type != id_CPE_L2T4)
                continue;
            // As repack() sets them: the lower half passes its LUT tree through L20
            bool upper = ctx->getBelLocation(bel).z == CPE_LT_U_Z;
            ci->params[id_L2T4_UPPER] = Property(upper ? 1 : 0, 1);
            if (upper)
                ci->params.erase(id_INIT_L20);
            else
                ci->params[id_INIT_L20] = Property(LUT_D1, 4);
        }
        used_cpes[tile] = cpe_cell(tile, CPE_FF_U_Z) != nullptr;
    };
    auto rip_up_for = [&](const Move &m) {
        if (m.tree) {
            std::vector<WireId> pins;
            for (IdString pin : tree_pins)
                pins.push_back(ctx->getBelPinWire(m.tree->bel, pin));
            rip_up_near(pins, true);
            return;
        }
        std::vector<WireId> inputs, outputs;
        for (int z : half_z) {
            CellInfo *ci = cpe_cell(tile_of(m), z);
            if (!ci)
                continue;
            BelId other = cpe_bel(m.tile, z ^ 1);
            for (auto &port : ci->ports) {
                WireId now = ctx->getBelPinWire(ci->bel, port.first), then = ctx->getBelPinWire(other, port.first);
                if (now == then)
                    continue;
                (port.second.type == PORT_OUT ? outputs : inputs).push_back(then);
                NetInfo *net = port.second.net;
                if (net && net->wires.count(now))
                    rip_up_from(net, now); // for an output, that is the whole net
            }
        }
        rip_up_near(inputs, true);
        rip_up_near(outputs, false);
    };
    auto make = [&](const Move &m) {
        if (m.tree)
            swap_tree(m.tree);
        else
            swap_cpe_halves(m.tile);
    };
    // Criticality-weighted delays of the connections a move changes, before and after; with only
    // the connections all estimates exist for
    auto move_costs = [&](const Move &m, const TimingAnalyser &tmg) {
        if (m.tree)
            return lut_tree_costs(m.tree, tmg, true);
        float cost[2] = {0, 0};
        auto add = [&](float crit, delay_t before, delay_t after) {
            if (before < 0 || after < 0)
                return;
            float weight = 1 + 9 * crit * crit;
            cost[0] += weight * before;
            cost[1] += weight * after;
        };
        std::set<CellInfo *> moved;
        for (int z : half_z)
            if (CellInfo *ci = cpe_cell(m.tile, z))
                moved.insert(ci);
        for (CellInfo *ci : moved) {
            BelId other = cpe_bel(m.tile, ctx->getBelLocation(ci->bel).z ^ 1);
            for (auto &port : ci->ports) {
                NetInfo *net = port.second.net;
                if (!net)
                    continue;
                WireId then = ctx->getBelPinWire(other, port.first);
                if (port.second.type == PORT_IN && ci->type == id_CPE_L2T4 && net->driver.cell &&
                    !moved.count(net->driver.cell))
                    add(tmg.get_criticality(CellPortKey(ci->name, port.first)),
                        ctx->getNetinfoRouteDelay(net, net->users.at(port.second.user_idx)),
                        lookahead.estimate(ctx->getNetinfoSourceWire(net), then));
                if (port.second.type != PORT_OUT)
                    continue;
                for (auto &user : net->users) {
                    if (!user.cell || moved.count(user.cell))
                        continue;
                    add(tmg.get_criticality(CellPortKey(user)), ctx->getNetinfoRouteDelay(net, user),
                        lookahead.estimate(then, ctx->getNetinfoSinkWire(net, user, 0)));
                }
            }
        }
        return std::make_pair(cost[0], cost[1]);
    };

    pool<IdString> done_trees;              // kept, or undone when tried alone
    pool<int> done_tiles;                   // the same, for half swaps
    std::vector<std::vector<Move>> pending; // batches still to try, the next one last
    float best = fmax();
    log_info("Swapping LUT inputs on critical paths, starting at %.2f MHz.\n", best);
    int kept = 0, tried = 0;
    for (int round = 1; round <= rounds; round++) {
        if (pending.empty()) {
            TimingAnalyser tmg(ctx);
            tmg.setup();
            tmg.run();
            // The best-looking move of each CPE with a critical LUT tree
            dict<int, std::tuple<float, float, Move>> best_move; // tile -> crit, gain, move
            for (auto &cell : ctx->cells) {
                CellInfo *ci = cell.second.get();
                if (!plain_tree(ci))
                    continue;
                float crit = 0, weight = 0;
                for (IdString pin : tree_pins)
                    if (ci->getPort(pin)) {
                        float c = tmg.get_criticality(CellPortKey(ci->name, pin));
                        crit = std::max(crit, c);
                        weight += 1 + 9 * c * c;
                    }
                if (crit < min_crit && !test_undo)
                    continue;
                int tile = ci->bel.tile;
                std::vector<Move> moves;
                bool inputs_weak = true;
                for (IdString pin : tree_pins)
                    if (NetInfo *net = ci->getPort(pin))
                        inputs_weak &= weakly_routed(net);
                if (swap_pairs && !done_trees.count(ci->name) && inputs_weak)
                    moves.push_back(Move{ci, -1});
                if (swap_halves && !done_tiles.count(tile) && halves_swappable(tile))
                    moves.push_back(Move{nullptr, tile});
                for (auto &m : moves) {
                    auto costs = move_costs(m, tmg);
                    float gain = costs.first - costs.second;
                    if (!test_undo && gain < min_gain * weight)
                        continue;
                    if (!best_move.count(tile) || gain > std::get<1>(best_move.at(tile)))
                        best_move[tile] = std::make_tuple(crit, gain, m);
                }
            }
            if (best_move.empty()) {
                log_info("    round %d: no more candidates\n", round);
                break;
            }
            std::vector<std::tuple<float, float, Move>> candidates;
            for (auto &entry : best_move)
                candidates.push_back(entry.second);
            std::stable_sort(candidates.begin(), candidates.end(),
                             [](const auto &a, const auto &b) { return std::get<0>(a) > std::get<0>(b); });
            pending.emplace_back();
            for (int i = 0; i < int(candidates.size()) && i < batch; i++)
                pending.back().push_back(std::get<2>(candidates.at(i)));
        }
        std::vector<Move> moves = pending.back();
        pending.pop_back();
        tried += moves.size();
        int halves = 0;
        for (auto &m : moves)
            halves += m.tree == nullptr;

        std::vector<std::pair<NetInfo *, dict<WireId, PipMap>>> snapshot;
        for (auto &net : ctx->nets)
            snapshot.emplace_back(net.second.get(), net.second->wires);
        std::vector<std::pair<CellInfo *, dict<IdString, Property>>> params;
        for (auto &m : moves) {
            for (int z : half_z)
                if (CellInfo *ci = cpe_cell(tile_of(m), z))
                    params.emplace_back(ci, ci->params);
            rip_up_for(m);
            make(m);
        }
        // Route only what was ripped up. A fresh router2 negotiates congestion from scratch, which would
        // unravel the converged routing around it, so the rest is held: router2 reserves wires bound
        // with STRENGTH_PLACER for their net.
        std::vector<std::pair<NetInfo *, WireId>> held;
        for (auto &net : ctx->nets)
            for (auto &w : net.second->wires)
                if (w.second.strength == STRENGTH_WEAK) {
                    w.second.strength = STRENGTH_PLACER;
                    held.emplace_back(net.second.get(), w.first);
                }
        bool routed = false;
        try {
            routed = router2(ctx, cfg);
        } catch (const log_execution_error_exception &) {
            // e.g. "Failed to route arc": treated like giving up, the moves are undone
        }
        for (auto &[net, wire] : held) {
            auto found = net->wires.find(wire);
            if (found != net->wires.end() && found->second.strength == STRENGTH_PLACER)
                found->second.strength = STRENGTH_WEAK;
        }
        float now = routed ? fmax() : 0;
        if (routed && now > best && !test_undo) {
            log_info("    round %d: %d moves (%d half swaps), %.2f -> %.2f MHz\n", round, int(moves.size()), halves,
                     best, now);
            best = now;
            kept += moves.size();
            for (auto &m : moves)
                m.tree ? (void)done_trees.insert(m.tree->name) : (void)done_tiles.insert(m.tile);
            // The critical paths have changed: look for candidates again
            pending.clear();
            continue;
        }
        log_info("    round %d: %d moves (%d half swaps), %.2f MHz %s, undone\n", round, int(moves.size()), halves, now,
                 routed ? "is no better" : "(not routable)");
        for (auto it = moves.rbegin(); it != moves.rend(); ++it)
            make(*it);
        for (auto &entry : params)
            entry.first->params = entry.second;
        // Some of them may still help: try each half on its own
        if (moves.size() > 1) {
            size_t half = moves.size() / 2;
            pending.emplace_back(moves.begin() + half, moves.end());
            pending.emplace_back(moves.begin(), moves.begin() + half);
        } else {
            moves.front().tree ? (void)done_trees.insert(moves.front().tree->name)
                               : (void)done_tiles.insert(moves.front().tile);
        }
        auto same = [](const dict<WireId, PipMap> &a, const dict<WireId, PipMap> &b) {
            if (a.size() != b.size())
                return false;
            for (auto &w : a) {
                auto found = b.find(w.first);
                if (found == b.end() || found->second.pip != w.second.pip ||
                    found->second.strength != w.second.strength)
                    return false;
            }
            return true;
        };
        std::vector<std::pair<NetInfo *, dict<WireId, PipMap>> *> changed;
        for (auto &entry : snapshot)
            if (!same(entry.first->wires, entry.second))
                changed.push_back(&entry);
        for (auto *entry : changed) {
            std::vector<WireId> bound;
            for (auto &w : entry->first->wires)
                bound.push_back(w.first);
            for (WireId w : bound)
                ctx->unbindWire(w);
        }
        for (auto *entry : changed)
            for (auto &w : entry->second) {
                if (w.second.pip == PipId())
                    ctx->bindWire(w.first, entry->first, w.second.strength);
                else
                    ctx->bindPip(w.second.pip, entry->first, w.second.strength);
            }
    }
    log_info("Kept %d of %d LUT moves tried.\n", kept, tried);
    ctx->check();
    if (!ctx->checkRoutedDesign())
        log_error("Swapping LUT inputs left the routing incomplete.\n");
    timing_analysis(ctx, false, true, false, false, true);
    return tried;
}

NEXTPNR_NAMESPACE_END
