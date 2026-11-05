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

#include "elementinfo.h"

NEXTPNR_NAMESPACE_BEGIN

namespace ElementInfo {

namespace {

// Appends "key: value" as a line, or as a part of one line
void add(std::string &out, bool multiline, const std::string &key, const std::string &value)
{
    out += multiline ? "\n" : "  |  ";
    out += key;
    out += ": ";
    out += value;
}

std::string netWithStrength(const Context *ctx, const NetInfo *net, WireId wire)
{
    std::string s = net->name.str(ctx);
    auto fnd = net->wires.find(wire);
    if (fnd != net->wires.end())
        s += std::string(" [") + strengthName(fnd->second.strength) + "]";
    return s;
}

} // namespace

const char *strengthName(PlaceStrength strength)
{
    switch (strength) {
    case STRENGTH_NONE:
        return "none";
    case STRENGTH_WEAK:
        return "weak";
    case STRENGTH_STRONG:
        return "strong";
    case STRENGTH_PLACER:
        return "placer";
    case STRENGTH_FIXED:
        return "fixed";
    case STRENGTH_LOCKED:
        return "locked";
    case STRENGTH_USER:
        return "user";
    default:
        return "?";
    }
}

std::string belText(const Context *ctx, BelId bel, bool withState, bool multiline)
{
    std::string out = "BEL " + ctx->getBelName(bel).str(ctx);
    add(out, multiline, "type", ctx->getBelType(bel).str(ctx));
    if (!withState)
        return out;
    CellInfo *cell = ctx->getBoundBelCell(bel);
    if (cell != nullptr) {
        add(out, multiline, "cell",
            cell->name.str(ctx) + " (" + cell->type.str(ctx) + ") [" + strengthName(cell->belStrength) + "]");
    } else if (ctx->checkBelAvail(bel)) {
        add(out, multiline, "state", "free");
    } else {
        CellInfo *conflict = ctx->getConflictingBelCell(bel);
        add(out, multiline, "state",
            conflict != nullptr ? "unavailable (conflicts with " + conflict->name.str(ctx) + ")" : "unavailable");
    }
    return out;
}

std::string wireText(const Context *ctx, WireId wire, bool withState, bool multiline)
{
    std::string out = "WIRE " + ctx->getWireName(wire).str(ctx);
    if (!withState)
        return out;
    NetInfo *net = ctx->getBoundWireNet(wire);
    if (net != nullptr) {
        add(out, multiline, "net", netWithStrength(ctx, net, wire));
    } else if (ctx->checkWireAvail(wire)) {
        add(out, multiline, "state", "free");
    } else {
        NetInfo *conflict = ctx->getConflictingWireNet(wire);
        add(out, multiline, "state",
            conflict != nullptr ? "blocked by net " + conflict->name.str(ctx) : std::string("unavailable"));
    }
    return out;
}

std::string pipText(const Context *ctx, PipId pip, bool withState, bool multiline)
{
    std::string out = "PIP " + ctx->getPipName(pip).str(ctx);
    add(out, multiline, "from", ctx->getWireName(ctx->getPipSrcWire(pip)).str(ctx));
    add(out, multiline, "to", ctx->getWireName(ctx->getPipDstWire(pip)).str(ctx));
    if (!withState)
        return out;
    NetInfo *net = ctx->getBoundPipNet(pip);
    if (net != nullptr) {
        add(out, multiline, "net", netWithStrength(ctx, net, ctx->getPipDstWire(pip)));
    } else if (ctx->checkPipAvail(pip)) {
        add(out, multiline, "state", "free");
    } else {
        NetInfo *conflict = ctx->getConflictingPipNet(pip);
        add(out, multiline, "state",
            conflict != nullptr ? "blocked by net " + conflict->name.str(ctx) : std::string("unavailable"));
    }
    return out;
}

std::string groupText(const Context *ctx, GroupId group, bool multiline)
{
    return "GROUP " + ctx->getGroupName(group).str(ctx);
}

} // namespace ElementInfo

NEXTPNR_NAMESPACE_END
