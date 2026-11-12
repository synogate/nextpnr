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

#include "editcommands.h"
#include <algorithm>
#include "log.h"
#include "netstatus.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {

CellInfo *findCell(Context *ctx, IdString name)
{
    auto fnd = ctx->cells.find(name);
    return fnd == ctx->cells.end() ? nullptr : fnd->second.get();
}

NetInfo *findNet(Context *ctx, IdString name)
{
    auto fnd = ctx->nets.find(name);
    return fnd == ctx->nets.end() ? nullptr : fnd->second.get();
}

const char *strengthName(PlaceStrength s)
{
    switch (s) {
    case STRENGTH_NONE:
        return "NONE";
    case STRENGTH_WEAK:
        return "WEAK";
    case STRENGTH_STRONG:
        return "STRONG";
    case STRENGTH_PLACER:
        return "PLACER";
    case STRENGTH_FIXED:
        return "FIXED";
    case STRENGTH_LOCKED:
        return "LOCKED";
    case STRENGTH_USER:
        return "USER";
    }
    return "?";
}

std::string q(const std::string &s) { return "'" + s + "'"; }

} // namespace

// ---------------------------------------------------------------------------
// BindBelCmd

std::string BindBelCmd::checkApply(Context *ctx)
{
    if (bel == BelId())
        return "invalid bel";
    CellInfo *ci = findCell(ctx, cell);
    if (ci == nullptr)
        return "cell " + q(cell.str(ctx)) + " does not exist";
    if (ci->bel != BelId())
        return "cell " + q(cell.str(ctx)) + " is already placed at " + q(ctx->nameOfBel(ci->bel));
    if (!ctx->checkBelAvail(bel)) {
        CellInfo *other = ctx->getBoundBelCell(bel);
        if (other != nullptr)
            return "bel " + q(ctx->nameOfBel(bel)) + " is occupied by " + q(other->name.str(ctx));
        return "bel " + q(ctx->nameOfBel(bel)) + " is not available";
    }
    if (!ctx->isValidBelForCellType(ci->type, bel))
        return "cell type " + q(ci->type.str(ctx)) + " cannot be placed on bel " + q(ctx->nameOfBel(bel)) +
               " of type " + q(ctx->getBelType(bel).str(ctx));
    return {};
}

void BindBelCmd::apply(Context *ctx) { ctx->bindBel(bel, findCell(ctx, cell), strength); }

std::string BindBelCmd::postApply(Context *ctx)
{
    if (check_location && !ctx->isBelLocationValid(bel))
        return "placing " + q(cell.str(ctx)) + " at " + q(ctx->nameOfBel(bel)) + " violates location constraints";
    return {};
}

std::string BindBelCmd::checkRevert(Context *ctx)
{
    CellInfo *ci = findCell(ctx, cell);
    if (ci == nullptr)
        return "cell " + q(cell.str(ctx)) + " does not exist";
    if (ci->bel != bel)
        return "cell " + q(cell.str(ctx)) + " is no longer placed at " + q(ctx->nameOfBel(bel));
    return {};
}

void BindBelCmd::revert(Context *ctx) { ctx->unbindBel(bel); }

std::string BindBelCmd::describe(Context *ctx) const
{
    return "Place " + cell.str(ctx) + " at " + ctx->nameOfBel(bel);
}

// ---------------------------------------------------------------------------
// UnbindBelCmd

std::string UnbindBelCmd::checkApply(Context *ctx)
{
    if (bel == BelId())
        return "invalid bel";
    if (ctx->getBoundBelCell(bel) == nullptr)
        return "bel " + q(ctx->nameOfBel(bel)) + " is empty";
    return {};
}

void UnbindBelCmd::apply(Context *ctx)
{
    CellInfo *ci = ctx->getBoundBelCell(bel);
    prev_cell = ci->name;
    prev_strength = ci->belStrength;
    ctx->unbindBel(bel);
}

std::string UnbindBelCmd::checkRevert(Context *ctx)
{
    CellInfo *ci = findCell(ctx, prev_cell);
    if (ci == nullptr)
        return "cell " + q(prev_cell.str(ctx)) + " does not exist";
    if (ci->bel != BelId())
        return "cell " + q(prev_cell.str(ctx)) + " has been placed elsewhere";
    if (!ctx->checkBelAvail(bel))
        return "bel " + q(ctx->nameOfBel(bel)) + " is no longer free";
    return {};
}

void UnbindBelCmd::revert(Context *ctx) { ctx->bindBel(bel, findCell(ctx, prev_cell), prev_strength); }

std::string UnbindBelCmd::describe(Context *ctx) const
{
    if (prev_cell != IdString())
        return "Unplace " + prev_cell.str(ctx) + " from " + ctx->nameOfBel(bel);
    return std::string("Unbind bel ") + ctx->nameOfBel(bel);
}

// ---------------------------------------------------------------------------
// BindPipCmd

std::string BindPipCmd::checkApply(Context *ctx)
{
    if (pip == PipId())
        return "invalid pip";
    NetInfo *ni = findNet(ctx, net);
    if (ni == nullptr)
        return "net " + q(net.str(ctx)) + " does not exist";
    NetInfo *bound = ctx->getBoundPipNet(pip);
    if (bound != nullptr)
        return "pip " + q(ctx->nameOfPip(pip)) + " is already used by net " + q(bound->name.str(ctx));
    WireId dst = ctx->getPipDstWire(pip);
    NetInfo *dst_net = ctx->getBoundWireNet(dst);
    if (dst_net != nullptr)
        return "wire " + q(ctx->nameOfWire(dst)) + " is already used by net " + q(dst_net->name.str(ctx));
    if (!ctx->checkWireAvail(dst))
        return "wire " + q(ctx->nameOfWire(dst)) + " is not available";
    if (!ctx->checkPipAvailForNet(pip, ni))
        return "pip " + q(ctx->nameOfPip(pip)) + " is not available for net " + q(net.str(ctx));
    std::string port;
    if (NetInfo *owner = NetStatus::sinkOfOtherNet(ctx, dst, ni, &port))
        return "wire " + q(ctx->nameOfWire(dst)) + " is the sink of net " + q(owner->name.str(ctx)) + " (" + port + ")";
    return {};
}

void BindPipCmd::apply(Context *ctx) { ctx->bindPip(pip, findNet(ctx, net), strength); }

std::string BindPipCmd::checkRevert(Context *ctx)
{
    NetInfo *ni = findNet(ctx, net);
    if (ni == nullptr || ctx->getBoundPipNet(pip) != ni)
        return "pip " + q(ctx->nameOfPip(pip)) + " is no longer bound to net " + q(net.str(ctx));
    return {};
}

void BindPipCmd::revert(Context *ctx) { ctx->unbindPip(pip); }

std::string BindPipCmd::describe(Context *ctx) const
{
    return "Bind pip " + std::string(ctx->nameOfPip(pip)) + " to " + net.str(ctx);
}

// ---------------------------------------------------------------------------
// UnbindPipCmd

std::string UnbindPipCmd::checkApply(Context *ctx)
{
    if (pip == PipId())
        return "invalid pip";
    if (ctx->getBoundPipNet(pip) == nullptr)
        return "pip " + q(ctx->nameOfPip(pip)) + " is not used";
    return {};
}

void UnbindPipCmd::apply(Context *ctx)
{
    NetInfo *ni = ctx->getBoundPipNet(pip);
    prev_net = ni->name;
    auto fnd = ni->wires.find(ctx->getPipDstWire(pip));
    prev_strength = fnd != ni->wires.end() ? fnd->second.strength : STRENGTH_NONE;
    ctx->unbindPip(pip);
}

