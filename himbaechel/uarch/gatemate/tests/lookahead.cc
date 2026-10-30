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

// The delay tables (--vopt lookahead=table, place-delay=table) against exact shortest paths on the
// empty chip.

#include <algorithm>
#include <climits>
#include <queue>
#include <random>
#include <thread>
#include "testing.h"

USING_NEXTPNR_NAMESPACE

class GateMateLookaheadTest : public GateMateTest
{
  protected:
    void SetUp() override
    {
        GateMateTest::SetUp();
        init_context("CCGM1A1", {"--lookahead=table", "--place-delay=table"});
        const ChipInfoPOD *chip = ctx->chip_info;
        base.assign(chip->tile_insts.ssize() + 1, 0);
        for (int t = 0; t < chip->tile_insts.ssize(); t++)
            base[t + 1] = base[t] + chip_tile_info(chip, t).wires.ssize();
    }

    std::vector<int> base; // flat index of each tile's first wire

    int flat(WireId wire) const { return base[wire.tile] + wire.index; }

    WireId wire(int x, int y, const std::string &name) const
    {
        int tile = tile_by_xy(ctx->chip_info, x, y);
        auto &type = chip_tile_info(ctx->chip_info, tile);
        for (int i = 0; i < type.wires.ssize(); i++)
            if (IdString(type.wires[i].name).str(ctx) == name)
                return ctx->normalise_wire(tile, i);
        return WireId();
    }

    // Least delay from every wire to sink, counted like the router: pip delay plus the delay of the
    // wire it drives
    std::vector<int> to_sink(WireId sink) const
    {
        std::vector<int> dist(base.back(), INT_MAX);
        using Item = std::pair<int, WireId>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
        dist[flat(sink)] = 0;
        queue.push({0, sink});
        while (!queue.empty()) {
            auto [cost, wire] = queue.top();
            queue.pop();
            if (cost > dist[flat(wire)])
                continue;
            int delay = ctx->getWireDelay(wire).maxDelay();
            for (PipId pip : ctx->getPipsUphill(wire)) {
                if (!impl->pip_allowed_by_options(pip))
                    continue;
                WireId src = ctx->getPipSrcWire(pip);
                int next = cost + ctx->getPipDelay(pip).maxDelay() + delay;
                if (next < dist[flat(src)]) {
                    dist[flat(src)] = next;
                    queue.push({next, src});
                }
            }
        }
        return dist;
    }
};

