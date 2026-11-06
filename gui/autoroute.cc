/*
 *  nextpnr -- Next Generation Place and Route
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

#include "autoroute.h"
#include <QAction>
#include <QMenu>
#include <algorithm>
#include <queue>
#include "editcommands.h"
#include "netstatus.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {

std::string q(const std::string &s) { return "'" + s + "'"; }

NetInfo *findNet(Context *ctx, IdString name)
{
    auto fnd = ctx->nets.find(name);
    return fnd == ctx->nets.end() ? nullptr : fnd->second.get();
}

// A grid location of a wire: that of a pip driving it, else of a pip it drives
bool wireLoc(Context *ctx, WireId wire, Loc &loc)
{
    for (PipId pip : ctx->getPipsUphill(wire)) {
        loc = ctx->getPipLocation(pip);
        return true;
    }
    for (PipId pip : ctx->getPipsDownhill(wire)) {
        loc = ctx->getPipLocation(pip);
        return true;
    }
    return false;
}

// Penalty per pip (ps), so that paths of zero-delay pips stay short
constexpr delay_t HOP_PENALTY = 10;

} // namespace

namespace AutoRoute {

std::vector<WireId> treeWires(Context *ctx, NetInfo *net)
{
    WireId src = ctx->getNetinfoSourceWire(net);
    if (src == WireId())
        return {};
    if (!net->wires.count(src))
        return {src};
    return NetStatus::subtree(NetStatus::downstream(ctx, net), src);
}

bool isRoutingLocked(const NetInfo *net)
{
    for (auto &w : net->wires)
        if (w.second.strength > STRENGTH_STRONG)
            return true;
    return false;
}

bool findPath(Context *ctx, NetInfo *net, const std::vector<WireId> &starts, WireId target, Result &res,
              const Options &opt)
{
    res = Result();
    if (target == WireId() || starts.empty()) {
        res.why = "no start or target wire";
        return false;
    }
    for (WireId s : starts)
        if (s == target) {
            res.start = s;
            return true;
        }
    NetInfo *tnet = ctx->getBoundWireNet(target);
    bool target_rebind = false;
    if (tnet != nullptr) {
        if (tnet != net) {
            res.why = "the target wire " + q(ctx->nameOfWire(target)) + " is used by net " + q(tnet->name.str(ctx));
            return false;
        }
        auto fnd = net->wires.find(target);
        if (fnd == net->wires.end() || fnd->second.pip != PipId()) {
            res.why = "the target wire " + q(ctx->nameOfWire(target)) + " is already driven in net " +
                      q(net->name.str(ctx));
            return false;
        }
        target_rebind = true;
    } else if (!ctx->checkWireAvail(target)) {
        res.why = "the target wire " + q(ctx->nameOfWire(target)) + " is not available";
        return false;
    }

    // Box around starts and target for the first pass
    bool have_box = false;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    auto extend = [&](WireId w) {
        Loc l;
        if (!wireLoc(ctx, w, l))
            return;
        if (!have_box) {
            x0 = x1 = l.x;
            y0 = y1 = l.y;
            have_box = true;
        }
        x0 = std::min(x0, l.x);
        x1 = std::max(x1, l.x);
        y0 = std::min(y0, l.y);
        y1 = std::max(y1, l.y);
    };
    extend(target);
    for (WireId s : starts)
        extend(s);
    x0 -= opt.margin;
    y0 -= opt.margin;
    x1 += opt.margin;
    y1 += opt.margin;

    struct Node
    {
        delay_t g;
        PipId pip; // driving pip on the best path (none for a start)
    };
    struct Item
    {
        delay_t f, g;
        WireId wire;
        bool operator<(const Item &o) const { return f > o.f; } // min-heap
    };

    for (int pass = 0; pass < 2; pass++) {
        bool boxed = pass == 0 && have_box;
        bool cut = false;
        dict<WireId, Node> best;
        std::priority_queue<Item> open;
        for (WireId s : starts) {
            if (best.count(s))
                continue;
            best[s] = Node{0, PipId()};
            open.push(Item{ctx->estimateDelay(s, target), 0, s});
        }
        while (!open.empty()) {
            Item it = open.top();
            open.pop();
            auto &node = best.at(it.wire);
            if (it.g > node.g)
                continue; // stale entry
            if (it.wire == target) {
                for (WireId w = target;;) {
                    PipId pip = best.at(w).pip;
                    if (pip == PipId()) {
                        res.start = w;
                        break;
                    }
                    res.path.push_back(pip);
                    w = ctx->getPipSrcWire(pip);
                }
                std::reverse(res.path.begin(), res.path.end());
                return true;
            }
            if (res.visits++ >= opt.max_visits) {
                res.why = "search limit reached (" + std::to_string(opt.max_visits) + " wires expanded)";
                return false;
            }
            for (PipId pip : ctx->getPipsDownhill(it.wire)) {
                WireId dst = ctx->getPipDstWire(pip);
                NetInfo *pnet = ctx->getBoundPipNet(pip);
                if (pnet != net) {
                    if (pnet != nullptr)
                        continue;
                    NetInfo *dnet = ctx->getBoundWireNet(dst);
                    if (dnet != nullptr) {
                        if (!(target_rebind && dst == target))
                            continue;
                    } else if (!ctx->checkWireAvail(dst)) {
                        continue;
                    }
                    if (!ctx->checkPipAvailForNet(pip, net))
                        continue;
                }
                if (boxed && dst != target) {
                    Loc l = ctx->getPipLocation(pip);
                    if (l.x < x0 || l.x > x1 || l.y < y0 || l.y > y1) {
                        cut = true;
                        continue;
                    }
                }
                delay_t g = it.g + ctx->getPipDelay(pip).maxDelay() + ctx->getWireDelay(dst).maxDelay() + HOP_PENALTY;
                auto fnd = best.find(dst);
                if (fnd != best.end() && fnd->second.g <= g)
                    continue;
                best[dst] = Node{g, pip};
                open.push(Item{g + ctx->estimateDelay(dst, target), g, dst});
            }
        }
        if (!cut)
            break; // the whole reachable set was searched
    }
    res.why = "no free path (" + std::to_string(res.visits) + " wires searched)";
    return false;
}

} // namespace AutoRoute

std::string addRoutePath(EditBatch &b, IdString net, const AutoRoute::Result &path)
{
    Context *ctx = b.getContext();
    NetInfo *ni = findNet(ctx, net);
    if (ni == nullptr)
        return "net " + q(net.str(ctx)) + " does not exist";
    if (path.start == WireId())
        return "empty path";
    if (ctx->getBoundWireNet(path.start) == nullptr) {
        if (path.start != ctx->getNetinfoSourceWire(ni))
            return "the start wire " + q(ctx->nameOfWire(path.start)) + " is not part of net " + q(net.str(ctx));
        std::string err = b.add(std::make_unique<BindWireCmd>(net, path.start, STRENGTH_MANUAL));
        if (!err.empty())
            return err;
    }
    for (PipId pip : path.path) {
        if (ctx->getBoundPipNet(pip) == ni)
            continue;
        WireId dst = ctx->getPipDstWire(pip);
        if (ctx->getBoundWireNet(dst) == ni) {
            // The top of a partial arc, bound without a driving pip: drive it through this pip
            auto fnd = ni->wires.find(dst);
            if (fnd == ni->wires.end() || fnd->second.pip != PipId())
                return "wire " + q(ctx->nameOfWire(dst)) + " is already driven in net " + q(net.str(ctx));
            std::string err = b.add(std::make_unique<UnbindWireCmd>(dst));
            if (!err.empty())
                return err;
        }
        std::string err = b.add(std::make_unique<BindPipCmd>(net, pip, STRENGTH_MANUAL));
        if (!err.empty())
            return err;
    }
    return {};
}

std::string addRouteToWire(EditBatch &b, IdString net, WireId from, WireId target, AutoRoute::Result *out)
{
    Context *ctx = b.getContext();
    NetInfo *ni = findNet(ctx, net);
    if (ni == nullptr)
        return "net " + q(net.str(ctx)) + " does not exist";
    if (from == WireId() || target == WireId())
        return "no start or target wire";
    AutoRoute::Result res;
    if (!AutoRoute::findPath(ctx, ni, {from}, target, res))
        return "no path from " + q(ctx->nameOfWire(from)) + " to " + q(ctx->nameOfWire(target)) + ": " + res.why;
    if (out)
        *out = res;
    return addRoutePath(b, net, res);
}

namespace {

// Route one arc; *skipped is set (and "" returned) if it needs no routing
std::string routeArc(EditBatch &b, NetInfo *ni, WireId sink, AutoRoute::Result *out, bool *skipped)
{
    Context *ctx = b.getContext();
    if (skipped)
        *skipped = false;
    NetStatus::Status st = NetStatus::compute(ctx, ni);
    const NetStatus::Arc *arc = nullptr;
    for (auto &a : st.arcs)
        if (a.sink == sink && sink != WireId())
            arc = &a;
    if (arc == nullptr)
        return q(ctx->nameOfWire(sink)) + " is not a sink wire of net " + q(ni->name.str(ctx));
    std::string arc_name = arc->cell.str(ctx) + "." + arc->port.str(ctx);
    if (arc->state == NetStatus::ArcState::ROUTED) {
        if (skipped)
            *skipped = true;
        return {};
    }
    if (arc->state == NetStatus::ArcState::NO_WIRE)
        return "the arc to " + arc_name + " has no sink or source wire";
    if (arc->blocked)
        return "the sink wire of " + arc_name + " is used by net " +
               q(ctx->getBoundWireNet(sink)->name.str(ctx));
    NetInfo *src_net = st.source != WireId() ? ctx->getBoundWireNet(st.source) : nullptr;
    if (src_net != nullptr && src_net != ni)
        return "the source wire of net " + q(ni->name.str(ctx)) + " is used by net " + q(src_net->name.str(ctx));
    // Unrouted: to the sink wire. Partial: to the top of the sink's chain (bound without a pip), or
    // to the unbound wire driving it
    WireId target = sink;
    if (arc->state == NetStatus::ArcState::PARTIAL && !arc->path.empty()) {
        target = arc->path.back();
        PipId pip = ni->wires.at(target).pip;
        if (pip != PipId())
            target = ctx->getPipSrcWire(pip);
    }
    std::vector<WireId> starts = AutoRoute::treeWires(ctx, ni);
    if (starts.empty())
        return "net " + q(ni->name.str(ctx)) + " has no source wire";
    AutoRoute::Result res;
    if (!AutoRoute::findPath(ctx, ni, starts, target, res))
        return "no path to " + arc_name + ": " + res.why;
    if (out)
        *out = res;
    return addRoutePath(b, ni->name, res);
}

} // namespace

std::string addRouteArc(EditBatch &b, IdString net, WireId sink, AutoRoute::Result *out)
{
    Context *ctx = b.getContext();
    NetInfo *ni = findNet(ctx, net);
    if (ni == nullptr)
        return "net " + q(net.str(ctx)) + " does not exist";
    bool skipped = false;
    std::string err = routeArc(b, ni, sink, out, &skipped);
    if (err.empty() && skipped)
        return "the arc to " + q(ctx->nameOfWire(sink)) + " is already routed";
    return err;
}

std::string addRouteNetRest(EditBatch &b, IdString net, int *routed, int *failed, std::string *report)
{
    Context *ctx = b.getContext();
    NetInfo *ni = findNet(ctx, net);
    if (ni == nullptr)
        return "net " + q(net.str(ctx)) + " does not exist";
    NetStatus::Status st = NetStatus::compute(ctx, ni);
    int n_ok = 0, n_fail = 0;
    std::string rep;
    for (auto &arc : st.arcs) {
        if (arc.state != NetStatus::ArcState::UNROUTED && arc.state != NetStatus::ArcState::PARTIAL)
            continue;
        size_t mark = b.size();
        bool skipped = false;
        std::string err = routeArc(b, ni, arc.sink, nullptr, &skipped);
        if (!err.empty()) {
            // Keep nothing of a path that could only be bound in part
            b.rollbackTo(mark);
            n_fail++;
            if (!rep.empty())
                rep += "; ";
            rep += err;
        } else if (!skipped) {
            n_ok++;
        }
    }
    if (routed)
        *routed = n_ok;
    if (failed)
        *failed = n_fail;
    if (report)
        *report = rep;
    if (n_ok == 0 && n_fail == 0)
        return "net " + q(net.str(ctx)) + " has no unrouted arcs";
    if (n_ok == 0)
        return rep;
    return {};
}

std::string addLockRouting(EditBatch &b, IdString net, bool lock)
{
    Context *ctx = b.getContext();
    NetInfo *ni = findNet(ctx, net);
    if (ni == nullptr)
        return "net " + q(net.str(ctx)) + " does not exist";
    if (ni->wires.empty())
        return "net " + q(net.str(ctx)) + " has no routing";
    PlaceStrength strength = lock ? STRENGTH_USER : STRENGTH_STRONG;
    bool change = false;
    for (auto &w : ni->wires)
        change |= lock ? w.second.strength != strength : w.second.strength > STRENGTH_STRONG;
    if (!change)
        return "the routing of net " + q(net.str(ctx)) + (lock ? " is already locked" : " is not locked");
    return b.add(std::make_unique<SetNetStrengthCmd>(net, strength));
}

// ---------------------------------------------------------------------------
// GUI actions

namespace {

// Delay of a path in ns (pips and their destination wires)
float pathDelay(Context *ctx, const AutoRoute::Result &res)
{
    delay_t d = 0;
    for (PipId pip : res.path)
        d += ctx->getPipDelay(pip).maxDelay() + ctx->getWireDelay(ctx->getPipDstWire(pip)).maxDelay();
    return ctx->getDelayNS(d);
}

std::string netSummary(Context *ctx, IdString net)
{
    std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
    std::lock_guard<std::mutex> lock(ctx->mutex);
    NetInfo *ni = findNet(ctx, net);
    return ni ? NetStatus::summary(NetStatus::compute(ctx, ni)) : std::string();
}

} // namespace

std::string runRouteArc(EditStack *es, IdString net, WireId sink)
{
    Context *ctx = es->getContext();
    if (ctx == nullptr)
        return "no design";
    QString name = ctx->nameOfWire(sink);
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        NetInfo *ni = findNet(ctx, net);
        if (ni != nullptr)
            for (auto &arc : NetStatus::compute(ctx, ni).arcs)
                if (arc.sink == sink)
                    name = QString("%1.%2").arg(arc.cell.c_str(ctx)).arg(arc.port.c_str(ctx));
    }
    AutoRoute::Result res;
    QString text = QString("Route arc %1 -> %2").arg(net.c_str(ctx)).arg(name);
    std::string err = es->run(text, [&](EditBatch &b) { return addRouteArc(b, net, sink, &res); });
    if (err.empty())
        es->notify(QString("%1: %2 pips, %3 ns (%4)")
                           .arg(text)
                           .arg(res.path.size())
                           .arg(pathDelay(ctx, res), 0, 'f', 3)
                           .arg(QString::fromStdString(netSummary(ctx, net))));
    return err;
}

std::string runRouteNetRest(EditStack *es, IdString net)
{
    Context *ctx = es->getContext();
    if (ctx == nullptr)
        return "no design";
    int routed = 0, failed = 0;
    std::string report;
    QString text = QString("Route rest of net %1").arg(net.c_str(ctx));
    std::string err =
            es->run(text, [&](EditBatch &b) { return addRouteNetRest(b, net, &routed, &failed, &report); });
    if (err.empty()) {
        QString msg = QString("%1: %2 arcs routed").arg(text).arg(routed);
        if (failed > 0)
            msg += QString(", %1 without a path (%2)").arg(failed).arg(QString::fromStdString(report));
        msg += QString(" (%1)").arg(QString::fromStdString(netSummary(ctx, net)));
        es->notify(msg);
    }
    return err;
}

std::string runLockRouting(EditStack *es, IdString net, bool lock)
{
    Context *ctx = es->getContext();
    if (ctx == nullptr)
        return "no design";
    QString text = QString("%1 routing of %2").arg(lock ? "Lock" : "Unlock").arg(net.c_str(ctx));
    return es->run(text, [&](EditBatch &b) { return addLockRouting(b, net, lock); });
}

void addAutoRouteActions(QMenu &menu, EditStack *es, IdString net, const std::function<void()> &after)
{
    Context *ctx = es ? es->getContext() : nullptr;
    if (ctx == nullptr || es->busy())
        return;
    QString why;
    bool can_edit = es->canEdit(&why);
    std::vector<std::pair<QString, WireId>> arcs; // unrouted / partial, routable
    int blocked = 0;
    bool any_wire = false, locked = false, has_source = false;
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        NetInfo *ni = findNet(ctx, net);
        if (ni == nullptr)
            return;
        NetStatus::Status st = NetStatus::compute(ctx, ni);
        has_source = st.source != WireId();
        for (auto &arc : st.arcs) {
            if (arc.state != NetStatus::ArcState::UNROUTED && arc.state != NetStatus::ArcState::PARTIAL)
                continue;
            if (arc.blocked) {
                blocked++;
                continue;
            }
            arcs.emplace_back(QString("%1.%2").arg(arc.cell.c_str(ctx)).arg(arc.port.c_str(ctx)), arc.sink);
        }
        any_wire = !ni->wires.empty();
        locked = AutoRoute::isRoutingLocked(ni);
    }
    menu.setToolTipsVisible(true);
    auto add = [&](QMenu &m, const QString &text, const QString &refused, const QString &tip = QString()) {
        QAction *a = m.addAction(text);
        a->setEnabled(can_edit && refused.isEmpty());
        a->setToolTip(!can_edit ? why : !refused.isEmpty() ? refused : tip);
        return a;
    };
    QString netName = net.c_str(ctx);
    QString no_arc;
    if (!has_source)
        no_arc = "the net has no source wire";
    else if (arcs.empty() && blocked > 0)
        no_arc = QString("%1 arcs blocked: their sink wire is used by another net").arg(blocked);
    else if (arcs.empty())
        no_arc = "all arcs are routed";
    const QString arc_tip = "Bind the shortest free path from the closest wire of the net's routing to the sink";
    if (arcs.size() <= 1) {
        QAction *a = add(menu,
                         arcs.empty() ? QString("Route arc (shortest free path)")
                                      : QString("Route arc to %1 (shortest free path)").arg(arcs[0].first),
                         no_arc, arc_tip);
        if (!arcs.empty()) {
            WireId sink = arcs[0].second;
            QObject::connect(a, &QAction::triggered, &menu, [es, net, sink, after] {
                if (runRouteArc(es, net, sink).empty() && after)
                    after();
            });
        }
    } else {
        QMenu *sub = menu.addMenu(QString("Route arc (%1 unrouted)").arg(arcs.size()));
        sub->setEnabled(can_edit);
        sub->setToolTipsVisible(true);
        const size_t max_arcs = 40;
        for (size_t i = 0; i < arcs.size() && i < max_arcs; i++) {
            WireId sink = arcs[i].second;
            QAction *a = add(*sub, "to " + arcs[i].first, QString(), arc_tip);
            QObject::connect(a, &QAction::triggered, &menu, [es, net, sink, after] {
                if (runRouteArc(es, net, sink).empty() && after)
                    after();
            });
        }
        if (arcs.size() > max_arcs)
            sub->addAction(QString("... %1 more").arg(arcs.size() - max_arcs))->setEnabled(false);
    }
    QAction *rest = add(menu, QString("Route rest of net %1").arg(netName), no_arc,
                        "Route every unrouted arc of the net (existing pips stay), as one undo entry");
    QObject::connect(rest, &QAction::triggered, &menu, [es, net, after] {
        if (runRouteNetRest(es, net).empty() && after)
            after();
    });
    QAction *lock = add(menu, QString("%1 routing of %2").arg(locked ? "Unlock" : "Lock").arg(netName),
                        any_wire ? QString() : QString("the net has no routing"),
                        locked ? "Set the routing strength of all its wires to STRONG"
                               : "Set the routing strength of all its wires to USER: rip-up and route mode "
                                 "refuse to change them");
    QObject::connect(lock, &QAction::triggered, &menu, [es, net, locked, after] {
        if (runLockRouting(es, net, !locked).empty() && after)
            after();
    });
}

NEXTPNR_NAMESPACE_END