std::string UnbindPipCmd::checkRevert(Context *ctx)
{
    NetInfo *ni = findNet(ctx, prev_net);
    if (ni == nullptr)
        return "net " + q(prev_net.str(ctx)) + " does not exist";
    if (ctx->getBoundPipNet(pip) != nullptr)
        return "pip " + q(ctx->nameOfPip(pip)) + " is used again";
    WireId dst = ctx->getPipDstWire(pip);
    if (ctx->getBoundWireNet(dst) != nullptr || !ctx->checkWireAvail(dst))
        return "wire " + q(ctx->nameOfWire(dst)) + " is used again";
    if (!ctx->checkPipAvailForNet(pip, ni))
        return "pip " + q(ctx->nameOfPip(pip)) + " is not available for net " + q(prev_net.str(ctx));
    return {};
}

void UnbindPipCmd::revert(Context *ctx) { ctx->bindPip(pip, findNet(ctx, prev_net), prev_strength); }

std::string UnbindPipCmd::describe(Context *ctx) const { return "Unbind pip " + std::string(ctx->nameOfPip(pip)); }

// ---------------------------------------------------------------------------
// BindWireCmd

std::string BindWireCmd::checkApply(Context *ctx)
{
    if (wire == WireId())
        return "invalid wire";
    if (findNet(ctx, net) == nullptr)
        return "net " + q(net.str(ctx)) + " does not exist";
    NetInfo *bound = ctx->getBoundWireNet(wire);
    if (bound != nullptr)
        return "wire " + q(ctx->nameOfWire(wire)) + " is already used by net " + q(bound->name.str(ctx));
    if (!ctx->checkWireAvail(wire))
        return "wire " + q(ctx->nameOfWire(wire)) + " is not available";
    return {};
}

void BindWireCmd::apply(Context *ctx) { ctx->bindWire(wire, findNet(ctx, net), strength); }

std::string BindWireCmd::checkRevert(Context *ctx)
{
    NetInfo *ni = findNet(ctx, net);
    if (ni == nullptr || ctx->getBoundWireNet(wire) != ni)
        return "wire " + q(ctx->nameOfWire(wire)) + " is no longer bound to net " + q(net.str(ctx));
    auto fnd = ni->wires.find(wire);
    if (fnd == ni->wires.end() || fnd->second.pip != PipId())
        return "wire " + q(ctx->nameOfWire(wire)) + " is now driven by a pip";
    return {};
}

void BindWireCmd::revert(Context *ctx) { ctx->unbindWire(wire); }

std::string BindWireCmd::describe(Context *ctx) const
{
    return "Bind wire " + std::string(ctx->nameOfWire(wire)) + " to " + net.str(ctx);
}

// ---------------------------------------------------------------------------
// UnbindWireCmd

std::string UnbindWireCmd::checkApply(Context *ctx)
{
    if (wire == WireId())
        return "invalid wire";
    if (ctx->getBoundWireNet(wire) == nullptr)
        return "wire " + q(ctx->nameOfWire(wire)) + " is not used";
    return {};
}

void UnbindWireCmd::apply(Context *ctx)
{
    NetInfo *ni = ctx->getBoundWireNet(wire);
    prev_net = ni->name;
    auto fnd = ni->wires.find(wire);
    prev_pip = fnd != ni->wires.end() ? fnd->second.pip : PipId();
    prev_strength = fnd != ni->wires.end() ? fnd->second.strength : STRENGTH_NONE;
    // Unbind through the pip if there is one, so the arch pip hooks (timing, uarch state) run.
    if (prev_pip != PipId())
        ctx->unbindPip(prev_pip);
    else
        ctx->unbindWire(wire);
}

std::string UnbindWireCmd::checkRevert(Context *ctx)
{
    NetInfo *ni = findNet(ctx, prev_net);
    if (ni == nullptr)
        return "net " + q(prev_net.str(ctx)) + " does not exist";
    if (ctx->getBoundWireNet(wire) != nullptr || !ctx->checkWireAvail(wire))
        return "wire " + q(ctx->nameOfWire(wire)) + " is used again";
    if (prev_pip != PipId() && (ctx->getBoundPipNet(prev_pip) != nullptr || !ctx->checkPipAvailForNet(prev_pip, ni)))
        return "pip " + q(ctx->nameOfPip(prev_pip)) + " is used again";
    return {};
}

void UnbindWireCmd::revert(Context *ctx)
{
    NetInfo *ni = findNet(ctx, prev_net);
    if (prev_pip != PipId())
        ctx->bindPip(prev_pip, ni, prev_strength);
    else
        ctx->bindWire(wire, ni, prev_strength);
}

std::string UnbindWireCmd::describe(Context *ctx) const
{
    return "Unbind wire " + std::string(ctx->nameOfWire(wire));
}

// ---------------------------------------------------------------------------
// SetCellStrengthCmd

std::string SetCellStrengthCmd::checkApply(Context *ctx)
{
    if (findCell(ctx, cell) == nullptr)
        return "cell " + q(cell.str(ctx)) + " does not exist";
    return {};
}

void SetCellStrengthCmd::apply(Context *ctx)
{
    CellInfo *ci = findCell(ctx, cell);
    prev_strength = ci->belStrength;
    ci->belStrength = strength;
    if (ci->bel != BelId())
        ctx->refreshUiBel(ci->bel);
}

std::string SetCellStrengthCmd::checkRevert(Context *ctx) { return checkApply(ctx); }

void SetCellStrengthCmd::revert(Context *ctx)
{
    CellInfo *ci = findCell(ctx, cell);
    ci->belStrength = prev_strength;
    if (ci->bel != BelId())
        ctx->refreshUiBel(ci->bel);
}

std::string SetCellStrengthCmd::describe(Context *ctx) const
{
    return "Set strength of " + cell.str(ctx) + " to " + strengthName(strength);
}

// ---------------------------------------------------------------------------
// SetNetStrengthCmd

std::string SetNetStrengthCmd::checkApply(Context *ctx)
{
    if (findNet(ctx, net) == nullptr)
        return "net " + q(net.str(ctx)) + " does not exist";
    return {};
}

void SetNetStrengthCmd::apply(Context *ctx)
{
    NetInfo *ni = findNet(ctx, net);
    prev.clear();
    for (auto &w : ni->wires) {
        prev.emplace_back(w.first, w.second.strength);
        w.second.strength = strength;
    }
}

std::string SetNetStrengthCmd::checkRevert(Context *ctx)
{
    NetInfo *ni = findNet(ctx, net);
    if (ni == nullptr)
        return "net " + q(net.str(ctx)) + " does not exist";
    for (auto &p : prev)
        if (!ni->wires.count(p.first))
            return "routing of net " + q(net.str(ctx)) + " has changed";
    return {};
}

void SetNetStrengthCmd::revert(Context *ctx)
{
    NetInfo *ni = findNet(ctx, net);
    for (auto &p : prev)
        ni->wires.at(p.first).strength = p.second;
}

std::string SetNetStrengthCmd::describe(Context *ctx) const
{
    return "Set routing strength of " + net.str(ctx) + " to " + strengthName(strength);
}

// ---------------------------------------------------------------------------
// PrepareCellForMoveCmd

std::vector<PrepareCellForMoveCmd::PortState> PrepareCellForMoveCmd::ports(const CellInfo *ci)
{
    std::vector<PortState> result;
    for (auto &p : ci->ports)
        result.push_back({p.first, p.second.type, p.second.net ? p.second.net->name : IdString()});
    std::sort(result.begin(), result.end(),
              [](const PortState &a, const PortState &b) { return a.name.index < b.name.index; });
    return result;
}

std::string PrepareCellForMoveCmd::checkApply(Context *ctx)
{
    CellInfo *ci = findCell(ctx, cell);
    if (ci == nullptr)
        return "cell " + q(cell.str(ctx)) + " does not exist";
    std::string err = ctx->prepareCellForMove(ci, true);
    if (!err.empty())
        return "cannot move " + q(cell.str(ctx)) + " of the routed design: " + err;
    return {};
}

