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
#include <limits>
#include <memory>
#include <queue>
#include <string>

#include "gatemate.h"
#include "pack.h"
#include "router2.h"
#include "timing.h"

#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

NEXTPNR_NAMESPACE_BEGIN

// Routing runs that learn which connections end up critical (--vopt route-runs).
//
// router2 routes the most critical connections first and lets them all but ignore congestion, but
// it knows criticality only from the delays so far, at first from estimates. Which connections end
// up on the longest paths after the negotiation is close to chance, and often not those critical at
// the start. So route again from scratch, with each connection's criticality scaled by an importance
// learnt from the runs before: raised on the longest paths, lowered where there is much slack. Keep
// the best run.
void GateMateImpl::route_learning(int runs)
{
    dict<CellPortKey, float> importance;
    Router2Cfg cfg(ctx);
    configureRouter2(cfg);
    // How the runs learn (route_learn_mode):
    //  - "scale": criticality times an importance, which each run multiplies by 1 + step (on the
    //    longest paths) down to 1 - step (where there is most slack);
    //  - "max": at least the highest criticality the connection had after a run;
    //  - "ema": at least a learnt criticality, which each run moves `step` of the way to the
    //    connection's criticality after it;
    //  - "none": no learning, only fresh runs.
    const std::string &mode = route_learn_mode;
    const float step = route_learn_step;
    if (mode != "none")
        cfg.adjust_crit = [&](const PortRef &user, float crit) {
            auto found = importance.find(CellPortKey(user));
            if (found == importance.end())
                return crit;
            return mode == "scale" ? std::min(1.0f, crit * found->second) : std::max(crit, found->second);
        };
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
    // The routing router2 made: of all nets but those routed before it (clocks, multipliers)
    using Routing = std::vector<std::pair<NetInfo *, dict<WireId, PipMap>>>;
    auto save = [&]() {
        Routing routing;
        for (auto &net : ctx->nets)
            if (!prerouted_nets.count(net.first))
                routing.emplace_back(net.second.get(), net.second->wires);
        return routing;
    };
    auto rip_up = [&]() {
        for (auto &net : ctx->nets) {
            if (prerouted_nets.count(net.first))
                continue;
            std::vector<WireId> wires;
            for (auto &w : net.second->wires)
                wires.push_back(w.first);
            for (WireId w : wires)
                ctx->unbindWire(w);
        }
    };
    auto restore = [&](const Routing &routing) {
        rip_up();
        for (auto &[net, wires] : routing)
            for (auto &w : wires) {
                if (w.second.pip == PipId())
                    ctx->bindWire(w.first, net, w.second.strength);
                else
                    ctx->bindPip(w.second.pip, net, w.second.strength);
            }
    };

    float best = fmax(), last = best;
    Routing best_routing = save();
    log_info("Routing %d more times, learning criticality by '%s', starting at %.2f MHz.\n", runs, mode.c_str(), best);
    for (int run = 1; run <= runs; run++) {
        // the GUI can show the design as it stands
        ctx->routeRepairProgress();
        // Learn from the routing so far: criticality 1 is on the longest path, 0 has the most slack
        TimingAnalyser tmg(ctx);
        tmg.setup();
        tmg.run();
        for (auto &net : ctx->nets)
            for (auto &user : net.second->users) {
                float crit = tmg.get_criticality(CellPortKey(user));
                float &learnt = importance.emplace(CellPortKey(user), mode == "scale" ? 1.0f : 0.0f).first->second;
                if (mode == "scale")
                    learnt = std::min(10.0f, std::max(0.1f, learnt * (1 + step * (2 * crit - 1))));
                else if (mode == "max")
                    learnt = std::max(learnt, crit);
                else
                    learnt += step * (crit - learnt);
            }
        rip_up();
        bool routed = false;
        try {
            routed = router2(ctx, cfg);
        } catch (const log_execution_error_exception &) {
            // treated as a failed run
        }
        last = routed ? fmax() : 0;
        log_info("    run %d: %.2f MHz%s\n", run, last, routed && last > best ? ", the best so far" : "");
        if (routed && last > best) {
            best = last;
            best_routing = save();
        } else if (!routed) {
            restore(best_routing);
            last = best;
        }
    }
    if (last < best)
        restore(best_routing);
    ctx->check();
    if (!ctx->checkRoutedDesign())
        log_error("Routing again left the routing incomplete.\n");
    timing_analysis(ctx, false, true, false, false, true);
}

// The fastest path from src to sink for net, ignoring the routing of other nets except what cannot be
// ripped up, or with `free_only` all of it (A* guided by the delay tables), within `margin` tiles of
// their bounding box if not negative; the pips in order, or none
std::vector<PipId> GateMateImpl::ideal_path(NetInfo *net, WireId src, WireId sink, delay_t &delay, bool free_only,
                                            int margin) const
{
    BoundingBox box{0, 0, ctx->getGridDimX(), ctx->getGridDimY()};
    if (margin >= 0) {
        box = ctx->getRouteBoundingBox(src, sink);
        box.x0 = std::max(box.x0 - margin, 0);
        box.y0 = std::max(box.y0 - margin, 0);
        box.x1 = std::min(box.x1 + margin, ctx->getGridDimX());
        box.y1 = std::min(box.y1 + margin, ctx->getGridDimY());
    }
    dict<WireId, std::pair<delay_t, PipId>> best; // wire -> delay, pip into it
    using Item = std::pair<delay_t, WireId>;      // delay + estimate
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
    best[src] = {0, PipId()};
    queue.push({0, src});
    while (!queue.empty() && best.size() < 2000000) {
        WireId w = queue.top().second;
        queue.pop();
        if (w == sink)
            break;
        delay_t d = best.at(w).first;
        for (PipId pip : ctx->getPipsDownhill(w)) {
            if (!checkPipAvail(pip) || resource_taken(pip))
                continue;
            WireId next = ctx->getPipDstWire(pip);
            NetInfo *held = ctx->getBoundWireNet(next);
            if (held && held != net && (free_only || held->wires.at(next).strength > STRENGTH_WEAK))
                continue;
            if (margin >= 0) {
                int x, y;
                tile_xy(ctx->chip_info, next.tile, x, y);
                if (x < box.x0 || x > box.x1 || y < box.y0 || y > box.y1)
                    continue;
            }
            delay_t nd = d + ctx->getPipDelay(pip).maxDelay() + ctx->getWireDelay(next).maxDelay();
            auto found = best.find(next);
            if (found != best.end() && found->second.first <= nd)
                continue;
            best[next] = {nd, pip};
            queue.push({nd + std::max<delay_t>(0, lookahead.estimate(next, sink)), next});
        }
    }
    std::vector<PipId> path;
    if (!best.count(sink))
        return path;
    delay = best.at(sink).first;
    for (WireId w = sink; best.at(w).second != PipId(); w = ctx->getPipSrcWire(best.at(w).second))
        path.push_back(best.at(w).second);
    std::reverse(path.begin(), path.end());
    return path;
}

// Pips sharing a resource (a mux select of a CPE, such as C_SEL_C) must agree on its value across all
// nets, which router2 keeps to but binding pips directly does not
bool GateMateImpl::resource_taken(PipId pip) const
{
    GroupId key = getResourceKeyForPip(pip);
    if (key == GroupId())
        return false;
    int value = getResourceValueForPip(pip);
    for (PipId other : ctx->getGroupPips(key))
        if (other != pip && ctx->getBoundPipNet(other) && getResourceValueForPip(other) != value)
            return true;
    return false;
}

// The routing on the critical paths against the least delay on the empty chip for the same pins (the
// delay tables), with the muxes each connection passes (--vopt report-crit-routing)
void GateMateImpl::report_critical_routing()
{
    if (!lookahead.built())
        log_error("report-crit-routing needs the delay tables (e.g. --vopt lookahead=table).\n");
    for (auto &clock : ctx->timing_result.clock_paths) {
        log_info("Critical path routing for clock '%s' (ns): routed, least on the empty chip, muxes passed\n",
                 clock.first.c_str(ctx));
        for (auto &seg : clock.second.segments) {
            if (seg.type != CriticalPath::Segment::Type::ROUTING || !ctx->nets.count(seg.net))
                continue;
            NetInfo *net = ctx->nets.at(seg.net).get();
            WireId src = ctx->getNetinfoSourceWire(net), sink;
            for (auto &user : net->users)
                if (user.cell && user.cell->name == seg.to.first && user.port == seg.to.second)
                    sink = ctx->getNetinfoSinkWire(net, user, 0);
            if (src == WireId() || sink == WireId())
                continue;
            std::vector<std::string> muxes;
            for (WireId w = sink; net->wires.count(w) && net->wires.at(w).pip != PipId();) {
                PipId pip = net->wires.at(w).pip;
                if (ctx->getPipDelay(pip).maxDelay() > 0)
                    muxes.push_back(ctx->getPipName(pip)[1].str(ctx));
                w = ctx->getPipSrcWire(pip);
            }
            std::reverse(muxes.begin(), muxes.end());
            std::string list;
            for (auto &m : muxes)
                list += " " + m.substr(0, m.find('/') == std::string::npos ? m.size() : m.find('/'));
            log_info("  %5.2f %5.2f  %s -> %s:%s\n", ctx->getDelayNS(seg.delay),
                     ctx->getDelayNS(lookahead.estimate(src, sink)), ctx->nameOfWire(src), ctx->nameOfWire(sink),
                     list.c_str());
            // The fastest path ignoring other nets' routing, and who holds its wires now
            delay_t ideal = 0;
            std::vector<PipId> path = ideal_path(net, src, sink, ideal);
            if (path.empty())
                continue;
            std::string holders;
            for (PipId pip : path) {
                WireId w = ctx->getPipDstWire(pip);
                NetInfo *held = ctx->getBoundWireNet(w);
                if (held && held != net) {
                    bool same_cell = false;
                    for (auto &user : held->users)
                        same_cell |= user.cell && user.cell->name == seg.to.first;
                    holders += stringf(" %s:%s%s", ctx->nameOfWire(w), held->name.c_str(ctx),
                                       same_cell ? "(same cell)" : "");
                }
            }
            log_info("         ideal path %.2f ns, held by:%s\n", ctx->getDelayNS(ideal),
                     holders.empty() ? " nobody" : holders.c_str());
        }
    }
}

