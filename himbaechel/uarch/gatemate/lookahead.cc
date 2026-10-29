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

#include "lookahead.h"

#include <atomic>
#include <chrono>
#include <climits>
#include <queue>
#if !defined(NPNR_DISABLE_THREADS)
#include <thread>
#endif

#include "log.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {
// How far beyond the table's radius the searches may wander
constexpr int MARGIN = 8;

int wire_delay(Context *ctx, WireId wire) { return ctx->getWireDelay(wire).maxDelay(); }
int pip_delay(Context *ctx, PipId pip) { return ctx->getPipDelay(pip).maxDelay(); }
} // namespace

void GateMateLookahead::build(Context *ctx, int radius, std::function<bool(PipId)> avail)
{
    auto start_time = std::chrono::steady_clock::now();
    const ChipInfoPOD *chip = ctx->chip_info;
    this->ctx = ctx;
    this->radius = radius;
    span = 2 * radius + 1;
    const int offsets = span * span;

    for (int i = 1; i <= 8; i++)
        anchor_names.push_back(ctx->idf("CPE.IN%d", i));
    for (const char *name : {"CPE.CLK", "CPE.EN", "CPE.SR"})
        anchor_names.push_back(ctx->id(name));
    inputs = anchor_names.size();
    for (const char *name : {"CPE.OUT1", "CPE.OUT2"})
        anchor_names.push_back(ctx->id(name));
    const int anchors = anchor_names.size();
    const int types = chip->tile_types.ssize();

    // The anchors' wire index in each tile type, if it has all of them
    std::vector<std::vector<int>> anchor_index(types);
    for (int t = 0; t < types; t++) {
        dict<IdString, int> by_name;
        auto &wires = chip->tile_types[t].wires;
        for (int i = 0; i < wires.ssize(); i++)
            by_name.emplace(IdString(wires[i].name), i);
        std::vector<int> index;
        for (auto name : anchor_names)
            if (by_name.count(name))
                index.push_back(by_name.at(name));
        if (int(index.size()) == anchors)
            anchor_index[t] = index;
    }
    auto has_cpe = [&](int tile) { return !anchor_index[chip->tile_insts[tile].type].empty(); };

    // One CPE tile near the centre per phase, the nearest (on devices of several dies the centre
    // can lie between them)
    std::vector<int> sample(PHASES, -1);
    int missing = PHASES;
    for (int d = 0; missing > 0 && d < std::max(chip->width, chip->height); d++)
        for (int dy = -d; dy <= d; dy++)
            for (int dx = -d; dx <= d; dx++) {
                if (std::max(std::abs(dx), std::abs(dy)) != d)
                    continue;
                int x = chip->width / 2 + dx, y = chip->height / 2 + dy;
                if (x < 0 || y < 0 || x >= chip->width || y >= chip->height)
                    continue;
                int tile = tile_by_xy(chip, x, y);
                if (has_cpe(tile) && sample[phase(x, y)] == -1) {
                    sample[phase(x, y)] = tile;
                    missing--;
                }
            }
    for (int p = 0; p < PHASES; p++)
        NPNR_ASSERT(sample[p] != -1);

    // Delays between any two wires of a CPE, over the tile type's own pips so that paths never
    // leave through the routing, and from them the hops between the anchors and the pins
    to_pin.resize(types);
    from_pin.resize(types);
    inside.resize(types);
    for (int t = 0; t < types; t++) {
        if (anchor_index[t].empty())
            continue;
        int tile = -1;
        for (int i = 0; i < chip->tile_insts.ssize() && tile == -1; i++)
            if (chip->tile_insts[i].type == t)
                tile = i;
        auto &type = chip->tile_types[t];
        const int wires = type.wires.ssize();
        auto &in = inside[t];
        in.id.assign(wires, -1);
        for (int i = 0; i < wires; i++)
            if (IdString(type.wires[i].name).str(ctx).rfind("CPE.", 0) == 0)
                in.id[i] = in.size++;
        std::vector<std::vector<std::pair<int, int>>> downhill(in.size); // (wire id, delay)
        for (int i = 0; i < type.pips.ssize(); i++) {
            auto &pip = type.pips[i];
            PipId pip_id(tile, i);
            if (in.id[pip.src_wire] != -1 && in.id[pip.dst_wire] != -1 && avail(pip_id))
                downhill[in.id[pip.src_wire]].emplace_back(
                        in.id[pip.dst_wire],
                        pip_delay(ctx, pip_id) + wire_delay(ctx, ctx->normalise_wire(tile, pip.dst_wire)));
        }
        in.delay.assign(in.size * in.size, -1);
        for (int from = 0; from < in.size; from++) {
            std::vector<int> dist(in.size, INT_MAX);
            using Item = std::pair<int, int>;
            std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
            dist[from] = 0;
            queue.push({0, from});
            while (!queue.empty()) {
                auto [cost, wire] = queue.top();
                queue.pop();
                if (cost > dist[wire])
                    continue;
                in.delay[from * in.size + wire] = cost;
                for (auto [next, delay] : downhill[wire])
                    if (cost + delay < dist[next]) {
                        dist[next] = cost + delay;
                        queue.push({dist[next], next});
                    }
            }
        }
        for (int i = 0; i < wires; i++) {
            to_pin[t].start.push_back(to_pin[t].hops.size());
            from_pin[t].start.push_back(from_pin[t].hops.size());
            if (in.id[i] == -1)
                continue;
            for (int k = 0; k < anchors; k++) {
                int anchor = in.id[anchor_index[t][k]];
                if (k < inputs && in.delay[anchor * in.size + in.id[i]] != -1)
                    to_pin[t].hops.push_back(Hop{k, in.delay[anchor * in.size + in.id[i]]});
                if (k >= inputs && in.delay[in.id[i] * in.size + anchor] != -1)
                    from_pin[t].hops.push_back(Hop{k, in.delay[in.id[i] * in.size + anchor]});
            }
        }
        to_pin[t].start.push_back(to_pin[t].hops.size());
        from_pin[t].start.push_back(from_pin[t].hops.size());
    }

    // Slots: the tile type at each offset of each phase, and where its delays start
    slot_type.assign(PHASES * offsets, -1);
    slot_base.assign(PHASES * anchors * offsets, -1);
    int64_t total = 0;
    for (int p = 0; p < PHASES; p++) {
        int sx, sy;
        tile_xy(chip, sample[p], sx, sy);
        for (int oy = -radius; oy <= radius; oy++)
            for (int ox = -radius; ox <= radius; ox++) {
                int x = sx + ox, y = sy + oy;
                if (x < 0 || y < 0 || x >= chip->width || y >= chip->height)
                    continue;
                int o = (oy + radius) * span + (ox + radius);
                int type = chip->tile_insts[tile_by_xy(chip, x, y)].type;
                slot_type[p * offsets + o] = type;
                for (int k = 0; k < anchors; k++) {
                    slot_base[(p * anchors + k) * offsets + o] = total;
                    total += chip->tile_types[type].wires.ssize();
                }
            }
    }
    values.assign(total, MISSING);

    // Shortest paths from each anchor of each phase's sample: backwards from the inputs, forwards
    // from the outputs. The last job searches the whole chip from an input, for the slopes.
    const int axis = std::max(chip->width, chip->height);
    std::vector<int> axis_x, axis_y; // least delay from an output at sample[0] + (d, 0) or (0, d)
    auto search = [&](int job, std::vector<int> &dist, std::vector<int> &tile_base) {
        bool whole_chip = job == PHASES * anchors;
        int p = whole_chip ? 0 : job / anchors, k = whole_chip ? 0 : job % anchors;
        bool forward = k >= inputs;
        int sx, sy;
        tile_xy(chip, sample[p], sx, sy);
        int reach = whole_chip ? axis : radius + MARGIN;
        int x0 = std::max(0, sx - reach), x1 = std::min(chip->width - 1, sx + reach);
        int y0 = std::max(0, sy - reach), y1 = std::min(chip->height - 1, sy + reach);
        int w = x1 - x0 + 1;
        tile_base.assign(w * (y1 - y0 + 1) + 1, 0);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                int i = (y - y0) * w + (x - x0);
                tile_base[i + 1] = tile_base[i] + chip_tile_info(chip, tile_by_xy(chip, x, y)).wires.ssize();
            }
        dist.assign(tile_base.back(), INT_MAX);
        auto local = [&](WireId wire) {
            int x, y;
            tile_xy(chip, wire.tile, x, y);
            if (x < x0 || x > x1 || y < y0 || y > y1)
                return -1;
            return tile_base[(y - y0) * w + (x - x0)] + wire.index;
        };

        WireId start = ctx->normalise_wire(sample[p], anchor_index[chip->tile_insts[sample[p]].type][k]);
        using Item = std::pair<int, WireId>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
        dist[local(start)] = 0;
        queue.push({0, start});
        while (!queue.empty()) {
            auto [cost, wire] = queue.top();
            queue.pop();
            if (cost > dist[local(wire)])
                continue;
            if (!whole_chip) {
                int x, y;
                tile_xy(chip, wire.tile, x, y);
                int ox = x - sx, oy = y - sy;
                if (std::abs(ox) <= radius && std::abs(oy) <= radius) {
                    int o = (oy + radius) * span + (ox + radius);
                    values[slot_base[(p * anchors + k) * offsets + o] + wire.index] = std::min(cost, MISSING - 1);
                }
            }
            auto relax = [&](PipId pip, WireId next, int delay) {
                int i = local(next);
                if (i == -1 || !avail(pip))
                    return;
                int next_cost = cost + pip_delay(ctx, pip) + delay;
                if (next_cost < dist[i]) {
                    dist[i] = next_cost;
                    queue.push({next_cost, next});
                }
            };
            if (forward) {
                for (PipId pip : ctx->getPipsDownhill(wire)) {
                    WireId next = ctx->getPipDstWire(pip);
                    relax(pip, next, wire_delay(ctx, next));
                }
            } else {
                int delay = wire_delay(ctx, wire);
                for (PipId pip : ctx->getPipsUphill(wire))
                    relax(pip, ctx->getPipSrcWire(pip), delay);
            }
        }
        if (whole_chip) {
            auto at = [&](int x, int y) {
                int best = INT_MAX;
                if (x < 0 || y < 0 || x >= chip->width || y >= chip->height || !has_cpe(tile_by_xy(chip, x, y)))
                    return best;
                int tile = tile_by_xy(chip, x, y);
                for (int j = inputs; j < anchors; j++) {
                    int i = local(ctx->normalise_wire(tile, anchor_index[chip->tile_insts[tile].type][j]));
                    if (i != -1)
                        best = std::min(best, dist[i]);
                }
                return best;
            };
            for (int d = -axis; d <= axis; d++) {
                axis_x.push_back(at(sx + d, sy));
                axis_y.push_back(at(sx, sy + d));
            }
        }
    };

    const int jobs = PHASES * anchors + 1;
    std::atomic<int> next_job{0};
    auto worker = [&]() {
        std::vector<int> dist, tile_base;
        for (int job = next_job++; job < jobs; job = next_job++)
            search(job, dist, tile_base);
    };