void PrepareCellForMoveCmd::record(CellInfo *ci)
{
    cell = ci->name;
    prev_ports = ports(ci);
    prev_params = ci->params;
    prev_bel_pins = ci->cell_bel_pins;
}

void PrepareCellForMoveCmd::apply(Context *ctx)
{
    CellInfo *ci = findCell(ctx, cell);
    record(ci);
    ctx->prepareCellForMove(ci, false);
    new_ports = ports(ci);
}

std::string PrepareCellForMoveCmd::checkRevert(Context *ctx)
{
    CellInfo *ci = findCell(ctx, cell);
    if (ci == nullptr)
        return "cell " + q(cell.str(ctx)) + " does not exist";
    auto now = ports(ci);
    if (now.size() != new_ports.size())
        return "ports of " + q(cell.str(ctx)) + " have changed";
    for (size_t i = 0; i < now.size(); i++)
        if (now[i].name != new_ports[i].name || now[i].net != new_ports[i].net)
            return "ports of " + q(cell.str(ctx)) + " have changed";
    for (auto &p : prev_ports)
        if (p.net != IdString() && findNet(ctx, p.net) == nullptr)
            return "net " + q(p.net.str(ctx)) + " does not exist";
    return {};
}

void PrepareCellForMoveCmd::revert(Context *ctx)
{
    CellInfo *ci = findCell(ctx, cell);
    if (prev_ports.size() == new_ports.size()) {
        bool same = true;
        for (size_t i = 0; i < prev_ports.size() && same; i++)
            same = prev_ports[i].name == new_ports[i].name && prev_ports[i].net == new_ports[i].net;
        if (same && ci->params == prev_params)
            return; // (nothing was changed)
    }
    for (auto &p : new_ports)
        ci->disconnectPort(p.name);
    for (auto &p : new_ports)
        ci->ports.erase(p.name);
    for (auto &p : prev_ports) {
        ci->ports[p.name].name = p.name;
        ci->ports[p.name].type = p.type;
        if (p.net != IdString())
            ci->connectPort(p.name, findNet(ctx, p.net));
    }
    ci->params = prev_params;
    ci->cell_bel_pins = prev_bel_pins;
}

std::string PrepareCellForMoveCmd::describe(Context *ctx) const
{
    return "Undo the post-route changes of " + cell.str(ctx);
}

std::string RetargetSinkCmd::checkApply(Context *ctx)
{
    CellInfo *ci = findCell(ctx, cell);
    if (ci == nullptr)
        return "cell " + q(cell.str(ctx)) + " does not exist";
    std::string err = ctx->applyRouteRetarget(ci, port, wire, true);
    if (!err.empty())
        return "cannot move " + q(cell.str(ctx) + "." + port.str(ctx)) + " to " + q(ctx->nameOfWire(wire)) + ": " +
               err;
    return {};
}

void RetargetSinkCmd::apply(Context *ctx)
{
    CellInfo *ci = findCell(ctx, cell);
    snapshot.record(ci);
    ctx->applyRouteRetarget(ci, port, wire, false);
    snapshot.new_ports = PrepareCellForMoveCmd::ports(ci);
}

std::string RetargetSinkCmd::checkRevert(Context *ctx) { return snapshot.checkRevert(ctx); }

void RetargetSinkCmd::revert(Context *ctx) { snapshot.revert(ctx); }

std::string RetargetSinkCmd::describe(Context *ctx) const
{
    return "Move " + cell.str(ctx) + "." + port.str(ctx) + " to " + ctx->nameOfWire(wire);
}

// ---------------------------------------------------------------------------
// Placement helpers

std::vector<CellInfo *> placementGroup(Context *ctx, CellInfo *ci)
{
    if (ci->cluster == ClusterId())
        return {ci};
    CellInfo *root = ctx->getClusterRootCell(ci->cluster);
    std::vector<CellInfo *> group{root};
    for (auto &c : ctx->cells)
        if (c.second->cluster == ci->cluster && c.second.get() != root)
            group.push_back(c.second.get());
    return group;
}

std::string cellFixedReason(Context *ctx, const CellInfo *ci)
{
    if (!isCellFixed(ci))
        return {};
    return q(ci->name.str(ctx)) + " is fixed by the packer to " + q(ctx->nameOfBel(ci->bel)) +
           " [FIXED: tied to its IO/RAM/clock site or routing] and cannot be moved";
}

std::vector<WireId> cellRouting(Context *ctx, const CellInfo *ci)
{
    std::vector<WireId> result;
    if (ci == nullptr || ci->bel == BelId())
        return result;
    pool<WireId> seen;
    dict<IdString, std::pair<pool<WireId>, dict<WireId, std::vector<WireId>>>> per_net;
    for (auto &port : ci->ports) {
        NetInfo *ni = port.second.net;
        if (ni == nullptr || ni->wires.empty())
            continue;
        if (ni->driver.cell == ci && ni->driver.port == port.first) {
            // The whole tree grows from the source wire at the cell's bel
            for (auto &w : ni->wires)
                if (seen.insert(w.first).second)
                    result.push_back(w.first);
            continue;
        }
        if (!port.second.user_idx)
            continue;
        auto fnd = per_net.find(ni->name);
        if (fnd == per_net.end())
            fnd = per_net.emplace(ni->name, std::make_pair(NetStatus::belowSinkWires(ctx, ni),
                                                           NetStatus::downstream(ctx, ni)))
                          .first;
        const auto &below = fnd->second.first;
        if (below.empty())
            continue;
        for (WireId sink : ctx->getNetinfoSinkWires(ni, ni->users.at(port.second.user_idx)))
            for (WireId w : NetStatus::subtree(fnd->second.second, sink))
                if (below.count(w) && seen.insert(w).second)
                    result.push_back(w);
    }
    return result;
}

std::string addUnbindBel(EditBatch &b, BelId bel)
{
    Context *ctx = b.getContext();
    CellInfo *ci = ctx->getBoundBelCell(bel);
    if (ci != nullptr) {
        // (also when locked: it belongs to the cell, not to the routing of the net above the sink wire)
        for (WireId w : cellRouting(ctx, ci)) {
            std::string err = b.add(std::make_unique<UnbindWireCmd>(w));
            if (!err.empty())
                return err;
        }
        // A cell of a routed design: what the post-route pass changed for the routing at this place goes too.
        // A net the cell was taken off (e.g. CP lines that only reach this place) loses its routing as well.
        pool<IdString> nets_before;
        for (auto &port : ci->ports)
            if (port.second.net != nullptr && port.second.type == PORT_IN)
                nets_before.insert(port.second.net->name);
        IdString name = ci->name;
        std::string err = b.add(std::make_unique<PrepareCellForMoveCmd>(name));
        if (!err.empty())
            return err;
        for (IdString n : nets_before) {
            NetInfo *ni = findNet(ctx, n);
            if (ni == nullptr || !ni->users.empty() || ni->wires.empty())
                continue;
            std::vector<WireId> wires;
            for (auto &w : ni->wires)
                wires.push_back(w.first);
            for (WireId w : wires) {
                if (!ni->wires.count(w))
                    continue; // (unbound with an earlier wire)
                err = b.add(std::make_unique<UnbindWireCmd>(w));
                if (!err.empty())
                    return err;
            }
        }
    }
    return b.add(std::make_unique<UnbindBelCmd>(bel));
}