namespace {
// Fmax of the slowest clock, as timing_analysis reports it
float slowest_fmax(TimingAnalyser &tmg)
{
    tmg.with_clock_skew = true;
    tmg.setup(false, false, true);
    float worst = std::numeric_limits<float>::infinity();
    for (auto &clock : tmg.get_timing_result().clock_fmax)
        worst = std::min(worst, clock.second.achieved);
    return worst;
}

// Whether every clock reaches its constraint raised by margin percent (after slowest_fmax() has set up
// the analyser)
bool timing_met(TimingAnalyser &tmg, float margin)
{
    for (auto &clock : tmg.get_timing_result().clock_fmax)
        if (clock.second.achieved < clock.second.constraint * (1 + margin / 100))
            return false;
    return true;
}

// How near-critical the design is beyond its worst path: the sum over all connections of how far
// their criticality exceeds 0.9. Many paths can share the worst delay (say, the 30 register enables
// of one write enable), and repairing one of them alone does not raise Fmax: such a repair is kept
// if this goes down while Fmax stays.
double near_critical(Context *ctx, TimingAnalyser &tmg)
{
    double sum = 0;
    for (auto &net : ctx->nets)
        for (auto &user : net.second->users)
            sum += std::max(0.0f, tmg.get_criticality(CellPortKey(user)) - 0.9f);
    return sum;
}

// Unbind wire and everything net reaches through it, and the wires leading only there
void rip_up_from(Context *ctx, NetInfo *net, WireId wire)
{
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
}

// Free the CPE wires a cell may need: those up to two pips before (`uphill`) or after the given
// wires, in the same tile. Other nets can pass through a CPE (IN1 -> MUXOUT -> OUT1, say).
void rip_up_near(Context *ctx, const std::vector<WireId> &wires, bool uphill)
{
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
            rip_up_from(ctx, net, w);
    }
}

bool weakly_routed(const NetInfo *net)
{
    for (auto &w : net->wires)
        if (w.second.strength > STRENGTH_WEAK)
            return false;
    return true;
}

// The routing of all nets, to restore what an attempt changed
using RoutingSnapshot = std::vector<std::pair<NetInfo *, dict<WireId, PipMap>>>;

RoutingSnapshot snapshot_routing(Context *ctx)
{
    RoutingSnapshot snapshot;
    for (auto &net : ctx->nets)
        snapshot.emplace_back(net.second.get(), net.second->wires);
    return snapshot;
}