TEST_F(GateMateLookaheadTest, estimates_follow_shortest_paths)
{
    const ChipInfoPOD *chip = ctx->chip_info;
    std::mt19937 rng(1);
    const std::vector<std::string> sinks = {"CPE.D0_00_int", "CPE.D1_01_int", "CPE.D0_02_int", "CPE.D1_03_int",
                                            "CPE.CLK_int",   "CPE.EN_int",    "CPE.IN5_int"};
    const std::vector<std::string> sources = {"CPE.COMBOUT1_int", "CPE.COMBOUT2_int", "CPE.DOUT1_int", "CPE.DOUT2_int"};
    // By distance from the sink, in tiles: the largest median error allowed. Within the radius (12
    // tiles in x and y) the tables are exact except for the few exceptions to the phase pattern.
    struct Bucket
    {
        int lo, hi, max_median_error;
        std::vector<int> table, linear; // |estimate - exact|
    };
    std::vector<Bucket> wires = {
            {0, 8, 25, {}, {}}, {9, 20, 50, {}, {}}, {21, 40, 150, {}, {}}, {41, 999, 300, {}, {}}};
    std::vector<Bucket> pins = wires;
    // The sinks and the wires sampled for each, drawn first, so that the searches from the sinks
    // can run at once (the draws do not depend on their results)
    struct Sink
    {
        int sx, sy;
        WireId wire;
        std::vector<WireId> random_wires, source_pins;
        std::vector<int> dist;
    };
    std::vector<Sink> drawn(8);
    for (int n = 0; n < int(drawn.size()); n++) {
        Sink &s = drawn[n];
        s.sx = 20 + rng() % (chip->width - 40);
        s.sy = 20 + rng() % (chip->height - 40);
        s.wire = wire(s.sx, s.sy, sinks[n % sinks.size()]);
        ASSERT_NE(s.wire, WireId());
        // Random wires
        for (int i = 0; i < 3000; i++) {
            int tile = rng() % chip->tile_insts.ssize();
            auto &type = chip_tile_info(chip, tile);
            if (type.wires.ssize() == 0)
                continue;
            int index = rng() % type.wires.ssize();
            if (is_root_wire(chip, tile, index))
                s.random_wires.push_back(WireId(tile, index));
        }
        // Source pins
        for (int i = 0; i < 300; i++) {
            int x = rng() % chip->width, y = rng() % chip->height;
            WireId w = wire(x, y, sources[i % sources.size()]);
            if (w != WireId())
                s.source_pins.push_back(w);
        }
    }
    std::vector<std::thread> threads;
    for (auto &s : drawn)
        threads.emplace_back([&]() { s.dist = to_sink(s.wire); });
    for (auto &thread : threads)
        thread.join();

    int unknown = 0, samples = 0, inside = 0;
    for (auto &s : drawn) {
        WireId sink = s.wire;
        auto &dist = s.dist;
        auto add = [&](std::vector<Bucket> &buckets, WireId w, int exact) {
            int x, y;
            tile_xy(chip, w.tile, x, y);
            int d = std::abs(x - s.sx) + std::abs(y - s.sy);
            delay_t table = impl->lookahead.estimate(w, sink);
            samples++;
            if (table < 0) {
                unknown++;
                return;
            }
            for (auto &b : buckets)
                if (d >= b.lo && d <= b.hi) {
                    b.table.push_back(std::abs(table - exact));
                    b.linear.push_back(std::abs(100 + 100 * d - exact));
                }
        };
        for (WireId w : s.random_wires)
            if (dist[flat(w)] != INT_MAX)
                add(wires, w, dist[flat(w)]);
        // The wires of the sink's CPE: those between its inputs and the pin once looked unreachable,
        // so the router avoided the last hops into the pin. (Its outputs only loop back through the
        // routing, and the tables, which hold a few percent of exceptions to the phase pattern, can
        // be some 10% off for them.)
        auto &type = chip_tile_info(chip, sink.tile);
        for (int index = 0; index < type.wires.ssize(); index++) {
            WireId w(sink.tile, index);
            bool in_cpe = IdString(type.wires[index].name).str(ctx).rfind("CPE.", 0) == 0;
            if (in_cpe && is_root_wire(chip, sink.tile, index) && dist[flat(w)] != INT_MAX) {
                inside++;
                EXPECT_NEAR(impl->lookahead.estimate(w, sink), dist[flat(w)], std::max(100, dist[flat(w)] / 5))
                        << ctx->nameOfWire(w);
            }
        }
        for (WireId w : s.source_pins)
            if (dist[flat(w)] != INT_MAX)
                add(pins, w, dist[flat(w)]);
    }
    auto check = [](const char *what, std::vector<Bucket> &buckets) {
        for (auto &b : buckets) {
            ASSERT_GT(b.table.size(), 10U) << what << " " << b.lo << "-" << b.hi;
            auto median = [](std::vector<int> v) {
                std::sort(v.begin(), v.end());
                return v[v.size() / 2];
            };
            int table = median(b.table), linear = median(b.linear);
            printf("%s %3d-%3d tiles: median error %4d ps (linear %4d ps), %zu samples\n", what, b.lo, b.hi, table,
                   linear, b.table.size());
            EXPECT_LE(table, b.max_median_error) << what << " " << b.lo << "-" << b.hi;
            EXPECT_LT(2 * table, linear) << what << " " << b.lo << "-" << b.hi;
        }
    };
    check("wire to pin", wires);
    check("pin to pin ", pins);
    EXPECT_LT(unknown, samples / 50);
    EXPECT_GT(inside, 8);
}

TEST_F(GateMateLookaheadTest, cp_lines_reach_the_lut_inputs)
{
    // The CP lines enter the LUT through PINY1, CINX and PINX, not through IN1..IN8; their wires once
    // all looked unreachable (20 ns), so the router hardly took them
    const int sx = 60, sy = 60;
    int checked = 0;
    for (const char *name : {"CPE.D1_00_int", "CPE.D0_01_int", "CPE.D0_03_int"}) {
        WireId sink = wire(sx, sy, name);
        ASSERT_NE(sink, WireId());
        auto dist = to_sink(sink);
        for (int dy = 0; dy <= 6; dy++)
            for (int dx = -1; dx <= 0; dx++)
                for (const char *n :
                     {"CPE.PINY1", "CPE.POUTY1", "CPE.CINY1", "CPE.COUTY1", "CPE.CINX", "CPE.PINX", "CPE.COUTX",
                      "CPE.POUTX", "CPE.CY1_OUT", "CPE.PY1_OUT", "CPE.OUT1_IN_int", "CPE.COMBOUT1_int"}) {
                    WireId w = wire(sx + dx, sy - dy, n);
                    if (w == WireId() || dist[flat(w)] == INT_MAX)
                        continue;
                    checked++;
                    EXPECT_NEAR(impl->lookahead.estimate(w, sink), dist[flat(w)], std::max(100, dist[flat(w)] / 5))
                            << ctx->nameOfWire(w) << " to " << ctx->nameOfWire(sink);
                }
    }
    EXPECT_GT(checked, 50);
}