std::string addPlaceCell(EditBatch &b, IdString cell, BelId bel)
{
    Context *ctx = b.getContext();
    CellInfo *ci = findCell(ctx, cell);
    if (ci == nullptr)
        return "cell " + q(cell.str(ctx)) + " does not exist";
    if (bel == BelId())
        return "invalid bel";
    bool clustered = ci->cluster != ClusterId();
    if (clustered && ctx->getClusterRootCell(ci->cluster) != ci)
        return "cell " + q(cell.str(ctx)) + " is part of the cluster of " +
               q(ctx->getClusterRootCell(ci->cluster)->name.str(ctx)) + ": place the cluster root instead";

    // Where each cell goes
    std::vector<std::pair<CellInfo *, BelId>> targets;
    if (clustered) {
        if (!ctx->isValidBelForCellType(ci->type, bel))
            return "cell type " + q(ci->type.str(ctx)) + " cannot be placed on bel " + q(ctx->nameOfBel(bel)) +
                   " of type " + q(ctx->getBelType(bel).str(ctx));
        if (!ctx->getClusterPlacement(ci->cluster, bel, targets) || targets.empty())
            return "the cluster of " + q(cell.str(ctx)) + " does not fit with its root at " + q(ctx->nameOfBel(bel));
    } else {
        targets.emplace_back(ci, bel);
    }
    for (auto &t : targets) {
        std::string fixed = cellFixedReason(ctx, t.first);
        if (!fixed.empty())
            return fixed;
    }
    BelId root_bel = bel;
    for (auto &t : targets)
        if (t.first == ci)
            root_bel = t.second;
    if (ci->bel == root_bel)
        return q(cell.str(ctx)) + " is already placed at " + q(ctx->nameOfBel(root_bel));

    CellInfo *other = nullptr;
    for (auto &t : targets) {
        CellInfo *c = t.first;
        BelId tb = t.second;
        if (!ctx->isValidBelForCellType(c->type, tb))
            return "cell type " + q(c->type.str(ctx)) + " cannot be placed on bel " + q(ctx->nameOfBel(tb)) +
                   " of type " + q(ctx->getBelType(tb).str(ctx));
        if (!c->testRegion(tb))
            return q(c->name.str(ctx)) + " is constrained to region " + q(c->region->name.str(ctx)) + ", " +
                   q(ctx->nameOfBel(tb)) + " is outside it";
        CellInfo *bound = ctx->getBoundBelCell(tb);
        if (bound == nullptr || bound == c || (clustered && bound->cluster == ci->cluster))
            continue;
        if (clustered)
            return "bel " + q(ctx->nameOfBel(tb)) + " (for " + q(c->name.str(ctx)) + ") is occupied by " +
                   q(bound->name.str(ctx)) + ": a cluster can only be moved to free bels";
        if (bound->cluster != ClusterId())
            return "bel " + q(ctx->nameOfBel(tb)) + " is occupied by " + q(bound->name.str(ctx)) +
                   ", which is part of a cluster: swapping with clusters is not supported";
        if (isCellFixed(bound))
            return "bel " + q(ctx->nameOfBel(tb)) + " is occupied by " + q(bound->name.str(ctx)) +
                   ", which is fixed by the packer [FIXED]";
        if (isCellLocked(bound))
            return "bel " + q(ctx->nameOfBel(tb)) + " is occupied by locked cell " + q(bound->name.str(ctx)) + " [" +
                   strengthName(bound->belStrength) + "]: unlock it first";
        other = bound;
    }
    BelId old_bel = ci->bel;
    bool swap_back = other != nullptr && old_bel != BelId();
    if (swap_back) {
        if (!ctx->isValidBelForCellType(other->type, old_bel))
            return "cannot swap: " + q(other->name.str(ctx)) + " of type " + q(other->type.str(ctx)) +
                   " cannot be placed at " + q(ctx->nameOfBel(old_bel));
        if (!other->testRegion(old_bel))
            return "cannot swap: " + q(other->name.str(ctx)) + " is constrained to region " +
                   q(other->region->name.str(ctx)) + ", " + q(ctx->nameOfBel(old_bel)) + " is outside it";
    }

    std::string err;
    IdString other_name;
    PlaceStrength other_strength = STRENGTH_NONE;
    if (other != nullptr) {
        other_name = other->name;
        other_strength = other->belStrength;
        err = addUnbindBel(b, other->bel);
        if (!err.empty())
            return err;
    }
    std::vector<PlaceStrength> strengths;
    for (auto &t : targets) {
        CellInfo *c = t.first;
        strengths.push_back(c->bel != BelId() && isCellLocked(c) ? c->belStrength : STRENGTH_MANUAL);
        if (c->bel != BelId()) {
            err = addUnbindBel(b, c->bel);
            if (!err.empty())
                return err;
        }
    }
    for (size_t i = 0; i < targets.size(); i++) {
        err = b.add(std::make_unique<BindBelCmd>(targets[i].first->name, targets[i].second, strengths[i], false));
        if (!err.empty())
            return err;
    }
    if (swap_back) {
        err = b.add(std::make_unique<BindBelCmd>(other_name, old_bel, other_strength, false));
        if (!err.empty())
            return err;
    }
    // Location validity once everything is in place
    for (auto &t : targets)
        if (!ctx->isBelLocationValid(t.second))
            return "placing " + q(t.first->name.str(ctx)) + " at " + q(ctx->nameOfBel(t.second)) +
                   " violates location constraints";
    if (swap_back && !ctx->isBelLocationValid(old_bel))
        return "placing " + q(other_name.str(ctx)) + " at " + q(ctx->nameOfBel(old_bel)) +
               " violates location constraints";
    return {};
}

std::string addMoveCells(EditBatch &b, const std::vector<IdString> &cells, int dx, int dy)
{
    Context *ctx = b.getContext();
    if (dx == 0 && dy == 0)
        return "nothing to move (offset 0, 0)";
    if (cells.empty())
        return "no cells to move";
    // The cells to move: whole clusters, each once
    std::vector<CellInfo *> roots;
    pool<IdString> moving;
    for (IdString name : cells) {
        CellInfo *ci = findCell(ctx, name);
        if (ci == nullptr)
            return "cell " + q(name.str(ctx)) + " does not exist";
        std::vector<CellInfo *> group = placementGroup(ctx, ci);
        if (moving.count(group.front()->name))
            continue;
        for (CellInfo *c : group) {
            if (c->bel == BelId())
                return q(c->name.str(ctx)) + " is not placed";
            std::string fixed = cellFixedReason(ctx, c);
            if (!fixed.empty())
                return fixed;
            moving.insert(c->name);
        }
        roots.push_back(group.front());
    }
    // Where each cell goes
    std::vector<std::pair<CellInfo *, BelId>> targets;
    auto shifted = [&](CellInfo *c, BelId &tb) -> std::string {
        Loc loc = ctx->getBelLocation(c->bel);
        Loc tloc(loc.x + dx, loc.y + dy, loc.z);
        tb = ctx->getBelByLocation(tloc);
        if (tb == BelId())
            return "no bel at (" + std::to_string(tloc.x) + ", " + std::to_string(tloc.y) + ", " +
                   std::to_string(tloc.z) + ") for " + q(c->name.str(ctx));
        if (ctx->getBelType(tb) != ctx->getBelType(c->bel))
            return "bel " + q(ctx->nameOfBel(tb)) + " (for " + q(c->name.str(ctx)) + ") is of type " +
                   q(ctx->getBelType(tb).str(ctx)) + ", not " + q(ctx->getBelType(c->bel).str(ctx));
        return {};
    };
    for (CellInfo *root : roots) {
        BelId tb;
        std::string err = shifted(root, tb);
        if (!err.empty())
            return err;
        if (root->cluster != ClusterId()) {
            std::vector<std::pair<CellInfo *, BelId>> placement;
            if (!ctx->isValidBelForCellType(root->type, tb) ||
                !ctx->getClusterPlacement(root->cluster, tb, placement) || placement.empty())
                return "the cluster of " + q(root->name.str(ctx)) + " does not fit with its root at " +
                       q(ctx->nameOfBel(tb));
            targets.insert(targets.end(), placement.begin(), placement.end());
        } else {
            targets.emplace_back(root, tb);
        }
    }
    for (auto &t : targets) {
        CellInfo *c = t.first;
        BelId tb = t.second;
        if (!moving.count(c->name))
            return "cluster placement of " + q(c->name.str(ctx)) + " is inconsistent";
        if (!ctx->isValidBelForCellType(c->type, tb))
            return "cell type " + q(c->type.str(ctx)) + " cannot be placed on bel " + q(ctx->nameOfBel(tb)) +
                   " of type " + q(ctx->getBelType(tb).str(ctx));
        if (!c->testRegion(tb))
            return q(c->name.str(ctx)) + " is constrained to region " + q(c->region->name.str(ctx)) + ", " +
                   q(ctx->nameOfBel(tb)) + " is outside it";
        CellInfo *bound = ctx->getBoundBelCell(tb);
        if (bound != nullptr && !moving.count(bound->name))
            return "bel " + q(ctx->nameOfBel(tb)) + " (for " + q(c->name.str(ctx)) + ") is occupied by " +
                   q(bound->name.str(ctx)) + ", which is not moved";
    }
    // Unbind everything, then bind at the targets (keeping locked strengths)
    std::vector<PlaceStrength> strengths;
    std::string err;
    for (auto &t : targets) {
        strengths.push_back(isCellLocked(t.first) ? t.first->belStrength : STRENGTH_MANUAL);
        err = addUnbindBel(b, t.first->bel);
        if (!err.empty())
            return err;
    }
    for (size_t i = 0; i < targets.size(); i++) {
        err = b.add(std::make_unique<BindBelCmd>(targets[i].first->name, targets[i].second, strengths[i], false));
        if (!err.empty())
            return err;
    }
    for (auto &t : targets)
        if (!ctx->isBelLocationValid(t.second))
            return "placing " + q(t.first->name.str(ctx)) + " at " + q(ctx->nameOfBel(t.second)) +
                   " violates location constraints";
    return {};
}