void restore_changed_routing(Context *ctx, const RoutingSnapshot &snapshot)
{
    auto same = [](const dict<WireId, PipMap> &a, const dict<WireId, PipMap> &b) {
        if (a.size() != b.size())
            return false;
        for (auto &w : a) {
            auto found = b.find(w.first);
            if (found == b.end() || found->second.pip != w.second.pip || found->second.strength != w.second.strength)
                return false;
        }
        return true;
    };
    std::vector<const std::pair<NetInfo *, dict<WireId, PipMap>> *> changed;
    for (auto &entry : snapshot)
        if (!same(entry.first->wires, entry.second))
            changed.push_back(&entry);
    for (auto *entry : changed) {
        std::vector<WireId> bound_wires;
        for (auto &w : entry->first->wires)
            bound_wires.push_back(w.first);
        for (WireId w : bound_wires)
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

void check_repaired_routing(Context *ctx, const char *what)
{
    ctx->check();
    if (!ctx->checkRoutedDesign()) {
        bool debug = ctx->debug;
        ctx->debug = true;
        ctx->checkRoutedDesign();
        ctx->debug = debug;
        log_error("%s left the routing incomplete.\n", what);
    }
    timing_analysis(ctx, false, true, false, false, true);
}
} // namespace

// Lay a connection on its ideal path, displacing the other nets there (to be routed again with the
// rest), and bind it with STRENGTH_PLACER, from the last wire of the path the net reaches already. As
// in the routing repair, the other inputs of the sink's cell are displaced too: they share its input
// muxes.
void GateMateImpl::lay_ideal_path(NetInfo *net, const PortRef &user, std::vector<std::pair<NetInfo *, WireId>> &laid)
{
    WireId src = ctx->getNetinfoSourceWire(net), sink = ctx->getNetinfoSinkWire(net, user, 0);
    if (src == WireId() || sink == WireId() || net->wires.count(sink))
        return;
    delay_t delay = 0;
    std::vector<PipId> path = ideal_path(net, src, sink, delay);
    if (path.empty())
        return;
    size_t first = 0;
    for (size_t i = 0; i < path.size(); i++)
        if (net->wires.count(ctx->getPipDstWire(path[i])))
            first = i + 1;
    for (auto &port : user.cell->ports) {
        NetInfo *other = port.second.net;
        if (port.second.type != PORT_IN || !other || other == net)
            continue;
        WireId pin = ctx->getNetinfoSinkWire(other, other->users.at(port.second.user_idx), 0);
        if (pin != WireId() && other->wires.count(pin) && other->wires.at(pin).strength <= STRENGTH_WEAK)
            rip_up_from(ctx, other, pin);
    }
    for (size_t i = first; i < path.size(); i++) {
        WireId w = ctx->getPipDstWire(path[i]);
        NetInfo *held = ctx->getBoundWireNet(w);
        if (held && held->wires.count(w) && held->wires.at(w).strength <= STRENGTH_WEAK)
            rip_up_from(ctx, held, w);
    }
    if (!net->wires.count(src)) {
        if (!ctx->checkWireAvail(src))
            return;
        ctx->bindWire(src, net, STRENGTH_PLACER);
        laid.emplace_back(net, src);
    }
    for (size_t i = first; i < path.size(); i++) {
        WireId w = ctx->getPipDstWire(path[i]);
        if (!ctx->checkPipAvailForNet(path[i], net) || resource_taken(path[i]) || !ctx->checkWireAvail(w))
            return;
        ctx->bindPip(path[i], net, STRENGTH_PLACER);
        laid.emplace_back(net, w);
    }
}

// Route what a repair ripped up, with all other routing staying: each unrouted connection on its
// fastest path through free wires near it (ideal_path()), the longest first, and where one finds no
// way because another took it, again with that one first, a few times. (router2 with all other
// routing held finds no more, and setting it up and searching the chip in vain cost far more for the
// few connections a repair touches.) The wires `laid` (bound with STRENGTH_PLACER) are lowered
// again. False if the routing stays incomplete.
bool GateMateImpl::route_ripped(std::vector<std::pair<NetInfo *, WireId>> laid)
{
    std::vector<std::tuple<delay_t, NetInfo *, WireId>> arcs; // estimated delay, net, sink
    for (auto &n : ctx->nets) {
        NetInfo *net = n.second.get();
        WireId src = ctx->getNetinfoSourceWire(net);
        if (src == WireId() || prerouted_nets.count(net->name))
            continue;
        for (auto &user : net->users)
            for (WireId sink : ctx->getNetinfoSinkWires(net, user))
                if (sink != WireId() && !net->wires.count(sink))
                    arcs.emplace_back(lookahead.estimate(src, sink), net, sink);
    }
    std::stable_sort(arcs.begin(), arcs.end(),
                     [](const auto &a, const auto &b) { return std::get<0>(a) > std::get<0>(b); });
    bool routed = false;
    std::string why;
    for (int attempt = 0; attempt < 4 && !routed; attempt++) {
        std::vector<WireId> bound; // by this attempt, to undo
        size_t failed = arcs.size();
        for (size_t i = 0; i < arcs.size() && failed == arcs.size(); i++) {
            auto &[estimate, net, sink] = arcs[i];
            if (net->wires.count(sink))
                continue;
            WireId src = ctx->getNetinfoSourceWire(net);
            if (!net->wires.count(src)) {
                if (!ctx->checkWireAvail(src)) {
                    failed = i;
                    why = stringf("its source %s is taken", ctx->nameOfWire(src));
                    break;
                }
                ctx->bindWire(src, net, STRENGTH_WEAK);
                bound.push_back(src);
            }
            delay_t delay = 0;
            // Near the connection first, as router2 does
            std::vector<PipId> path = ideal_path(net, src, sink, delay, true, 4);
            if (path.empty())
                path = ideal_path(net, src, sink, delay, true, 16);
            size_t first = 0;
            for (size_t j = 0; j < path.size(); j++)
                if (net->wires.count(ctx->getPipDstWire(path[j])))
                    first = j + 1;
            if (path.empty()) {
                failed = i;
                why = "no free path";
            }
            for (size_t j = first; j < path.size() && failed == arcs.size(); j++) {
                WireId w = ctx->getPipDstWire(path[j]);
                if (!ctx->checkPipAvailForNet(path[j], net) || resource_taken(path[j]) || !ctx->checkWireAvail(w)) {
                    failed = i;
                    why = stringf("%s is taken", ctx->nameOfWire(w));
                } else {
                    ctx->bindPip(path[j], net, STRENGTH_WEAK);
                    bound.push_back(w);
                }
            }
        }
        routed = failed == arcs.size();
        if (!routed && attempt == 3)
            log_info("    routing %s to %s: %s\n", std::get<1>(arcs[failed])->name.c_str(ctx),
                     ctx->nameOfWire(std::get<2>(arcs[failed])), why.c_str());
        if (!routed) {
            for (auto w = bound.rbegin(); w != bound.rend(); ++w)
                ctx->unbindWire(*w);
            std::rotate(arcs.begin(), arcs.begin() + failed, arcs.begin() + failed + 1);
        }
    }
    for (auto &[net, wire] : laid) {
        auto found = net->wires.find(wire);
        if (found != net->wires.end() && found->second.strength == STRENGTH_PLACER)
            found->second.strength = STRENGTH_WEAK;
    }
    if (routed && !ctx->checkRoutedDesign()) {
        log_info("    the routing is incomplete\n");
        routed = false;
    }
    return routed;
}

// Repairing the routing of the critical path (--vopt route-repair). Its connections are often slower
// than their ideal path because other nets hold wires on it, or the other input of the same LUT2
// took the input mux the connection needed. Each round takes the connection of the worst path that
// loses most against its ideal path, frees that path (ripping up the parts of other nets on it) and
// binds the connection along it, re-routes what was ripped up with all other routing held, and keeps
// the result if Fmax improved.
void GateMateImpl::repair_critical_routing(int rounds)
{
    if (!lookahead.built())
        log_error("route-repair needs the delay tables (e.g. --vopt lookahead=table).\n");
    pool<std::string> tried;
    float best;
    double best_near;
    // The timing of the design as it stands: at the start, or after the last kept round
    auto current = std::make_unique<TimingAnalyser>(ctx);
    best = slowest_fmax(*current);
    best_near = near_critical(ctx, *current);
    log_info("Repairing the routing of critical paths, starting at %.2f MHz.\n", best);
    int kept = 0;
    for (int round = 1; rounds < 0 || round <= rounds; round++) {
        // the GUI can show the design as it stands
        ctx->routeRepairProgress();
        if (rounds < 0 && timing_met(*current, repair_margin)) {
            log_info("    round %d: timing met\n", round);
            break;
        }
        // The connections of the worst paths, by how much slower they are than their ideal path
        struct Candidate
        {
            NetInfo *net;
            CellInfo *cell;
            WireId sink;
            std::vector<PipId> path;
            delay_t loss;
            std::string key;
        };
        std::vector<Candidate> candidates;
        {
            TimingAnalyser &tmg = *current;
            for (auto &clock : tmg.get_timing_result().clock_paths)
                for (auto &seg : clock.second.segments) {
                    if (seg.type != CriticalPath::Segment::Type::ROUTING || !ctx->nets.count(seg.net))
                        continue;
                    NetInfo *net = ctx->nets.at(seg.net).get();
                    std::string key = stringf("%s>%s.%s", net->name.c_str(ctx), seg.to.first.c_str(ctx),
                                              seg.to.second.c_str(ctx));
                    if (tried.count(key))
                        continue;
                    for (auto &user : net->users) {
                        if (!user.cell || user.cell->name != seg.to.first || user.port != seg.to.second)
                            continue;
                        WireId src = ctx->getNetinfoSourceWire(net), sink = ctx->getNetinfoSinkWire(net, user, 0);
                        if (src == WireId() || sink == WireId())
                            continue;
                        delay_t ideal = 0;
                        auto path = ideal_path(net, src, sink, ideal);
                        delay_t loss = ctx->getNetinfoRouteDelay(net, user) - ideal;
                        if (!path.empty() && loss > 50)
                            candidates.push_back(Candidate{net, user.cell, sink, path, loss, key});
                    }
                }
        }
        if (candidates.empty()) {
            log_info("    round %d: no connection on the critical paths to repair\n", round);
            break;
        }
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](const Candidate &a, const Candidate &b) { return a.loss > b.loss; });
        Candidate &c = candidates.front();
        tried.insert(c.key);

        RoutingSnapshot snapshot = snapshot_routing(ctx);
        // Free the ideal path: the connection's own branch, and the other nets on it. Also the other
        // inputs of the same cell: inputs of a CPE share the settings of its input muxes (which of
        // two inputs takes IN1 or IN2), so they must be routed again to agree with the new path.
        if (c.net->wires.count(c.sink))
            rip_up_from(ctx, c.net, c.sink);
        int displaced = 0;
        for (auto &port : c.cell->ports) {
            NetInfo *other = port.second.net;
            if (port.second.type != PORT_IN || !other || other == c.net)
                continue;
            WireId pin = ctx->getNetinfoSinkWire(other, other->users.at(port.second.user_idx), 0);
            if (pin != WireId() && other->wires.count(pin) && other->wires.at(pin).strength <= STRENGTH_WEAK) {
                rip_up_from(ctx, other, pin);
                displaced++;
            }
        }
        // The net's own routing too, where it reaches a wire of the path another way: for a net with
        // many sinks, its tree can make a long detour to the critical one. The branches hanging off
        // such a wire are routed again.
        for (PipId pip : c.path) {
            WireId w = ctx->getPipDstWire(pip);
            NetInfo *held = ctx->getBoundWireNet(w);
            if (!held || !held->wires.count(w) || (held == c.net && held->wires.at(w).pip == pip))
                continue;
            rip_up_from(ctx, held, w);
            displaced++;
        }
        // Bind the connection along it, where it is not already
        bool bound = true;
        std::vector<std::pair<NetInfo *, WireId>> laid;
        for (PipId pip : c.path) {
            WireId w = ctx->getPipDstWire(pip);
            if (c.net->wires.count(w))
                continue;
            if (!ctx->checkPipAvailForNet(pip, c.net) || resource_taken(pip) || !ctx->checkWireAvail(w)) {
                bound = false;
                break;
            }
            ctx->bindPip(pip, c.net, STRENGTH_PLACER);
            laid.emplace_back(c.net, w);
        }
        bool routed = false;
        if (bound)
            routed = route_ripped(laid);
        else
            for (auto &[net, wire] : laid)
                net->wires.at(wire).strength = STRENGTH_WEAK;
        float now = 0;
        double now_near = 0;
        std::unique_ptr<TimingAnalyser> after;
        if (routed) {
            after = std::make_unique<TimingAnalyser>(ctx);
            now = slowest_fmax(*after);
            now_near = near_critical(ctx, *after);
        }
        bool faster = now > best * 1.0001f, as_fast = now >= best * 0.9999f;
        if (routed && (faster || (as_fast && now_near < best_near * 0.999))) {
            log_info("    round %d: %s, %.2f ns faster alone, %d nets displaced: %.2f -> %.2f MHz, near-critical "
                     "%.1f -> %.1f\n",
                     round, c.key.c_str(), ctx->getDelayNS(c.loss), displaced, best, now, best_near, now_near);
            best = std::max(best, now);
            best_near = now_near;
            current = std::move(after);
            kept++;
            continue;
        }
        log_info("    round %d: %s, %.2f ns faster alone, %d nets displaced: %s, undone\n", round, c.key.c_str(),
                 ctx->getDelayNS(c.loss), displaced,
                 routed ? stringf("%.2f MHz is no better", now).c_str() : "not routable");
        restore_changed_routing(ctx, snapshot);
    }
    log_info("Kept %d critical path repairs.\n", kept);
    check_repaired_routing(ctx, "Repairing the routing");
}