#if !defined(NPNR_DISABLE_THREADS)
    std::vector<std::thread> threads;
    int count = std::max(1, std::min(jobs, int(std::thread::hardware_concurrency())));
    for (int i = 0; i < count; i++)
        threads.emplace_back(worker);
    for (auto &thread : threads)
        thread.join();
#else
    worker();
#endif

    // Slopes: growth of the delay from an output to an input of the same phase, from the first
    // multiple of 4 beyond the radius to 48 tiles further
    int near = (radius + 3) / 4 * 4, far = near + 48;
    auto slope = [&](const std::vector<int> &line) {
        float sum = 0;
        int count = 0;
        for (int sign : {-1, 1}) {
            int a = line[axis + sign * near], b = line[axis + sign * far];
            if (a != INT_MAX && b != INT_MAX) {
                sum += std::max(0.0f, float(b - a) / (far - near));
                count++;
            }
        }
        return count ? sum / count : 0.0f;
    };
    slope_x = slope(axis_x);
    slope_y = slope(axis_y);

    // Remaps from every tile type into those of the slots
    pool<int> slot_types;
    for (int type : slot_type)
        if (type != -1)
            slot_types.insert(type);
    remap.assign(types, std::vector<std::vector<int>>(types));
    for (int to : slot_types) {
        dict<IdString, int> by_name;
        auto &wires = chip->tile_types[to].wires;
        for (int i = 0; i < wires.ssize(); i++)
            by_name.emplace(IdString(wires[i].name), i);
        for (int from = 0; from < types; from++) {
            if (from == to)
                continue;
            auto &from_wires = chip->tile_types[from].wires;
            auto &map = remap[from][to];
            for (int i = 0; i < from_wires.ssize(); i++) {
                auto found = by_name.find(IdString(from_wires[i].name));
                map.push_back(found == by_name.end() ? -1 : found->second);
            }
        }
    }

    log_info("Built the GateMate delay tables: radius %d, %.0f MB, %.1f/%.1f ps per tile beyond, %.1fs\n", radius,
             values.size() * sizeof(uint16_t) / 1e6, slope_x, slope_y,
             std::chrono::duration<float>(std::chrono::steady_clock::now() - start_time).count());
}