QString describePlaceCell(Context *ctx, IdString cell, BelId bel)
{
    CellInfo *ci = findCell(ctx, cell);
    if (ci == nullptr || bel == BelId())
        return "Place " + QString::fromStdString(cell.str(ctx));
    std::string name = cell.str(ctx);
    if (ci->cluster != ClusterId())
        name = "cluster " + name;
    CellInfo *bound = ctx->getBoundBelCell(bel);
    std::string text;
    if (ci->cluster == ClusterId() && bound != nullptr && bound != ci) {
        if (ci->bel != BelId())
            text = "Swap " + name + " with " + bound->name.str(ctx);
        else
            text = "Place " + name + " at " + ctx->nameOfBel(bel) + " (unplacing " + bound->name.str(ctx) + ")";
    } else if (ci->bel != BelId()) {
        text = "Move " + name + " to " + ctx->nameOfBel(bel);
    } else {
        text = "Place " + name + " at " + ctx->nameOfBel(bel);
    }
    return QString::fromStdString(text);
}

// ---------------------------------------------------------------------------
// Rip-up

namespace {

std::string unbindWires(EditBatch &b, const std::vector<WireId> &wires)
{
    Context *ctx = b.getContext();
    for (WireId w : wires) {
        NetInfo *ni = ctx->getBoundWireNet(w);
        if (ni != nullptr && ni->wires.count(w) && ni->wires.at(w).strength > STRENGTH_STRONG)
            return "the routing of net " + q(ni->name.str(ctx)) + " at " + q(ctx->nameOfWire(w)) +
                   " is locked (" + strengthName(ni->wires.at(w).strength) + "): unlock its routing first";
    }
    for (WireId w : wires) {
        std::string err = b.add(std::make_unique<UnbindWireCmd>(w));
        if (!err.empty())
            return err;
    }
    return {};
}

} // namespace

std::string addUnrouteNet(EditBatch &b, IdString net)
{
    Context *ctx = b.getContext();
    NetInfo *ni = findNet(ctx, net);
    if (ni == nullptr)
        return "net " + q(net.str(ctx)) + " does not exist";
    WireId src = ctx->getNetinfoSourceWire(ni);
    // (the routing below the sink wires stays, see NetStatus::belowSinkWires)
    pool<WireId> below = NetStatus::belowSinkWires(ctx, ni);
    std::vector<WireId> wires;
    for (auto &w : ni->wires)
        if (w.first != src && !below.count(w.first))
            wires.push_back(w.first);
    if (wires.empty())
        return "net " + q(net.str(ctx)) + " has no routing";
    return unbindWires(b, wires);
}

std::string addUnrouteArc(EditBatch &b, IdString net, WireId sink)
{
    Context *ctx = b.getContext();
    NetInfo *ni = findNet(ctx, net);
    if (ni == nullptr)
        return "net " + q(net.str(ctx)) + " does not exist";
    NetStatus::Status st = NetStatus::compute(ctx, ni);
    std::vector<WireId> wires = NetStatus::arcOnlyWires(ctx, ni, st, sink);
    if (wires.empty())
        return "no routing serves only the arc to " + q(ctx->nameOfWire(sink));
    return unbindWires(b, wires);
}

std::string addRemovePip(EditBatch &b, PipId pip)
{
    Context *ctx = b.getContext();
    if (pip == PipId())
        return "invalid pip";
    NetInfo *ni = ctx->getBoundPipNet(pip);
    if (ni == nullptr)
        return "pip " + q(ctx->nameOfPip(pip)) + " is not used";
    WireId dst = ctx->getPipDstWire(pip);
    auto down = NetStatus::downstream(ctx, ni);
    // The routing below the sink wires stays (see NetStatus::belowSinkWires), unless the pip is part of it
    pool<WireId> below = NetStatus::belowSinkWires(ctx, ni);
    std::vector<WireId> wires;
    for (WireId w : NetStatus::subtree(down, dst))
        if (below.count(dst) || !below.count(w))
            wires.push_back(w);
    return unbindWires(b, wires);
}

void prepareDesignForWrite(Context *ctx)
{
    ctx->archInfoToAttributes();
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (ci->bel == BelId()) {
            ci->attrs.erase(ctx->id("NEXTPNR_BEL"));
            ci->attrs.erase(ctx->id("BEL_STRENGTH"));
        }
    }
}

// ---------------------------------------------------------------------------
// Netlist edits

std::string CreateCellCmd::checkApply(Context *ctx)
{
    if (cell == IdString())
        return "a new cell needs a name";
    if (findCell(ctx, cell))
        return "cell " + q(cell.str(ctx)) + " exists already";
    return {};
}

void CreateCellCmd::apply(Context *ctx)
{
    CellInfo *ci = ctx->createCell(cell, type);
    for (auto &p : ports) {
        ci->ports[p.name].name = p.name;
        ci->ports[p.name].type = p.type;
    }
    ci->params = params;
    ci->attrs = attrs;
    ctx->assignArchInfo();
}

std::string CreateCellCmd::checkRevert(Context *ctx)
{
    CellInfo *ci = findCell(ctx, cell);
    if (ci == nullptr)
        return "cell " + q(cell.str(ctx)) + " does not exist";
    if (ci->bel != BelId())
        return "cell " + q(cell.str(ctx)) + " is placed";
    for (auto &p : ci->ports)
        if (p.second.net)
            return "port " + q(p.first.str(ctx)) + " of " + q(cell.str(ctx)) + " is connected";
    return {};
}

void CreateCellCmd::revert(Context *ctx)
{
    ctx->cells.erase(cell);
    ctx->assignArchInfo();
}

std::string CreateCellCmd::describe(Context *ctx) const
{
    return "Create cell " + cell.str(ctx) + " (" + type.str(ctx) + ")";
}