// Repairing whole nets on the critical paths (--vopt net-repair). The connection repair takes one
// connection a round, but a net with many sinks on the worst paths (a register file's write enable
// feeding the enables of 30 registers, say) is only as fast as the slowest of them, and repairing
// one rarely raises Fmax. Each round takes the net whose near-critical sinks lose most against the
// delay tables, rips all of it up, lays those sinks on their ideal paths (the most critical first),
// routes the rest with all other routing held, and keeps the result if Fmax improved.
void GateMateImpl::repair_critical_nets(int rounds)
{
    if (!lookahead.built())
        log_error("net-repair needs the delay tables (e.g. --vopt lookahead=table).\n");
    const float window = 1000;     // ps: sinks within this of the worst slack count
    const float lay_window = 2000; // ps: sinks within this are laid on their ideal paths
    pool<IdString> tried;
    float best;
    double best_near;
    // The timing of the design as it stands: at the start, or after the last kept round
    auto current = std::make_unique<TimingAnalyser>(ctx);
    best = slowest_fmax(*current);
    best_near = near_critical(ctx, *current);
    log_info("Repairing whole nets of critical paths, starting at %.2f MHz.\n", best);
    int kept = 0;
    for (int round = 1; round <= rounds; round++) {
        // the GUI can show the design as it stands
        ctx->routeRepairProgress();
        NetInfo *net = nullptr;
        float loss = 0;
        int sinks = 0;
        std::vector<std::pair<float, PortRef>> to_lay; // slack, sink
        {
            TimingAnalyser &tmg = *current;
            float worst = std::numeric_limits<float>::infinity();
            for (auto &n : ctx->nets)
                for (auto &user : n.second->users)
                    worst = std::min(worst, tmg.get_setup_slack(CellPortKey(user)));
            for (auto &n : ctx->nets) {
                NetInfo *ni = n.second.get();
                if (tried.count(ni->name) || prerouted_nets.count(ni->name) || ni->users.entries() < 2 ||
                    !weakly_routed(ni))
                    continue;
                WireId src = ctx->getNetinfoSourceWire(ni);
                if (src == WireId())
                    continue;
                float l = 0;
                int near = 0;
                for (auto &user : ni->users) {
                    if (!user.cell || tmg.get_setup_slack(CellPortKey(user)) > worst + window)
                        continue;
                    WireId sink = ctx->getNetinfoSinkWire(ni, user, 0);
                    delay_t least = sink == WireId() ? -1 : lookahead.estimate(src, sink);
                    if (least < 0)
                        continue;
                    near++;
                    l += std::max<float>(0, ctx->getNetinfoRouteDelay(ni, user) - least - 50);
                }
                if (near >= 2 && l > loss) {
                    net = ni;
                    loss = l;
                    sinks = near;
                }
            }
            if (net)
                for (auto &user : net->users) {
                    float slack = tmg.get_setup_slack(CellPortKey(user));
                    if (user.cell && slack <= worst + lay_window)
                        to_lay.emplace_back(slack, user);
                }
        }
        if (!net) {
            log_info("    round %d: no net on the critical paths to repair\n", round);
            break;
        }
        RoutingSnapshot snapshot = snapshot_routing(ctx);
        rip_up_from(ctx, net, ctx->getNetinfoSourceWire(net));
        std::stable_sort(to_lay.begin(), to_lay.end(),
                         [](const std::pair<float, PortRef> &a, const std::pair<float, PortRef> &b) {
                             return a.first < b.first;
                         });
        std::vector<std::pair<NetInfo *, WireId>> laid;
        for (auto &sink : to_lay)
            lay_ideal_path(net, sink.second, laid);
        bool routed = route_ripped(laid);
        float now = 0;
        double now_near = 0;
        std::unique_ptr<TimingAnalyser> after;
        if (routed) {
            after = std::make_unique<TimingAnalyser>(ctx);
            now = slowest_fmax(*after);
            now_near = near_critical(ctx, *after);
        }
        std::string what = stringf("%s, %d near-critical sinks %.2f ns slower than the tables, %d laid",
                                   net->name.c_str(ctx), sinks, ctx->getDelayNS(delay_t(loss)), int(to_lay.size()));
        bool faster = now > best * 1.0001f, as_fast = now >= best * 0.9999f;
        if (routed && (faster || (as_fast && now_near < best_near * 0.999))) {
            log_info("    round %d: %s: %.2f -> %.2f MHz, near-critical %.1f -> %.1f\n", round, what.c_str(), best, now,
                     best_near, now_near);
            best = std::max(best, now);
            best_near = now_near;
            current = std::move(after);
            kept++;
            continue;
        }
        log_info("    round %d: %s: %s, undone\n", round, what.c_str(),
                 routed ? stringf("%.2f MHz is no better", now).c_str() : "not routable");
        restore_changed_routing(ctx, snapshot);
        tried.insert(net->name);
    }
    log_info("Kept %d net repairs.\n", kept);
    check_repaired_routing(ctx, "Repairing nets");
}

namespace {
// As repack() sets them for the half a LUT tree is in: the lower half passes it through L20
void set_lut_tree_half(Context *ctx, CellInfo *ci)
{
    bool upper = ctx->getBelLocation(ci->bel).z == CPE_LT_U_Z;
    ci->params[id_L2T4_UPPER] = Property(upper ? 1 : 0, 1);
    if (upper)
        ci->params.erase(id_INIT_L20);
    else
        ci->params[id_INIT_L20] = Property(LUT_D1, 4);
}

// A LUT tree fed only through its LUTs, passing its result unchanged through L20 in a lower half
bool plain_lut_tree(Context *ctx, const CellInfo *ci)
{
    if (ci->type != id_CPE_L2T4 || ci->bel == BelId() || ci->belStrength > STRENGTH_STRONG || !ci->getPort(id_OUT))
        return false;
    for (auto &port : ci->ports) {
        if (!port.second.net)
            continue;
        if (port.second.type == PORT_IN ? !port.first.in(id_D0_00, id_D1_00, id_D0_01, id_D1_01, id_D0_10, id_D1_10)
                                        : port.first != id_OUT)
            return false;
    }
    for (IdString param : {id_C_I1, id_C_I2, id_C_I3, id_C_I4})
        if (int_or_default(ci->params, param, 0))
            return false;
    return ctx->getBelLocation(ci->bel).z == CPE_LT_U_Z || int_or_default(ci->params, id_INIT_L20, LUT_D1) == LUT_D1;
}
} // namespace

// LUT trees that can move to another free LUT tree position after placement: plain ones not merged
// with a register or other cells of their CPE
bool GateMateImpl::relocatable_lut_tree(const CellInfo *ci) const
{
    return ci->cluster == ClusterId() && plain_lut_tree(ctx, ci);
}

// Registers that can move with the LUT tree feeding them: the two alone in their cluster and half,
// the tree plain, the register clocked through CLK_INT (routed again by reroute_clock_sink()).
// Returns the LUT tree, or nullptr.
CellInfo *GateMateImpl::relocatable_register(const CellInfo *ff) const
{
    if (ff->type != id_CPE_FF || ff->bel == BelId() || ff->cluster == ClusterId() ||
        ff->belStrength > STRENGTH_STRONG || !ff->getPort(id_CLK_INT))
        return nullptr;
    Loc loc = ctx->getBelLocation(ff->bel);
    if (loc.z != CPE_FF_U_Z && loc.z != CPE_FF_L_Z)
        return nullptr;
    CellInfo *lut = ctx->getBoundBelCell(ctx->getBelByLocation(Loc(loc.x, loc.y, loc.z - 2)));
    const NetInfo *din = ff->getPort(id_DIN);
    if (!lut || lut->cluster != ff->cluster || !plain_lut_tree(ctx, lut) || !din || din->driver.cell != lut ||
        din->users.entries() != 1 || ctx->getClusterRootCell(ff->cluster)->constr_children.size() != 1)
        return nullptr;
    for (auto &port : ff->ports)
        if (port.second.net && !port.first.in(id_CLK_INT, id_EN_INT, id_SR, id_DIN, id_DOUT))
            return nullptr;
    return lut;
}

// A free LUT tree position for ci (or one `leaving` frees), in a CPE with nothing but LUT trees and
// registers; the register of the half is free too
bool GateMateImpl::free_lut_tree_bel(const CellInfo *ci, BelId bel, const CellInfo *leaving) const
{
    if (bel == BelId() || (!ctx->checkBelAvail(bel) && ctx->getBoundBelCell(bel) != leaving) ||
        !ctx->isValidBelForCellType(ci->type, bel) || !ci->testRegion(bel))
        return false;
    Loc loc = ctx->getBelLocation(bel);
    if (loc.z != CPE_LT_U_Z && loc.z != CPE_LT_L_Z)
        return false;
    for (int z : {CPE_RAMIO_U_Z, CPE_RAMIO_L_Z, CPE_COMP_Z, CPE_CPLINES_Z, CPE_LT_FULL_Z, CPE_BRIDGE_Z,
                  loc.z == CPE_LT_U_Z ? CPE_FF_U_Z : CPE_FF_L_Z}) {
        BelId other = ctx->getBelByLocation(Loc(loc.x, loc.y, z));
        if (other != BelId() && !ctx->checkBelAvail(other))
            return false;
    }
    return true;
}

// A free half for a register and the LUT tree feeding it, at the LUT tree position lt; the other
// register of the CPE, if any, shares clock, enable, set/reset and configuration
bool GateMateImpl::free_register_half(const CellInfo *lut, const CellInfo *ff, BelId lt) const
{
    if (!free_lut_tree_bel(lut, lt))
        return false;
    Loc loc = ctx->getBelLocation(lt);
    BelId ff_bel = ctx->getBelByLocation(Loc(loc.x, loc.y, loc.z + 2));
    if (ff_bel == BelId() || !ctx->isValidBelForCellType(ff->type, ff_bel) || !ff->testRegion(ff_bel))
        return false;
    const CellInfo *other = ctx->getBoundBelCell(ctx->getBelByLocation(Loc(loc.x, loc.y, (loc.z ^ 1) + 2)));
    if (other && other != ff) {
        const auto &a = fast_cell_info.at(ff->flat_index), &b = fast_cell_info.at(other->flat_index);
        if (b.used && (a.config != b.config || a.ff_en != b.ff_en || a.ff_clk != b.ff_clk || a.ff_sr != b.ff_sr))
            return false;
    }
    return true;
}

void GateMateImpl::relocate_lut_tree(CellInfo *ci, BelId bel)
{
    PlaceStrength strength = ci->belStrength;
    ctx->unbindBel(ci->bel);
    ctx->bindBel(bel, ci, strength);
    set_lut_tree_half(ctx, ci);
}

void GateMateImpl::relocate_register(CellInfo *lut, CellInfo *ff, BelId lt)
{
    Loc loc = ctx->getBelLocation(lt);
    int from = ff->bel.tile;
    PlaceStrength strength = ff->belStrength;
    ctx->unbindBel(ff->bel);
    ctx->bindBel(ctx->getBelByLocation(Loc(loc.x, loc.y, loc.z + 2)), ff, strength);
    relocate_lut_tree(lut, lt);
    update_used_cpe(from);
    update_used_cpe(lt.tile);
}

