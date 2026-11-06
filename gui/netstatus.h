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

/*
 * Routing status of a net, per arc (driver -> one physical sink wire of a user), computed from
 * the net's bound wires: from the sink wire back via net->wires[w].pip to a wire without a
 * driving pip, which must be the source wire (or, for a constant net, a wire with that constant
 * value). Read-only; the caller holds the context lock.
 */

#ifndef NETSTATUS_H
#define NETSTATUS_H

#include <string>
#include <vector>
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

namespace NetStatus {

enum class ArcState
{
    ROUTED,   // the sink wire is reached from the source through the net's wires
    PARTIAL,  // the sink wire is bound to the net, but its chain of pips does not reach the source
    UNROUTED, // the sink wire is not bound to the net
    NO_WIRE,  // no sink or source wire (cell unplaced, no bel pin)
};

const char *stateName(ArcState s);

struct Arc
{
    store_index<PortRef> user; // index in net->users
    size_t phys_idx = 0;       // which of the user's sink wires
    IdString cell, port;
    WireId sink;
    ArcState state = ArcState::NO_WIRE;
    // The net's wires from the sink up the chain of driving pips, sink first (ROUTED: up to and
    // including the source wire). Empty if the sink wire is not bound to the net.
    std::vector<WireId> path;
    // The sink wire is bound to another net
    bool blocked = false;
};

struct Status
{
    IdString net;
    WireId source;
    std::vector<Arc> arcs;
    int routed = 0, partial = 0, unrouted = 0, no_wire = 0;
    // Source / sink wires of the net taken by another net
    int conflicts = 0;
    // Wires of the net driven by a pip whose source wire is not bound to the net
    int dangling = 0;
    // Arcs that have a sink and source wire (routable)
    int routable() const { return routed + partial + unrouted; }
    bool fullyRouted() const { return routed == routable() && dangling == 0; }
};

Status compute(Context *ctx, NetInfo *net);
// "N/M arcs routed, K conflicts" (+ partial / dangling / unplaced counts when there are any)
std::string summary(const Status &st);

// The wires of the net driven (through a bound pip) by each wire of the net
dict<WireId, std::vector<WireId>> downstream(Context *ctx, NetInfo *net);
// The wire and every wire of the net below it (through bound pips), wire first
std::vector<WireId> subtree(const dict<WireId, std::vector<WireId>> &down, WireId wire);
// The wires the net routes below one of its sink wires, reaching no other sink: e.g. on GateMate, in a design
// routed and finished before, the routing inside a CPE from a LUT input's bel pin wire on (the LUT inputs were
// renamed to the CPE inputs the routing reaches; the routing below is part of the CPE's configuration). Rip-up
// keeps it (also when the arc above it goes), the router does the same.
pool<WireId> belowSinkWires(Context *ctx, NetInfo *net);
// The wires to unbind to remove the routing of one arc: from the sink up its path while a wire
// drives nothing else of the net, is not the sink of another routed arc and is not the source
// (what the net routes below the sink wire stays, see belowSinkWires)
std::vector<WireId> arcOnlyWires(Context *ctx, NetInfo *net, const Status &st, WireId sink);

// Centre of a bel's graphics (or of its grid location if it has none), in view coordinates
bool belCentre(Context *ctx, BelId bel, float &x, float &y);

} // namespace NetStatus

NEXTPNR_NAMESPACE_END

#endif // NETSTATUS_H