std::string RemoveCellCmd::checkApply(Context *ctx)
{
    CellInfo *ci = findCell(ctx, cell);
    if (ci == nullptr)
        return "cell " + q(cell.str(ctx)) + " does not exist";
    if (ci->bel != BelId())
        return "cell " + q(cell.str(ctx)) + " is placed";
    if (ci->cluster != ClusterId())
        return "cell " + q(cell.str(ctx)) + " is part of a cluster";
    for (auto &p : ci->ports)
        if (p.second.net)
            return "port " + q(p.first.str(ctx)) + " of " + q(cell.str(ctx)) + " is connected";
    return {};
}

void RemoveCellCmd::apply(Context *ctx)
{
    CellInfo *ci = findCell(ctx, cell);
    type = ci->type;
    hierpath = ci->hierpath;
    params = ci->params;
    attrs = ci->attrs;
    ports.clear();
    for (auto &p : ci->ports)
        ports.push_back({p.first, p.second.type});
    ctx->cells.erase(cell);
    ctx->assignArchInfo();
}

std::string RemoveCellCmd::checkRevert(Context *ctx)
{
    if (findCell(ctx, cell))
        return "cell " + q(cell.str(ctx)) + " exists again";
    return {};
}

void RemoveCellCmd::revert(Context *ctx)
{
    CellInfo *ci = ctx->createCell(cell, type);
    ci->hierpath = hierpath;
    for (auto &p : ports) {
        ci->ports[p.name].name = p.name;
        ci->ports[p.name].type = p.type;
    }
    ci->params = params;
    ci->attrs = attrs;
    ctx->assignArchInfo();
}

std::string RemoveCellCmd::describe(Context *ctx) const { return "Remove cell " + cell.str(ctx); }

std::string CreateNetCmd::checkApply(Context *ctx)
{
    if (net == IdString())
        return "a new net needs a name";
    if (findNet(ctx, net) || ctx->net_aliases.count(net))
        return "net " + q(net.str(ctx)) + " exists already";
    return {};
}

void CreateNetCmd::apply(Context *ctx)
{
    ctx->createNet(net);
    ctx->assignArchInfo();
}

std::string CreateNetCmd::checkRevert(Context *ctx)
{
    NetInfo *ni = findNet(ctx, net);
    if (ni == nullptr)
        return "net " + q(net.str(ctx)) + " does not exist";
    if (ni->driver.cell || ni->users.entries() > 0 || !ni->wires.empty())
        return "net " + q(net.str(ctx)) + " is in use";
    return {};
}

void CreateNetCmd::revert(Context *ctx)
{
    ctx->nets.erase(net);
    ctx->net_aliases.erase(net); // (createNet adds the net as its own alias)
    ctx->assignArchInfo();
}

std::string CreateNetCmd::describe(Context *ctx) const { return "Create net " + net.str(ctx); }

std::string RemoveNetCmd::checkApply(Context *ctx)
{
    NetInfo *ni = findNet(ctx, net);
    if (ni == nullptr)
        return "net " + q(net.str(ctx)) + " does not exist";
    if (ni->driver.cell || ni->users.entries() > 0)
        return "net " + q(net.str(ctx)) + " is still connected";
    if (!ni->wires.empty())
        return "net " + q(net.str(ctx)) + " still has routing";
    return {};
}

void RemoveNetCmd::apply(Context *ctx)
{
    attrs = findNet(ctx, net)->attrs;
    ctx->nets.erase(net);
    ctx->net_aliases.erase(net); // (createNet adds the net as its own alias)
    ctx->assignArchInfo();
}

std::string RemoveNetCmd::checkRevert(Context *ctx)
{
    if (findNet(ctx, net))
        return "net " + q(net.str(ctx)) + " exists again";
    return {};
}

void RemoveNetCmd::revert(Context *ctx)
{
    ctx->createNet(net)->attrs = attrs;
    ctx->assignArchInfo();
}

std::string RemoveNetCmd::describe(Context *ctx) const { return "Remove net " + net.str(ctx); }

std::string ConnectPortCmd::checkApply(Context *ctx)
{
    CellInfo *ci = findCell(ctx, cell);
    if (ci == nullptr)
        return "cell " + q(cell.str(ctx)) + " does not exist";
    NetInfo *ni = findNet(ctx, net);
    if (ni == nullptr)
        return "net " + q(net.str(ctx)) + " does not exist";
    auto port_it = ci->ports.find(port);
    PortType t = port_it == ci->ports.end() ? type : port_it->second.type;
    if (port_it != ci->ports.end() && port_it->second.net)
        return "port " + q(port.str(ctx)) + " of " + q(cell.str(ctx)) + " is connected to " +
               q(port_it->second.net->name.str(ctx));
    if (t == PORT_OUT) {
        if (ni->driver.cell)
            return "net " + q(net.str(ctx)) + " is driven by " + q(ni->driver.cell->name.str(ctx)) + " already";
        if (!ni->wires.empty())
            return "net " + q(net.str(ctx)) + " has routing: remove it before giving the net a new driver";
    }
    return {};
}

void ConnectPortCmd::apply(Context *ctx)
{
    CellInfo *ci = findCell(ctx, cell);
    added_port = !ci->ports.count(port);
    if (added_port) {
        ci->ports[port].name = port;
        ci->ports[port].type = type;
    }
    ci->connectPort(port, findNet(ctx, net));
    ctx->assignArchInfo();
}

std::string ConnectPortCmd::checkRevert(Context *ctx)
{
    CellInfo *ci = findCell(ctx, cell);
    if (ci == nullptr)
        return "cell " + q(cell.str(ctx)) + " does not exist";
    auto port_it = ci->ports.find(port);
    if (port_it == ci->ports.end() || port_it->second.net == nullptr || port_it->second.net->name != net)
        return "port " + q(port.str(ctx)) + " of " + q(cell.str(ctx)) + " is no longer on " + q(net.str(ctx));
    if (port_it->second.type == PORT_OUT && !port_it->second.net->wires.empty())
        return "net " + q(net.str(ctx)) + " has routing from " + q(cell.str(ctx)) + "." + q(port.str(ctx));
    return {};
}

void ConnectPortCmd::revert(Context *ctx)
{
    CellInfo *ci = findCell(ctx, cell);
    ci->disconnectPort(port);
    if (added_port) {
        ci->ports.erase(port);
        ci->cell_bel_pins.erase(port);
    }
    ctx->assignArchInfo();
}

std::string ConnectPortCmd::describe(Context *ctx) const
{
    return "Connect " + cell.str(ctx) + "." + port.str(ctx) + " to " + net.str(ctx);
}

std::string DisconnectPortCmd::checkApply(Context *ctx)
{
    CellInfo *ci = findCell(ctx, cell);
    if (ci == nullptr)
        return "cell " + q(cell.str(ctx)) + " does not exist";
    auto port_it = ci->ports.find(port);
    if (port_it == ci->ports.end())
        return "cell " + q(cell.str(ctx)) + " has no port " + q(port.str(ctx));
    NetInfo *ni = port_it->second.net;
    if (ni == nullptr && !remove_port)
        return "port " + q(port.str(ctx)) + " of " + q(cell.str(ctx)) + " is not connected";
    if (ni != nullptr && ni->driver.cell == ci && ni->driver.port == port && !ni->wires.empty())
        return "net " + q(ni->name.str(ctx)) + " has routing: remove it before disconnecting its driver";
    return {};
}

void DisconnectPortCmd::apply(Context *ctx)
{
    CellInfo *ci = findCell(ctx, cell);
    PortInfo &pi = ci->ports.at(port);
    prev_net = pi.net ? pi.net->name : IdString();
    prev_type = pi.type;
    ci->disconnectPort(port);
    if (remove_port) {
        ci->ports.erase(port);
        ci->cell_bel_pins.erase(port);
    }
    ctx->assignArchInfo();
}