// Bridges through a CPE drive DIN2 (C_BR), so a register or latch in its upper half blocks them (as postPlace() marks it)
void GateMateImpl::update_used_cpe(int tile)
{
    if (used_cpes.empty())
        return;
    int x, y;
    tile_xy(ctx->chip_info, tile, x, y);
    BelId ff = ctx->getBelByLocation(Loc(x, y, CPE_FF_U_Z));
    used_cpes[tile] =
            ff != BelId() && ctx->getBoundBelCell(ff) && ctx->getBoundBelCell(ff)->type.in(id_CPE_FF, id_CPE_LATCH);
}

// After a register moved: route its clock to its new CLK_INT from the clock net's routing as it
// stands, with the restrictions of route_clock(), and remove the branch to the old one (old_sink)
// unless another register still uses it. False if there is no way.
bool GateMateImpl::reroute_clock_sink(NetInfo *clk, const PortRef &user, WireId old_sink)
{
    WireId sink = ctx->getNetinfoSinkWire(clk, user, 0);
    bool shared = false;
    for (auto &other : clk->users)
        if ((other.cell != user.cell || other.port != user.port) && ctx->getNetinfoSinkWire(clk, other, 0) == old_sink)
            shared = true;
    if (!shared && old_sink != sink && clk->wires.count(old_sink)) {
        dict<WireId, int> children;
        for (auto &w : clk->wires)
            if (w.second.pip != PipId())
                children[ctx->getPipSrcWire(w.second.pip)]++;
        WireId w = old_sink;
        while (clk->wires.count(w) && children[w] == 0 && clk->wires.at(w).pip != PipId()) {
            WireId up = ctx->getPipSrcWire(clk->wires.at(w).pip);
            ctx->unbindWire(w);
            children[up]--;
            w = up;
        }
    }
    if (sink == WireId())
        return false;
    if (clk->wires.count(sink))
        return true;
    if (NetInfo *held = ctx->getBoundWireNet(sink)) {
        if (held->wires.at(sink).strength > STRENGTH_WEAK)
            return false;
        rip_up_from(ctx, held, sink);
    }
    auto occupied = [&](int tile) {
        int x, y;
        tile_xy(ctx->chip_info, tile, x, y);
        for (int z : {CPE_LT_U_Z, CPE_LT_L_Z, CPE_FF_U_Z, CPE_FF_L_Z, CPE_RAMIO_U_Z, CPE_RAMIO_L_Z, CPE_COMP_Z,
                      CPE_CPLINES_Z, CPE_LT_FULL_Z, CPE_BRIDGE_Z}) {
            BelId bel = ctx->getBelByLocation(Loc(x, y, z));
            if (bel != BelId() && ctx->getBoundBelCell(bel))
                return true;
        }
        return false;
    };
    dict<WireId, std::pair<delay_t, PipId>> best; // wire -> delay, pip into it
    using Item = std::pair<delay_t, WireId>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
    for (auto &w : clk->wires) {
        best[w.first] = {0, PipId()};
        queue.push({0, w.first});
    }
    while (!queue.empty() && best.size() < 1000000) {
        auto [delay, w] = queue.top();
        queue.pop();
        if (w == sink)
            break;
        if (delay > best.at(w).first)
            continue;
        for (PipId pip : ctx->getPipsDownhill(w)) {
            const auto &extra_data = *pip_extra_data(pip);
            if (extra_data.type == PipExtra::PIP_EXTRA_MUX && extra_data.resource != 0 &&
                !(extra_data.resource == PipMask::C_CY2_I && extra_data.value == 0))
                continue;
            if (extra_data.type == PipExtra::PIP_EXTRA_MUX && (extra_data.flags & MUX_ROUTING) && occupied(pip.tile))
                continue;
            WireId dst = ctx->getPipDstWire(pip);
            if (clk->wires.count(dst) || !ctx->checkPipAvailForNet(pip, clk) || !ctx->checkWireAvail(dst))
                continue;
            delay_t next = delay + ctx->getPipDelay(pip).maxDelay() + ctx->getWireDelay(dst).maxDelay() +
                           ctx->getDelayEpsilon();
            auto found = best.find(dst);
            if (found != best.end() && found->second.first <= next)
                continue;
            best[dst] = {next, pip};
            queue.push({next, dst});
        }
    }
    if (!best.count(sink))
        return false;
    std::vector<PipId> path;
    for (WireId w = sink; best.at(w).second != PipId(); w = ctx->getPipSrcWire(best.at(w).second))
        path.push_back(best.at(w).second);
    PlaceStrength strength = clk->driver.cell && clk->driver.cell->type == id_GLBOUT ? STRENGTH_LOCKED : STRENGTH_WEAK;
    for (auto pip = path.rbegin(); pip != path.rend(); ++pip)
        ctx->bindPip(*pip, clk, strength);
    return true;
}