int GateMateLookahead::lookup(int k, int anchor_tile, WireId w) const
{
    const ChipInfoPOD *chip = ctx->chip_info;
    int ax, ay, wx, wy;
    tile_xy(chip, anchor_tile, ax, ay);
    tile_xy(chip, w.tile, wx, wy);
    // Beyond the radius, step back to the same phase by multiples of 4
    auto clamp = [&](int &d) {
        if (std::abs(d) <= radius)
            return 0;
        int steps = (std::abs(d) - radius + 3) / 4 * 4;
        d += d > 0 ? -steps : steps;
        return steps;
    };
    int ox = wx - ax, oy = wy - ay;
    int ex = clamp(ox), ey = clamp(oy);
    const int offsets = span * span, o = (oy + radius) * span + (ox + radius), p = phase(ax, ay);
    int type = slot_type[p * offsets + o];
    if (type == -1)
        return -1;
    int index = w.index, wire_type = chip->tile_insts[w.tile].type;
    if (wire_type != type) {
        auto &map = remap[wire_type][type];
        if (map.empty() || map[index] == -1)
            return -1;
        index = map[index];
    }
    uint16_t value = values[slot_base[(p * int(anchor_names.size()) + k) * offsets + o] + index];
    if (value == MISSING)
        return UNREACHABLE;
    return value + int(slope_x * ex + slope_y * ey);
}

delay_t GateMateLookahead::estimate(WireId src, WireId dst) const
{
    auto best_of = [&](const Hops &hops, WireId pin, WireId other) {
        int best = -1;
        if (hops.start.empty())
            return best;
        for (int i = hops.start[pin.index]; i < hops.start[pin.index + 1]; i++) {
            int delay = lookup(hops.hops[i].anchor, pin.tile, other);
            if (delay != -1 && (best == -1 || delay + hops.hops[i].delay < best))
                best = delay + hops.hops[i].delay;
        }
        return best;
    };
    const ChipInfoPOD *chip = ctx->chip_info;
    if (src.tile == dst.tile) {
        auto &in = inside[chip->tile_insts[src.tile].type];
        if (!in.id.empty() && in.id[src.index] != -1 && in.id[dst.index] != -1) {
            int delay = in.delay[in.id[src.index] * in.size + in.id[dst.index]];
            if (delay != -1)
                return delay;
        }
    }
    int to_dst = best_of(to_pin[chip->tile_insts[dst.tile].type], dst, src);
    if (to_dst != -1)
        return to_dst;
    return best_of(from_pin[chip->tile_insts[src.tile].type], src, dst);
}

NEXTPNR_NAMESPACE_END