std::string DisconnectPortCmd::checkRevert(Context *ctx)
{
    CellInfo *ci = findCell(ctx, cell);
    if (ci == nullptr)
        return "cell " + q(cell.str(ctx)) + " does not exist";
    auto port_it = ci->ports.find(port);
    if (remove_port ? port_it != ci->ports.end() : (port_it == ci->ports.end() || port_it->second.net))
        return "port " + q(port.str(ctx)) + " of " + q(cell.str(ctx)) + " has changed";
    if (prev_net != IdString()) {
        NetInfo *ni = findNet(ctx, prev_net);
        if (ni == nullptr)
            return "net " + q(prev_net.str(ctx)) + " does not exist";
        if (prev_type == PORT_OUT && ni->driver.cell)
            return "net " + q(prev_net.str(ctx)) + " has another driver";
    }
    return {};
}

void DisconnectPortCmd::revert(Context *ctx)
{
    CellInfo *ci = findCell(ctx, cell);
    if (remove_port) {
        ci->ports[port].name = port;
        ci->ports[port].type = prev_type;
    }
    if (prev_net != IdString())
        ci->connectPort(port, findNet(ctx, prev_net));
    ctx->assignArchInfo();
}

std::string DisconnectPortCmd::describe(Context *ctx) const
{
    return (remove_port ? "Remove port " : "Disconnect ") + cell.str(ctx) + "." + port.str(ctx);
}

std::string SetCellParamCmd::checkApply(Context *ctx)
{
    if (findCell(ctx, cell) == nullptr)
        return "cell " + q(cell.str(ctx)) + " does not exist";
    return {};
}

void SetCellParamCmd::apply(Context *ctx)
{
    CellInfo *ci = findCell(ctx, cell);
    auto &map = is_attr ? ci->attrs : ci->params;
    auto found = map.find(key);
    prev = found == map.end() ? std::nullopt : std::optional<Property>(found->second);
    if (value)
        map[key] = *value;
    else
        map.erase(key);
    ctx->assignArchInfo();
}

std::string SetCellParamCmd::checkRevert(Context *ctx) { return checkApply(ctx); }

void SetCellParamCmd::revert(Context *ctx)
{
    CellInfo *ci = findCell(ctx, cell);
    auto &map = is_attr ? ci->attrs : ci->params;
    if (prev)
        map[key] = *prev;
    else
        map.erase(key);
    ctx->assignArchInfo();
}

std::string SetCellParamCmd::describe(Context *ctx) const
{
    std::string what = is_attr ? "attribute " : "parameter ";
    if (!value)
        return "Remove " + what + key.str(ctx) + " of " + cell.str(ctx);
    return "Set " + what + key.str(ctx) + " of " + cell.str(ctx) + " to " + value->to_string();
}

std::string addDisconnectPort(EditBatch &b, IdString cell, IdString port, bool remove_port)
{
    Context *ctx = b.getContext();
    CellInfo *ci = findCell(ctx, cell);
    if (ci == nullptr)
        return "cell " + q(cell.str(ctx)) + " does not exist";
    auto port_it = ci->ports.find(port);
    if (port_it == ci->ports.end())
        return "cell " + q(cell.str(ctx)) + " has no port " + q(port.str(ctx));
    NetInfo *ni = port_it->second.net;
    if (ni != nullptr) {
        if (ni->driver.cell == ci && ni->driver.port == port) {
            // All of the routing grows from this driver, its source wire too
            std::vector<WireId> wires;
            for (auto &w : ni->wires)
                wires.push_back(w.first);
            std::string err = unbindWires(b, wires);
            if (!err.empty())
                return err;
        } else if (port_it->second.user_idx) {
            PortRef ref;
            ref.cell = ci;
            ref.port = port;
            std::vector<WireId> sinks;
            for (WireId w : ctx->getNetinfoSinkWires(ni, ref))
                sinks.push_back(w);
            for (WireId w : sinks) {
                if (!ni->wires.count(w))
                    continue;
                // (nothing goes where other arcs need the same wires: nothing serves only this arc then)
                NetStatus::Status st = NetStatus::compute(ctx, ni);
                std::vector<WireId> only = NetStatus::arcOnlyWires(ctx, ni, st, w);
                std::string err = unbindWires(b, only);
                if (!err.empty())
                    return err;
            }
        }
    }
    return b.add(std::make_unique<DisconnectPortCmd>(cell, port, remove_port));
}

std::string addConnectPort(EditBatch &b, IdString cell, IdString port, IdString net, PortType type)
{
    Context *ctx = b.getContext();
    CellInfo *ci = findCell(ctx, cell);
    if (ci == nullptr)
        return "cell " + q(cell.str(ctx)) + " does not exist";
    auto port_it = ci->ports.find(port);
    if (port_it != ci->ports.end() && port_it->second.net) {
        if (port_it->second.net->name == net)
            return {};
        std::string err = addDisconnectPort(b, cell, port);
        if (!err.empty())
            return err;
    }
    return b.add(std::make_unique<ConnectPortCmd>(cell, port, net, type));
}

std::string addRemoveCell(EditBatch &b, IdString cell)
{
    Context *ctx = b.getContext();
    CellInfo *ci = findCell(ctx, cell);
    if (ci == nullptr)
        return "cell " + q(cell.str(ctx)) + " does not exist";
    if (ci->cluster != ClusterId())
        return "cell " + q(cell.str(ctx)) + " is part of a cluster: it can not be removed on its own";
    std::string err = cellFixedReason(ctx, ci);
    if (!err.empty())
        return err;
    if (ci->bel != BelId()) {
        err = addUnbindBel(b, ci->bel);
        if (!err.empty())
            return err;
    }
    std::vector<IdString> ports;
    for (auto &p : ci->ports)
        if (p.second.net)
            ports.push_back(p.first);
    for (IdString p : ports) {
        err = addDisconnectPort(b, cell, p);
        if (!err.empty())
            return err;
    }
    return b.add(std::make_unique<RemoveCellCmd>(cell));
}

std::string addCreateCellLike(EditBatch &b, IdString cell, IdString like)
{
    Context *ctx = b.getContext();
    CellInfo *tmpl = findCell(ctx, like);
    if (tmpl == nullptr)
        return "cell " + q(like.str(ctx)) + " does not exist";
    std::vector<CreateCellCmd::Port> ports;
    for (auto &p : tmpl->ports)
        ports.push_back({p.first, p.second.type});
    return b.add(std::make_unique<CreateCellCmd>(cell, tmpl->type, ports, tmpl->params));
}

IdString unusedCellName(Context *ctx, const std::string &base)
{
    IdString name = ctx->id(base);
    for (int i = 1; findCell(ctx, name); i++)
        name = ctx->idf("%s$%d", base.c_str(), i);
    return name;
}

IdString unusedNetName(Context *ctx, const std::string &base)
{
    IdString name = ctx->id(base);
    for (int i = 1; findNet(ctx, name); i++)
        name = ctx->idf("%s$%d", base.c_str(), i);
    return name;
}

// ---------------------------------------------------------------------------
// EditBatch

std::string EditBatch::add(std::unique_ptr<EditCommand> cmd)
{
    std::string err = cmd->checkApply(ctx);
    if (!err.empty())
        return err;
    cmd->apply(ctx);
    err = cmd->postApply(ctx);
    if (!err.empty()) {
        cmd->revert(ctx);
        return err;
    }
    cmds.push_back(std::move(cmd));
    return {};
}

void EditBatch::rollbackTo(size_t n)
{
    while (cmds.size() > n) {
        cmds.back()->revert(ctx);
        cmds.pop_back();
    }
}

// ---------------------------------------------------------------------------
// Undo stack entry

namespace {

std::string runGuarded(const std::function<std::string()> &fn)
{
    try {
        return fn();
    } catch (const log_execution_error_exception &) {
        return "error during edit (see log)";
    } catch (const std::exception &e) {
        return std::string("exception during edit: ") + e.what();
    }
}

class MacroUndoCommand : public QUndoCommand
{
  public:
    MacroUndoCommand(Context *ctx, const QString &text, std::vector<std::unique_ptr<EditCommand>> cmds)
            : QUndoCommand(text), ctx(ctx), cmds(std::move(cmds))
    {
    }