// Moving the cells of the critical paths after routing (--vopt place-repair). Once the routing
// repair has brought the critical connections close to the least delay between their cells, what is
// left is where the cells are. The moves: a LUT tree to a free position, two LUT trees swapping
// places where the other one keeps slack to spare, and a register with the LUT tree feeding it to a
// free CPE half (its clock routed again). Each round takes the move within a radius that promises
// most slack on the paths through the moved cells, makes it, lays their critical connections on
// their ideal paths, routes the rest again with all other routing held, and keeps the move if Fmax
// improved.
int GateMateImpl::move_critical_cells(int rounds)
{
    if (!lookahead.built())
        log_error("place-repair needs the delay tables (e.g. --vopt lookahead=table).\n");
    // The cells were numbered again since placement (repack() and assignArchInfo() in postPlace()),
    // free_register_half() compares the registers of a CPE by their numbers
    assign_cell_info();
    const int radius = 12;
    const float window = 250;      // ps: cells within this of the worst slack are candidates
    const float min_gain = 50;     // ps of slack
    const float swap_margin = 100; // ps: a LUT tree swapped away keeps this much more slack than the moved one
    // What moves together: a LUT tree, a register with the LUT tree feeding it, the two halves of an
    // L20 pair (a 5-input LUT: the lower half with its L20, and the upper half feeding its COMBIN), or a 4:1
    // multiplexer taking a whole CPE (`lut` then is the CPE_MX4)
    struct Unit
    {
        CellInfo *lut = nullptr, *ff = nullptr, *upper = nullptr;
        bool full = false;
    };
    // Every position of a CPE free (or held by `leaving`)
    auto free_cpe = [&](const CellInfo *ci, int x, int y, const CellInfo *leaving) {
        // (the same kind of position as the one the multiplexer holds: after repack() its cell type is
        // no longer one the database lists for it)
        BelId full = ctx->getBelByLocation(Loc(x, y, CPE_LT_FULL_Z));
        if (full == BelId() || ctx->getBelType(full) != ctx->getBelType(ci->bel) || !ci->testRegion(full))
            return false;
        for (int z : {CPE_LT_U_Z, CPE_LT_L_Z, CPE_FF_U_Z, CPE_FF_L_Z, CPE_RAMIO_U_Z, CPE_RAMIO_L_Z, CPE_COMP_Z,
                      CPE_CPLINES_Z, CPE_LT_FULL_Z, CPE_BRIDGE_Z}) {
            BelId bel = ctx->getBelByLocation(Loc(x, y, z));
            if (bel != BelId() && !ctx->checkBelAvail(bel) && ctx->getBoundBelCell(bel) != leaving)
                return false;
        }
        return true;
    };
    // A 4:1 multiplexer alone in its CPE (no register merged with it), fed and read only through
    // routing
    auto movable_mux = [&](const CellInfo *ci) {
        if (ci->type != id_CPE_MX4 || ci->bel == BelId() || ci->belStrength > STRENGTH_STRONG)
            return false;
        Loc l = ctx->getBelLocation(ci->bel);
        if (!free_cpe(ci, l.x, l.y, ci))
            return false;
        for (auto &port : ci->ports)
            if (port.second.net &&
                !(port.second.type == PORT_IN
                          ? port.first.in(id_IN1, id_IN2, id_IN3, id_IN4, id_IN5, id_IN6, id_IN7, id_IN8, id_D0_00,
                                          id_D1_00, id_D0_01, id_D1_01, id_D0_02, id_D1_02, id_D0_03, id_D1_03)
                          : port.first.in(id_OUT1, id_OUT2)))
                return false;
        for (IdString param : {id_C_I1, id_C_I2, id_C_I3, id_C_I4})
            if (int_or_default(ci->params, param, 0))
                return false;
        return true;
    };
    auto movable_nets = [&](CellInfo *ci) {
        for (auto &port : ci->ports)
            if (port.second.net && port.first != id_CLK_INT &&
                (prerouted_nets.count(port.second.net->name) || !weakly_routed(port.second.net)))
                return false;
        return true;
    };
    // The upper half of ci's L20 pair, if ci is the lower one and the two are alone in their cluster
    auto l20_upper = [&](CellInfo *ci) -> CellInfo * {
        if (ci->type != id_CPE_L2T4 || ci->bel == BelId() || ci->belStrength > STRENGTH_STRONG ||
            !ci->getPort(id_COMBIN) || !ci->getPort(id_OUT) || ci->cluster != ci->name ||
            ctx->getBelLocation(ci->bel).z != CPE_LT_L_Z)
            return nullptr;
        for (auto &port : ci->ports)
            if (port.second.net && (port.second.type == PORT_IN ? !port.first.in(id_D0_00, id_D1_00, id_D0_01, id_D1_01,
                                                                                   id_D0_10, id_D1_10, id_COMBIN)
                                                                : port.first != id_OUT))
                return nullptr;
        for (IdString param : {id_C_I1, id_C_I2, id_C_I3, id_C_I4})
            if (int_or_default(ci->params, param, 0))
                return nullptr;
        CellInfo *up = ci->getPort(id_COMBIN)->driver.cell;
        if (!up || up->bel == BelId() || up->bel.tile != ci->bel.tile || up->cluster != ci->cluster ||
            ci->constr_children.size() != 1 || ci->constr_children.front() != up || !plain_lut_tree(ctx, up))
            return nullptr;
        return up;
    };
    auto unit_of = [&](CellInfo *ci, Unit &unit) {
        if (relocatable_lut_tree(ci) && movable_nets(ci)) {
            unit = Unit{ci, nullptr};
            return true;
        }
        if (CellInfo *up = l20_upper(ci); up && movable_nets(ci) && movable_nets(up)) {
            unit = Unit{ci, nullptr, up};
            return true;
        }
        if (movable_mux(ci) && movable_nets(ci)) {
            unit = Unit{ci, nullptr, nullptr, true};
            return true;
        }
        CellInfo *lut = ci->type == id_CPE_FF ? relocatable_register(ci) : nullptr;
        if (lut && movable_nets(lut) && movable_nets(ci) && prerouted_nets.count(ci->getPort(id_CLK_INT)->name)) {
            unit = Unit{lut, ci};
            return true;
        }
        return false;
    };
    // A move's effect on the paths through the moved cells. With s the slack of each of their
    // connections now (at their inputs, and at the sinks of their outputs), S the worst of them, and d
    // the change of each connection's delay (by the tables, at the new position against the old), the
    // worst slack through a LUT tree becomes
    //     min over sinks (s - d) - max over inputs (d - (s - S)),
    // and with a register, where paths end and begin, min over all connections (s - d); exactly, for
    // as long as the rest of the design stays as it is (and the register's clock arrives as before).
    struct Connection
    {
        CellInfo *cell;
        IdString port;
        PortRef user;
        WireId wire; // the source of an input's net, or a sink of an output
        bool input;
        float slack;
        delay_t now; // by the tables
    };
    auto connections = [&](const Unit &unit, TimingAnalyser &tmg, std::vector<Connection> &conns) {
        conns.clear();
        for (CellInfo *ci : {unit.lut, unit.ff, unit.upper}) {
            if (!ci)
                continue;
            for (auto &port : ci->ports) {
                NetInfo *net = port.second.net;
                // The clock is routed apart, the connection from the LUT tree to the register stays, and
                // so does the one inside an L20 pair
                if (!net || port.first == id_CLK_INT || (unit.ff && net == unit.ff->getPort(id_DIN)) ||
                    port.first == id_COMBIN)
                    continue;
                if (port.second.type == PORT_IN) {
                    if (!net->driver.cell || net->driver.cell->bel == BelId())
                        return false;
                    WireId src = ctx->getNetinfoSourceWire(net);
                    conns.push_back(Connection{ci, port.first, net->users.at(port.second.user_idx), src, true,
                                               tmg.get_setup_slack(CellPortKey(ci->name, port.first)),
                                               lookahead.estimate(src, ctx->getBelPinWire(ci->bel, port.first))});
                    continue;
                }
                for (auto &user : net->users) {
                    if (unit.upper && user.cell == unit.lut && user.port == id_COMBIN)
                        continue;
                    if (!user.cell || user.cell->bel == BelId())
                        return false;
                    WireId sink = ctx->getNetinfoSinkWire(net, user, 0);
                    conns.push_back(Connection{ci, port.first, user, sink, false,
                                               tmg.get_setup_slack(CellPortKey(user)),
                                               lookahead.estimate(ctx->getBelPinWire(ci->bel, port.first), sink)});
                }
            }
        }
        if (conns.empty())
            return false;
        for (auto &c : conns)
            if (c.wire == WireId() || c.now < 0)
                return false;
        return true;
    };
    auto worst_of = [](const std::vector<Connection> &conns) {
        float worst = std::numeric_limits<float>::infinity();
        for (auto &c : conns)
            worst = std::min(worst, c.slack);
        return worst;
    };
    // The worst slack through the unit with its LUT tree at lt
    auto slack_at = [&](const Unit &unit, BelId lt, const std::vector<Connection> &conns, float worst) {
        Loc loc = ctx->getBelLocation(lt);
        BelId ff = unit.ff ? ctx->getBelByLocation(Loc(loc.x, loc.y, loc.z + 2)) : BelId();
        BelId up = unit.upper ? ctx->getBelByLocation(Loc(loc.x, loc.y, CPE_LT_U_Z)) : BelId();
        float out = std::numeric_limits<float>::infinity(), in = -std::numeric_limits<float>::infinity();
        for (auto &c : conns) {
            WireId pin = ctx->getBelPinWire(c.cell == unit.ff ? ff : c.cell == unit.upper ? up : lt, c.port);
            delay_t delay = c.input ? lookahead.estimate(c.wire, pin) : lookahead.estimate(pin, c.wire);
            if (delay < 0)
                return -std::numeric_limits<float>::infinity();
            float change = delay - c.now;
            if (c.input && !unit.ff)
                in = std::max(in, change - (c.slack - worst));
            else
                out = std::min(out, c.slack - change);
        }
        return unit.ff ? out : out - in;
    };

    pool<std::pair<IdString, BelId>> tried;
    float best;
    double best_near;
    // The timing of the design as it stands: at the start, or after the last kept round
    auto current = std::make_unique<TimingAnalyser>(ctx);
    best = slowest_fmax(*current);
    best_near = near_critical(ctx, *current);
    log_info("Moving cells of critical paths, starting at %.2f MHz.\n", best);
    int kept = 0, kept_pairs = 0, kept_muxes = 0;
    for (int round = 1; rounds < 0 || round <= rounds; round++) {
        // the GUI can show the design as it stands
        ctx->routeRepairProgress();
        if (rounds < 0 && timing_met(*current, repair_margin)) {
            log_info("    round %d: timing met\n", round);
            break;
        }
        Unit unit;
        BelId to;
        CellInfo *swapped = nullptr; // a LUT tree taking the moved one's place
        float gain = 0, worst_all = std::numeric_limits<float>::infinity();
        int candidates = 0;
        std::vector<Connection> unit_conns;
        {
            TimingAnalyser &tmg = *current;
            for (auto &net : ctx->nets)
                for (auto &user : net.second->users)
                    worst_all = std::min(worst_all, tmg.get_setup_slack(CellPortKey(user)));
            dict<IdString, std::pair<std::vector<Connection>, float>> others; // swap partners: connections, worst
            std::vector<Connection> conns;
            for (auto &c : ctx->cells) {
                Unit u;
                if (!unit_of(c.second.get(), u) || !connections(u, tmg, conns))
                    continue;
                float worst = worst_of(conns);
                bool has_input = false, has_output = false;
                for (auto &conn : conns)
                    (conn.input ? has_input : has_output) = true;
                if ((!u.ff && (!has_input || !has_output)) || worst > worst_all + window)
                    continue;
                candidates++;
                Loc loc = ctx->getBelLocation(u.lut->bel);
                for (int y = loc.y - radius; y <= loc.y + radius; y++)
                    for (int x = loc.x - radius; x <= loc.x + radius; x++) {
                        if (x < 0 || y < 0 || x >= ctx->getGridDimX() || y >= ctx->getGridDimY())
                            continue;
                        for (int z : {CPE_LT_U_Z, CPE_LT_L_Z, CPE_LT_FULL_Z}) {
                            if ((z == CPE_LT_FULL_Z) != u.full)
                                continue;
                            BelId bel = ctx->getBelByLocation(Loc(x, y, z));
                            if (bel == BelId() || bel == u.lut->bel || tried.count({u.lut->name, bel}))
                                continue;
                            CellInfo *other = nullptr;
                            if (u.full) {
                                // a multiplexer takes a whole free CPE, without swapping
                                if (!free_cpe(u.lut, x, y, nullptr))
                                    continue;
                            } else if (u.upper) {
                                // an L20 pair takes a whole free CPE, without swapping
                                BelId up = ctx->getBelByLocation(Loc(x, y, CPE_LT_U_Z));
                                if (z != CPE_LT_L_Z || !free_lut_tree_bel(u.lut, bel) ||
                                    !free_lut_tree_bel(u.upper, up))
                                    continue;
                            } else if (u.ff ? !free_register_half(u.lut, u.ff, bel) : !free_lut_tree_bel(u.lut, bel)) {
                                Unit o;
                                other = ctx->getBoundBelCell(bel);
                                if (u.ff || !other || !unit_of(other, o) || o.ff || o.upper ||
                                    !free_lut_tree_bel(u.lut, bel, other) ||
                                    !free_lut_tree_bel(other, u.lut->bel, u.lut))
                                    continue;
                            }
                            float g = slack_at(u, bel, conns, worst) - worst;
                            if (g <= gain)
                                continue;
                            if (other) {
                                auto found = others.find(other->name);
                                if (found == others.end()) {
                                    std::vector<Connection> oconns;
                                    if (!connections(Unit{other, nullptr}, tmg, oconns))
                                        oconns.clear();
                                    found = others.emplace(other->name, std::make_pair(oconns, worst_of(oconns))).first;
                                }
                                auto &[oconns, oworst] = found->second;
                                if (oconns.empty() || slack_at(Unit{other, nullptr}, u.lut->bel, oconns, oworst) <
                                                              worst + g + swap_margin)
                                    continue;
                            }
                            unit = u;
                            to = bel;
                            swapped = other;
                            gain = g;
                            unit_conns = conns;
                        }
                    }
            }
        }
        if (!unit.lut || gain < min_gain) {
            log_info("    round %d: no cell near the critical paths to move (%d candidates, best gain %.2f ns)\n",
                     round, candidates, ctx->getDelayNS(delay_t(gain)));
            break;
        }

        struct Relocation
        {
            CellInfo *cell;
            BelId from, to;
            PlaceStrength strength;
            dict<IdString, Property> params;
        };
        std::vector<Relocation> moves;
        auto add_move = [&](CellInfo *ci, BelId bel) {
            moves.push_back(Relocation{ci, ci->bel, bel, ci->belStrength, ci->params});
        };
        Loc lt = ctx->getBelLocation(to);
        add_move(unit.lut, to);
        if (unit.ff)
            add_move(unit.ff, ctx->getBelByLocation(Loc(lt.x, lt.y, lt.z + 2)));
        if (unit.upper)
            add_move(unit.upper, ctx->getBelByLocation(Loc(lt.x, lt.y, CPE_LT_U_Z)));
        if (swapped)
            add_move(swapped, unit.lut->bel);
        std::string move;
        for (auto &m : moves) {
            Loc a = ctx->getBelLocation(m.from), b = ctx->getBelLocation(m.to);
            move += stringf("%s%s (%d, %d, %d) -> (%d, %d, %d)", move.empty() ? "" : ", ", m.cell->name.c_str(ctx), a.x,
                            a.y, a.z, b.x, b.y, b.z);
        }
        auto place = [&](bool back) {
            for (auto &m : moves)
                ctx->unbindBel(m.cell->bel);
            for (auto &m : moves) {
                ctx->bindBel(back ? m.from : m.to, m.cell, m.strength);
                if (back)
                    m.cell->params = m.params;
                else if (m.cell->type == id_CPE_L2T4 && !unit.upper) // (an L20 pair keeps its halves and tables)
                    set_lut_tree_half(ctx, m.cell);
            }
            for (auto &m : moves)
                if (m.cell == unit.ff) {
                    update_used_cpe(m.from.tile);
                    update_used_cpe(m.to.tile);
                }
        };

        RoutingSnapshot snapshot = snapshot_routing(ctx);
        // Rip up the moved cells' connections: the branches to their inputs and all of their outputs;
        // the clock apart
        for (auto &m : moves)
            for (auto &port : m.cell->ports) {
                NetInfo *net = port.second.net;
                if (!net || port.first == id_CLK_INT)
                    continue;
                WireId pin = port.second.type == PORT_IN
                                     ? ctx->getNetinfoSinkWire(net, net->users.at(port.second.user_idx), 0)
                                     : ctx->getNetinfoSourceWire(net);
                if (pin != WireId() && net->wires.count(pin))
                    rip_up_from(ctx, net, pin);
            }
        place(false);
        bool routable = true;
        if (unit.ff) {
            // A register in an upper half closes the CPE to bridges
            if (lt.z == CPE_LT_U_Z) {
                std::vector<std::pair<NetInfo *, WireId>> bridged;
                for (auto &net : ctx->nets)
                    for (auto &w : net.second->wires)
                        if (w.second.pip != PipId() && w.second.pip.tile == to.tile) {
                            const auto &extra_data = *pip_extra_data(w.second.pip);
                            if (extra_data.type == PipExtra::PIP_EXTRA_MUX && (extra_data.flags & MUX_ROUTING))
                                bridged.emplace_back(net.second.get(), w.first);
                        }
                for (auto &[net, wire] : bridged)
                    if (net->wires.count(wire)) {
                        if (net->wires.at(wire).strength > STRENGTH_WEAK)
                            routable = false;
                        else
                            rip_up_from(ctx, net, wire);
                    }
            }
            NetInfo *clk = unit.ff->getPort(id_CLK_INT);
            routable = routable && reroute_clock_sink(clk, clk->users.at(unit.ff->ports.at(id_CLK_INT).user_idx),
                                                      ctx->getBelPinWire(moves.at(1).from, id_CLK_INT));
        }
        std::vector<WireId> inputs, outputs;
        for (auto &m : moves)
            for (auto &port : m.cell->ports)
                if (port.second.net && port.first != id_CLK_INT)
                    (port.second.type == PORT_IN ? inputs : outputs).push_back(ctx->getBelPinWire(m.to, port.first));
        rip_up_near(ctx, inputs, true);
        rip_up_near(ctx, outputs, false);
        // The moved unit's critical connections first, on their ideal paths, the most critical first
        std::stable_sort(unit_conns.begin(), unit_conns.end(),
                         [](const Connection &a, const Connection &b) { return a.slack < b.slack; });
        std::vector<std::pair<NetInfo *, WireId>> laid;
        if (routable)
            for (auto &c : unit_conns)
                if (c.slack <= worst_all + window)
                    lay_ideal_path(c.cell->getPort(c.port), c.user, laid);
        bool routed = false;
        if (routable)
            routed = route_ripped(laid);
        else
            for (auto &[net, wire] : laid)
                net->wires.at(wire).strength = STRENGTH_WEAK;
        float now = 0;
        double now_near = 0;
        std::unique_ptr<TimingAnalyser> after;
        if (routed) {
            after = std::make_unique<TimingAnalyser>(ctx);
            now = slowest_fmax(*after);
            now_near = near_critical(ctx, *after);
        }
        bool faster = now > best * 1.0001f, as_fast = now >= best * 0.9999f;
        if (routed && (faster || (as_fast && now_near < best_near * 0.999))) {
            log_info("    round %d: %s, %.2f ns more slack by the tables: %.2f -> %.2f MHz, near-critical %.1f -> "
                     "%.1f\n",
                     round, move.c_str(), ctx->getDelayNS(delay_t(gain)), best, now, best_near, now_near);
            best = std::max(best, now);
            best_near = now_near;
            current = std::move(after);
            kept++;
            kept_pairs += unit.upper != nullptr;
            kept_muxes += unit.full;
            continue;
        }
        log_info("    round %d: %s, %.2f ns more slack by the tables: %s, undone\n", round, move.c_str(),
                 ctx->getDelayNS(delay_t(gain)),
                 routed ? stringf("%.2f MHz is no better", now).c_str() : "not routable");
        restore_changed_routing(ctx, snapshot);
        place(true);
        tried.insert({unit.lut->name, to});
    }
    log_info("Kept %d cell moves (%d of L20 pairs, %d of multiplexers).\n", kept, kept_pairs, kept_muxes);
    check_repaired_routing(ctx, "Moving cells");
    return kept;
}

