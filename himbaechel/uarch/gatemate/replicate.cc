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
#include <cmath>

#include "gatemate.h"
#include "pack.h"
#include "timing.h"

#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

NEXTPNR_NAMESPACE_BEGIN

// Replicating the drivers of nets with many sinks spread far apart (--vopt replicate-fanout).
//
// A net driving, say, the enables of 30 registers spread over 40 tiles is slow to its farthest sinks
// however it is routed. If a LUT tree drives it, copies of that LUT (same inputs and function) near
// groups of sinks shorten those connections at the cost of one more sink on each input. The sinks
// are grouped by position, keeping the sinks in one CPE together (its two registers share the enable
// and clock inputs). The original keeps the group nearest to it.
int GateMateImpl::replicate_drivers(int min_fanout, bool all)
{
    TimingAnalyser tmg(ctx);
    if (!all) {
        tmg.setup();
        tmg.run();
    }
    auto loc_of = [&](CellInfo *ci) { return ctx->getBelLocation(ci->bel); };
    // A free LUT tree position nearest to (x, y) in the region of ci, in a CPE with nothing but LUT trees
    // and registers
    auto free_lut_bel = [&](const CellInfo *ci, float x, float y) {
        int cx = int(std::lround(x)), cy = int(std::lround(y));
        for (int radius = 0; radius < 12; radius++)
            for (int dy = -radius; dy <= radius; dy++)
                for (int dx = -radius; dx <= radius; dx++) {
                    if (std::max(std::abs(dx), std::abs(dy)) != radius)
                        continue;
                    int tx = cx + dx, ty = cy + dy;
                    if (tx < 0 || ty < 0 || tx >= ctx->getGridDimX() || ty >= ctx->getGridDimY())
                        continue;
                    // Not into imported partitions, nor into the CPEs a partition keeps free for IOs
                    int tile = tile_by_xy(ctx->chip_info, tx, ty);
                    if (partition_tiles.count(tile) || (as_partition && partition_io_tiles.count(tile)))
                        continue;
                    bool special = false;
                    for (int z :
                         {CPE_RAMIO_U_Z, CPE_RAMIO_L_Z, CPE_COMP_Z, CPE_CPLINES_Z, CPE_LT_FULL_Z, CPE_BRIDGE_Z}) {
                        BelId bel = ctx->getBelByLocation(Loc(tx, ty, z));
                        special |= bel != BelId() && !ctx->checkBelAvail(bel);
                    }
                    if (special)
                        continue;
                    for (int z : {CPE_LT_U_Z, CPE_LT_L_Z}) {
                        BelId bel = ctx->getBelByLocation(Loc(tx, ty, z));
                        if (bel != BelId() && ctx->checkBelAvail(bel) && ctx->isValidBelForCellType(id_CPE_L2T4, bel) &&
                            ci->testRegion(bel))
                            return bel;
                    }
                }
        return BelId();
    };

    // The drivers, with the criticality of their nets measured before any copies exist (a copy can
    // be a sink of another driver's net, unknown to the timing analysis)
    std::vector<std::pair<CellInfo *, float>> drivers;
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        NetInfo *out = ci->getPort(id_OUT);
        if (ci->type != id_CPE_L2T4 || ci->bel == BelId() || ci->cluster != ClusterId() || !out ||
            int(out->users.entries()) < min_fanout || partition_locked_nets.count(out->name))
            continue;
        // Only LUT trees fed through their first level, whose function is all in INIT_L00/L01/L10
        bool plain = true;
        for (auto &port : ci->ports)
            if (port.second.type == PORT_IN && port.second.net &&
                !port.first.in(id_D0_00, id_D1_00, id_D0_01, id_D1_01, id_D0_10, id_D1_10))
                plain = false;
        if (!plain)
            continue;
        float crit = 0;
        if (!all)
            for (auto &user : out->users)
                crit = std::max(crit, tmg.get_criticality(CellPortKey(user)));
        drivers.emplace_back(ci, crit);
    }

    int copies = 0, nets = 0;
    for (auto [ci, crit] : drivers) {
        NetInfo *out = ci->getPort(id_OUT);
        // Sinks by CPE
        dict<int, std::vector<PortRef>> by_tile;
        int x0 = 1 << 30, x1 = -1, y0 = 1 << 30, y1 = -1;
        bool placed = true;
        for (auto &user : out->users) {
            if (!user.cell || user.cell->bel == BelId()) {
                placed = false;
                break;
            }
            by_tile[user.cell->bel.tile].push_back(user);
            Loc l = loc_of(user.cell);
            x0 = std::min(x0, l.x), x1 = std::max(x1, l.x), y0 = std::min(y0, l.y), y1 = std::max(y1, l.y);
        }
        int spread = (x1 - x0) + (y1 - y0);
        if (!placed || (!all && (crit < 0.8f || spread < 12)) || by_tile.size() < 2)
            continue;
        int groups = std::min<int>(by_tile.size(), all ? 2 : std::min(4, 1 + spread / 12));
        // k-means over the CPEs' positions, weighted by their sinks
        std::vector<std::pair<int, std::vector<PortRef>>> tiles(by_tile.begin(), by_tile.end());
        std::vector<std::pair<float, float>> centre(groups);
        for (int g = 0; g < groups; g++) {
            Loc l = loc_of(tiles.at(g * tiles.size() / groups).second.front().cell);
            centre[g] = {float(l.x), float(l.y)};
        }
        std::vector<int> group_of(tiles.size());
        for (int iter = 0; iter < 10; iter++) {
            std::vector<std::pair<float, float>> sum(groups, {0, 0});
            std::vector<float> weight(groups, 0);
            for (size_t t = 0; t < tiles.size(); t++) {
                Loc l = loc_of(tiles[t].second.front().cell);
                int best = 0;
                for (int g = 1; g < groups; g++)
                    if (std::abs(l.x - centre[g].first) + std::abs(l.y - centre[g].second) <
                        std::abs(l.x - centre[best].first) + std::abs(l.y - centre[best].second))
                        best = g;
                group_of[t] = best;
                float w = tiles[t].second.size();
                sum[best].first += w * l.x, sum[best].second += w * l.y, weight[best] += w;
            }
            for (int g = 0; g < groups; g++)
                if (weight[g] > 0)
                    centre[g] = {sum[g].first / weight[g], sum[g].second / weight[g]};
        }
        // The original keeps the group nearest to it
        Loc here = loc_of(ci);
        int keep = 0;
        for (int g = 1; g < groups; g++)
            if (std::abs(here.x - centre[g].first) + std::abs(here.y - centre[g].second) <
                std::abs(here.x - centre[keep].first) + std::abs(here.y - centre[keep].second))
                keep = g;
        bool replicated = false;
        for (int g = 0; g < groups; g++) {
            if (g == keep)
                continue;
            std::vector<PortRef> sinks;
            for (size_t t = 0; t < tiles.size(); t++)
                if (group_of[t] == g)
                    sinks.insert(sinks.end(), tiles[t].second.begin(), tiles[t].second.end());
            BelId bel = sinks.empty() ? BelId() : free_lut_bel(ci, centre[g].first, centre[g].second);
            if (bel == BelId())
                continue;
            CellInfo *copy = ctx->createCell(ctx->idf("%s$replica%d", ci->name.c_str(ctx), g), ci->type);
            copy->params = ci->params;
            copy->region = ci->region;
            for (auto &port : ci->ports) {
                if (port.second.type == PORT_IN) {
                    copy->addInput(port.first);
                    if (port.second.net)
                        copy->connectPort(port.first, port.second.net);
                } else {
                    copy->addOutput(port.first);
                }
            }
            NetInfo *copy_out = ctx->createNet(ctx->idf("%s$replica%d", out->name.c_str(ctx), g));
            copy->connectPort(id_OUT, copy_out);
            for (auto &sink : sinks) {
                sink.cell->disconnectPort(sink.port);
                sink.cell->connectPort(sink.port, copy_out);
            }
            ctx->bindBel(bel, copy, STRENGTH_WEAK);
            // As repack() sets them for the half the copy is in
            bool upper = ctx->getBelLocation(bel).z == CPE_LT_U_Z;
            copy->params[id_L2T4_UPPER] = Property(upper ? 1 : 0, 1);
            if (upper)
                copy->params.erase(id_INIT_L20);
            else if (!copy->params.count(id_INIT_L20))
                copy->params[id_INIT_L20] = Property(LUT_D1, 4);
            copies++;
            replicated = true;
        }
        nets += replicated;
    }
    ctx->assignArchInfo();
    assign_cell_info();
    log_info("Replicated the drivers of %d nets with %d copies.\n", nets, copies);
    return copies;
}

NEXTPNR_NAMESPACE_END