    void redo() override
    {
        error.clear();
        if (first) {
            // Already applied by EditStack::run.
            first = false;
            return;
        }
        ctx->lock();
        error = runGuarded([&]() -> std::string {
            for (size_t i = 0; i < cmds.size(); i++) {
                std::string err = cmds[i]->checkApply(ctx);
                if (err.empty()) {
                    cmds[i]->apply(ctx);
                    err = cmds[i]->postApply(ctx);
                    if (!err.empty())
                        cmds[i]->revert(ctx);
                }
                if (!err.empty()) {
                    for (size_t j = i; j-- > 0;)
                        cmds[j]->revert(ctx);
                    return err;
                }
            }
            return {};
        });
        ctx->notifyDesignEdited();
        ctx->refreshUi();
        ctx->unlock();
    }

    void undo() override
    {
        error.clear();
        ctx->lock();
        error = runGuarded([&]() -> std::string {
            for (size_t i = cmds.size(); i-- > 0;) {
                std::string err = cmds[i]->checkRevert(ctx);
                if (!err.empty()) {
                    for (size_t j = i + 1; j < cmds.size(); j++)
                        cmds[j]->apply(ctx);
                    return err;
                }
                cmds[i]->revert(ctx);
            }
            return {};
        });
        ctx->notifyDesignEdited();
        ctx->refreshUi();
        ctx->unlock();
    }

    std::string error;

  private:
    Context *ctx;
    std::vector<std::unique_ptr<EditCommand>> cmds;
    bool first = true;
};

} // namespace

// ---------------------------------------------------------------------------
// EditStack

EditStack::EditStack(QObject *parent) : QObject(parent)
{
    connect(&stack, &QUndoStack::canUndoChanged, this, &EditStack::stateChanged);
    connect(&stack, &QUndoStack::canRedoChanged, this, &EditStack::stateChanged);
    connect(&stack, &QUndoStack::indexChanged, this, &EditStack::stateChanged);
}

EditStack::~EditStack() {}

void EditStack::setContext(Context *new_ctx)
{
    ctx = new_ctx;
    clear();
}

bool EditStack::canEdit(QString *why) const
{
    QString reason;
    if (ctx == nullptr)
        reason = "No design loaded";
    else if (task_busy)
        reason = "Cannot edit while a pack/place/route task is running";
    else if (!edit_mode)
        reason = "Edit mode is off (press E to enable it)";
    if (why)
        *why = reason;
    return reason.isEmpty();
}

void EditStack::setEditMode(bool on)
{
    if (edit_mode == on)
        return;
    edit_mode = on;
    if (on && ctx != nullptr && !task_busy && before_edit)
        before_edit();
    Q_EMIT message(on ? "Edit mode on" : "Edit mode off");
    Q_EMIT stateChanged();
}

void EditStack::setBusy(bool on)
{
    if (task_busy == on)
        return;
    task_busy = on;
    // A pass changes the design behind our back: the history is no longer valid.
    if (on)
        stack.clear();
    Q_EMIT stateChanged();
}

void EditStack::clear()
{
    stack.clear();
    Q_EMIT stateChanged();
}

std::string EditStack::run(const QString &text, const std::function<std::string(EditBatch &)> &fn)
{
    QString why;
    if (!canEdit(&why)) {
        Q_EMIT message(why);
        return why.toStdString();
    }
    if (before_edit)
        before_edit();
    EditBatch batch(ctx);
    bool rollback_failed = false;
    ctx->lock();
    std::string err = runGuarded([&]() { return fn(batch); });
    if (!err.empty()) {
        std::string rerr = runGuarded([&]() -> std::string {
            for (size_t i = batch.cmds.size(); i-- > 0;)
                batch.cmds[i]->revert(ctx);
            return {};
        });
        if (!rerr.empty()) {
            err += "; rollback failed: " + rerr;
            rollback_failed = true;
        }
        batch.cmds.clear();
    }
    bool changed = !batch.cmds.empty();
    if (changed || !err.empty()) {
        ctx->notifyDesignEdited();
        ctx->refreshUi();
    }
    ctx->unlock();

    if (!err.empty()) {
        Q_EMIT message(text + " failed: " + QString::fromStdString(err));
        // A clean rollback restored the design: no refresh (it would reset the trees and properties)
        if (rollback_failed)
            Q_EMIT designChanged();
        return err;
    }
    if (changed) {
        stack.push(new MacroUndoCommand(ctx, text, std::move(batch.cmds)));
        Q_EMIT message(text);
        Q_EMIT designChanged();
    }
    return {};
}

std::string EditStack::execute(std::unique_ptr<EditCommand> cmd, const QString &text)
{
    QString t = text;
    if (t.isEmpty() && ctx != nullptr)
        t = QString::fromStdString(cmd->describe(ctx));
    // fn is called exactly once by run(), so moving out of the captured reference is fine.
    return run(t, [&](EditBatch &b) { return b.add(std::move(cmd)); });
}

std::string EditStack::dryRun(const std::function<std::string(EditBatch &)> &fn)
{
    if (ctx == nullptr)
        return "No design loaded";
    if (task_busy)
        return "Cannot edit while a pack/place/route task is running";
    EditBatch batch(ctx);
    ctx->lock();
    // Binding marks the bels for a redraw: a trial must not make the view redraw everything
    bool all_reload = ctx->allUiReload, frame_reload = ctx->frameUiReload;
    pool<BelId> bel_reload = ctx->belUiReload;
    pool<WireId> wire_reload = ctx->wireUiReload;
    pool<PipId> pip_reload = ctx->pipUiReload;
    std::string err = runGuarded([&]() { return fn(batch); });
    std::string rerr = runGuarded([&]() -> std::string {
        for (size_t i = batch.cmds.size(); i-- > 0;)
            batch.cmds[i]->revert(ctx);
        return {};
    });
    // Whatever the Arch derived from the trial state (e.g. computed lazily) is stale now
    if (!batch.cmds.empty())
        ctx->notifyDesignEdited();
    batch.cmds.clear();
    if (rerr.empty()) {
        ctx->allUiReload = all_reload;
        ctx->frameUiReload = frame_reload;
        ctx->belUiReload = std::move(bel_reload);
        ctx->wireUiReload = std::move(wire_reload);
        ctx->pipUiReload = std::move(pip_reload);
    } else {
        err += "; rollback failed: " + rerr;
        ctx->refreshUi();
    }
    ctx->unlock();
    return err;
}

void EditStack::undoRedo(bool is_undo)
{
    QString why;
    if (!canEdit(&why)) {
        Q_EMIT message(why);
        return;
    }
    int idx = is_undo ? stack.index() - 1 : stack.index();
    if (idx < 0 || idx >= stack.count()) {
        Q_EMIT message(is_undo ? "Nothing to undo" : "Nothing to redo");
        return;
    }
    auto *cmd = static_cast<MacroUndoCommand *>(const_cast<QUndoCommand *>(stack.command(idx)));
    QString text = cmd->text();
    if (is_undo)
        stack.undo();
    else
        stack.redo();
    if (!cmd->error.empty()) {
        Q_EMIT message(QString(is_undo ? "Cannot undo '" : "Cannot redo '") + text +
                       "': " + QString::fromStdString(cmd->error) + " - edit history cleared");
        stack.clear();
    } else {
        Q_EMIT message(QString(is_undo ? "Undo: " : "Redo: ") + text);
    }
    Q_EMIT designChanged();
}

void EditStack::undo() { undoRedo(true); }

void EditStack::redo() { undoRedo(false); }

NEXTPNR_NAMESPACE_END
