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
};

extern bool placer1(Context *ctx, Placer1Cfg cfg);
extern bool placer1_refine(Context *ctx, Placer1Cfg cfg);

NEXTPNR_NAMESPACE_END

#endif // PLACE_H
