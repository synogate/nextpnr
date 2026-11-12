/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2022-23  gatecat <gatecat@ds0.me>
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
 *
 */

#ifndef PLACER_STATIC_H
#define PLACER_STATIC_H
#include "log.h"
#include "nextpnr.h"

#include <optional>

NEXTPNR_NAMESPACE_BEGIN

struct StaticRect
{
    StaticRect() : w(0), h(0) {};
    StaticRect(float w, float h) : w(w), h(h) {};
    float w, h;
    float area() const { return w * h; }
};

struct StaticCellGroupCfg
{
    // name of the group for debugging purposes
    IdString name;
    // bel buckets in this group
    pool<BelBucketId> bel_buckets;
    // cell & bel types in this group and the (normalised) area of that cell/bel type
    dict<IdString, StaticRect> cell_area, bel_area;
    // these cells (generally auxilliary CLB cells like RAMW; carry; MUX) are considered to have zero area when part of
    // a macro with other non-zero-area cells
    pool<IdString> zero_area_cells;
    // size of spacers to insert
    StaticRect spacer_rect{0.5, 0.5};
};

struct PlacerStaticCfg
{
    PlacerStaticCfg(Context *ctx);

    // These cell types will be randomly locked to prevent singular matrices
    pool<IdString> ioBufTypes;
    // Nets driven by these cell types will be ignored
    pool<IdString> glbBufTypes;
    int hpwl_scale_x = 1;
    int hpwl_scale_y = 1;
    bool timing_driven = false;
    // for calculating timing estimates based on distance
    // estimate = c + mx*dx + my * dy
    delay_t timing_c = 100, timing_mx = 100, timing_my = 100;
    // A pin's pull grows with the criticality of its arc: 1 + timing_weight * crit^crit_exponent (settings
    // placerStatic/timingWeight and placerStatic/critExponent), with the criticalities of an analysis every
    // timing_period iterations (placerStatic/timingPeriod)
    float timing_weight = 5.0f;
    float crit_exponent = 2.0f;
    int timing_period = 10;
    // As Placer1Cfg::clockSkew (the same setting, placer/clockSkew)
    bool clockSkew = false;
    // A critical arc pulls its driver and its user together, on top of the wirelength of the net: with
    // arc_weight * crit^arc_exponent of the pull of a two-pin net, for arcs of at least arc_min_crit (settings
    // placerStatic/arcWeight, 0 = off, placerStatic/arcExponent, placerStatic/arcMinCrit). In the wirelength
    // of a net only the pins at the edges of its box are pulled, so a critical user among many users of a net is
    // not drawn to the driver by the weight of its pin.
    float arc_weight = 0.0f;
    float arc_exponent = 4.0f;
    float arc_min_crit = 0.5f;
    // The macros of the IP groups (block RAMs, DSPs) go to their places as a whole, by the weighted wirelength of
    // their nets, instead of each to the first free place found around the solver's position (setting
    // placerStatic/ipAssign)
    bool ip_assign = false;
    // groups of cells that should be placed together.
    // groups < logic_groups are logic like LUTs and FFs, further groups for BRAM/DSP/misc
    std::vector<StaticCellGroupCfg> cell_groups;
    int logic_groups = 2;

    // this is an optional callback to override the area of a cell e.g. based on configuration
    std::function<std::optional<StaticRect>(Context *, const CellInfo *)> get_cell_area_override =
            [](Context *, const CellInfo *) { return std::optional<StaticRect>{}; };

    // override the default delay estimation formula (we can't use predictDelay because we don't have concrete bels)
    std::function<delay_t(Context *, const PlacerStaticCfg &, Loc, IdString, Loc, IdString)> predict_delay;
};

extern bool placer_static(Context *ctx, PlacerStaticCfg cfg);
NEXTPNR_NAMESPACE_END
#endif