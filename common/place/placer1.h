/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Claire Xenia Wolf <claire@yosyshq.com>
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
#ifndef PLACE_H
#define PLACE_H

#include <functional>
#include "log.h"
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

struct Placer1Cfg
{
    Placer1Cfg(Context *ctx);
    float constraintWeight, netShareWeight;
    int minBelsForGridPick;
    float startTemp;
    int timingFanoutThresh;
    bool timing_driven;
    int hpwl_scale_x, hpwl_scale_y;
    // Penalty on pin density, if densityWeight > 0: the connected cell pins in each bin of
    // densityBin x densityBin tiles above densityPins per tile, squared. TEMPORARY: no gain in the
    // GateMate timing experiments (himbaechel/uarch/gatemate/docs/timing_experiments.md)
    float densityWeight;
    int densityBin;
    float densityPins;
    // If set, adjusts the criticality of a connection (its sink), e.g. by what earlier runs learnt.
    // TEMPORARY: only the GateMate placement learning uses it, which gave no gain
    std::function<float(const PortRef &user, float crit)> adjust_crit;
    // Directed moves in the refinement of a placement (placer1_refine): in every iteration, a cell or cluster with a
    // connection of at least this criticality also tries a bel where its connections pull it, the mean of the
    // cells at their other ends weighted like the timing cost (within directedMoveRadius of it; a type with too
    // few bels for a pick by location, like a block RAM, the bel nearest to it). Negative: none. The random moves
    // of the refinement reach a few tiles and are taken only if each step pays, so a cell on a critical path that
    // the analytic placement left far from the rest of the path, among cells it shares uncritical nets with,
    // stays there.
    float directedMoveCrit;
    int directedMoveRadius;
    // The timing analysis of the placer counts the delays of the clocks to the start and the end of a path (setting
    // placer/clockSkew, off by default, also for the analytic placer). While the design is placed the clocks are
    // not routed, so these delays are the architecture's estimates (predictDelay): only for an architecture that
    // estimates its clock network
    bool clockSkew;
    // The temperature the refinement of a placement (placer1_refine) starts at (setting placer1/refineTemp). By
    // default it is the temperature annealing ends at: a move is taken only if it improves the placement, and the
    // moves soon reach no further than the next tile. From a higher one the refinement is an annealing of the
    // analytic placement that cools as the annealer's does: several times the iterations, and a placement the
    // moves that pay at once do not get to
    float refineTemp;
};

extern bool placer1(Context *ctx, Placer1Cfg cfg);
extern bool placer1_refine(Context *ctx, Placer1Cfg cfg);

NEXTPNR_NAMESPACE_END

#endif // PLACE_H
