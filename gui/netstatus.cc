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

#include "netstatus.h"
#include <algorithm>
#include <limits>

NEXTPNR_NAMESPACE_BEGIN

namespace NetStatus {

const char *stateName(ArcState s)
{
    switch (s) {
    case ArcState::ROUTED:
        return "routed";
    case ArcState::PARTIAL:
        return "partial";
    case ArcState::UNROUTED:
        return "unrouted";
    case ArcState::NO_WIRE:
        return "no wire";
    }
    return "?";
}

namespace {

bool isRoot(Context *ctx, NetInfo *net, WireId wire, WireId source)
{
    if (wire == source)
        return true;
    return net->constant_value != IdString() && ctx->getWireConstantValue(wire) == net->constant_value;
}

void walkArc(Context *ctx, NetInfo *net, WireId source, Arc &arc)
{
    WireId cursor = arc.sink;
    // A loop in the pips cannot be longer than the net
    size_t limit = net->wires.size() + 1;
    while (limit-- > 0) {
        auto fnd = net->wires.find(cursor);
        if (fnd == net->wires.end()) {
            arc.state = cursor == arc.sink ? ArcState::UNROUTED : ArcState::PARTIAL;
            return;
        }
        arc.path.push_back(cursor);
        PipId pip = fnd->second.pip;
        if (pip == PipId()) {
            arc.state = isRoot(ctx, net, cursor, source) ? ArcState::ROUTED : ArcState::PARTIAL;
            return;
        }
        cursor = ctx->getPipSrcWire(pip);
    }
    arc.state = ArcState::PARTIAL;
}

} // namespace

Status compute(Context *ctx, NetInfo *net)
{
    Status st;
    if (net == nullptr)
        return st;
    st.net = net->name;
    st.source = ctx->getNetinfoSourceWire(net);
    if (st.source != WireId()) {
        NetInfo *bound = ctx->getBoundWireNet(st.source);
        if (bound != nullptr && bound != net)
            st.conflicts++;
    }
    for (auto usr : net->users.enumerate()) {
        const PortRef &ref = usr.value;
        size_t phys = 0;
        for (WireId sink : ctx->getNetinfoSinkWires(net, ref)) {
            Arc arc;
            arc.user = usr.index;
            arc.phys_idx = phys++;
            arc.cell = ref.cell != nullptr ? ref.cell->name : IdString();
            arc.port = ref.port;
            arc.sink = sink;
            if (sink == WireId() || (st.source == WireId() && net->constant_value == IdString())) {
                arc.state = ArcState::NO_WIRE;
            } else if (sink == st.source) {
                arc.state = ctx->getBoundWireNet(sink) == net ? ArcState::ROUTED : ArcState::UNROUTED;
                if (arc.state == ArcState::ROUTED)
                    arc.path.push_back(sink);
            } else {
                walkArc(ctx, net, st.source, arc);
            }
            if (arc.state == ArcState::UNROUTED) {
                NetInfo *bound = ctx->getBoundWireNet(sink);
                if (bound != nullptr && bound != net) {
                    arc.blocked = true;
                    st.conflicts++;
                }
            }
            st.arcs.push_back(std::move(arc));
        }
        if (phys == 0) {
            Arc arc;
            arc.user = usr.index;
            arc.cell = ref.cell != nullptr ? ref.cell->name : IdString();
            arc.port = ref.port;
            arc.state = ArcState::NO_WIRE;
            st.arcs.push_back(std::move(arc));
        }
    }
    for (auto &arc : st.arcs) {
        switch (arc.state) {
        case ArcState::ROUTED:
            st.routed++;
            break;
        case ArcState::PARTIAL:
            st.partial++;
            break;
        case ArcState::UNROUTED:
            st.unrouted++;
            break;
        case ArcState::NO_WIRE:
            st.no_wire++;
            break;
        }
    }
    for (auto &w : net->wires) {
        PipId pip = w.second.pip;
        if (pip != PipId() && !net->wires.count(ctx->getPipSrcWire(pip)))
            st.dangling++;
    }
    return st;
}

std::string summary(const Status &st)
{
    std::string s = std::to_string(st.routed) + "/" + std::to_string(st.routable()) + " arcs routed, " +
                    std::to_string(st.conflicts) + (st.conflicts == 1 ? " conflict" : " conflicts");
    if (st.partial > 0)
        s += ", " + std::to_string(st.partial) + " partial";
    if (st.dangling > 0)
        s += ", " + std::to_string(st.dangling) + " dangling";
    if (st.no_wire > 0)
        s += ", " + std::to_string(st.no_wire) + " unplaced";
    return s;
}

dict<WireId, std::vector<WireId>> downstream(Context *ctx, NetInfo *net)
{
    dict<WireId, std::vector<WireId>> down;
    for (auto &w : net->wires)
        if (w.second.pip != PipId())
            down[ctx->getPipSrcWire(w.second.pip)].push_back(w.first);
    return down;
}

std::vector<WireId> subtree(const dict<WireId, std::vector<WireId>> &down, WireId wire)
{
    std::vector<WireId> result{wire};
    pool<WireId> seen{wire};
    for (size_t i = 0; i < result.size(); i++) {
        auto fnd = down.find(result[i]);
        if (fnd == down.end())
            continue;
        for (WireId child : fnd->second)
            if (seen.insert(child).second)
                result.push_back(child);
    }
    return result;
}

std::vector<WireId> arcOnlyWires(Context *ctx, NetInfo *net, const Status &st, WireId sink)
{
    std::vector<WireId> result;
    const Arc *arc = nullptr;
    pool<WireId> other_sinks;
    for (auto &a : st.arcs) {
        if (a.sink == sink && arc == nullptr)
            arc = &a;
        else if (a.sink != WireId() && a.state != ArcState::UNROUTED)
            other_sinks.insert(a.sink);
    }
    if (arc == nullptr || arc->path.empty())
        return result;
    auto down = downstream(ctx, net);
    pool<WireId> removed;
    for (size_t i = 0; i < arc->path.size(); i++) {
        WireId w = arc->path[i];
        if (w == st.source || isRoot(ctx, net, w, st.source))
            break;
        if (i > 0 && other_sinks.count(w))
            break;
        if (i == 0) {
            // Routing below the sink wire (e.g. GateMate's LUT-input routing inside the CPE, from the
            // bel pin wire on) belongs to this arc, unless it reaches another sink
            std::vector<WireId> tail = subtree(down, w);
            bool shared = false;
            for (size_t j = 1; j < tail.size(); j++)
                shared |= other_sinks.count(tail[j]) > 0;
            if (shared)
                break;
            for (WireId t : tail) {
                result.push_back(t);
                removed.insert(t);
            }
            continue;
        }
        // Drives anything else of the net (other than what is removed already)?
        bool other = false;
        auto fnd = down.find(w);
        if (fnd != down.end())
            for (WireId child : fnd->second)
                if (!removed.count(child))
                    other = true;
        if (other)
            break;
        result.push_back(w);
        removed.insert(w);
    }
    return result;
}

bool belCentre(Context *ctx, BelId bel, float &x, float &y)
{
    if (bel == BelId())
        return false;
    DecalXY decal = ctx->getBelDecal(bel);
    float x0 = std::numeric_limits<float>::infinity(), y0 = x0, x1 = -x0, y1 = -x0;
    for (auto &el : ctx->getDecalGraphics(decal.decal)) {
        if (el.type != GraphicElement::TYPE_BOX)
            continue;
        x0 = std::min(x0, decal.x + std::min(el.x1, el.x2));
        x1 = std::max(x1, decal.x + std::max(el.x1, el.x2));
        y0 = std::min(y0, decal.y + std::min(el.y1, el.y2));
        y1 = std::max(y1, decal.y + std::max(el.y1, el.y2));
    }
    if (x0 <= x1) {
        x = (x0 + x1) / 2;
        y = (y0 + y1) / 2;
        return true;
    }
    Loc loc = ctx->getBelLocation(bel);
    x = ctx->getGuiColumnX(loc.x) + 0.5f;
    y = loc.y + 0.5f;
    return true;
}

} // namespace NetStatus

NEXTPNR_NAMESPACE_END