// Repairing the placement of the critical paths (--vopt place-repair): moving cells, and fusing LUT2
// levels, in turns while either keeps something (each changes what the other finds)
void GateMateImpl::repair_critical_placement(int rounds)
{
    for (int pass = 0; pass < 4; pass++)
        if (move_critical_cells(rounds) + fuse_critical_luts(rounds) == 0)
            break;
}

// Fusing the LUT2 levels of critical paths, the other half of place-repair.
//
// A critical connection from a LUT2 A to a LUT2 C alone in its half costs a CPE pass and a routing
// hop. A copy of A can compute in C's L00 (or L01) instead, from A's inputs; A stays for its other
// sinks, or goes if C was its only one. Each round takes the critical connection where, by the delay
// tables, this gains most slack, lays A's inputs to C on their ideal paths, re-routes the rest with
// all other routing held, and keeps the result if Fmax rose (or tied with less near-critical slack).
int GateMateImpl::fuse_critical_luts(int rounds)
{
    if (!lookahead.built())
        log_error("place-repair needs the delay tables (e.g. --vopt lookahead=table).\n");
    assign_cell_info();
    const float window = 250;           // ps: connections within this of the worst slack are critical
    const float min_gain = 50;          // ps of slack
    const float stage = 300;            // ps: a CPE pass and its input less the first-level LUT, roughly
    const IdString first[2][2] = {{id_D0_00, id_D1_00}, {id_D0_01, id_D1_01}};
    const IdString first_init[2] = {id_INIT_L00, id_INIT_L01};
    auto is_ram_io = [](const CellInfo *x) { return x && x->type.in(id_CPE_RAMIO, id_CPE_RAMI, id_CPE_RAMO); };
    // A LUT2 alone in a half (inputs on D0_10/D1_10, maybe a first-level LUT computing and COMBIN)
    auto lone = [&](const CellInfo *ci, bool tree_ok) {
        if (ci->type != id_CPE_L2T4 || ci->bel == BelId() || ci->belStrength > STRENGTH_STRONG)
            return false;
        for (IdString param : {id_C_I1, id_C_I2, id_C_I3, id_C_I4})
            if (int_or_default(ci->params, param, 0))
                return false;
        for (auto &port : ci->ports) {
            NetInfo *net = port.second.net;
            if (!net)
                continue;
            bool ok = port.second.type == PORT_IN
                              ? port.first.in(id_D0_10, id_D1_10) ||
                                        (tree_ok && port.first.in(id_D0_00, id_D1_00, id_D0_01, id_D1_01, id_COMBIN))
                              : port.first == id_OUT;
            if (!ok || is_ram_io(net->driver.cell) || (port.first != id_COMBIN && (prerouted_nets.count(net->name) ||
                                                                                  !weakly_routed(net))))
                return false;
            for (auto &user : net->users)
                if (is_ram_io(user.cell))
                    return false;
        }
        return true;
    };
    pool<std::pair<IdString, IdString>> tried; // C, its port
    auto current = std::make_unique<TimingAnalyser>(ctx);
    float best = slowest_fmax(*current);
    double best_near = near_critical(ctx, *current);
    log_info("Fusing LUT2 levels of critical paths, starting at %.2f MHz.\n", best);
    int kept = 0, copied = 0;
    for (int round = 1; rounds < 0 || round <= rounds; round++) {
        if (rounds < 0 && timing_met(*current, repair_margin)) {
            log_info("    round %d: timing met\n", round);
            break;
        }
        TimingAnalyser &tmg = *current;
        float worst_all = std::numeric_limits<float>::infinity();
        for (auto &net : ctx->nets)
            for (auto &user : net.second->users)
                worst_all = std::min(worst_all, tmg.get_setup_slack(CellPortKey(user)));
        // The best fusion: C, its port fed by A, the first-level LUT j
        float gain = 0;
        CellInfo *best_c = nullptr;
        IdString best_port;
        int best_j = -1, candidates = 0;
        for (auto &cell : ctx->cells) {
            CellInfo *c = cell.second.get();
            if (!lone(c, true))
                continue;
            for (IdString port : {id_D0_10, id_D1_10}) {
                NetInfo *net = c->getPort(port);
                if (!net || tried.count({c->name, port}))
                    continue;
                float s = tmg.get_setup_slack(CellPortKey(c->name, port));
                CellInfo *a = net->driver.cell;
                if (s > worst_all + window || !a || a == c || net->driver.port != id_OUT || !lone(a, false) ||
                    a->getPort(id_COMBIN) || int_or_default(a->params, id_INIT_L20, LUT_D1) != LUT_D1)
                    continue;
                NetInfo *n1 = a->getPort(id_D0_10), *n2 = a->getPort(id_D1_10);
                if (!n1 || !n2 || n1 == n2 || !n1->driver.cell || !n2->driver.cell)
                    continue;
                // A first-level LUT free, the other one free to pass C's other input on (or that input gone)
                IdString other = port == id_D0_10 ? id_D1_10 : id_D0_10;
                int j = -1;
                for (int t = 0; t < 2 && j < 0; t++)
                    if (!c->getPort(first[t][0]) && !c->getPort(first[t][1]) &&
                        (!c->getPort(other) || (!c->getPort(first[1 - t][0]) && !c->getPort(first[1 - t][1]))))
                        j = t;
                if (j < 0)
                    continue;
                candidates++;
                // The worst slack through A's inputs afterwards: each saves A's routing, A's pass and the
                // hop to C, and gains the route to C's first-level LUT
                WireId a_out = ctx->getNetinfoSourceWire(net), c_in = ctx->getBelPinWire(c->bel, port);
                delay_t hop = lookahead.estimate(a_out, c_in);
                float after = std::numeric_limits<float>::infinity();
                bool ok = hop >= 0;
                for (int i = 0; i < 2 && ok; i++) {
                    NetInfo *n = i == 0 ? n1 : n2;
                    WireId src = ctx->getNetinfoSourceWire(n);
                    delay_t old_in = lookahead.estimate(src, ctx->getBelPinWire(a->bel, i == 0 ? id_D0_10 : id_D1_10));
                    delay_t new_in = lookahead.estimate(src, ctx->getBelPinWire(c->bel, first[j][i]));
                    ok = old_in >= 0 && new_in >= 0;
                    float si = tmg.get_setup_slack(CellPortKey(a->name, i == 0 ? id_D0_10 : id_D1_10));
                    after = std::min(after, si + float(old_in + hop - new_in) + stage);
                }
                if (!ok)
                    continue;
                float g = after - s;
                if (g > gain) {
                    gain = g;
                    best_c = c;
                    best_port = port;
                    best_j = j;
                }
            }
        }
        if (!best_c || gain < min_gain) {
            log_info("    round %d: no LUT2 level of a critical path to fuse (%d candidates, best gain %.2f ns)\n",
                     round, candidates, ctx->getDelayNS(delay_t(gain)));
            break;
        }
        CellInfo *c = best_c;
        int j = best_j;
        NetInfo *net = c->getPort(best_port);
        CellInfo *a = net->driver.cell;
        NetInfo *n1 = a->getPort(id_D0_10), *n2 = a->getPort(id_D1_10);
        tried.insert({c->name, best_port});
        bool a_shared = net->users.entries() > 1;

        RoutingSnapshot snapshot = snapshot_routing(ctx);
        auto params = c->params;
        // C: A's copy in L0j, A's net no longer an input
        WireId c_sink = ctx->getNetinfoSinkWire(net, net->users.at(c->ports.at(best_port).user_idx), 0);
        if (c_sink != WireId() && net->wires.count(c_sink))
            rip_up_from(ctx, net, c_sink);
        c->disconnectPort(best_port);
        if (!a_shared)
            ctx->ripupNet(net->name); // left without sinks (the snapshot has its routing for an undo)
        c->params[first_init[j]] = Property(int_or_default(a->params, id_INIT_L10, 0), 4);
        std::vector<WireId> pins;
        for (int i = 0; i < 2; i++) {
            if (!c->ports.count(first[j][i]))
                c->addInput(first[j][i]);
            c->connectPort(first[j][i], i == 0 ? n1 : n2);
            pins.push_back(ctx->getBelPinWire(c->bel, first[j][i]));
        }
        // With both of L10's inputs now from the first level, C is a plain tree, which reads L00 as the
        // side of D0_10 and L01 as that of D1_10: swap L10's inputs where it is the other way round
        if (!c->getPort(id_D0_10) && !c->getPort(id_D1_10) && (best_port == id_D0_10 ? j == 1 : j == 0)) {
            int t = int_or_default(c->params, id_INIT_L10, 0);
            c->params[id_INIT_L10] = Property((t & 0b1001) | ((t & 0b0010) << 1) | ((t & 0b0100) >> 1), 4);
        }
        // The CPE wires the new inputs need (C's other input may pass them now)
        rip_up_near(ctx, pins, true);
        if (NetInfo *x = c->getPort(best_port == id_D0_10 ? id_D1_10 : id_D0_10)) {
            WireId sink = ctx->getNetinfoSinkWire(x, x->users.at(c->ports.at(best_port == id_D0_10 ? id_D1_10
                                                                                                  : id_D0_10)
                                                                   .user_idx),
                                                 0);
            if (sink != WireId() && x->wires.count(sink) && x->wires.at(sink).strength <= STRENGTH_WEAK)
                rip_up_from(ctx, x, sink);
        }
        ctx->assignArchInfo();
        assign_cell_info();
        std::vector<std::pair<NetInfo *, WireId>> laid;
        for (int i = 0; i < 2; i++) {
            NetInfo *n = i == 0 ? n1 : n2;
            lay_ideal_path(n, n->users.at(c->ports.at(first[j][i]).user_idx), laid);
        }
        bool routed = route_ripped(laid);
        float now = 0;
        double now_near = 0;
        std::unique_ptr<TimingAnalyser> after;
        if (routed) {
            after = std::make_unique<TimingAnalyser>(ctx);
            now = slowest_fmax(*after);
            now_near = near_critical(ctx, *after);
        }
        std::string what = stringf("%s into L0%d of %s%s", a->name.c_str(ctx), j, c->name.c_str(ctx),
                                   a_shared ? " (a copy)" : "");
        bool faster = now > best * 1.0001f, as_fast = now >= best * 0.9999f;
        if (routed && (faster || (as_fast && now_near < best_near * 0.999))) {
            log_info("    round %d: %s, %.2f ns more slack by the tables: %.2f -> %.2f MHz, near-critical %.1f -> "
                     "%.1f\n",
                     round, what.c_str(), ctx->getDelayNS(delay_t(gain)), best, now, best_near, now_near);
            best = std::max(best, now);
            best_near = now_near;
            kept++;
            copied += a_shared;
            if (!a_shared) {
                // A has nothing left to drive
                for (IdString p : {id_D0_10, id_D1_10}) {
                    NetInfo *n = a->getPort(p);
                    WireId sink = ctx->getNetinfoSinkWire(n, n->users.at(a->ports.at(p).user_idx), 0);
                    if (sink != WireId() && n->wires.count(sink))
                        rip_up_from(ctx, n, sink);
                    a->disconnectPort(p);
                }
                ctx->ripupNet(net->name);
                a->disconnectPort(id_OUT);
                ctx->unbindBel(a->bel);
                ctx->net_aliases.erase(net->name);
                ctx->nets.erase(net->name);
                ctx->cells.erase(a->name);
                ctx->assignArchInfo();
                assign_cell_info();
                after = std::make_unique<TimingAnalyser>(ctx);
                slowest_fmax(*after);
            }
            current = std::move(after);
            continue;
        }
        log_info("    round %d: %s, %.2f ns more slack by the tables: %s, undone\n", round, what.c_str(),
                 ctx->getDelayNS(delay_t(gain)),
                 routed ? stringf("%.2f MHz is no better", now).c_str() : "not routable");
        for (int i = 0; i < 2; i++) {
            NetInfo *n = i == 0 ? n1 : n2;
            WireId sink = ctx->getNetinfoSinkWire(n, n->users.at(c->ports.at(first[j][i]).user_idx), 0);
            if (sink != WireId() && n->wires.count(sink))
                rip_up_from(ctx, n, sink);
            c->disconnectPort(first[j][i]);
        }
        c->params = params;
        c->connectPort(best_port, net);
        restore_changed_routing(ctx, snapshot);
        ctx->assignArchInfo();
        assign_cell_info();
        current = std::make_unique<TimingAnalyser>(ctx);
        slowest_fmax(*current);
    }
    log_info("Kept %d fused LUT2 levels (%d with a copy of the first LUT2).\n", kept, copied);
    check_repaired_routing(ctx, "Fusing LUT2 levels");
    return kept;
}

NEXTPNR_NAMESPACE_END
