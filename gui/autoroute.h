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
 * Small path search for routing by hand in the GUI (waypoints, "Route this arc", "Route rest of
 * net"), and the edit-stack helpers binding what it finds (see editcommands.h).
 *
 * findPath is an A* from a set of start wires to one target wire over getPipsDownhill, using only
 * pips the net may take now: already bound to the net, or free with checkPipAvailForNet and a free,
 * available destination wire. Cost = pip delay + wire delay + a small penalty per pip, heuristic
 * estimateDelay. It is bounded (a box around starts and target, then the whole device, and a
 * maximum number of expanded wires), so it never hangs the GUI. The caller holds the context lock
 * (inside EditStack::run, the lambda does).
 */

#ifndef AUTOROUTE_H
#define AUTOROUTE_H

#include <functional>
#include <string>
#include <vector>
#include "nextpnr.h"

class QMenu;

NEXTPNR_NAMESPACE_BEGIN

class EditBatch;
class EditStack;

namespace AutoRoute {

struct Options
{
    int max_visits = 300000; // wires expanded at most (both passes together)
    int margin = 6;          // tiles around starts and target in the first pass
};

struct Result
{
    // Pips from a start wire to the target, in order (may contain pips already bound to the net)
    std::vector<PipId> path;
    WireId start; // the start wire the path begins at
    int visits = 0;
    std::string why; // reason when nothing was found
};

// Search a path for net from any of `starts` to `target`. A target bound to the net without a
// driving pip (the top of a partial arc) may be reached; the binding helpers rebind it.
bool findPath(Context *ctx, NetInfo *net, const std::vector<WireId> &starts, WireId target, Result &res,
              const Options &opt = Options());

// The wires of the net connected to its source wire through bound pips (the source wire alone if
// it is not bound yet). Empty if the net has no source wire.
std::vector<WireId> treeWires(Context *ctx, NetInfo *net);

// Whether the routing of a net is locked (a wire bound above STRENGTH_STRONG)
bool isRoutingLocked(const NetInfo *net);

} // namespace AutoRoute

// Edit-stack helpers (one batch = one undo entry). All bind new pips with STRENGTH_MANUAL and leave
// pips already bound untouched. Each returns "" or the reason for refusing.

// Bind a path found by findPath: the net's source wire first if it is the unbound start, then
// every pip not yet bound to the net (a target bound without a pip is rebound through the last pip).
std::string addRoutePath(EditBatch &b, IdString net, const AutoRoute::Result &path);
// Route from `from` (a wire of the net, or its unbound source wire) to `target`. *out gets the path.
std::string addRouteToWire(EditBatch &b, IdString net, WireId from, WireId target,
                           AutoRoute::Result *out = nullptr);
// Route the arc to `sink` (a sink wire of the net, unrouted or partial) from the closest wire of
// the net's routing tree.
std::string addRouteArc(EditBatch &b, IdString net, WireId sink, AutoRoute::Result *out = nullptr);
// Route every unrouted / partial arc of the net (as addRouteArc). Arcs without a path are skipped
// and listed in *report; fails only if no arc could be routed. *routed / *failed get the counts.
std::string addRouteNetRest(EditBatch &b, IdString net, int *routed = nullptr, int *failed = nullptr,
                            std::string *report = nullptr);
// Lock (STRENGTH_USER) or unlock (STRENGTH_STRONG) all routing of a net
std::string addLockRouting(EditBatch &b, IdString net, bool lock);

// GUI actions: run the helpers above on the edit stack as one undo entry and report the outcome in
// the status bar (EditStack::notify). Must be called without holding the context lock.
std::string runRouteArc(EditStack *es, IdString net, WireId sink);
std::string runRouteNetRest(EditStack *es, IdString net);
std::string runLockRouting(EditStack *es, IdString net, bool lock);
// Add "Route arc to ..." (unrouted / partial arcs), "Route rest of net" and "Lock routing" /
// "Unlock routing" of a net to a context menu (greyed with the reason when not possible); `after`
// runs after a successful action. Takes the context lock itself.
void addAutoRouteActions(QMenu &menu, EditStack *es, IdString net, const std::function<void()> &after = {});

NEXTPNR_NAMESPACE_END

#endif // AUTOROUTE_H
