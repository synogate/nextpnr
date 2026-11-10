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

#include "axis.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <chrono>
#include <map>
#include <queue>
#include <regex>
#if !defined(NPNR_DISABLE_THREADS)
#include <thread>
#endif

#include "log.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {
// Directions 1..4: east, north, west, south (the SB outputs Y1..Y4)
const int UX[5] = {0, 1, 0, -1, 0};
const int UY[5] = {0, 0, 1, 0, -1};
// Diagonal step at a corner: none, (1, 1) and (-1, -1) (X34 / X12)
const int OX[3] = {0, 1, -1};
const int OY[3] = {0, 1, -1};

int pip_delay(Context *ctx, PipId pip) { return ctx->getPipDelay(pip).maxDelay(); }
int wire_delay(Context *ctx, WireId wire) { return ctx->getWireDelay(wire).maxDelay(); }
int mod4(int v) { return ((v % 4) + 4) % 4; }
int sb_class(int x, int y) { return mod4(x) * 4 + mod4(y); }
int ring_distance(int p, int q)
{
    int d = std::abs(p - q);
    return std::min(d, 12 - d);
}

// What a pip drives, by the name of its destination wire in the tile
struct PipKind
{
    uint8_t y_plane = 0, y_dir = 0; // an SB output Yi of plane p (y_dir 0: not one)
    int8_t cp = -1;                  // a CP line output (COUTX, COUTY1, COUTY2, POUTX, POUTY1, POUTY2)
    uint8_t plane = 0;               // plane of an SB wire, 0 otherwise
    bool sb = false, cpe = false, big = false;
};

template <typename T> int median_of(std::vector<T> &values, int fallback)
{
    if (values.empty())
        return fallback;
    std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
    return values[values.size() / 2];
}
} // namespace

namespace {
std::atomic<uint64_t> next_build_id{1};

// The estimate depends on the positions only through the classes (x mod 4, y mod 4) of the two wires'
// tiles and their offset, so it is cached per thread by (class, wire, class, wire, dx, dy)
struct EstimateCache
{
    static constexpr int BITS = 20;
    uint64_t build_id = 0;
    std::vector<uint64_t> keys;
    std::vector<int32_t> values;
    // GATEMATE_AXIS_STATS: calls, cache hits, calls outside the tables, time in misses (ns)
    int64_t calls = 0, hits = 0, outside = 0, miss_ns = 0;
    // work in compute(): exit groups, exits processed, fallbacks, entry loop steps, paths
    int64_t w_exits = 0, w_done = 0, w_fallback = 0, w_inner = 0, w_paths = 0;
    // by (source wire name, destination wire name): misses and their time
    std::map<std::pair<int, int>, std::pair<int64_t, int64_t>> miss_by_name;
};
thread_local EstimateCache estimate_cache;
} // namespace

namespace {
std::mutex kept_mutex;
std::map<std::string, GateMateAxis> kept;
} // namespace

void GateMateAxis::build(Context *ctx, std::function<bool(PipId)> avail, int prune, int keep,
                         const std::string &key)
{
    if (!key.empty()) {
        std::lock_guard<std::mutex> lock(kept_mutex);
        auto found = kept.find(key);
        if (found != kept.end()) {
            *this = found->second;
            this->ctx = ctx;
            return;
        }
    }
    auto start_time = std::chrono::steady_clock::now();
    this->ctx = ctx;
    build_id = next_build_id++;
    check_mode = getenv("GATEMATE_AXIS_CHECK") != nullptr;
    stats_mode = getenv("GATEMATE_AXIS_STATS") != nullptr;
    const ChipInfoPOD *chip = ctx->chip_info;
    shape_anchor.assign(chip->node_shapes.ssize(), Anchor());
    for (int n = 0; n < chip->node_shapes.ssize(); n++) {
        // (the first wire of a shape is the root, at (0, 0))
        auto &wires = chip->node_shapes[n].tile_wires;
        if (wires.ssize() == 0)
            continue;
        int best = 0;
        for (int i = 1; i < wires.ssize(); i++)
            if (std::make_tuple(wires[i].dx, wires[i].dy, wires[i].wire) <
                std::make_tuple(wires[best].dx, wires[best].dy, wires[best].wire))
                best = i;
        if (best != 0) {
            shape_anchor[n].dx = wires[best].dx;
            shape_anchor[n].dy = wires[best].dy;
            shape_anchor[n].index = wires[best].wire;
        }
    }
    const int types = chip->tile_types.ssize();
    auto type_of = [&](int tile) { return chip->tile_insts[tile].type; };

    // Pip kinds per tile type, and per tile type the pip that drives each Yi of each plane and each CP line
    static const std::regex y_re(R"(SB_(BIG|SML)\.P(\d\d)\.Y([1-4]))"), plane_re(R"(\.P(\d\d)\.)");
    static const char *cp_names[6] = {"CPE.COUTX", "CPE.COUTY1", "CPE.COUTY2",
                                      "CPE.POUTX", "CPE.POUTY1", "CPE.POUTY2"};
    std::vector<std::vector<PipKind>> kinds(types);
    std::vector<std::vector<int>> y_pip(types, std::vector<int>(13 * 5, -1)), cp_pip(types, std::vector<int>(6, -1));
    for (int t = 0; t < types; t++) {
        auto &type = chip->tile_types[t];
        std::vector<PipKind> wire_kind(type.wires.ssize());
        for (int w = 0; w < type.wires.ssize(); w++) {
            std::string name = IdString(type.wires[w].name).str(ctx);
            PipKind &k = wire_kind[w];
            std::smatch m;
            if (std::regex_match(name, m, y_re)) {
                k.y_plane = std::stoi(m[2]);
                k.big = m[1] == "BIG";
                k.y_dir = std::stoi(m[3]);
            }
            for (int l = 0; l < 6; l++)
                if (name == cp_names[l])
                    k.cp = l;
            k.sb = name.rfind("SB_", 0) == 0;
            k.cpe = name.rfind("CPE.", 0) == 0;
            if (k.sb && std::regex_search(name, m, plane_re))
                k.plane = std::stoi(m[1]);
        }
        kinds[t].resize(type.pips.ssize());
        for (int p = 0; p < type.pips.ssize(); p++) {
            const PipKind &k = wire_kind[type.pips[p].dst_wire];
            kinds[t][p] = k;
            if (k.y_dir && y_pip[t][k.y_plane * 5 + k.y_dir] == -1)
                y_pip[t][k.y_plane * 5 + k.y_dir] = p;
            if (k.cp >= 0 && cp_pip[t][k.cp] == -1)
                cp_pip[t][k.cp] = p;
        }
    }
    auto kind = [&](PipId pip) -> const PipKind & { return kinds[type_of(pip.tile)][pip.index]; };
    auto on_chip = [&](int x, int y) { return x >= 0 && y >= 0 && x < chip->width && y < chip->height; };
    // The node of output Yi of plane p of the SB at (x, y), or of CP line l leaving the CPE at (x, y)
    auto y_node = [&](int x, int y, int plane, int dir) {
        if (!on_chip(x, y))
            return WireId();
        int tile = tile_by_xy(chip, x, y);
        int p = y_pip[type_of(tile)][plane * 5 + dir];
        return p == -1 ? WireId() : ctx->getPipDstWire(PipId(tile, p));
    };
    auto cp_node = [&](int x, int y, int line) {
        if (!on_chip(x, y))
            return WireId();
        int tile = tile_by_xy(chip, x, y);
        int p = cp_pip[type_of(tile)][line];
        return p == -1 ? WireId() : ctx->getPipDstWire(PipId(tile, p));
    };
    // Whether a node is an SB output or a CP line node: the pips into it say so
    auto terminal = [&](WireId wire, State &state, int &tile) {
        for (PipId pip : ctx->getPipsUphill(wire)) {
            const PipKind &k = kind(pip);
            if (k.y_dir) {
                state.plane = k.y_plane;
                state.dir = k.y_dir;
                state.cp = 0;
                tile = pip.tile;
                return true;
            }
            if (k.cp >= 0) {
                state.plane = 0;
                state.dir = k.cp;
                state.cp = 1;
                tile = pip.tile;
                return true;
            }
        }
        return false;
    };

    // Dijkstra over the pips `follow` allows, forwards or backwards, up to `limit` ps; `visit(wire, cost)`
    // returns false where the search is not to go on from the wire
    using Item = std::pair<int, WireId>;
    auto search = [&](WireId start, bool forward, int limit, auto follow, auto visit, dict<WireId, int> &dist) {
        dist.clear();
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
        dist[start] = 0;
        queue.push({0, start});
        while (!queue.empty()) {
            auto [cost, wire] = queue.top();
            queue.pop();
            if (cost > dist.at(wire))
                continue;
            if (!visit(wire, cost) || cost > limit)
                continue;
            auto relax = [&](PipId pip, WireId next, int delay) {
                if (!avail(pip) || !follow(pip))
                    return;
                int next_cost = cost + pip_delay(ctx, pip) + delay;
                auto found = dist.find(next);
                if (found == dist.end() || next_cost < found->second) {
                    dist[next] = next_cost;
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
    };
    auto run_jobs = [&](int jobs, auto fn) {
        std::atomic<int> next_job{0};
        auto worker = [&]() {
            for (int job = next_job++; job < jobs; job = next_job++)
                fn(job);
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
    };
    const int cx = chip->width / 2 / 4 * 4, cy = chip->height / 2 / 4 * 4;
    auto get = [](const dict<WireId, int> &dist, WireId wire) {
        if (wire == WireId())
            return INF;
        auto found = dist.find(wire);
        return found == dist.end() ? INF : found->second;
    };

    // RUN and TURN: one search per SB class and direction on plane 1, SB wires only, from an SB near the
    // edge the run starts from, so that it spans the chip
    auto plane1 = [&](PipId pip) {
        const PipKind &k = kind(pip);
        return k.sb && k.plane == 1;
    };
    std::vector<std::vector<int>> turn_samples(16 * 5 * 5 * 3);
    std::mutex turn_mutex;
    for (int c = 0; c < 16; c++)
        for (int i = 0; i < 5; i++)
            run[c][i].clear();
    run_jobs(16 * 4, [&](int job) {
        int c = job / 4, i = job % 4 + 1;
        int ccx = c / 4, ccy = c % 4;
        if ((ccx + ccy) % 2)
            return; // no SB there
        int x0 = cx + ccx, y0 = cy + ccy;
        if (i == 1)
            x0 = 8 + ccx;
        if (i == 3)
            x0 = (chip->width - 9) / 4 * 4 + ccx;
        if (i == 2)
            y0 = 8 + ccy;
        if (i == 4)
            y0 = (chip->height - 9) / 4 * 4 + ccy;
        WireId start = y_node(x0, y0, 1, i);
        if (start == WireId())
            return;
        dict<WireId, int> dist;
        search(start, true, INF, plane1, [](WireId, int) { return true; }, dist);
        std::vector<int> values;
        for (int a = 0;; a += 2) {
            int x = x0 + UX[i] * a, y = y0 + UY[i] * a;
            if (!on_chip(x, y))
                break;
            values.push_back(get(dist, y_node(x, y, 1, i)));
        }
        std::vector<std::pair<int, int>> samples; // (slot, value)
        for (int a = 4; a <= 20; a += 2) {
            int x = x0 + UX[i] * a, y = y0 + UY[i] * a;
            int base = get(dist, y_node(x, y, 1, i));
            if (base >= INF)
                continue;
            for (int j = 1; j <= 4; j++)
                for (int o = 0; o < 3; o++) {
                    int value = get(dist, y_node(x + OX[o], y + OY[o], 1, j));
                    if (j != i && value < INF)
                        samples.emplace_back(((sb_class(x, y) * 5 + i) * 5 + j) * 3 + o, value - base);
                }
        }
        std::lock_guard<std::mutex> lock(turn_mutex);
        run[c][i] = std::move(values);
        for (auto [slot, value] : samples)
            turn_samples[slot].push_back(value);
    });
    for (int c = 0; c < 16; c++)
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 5; j++)
                for (int o = 0; o < 3; o++)
                    turn[c][i][j][o] = median_of(turn_samples[((c * 5 + i) * 5 + j) * 3 + o], INF);
    min_turn = INF;
    for (int c = 0; c < 16; c++)
        for (int i = 1; i <= 4; i++)
            for (int j = 1; j <= 4; j++)
                for (int o = 0; o < 3; o++)
                    if (i != j)
                        min_turn = std::min(min_turn, turn[c][i][j][o]);
    for (int c = 0; c < 16; c++) {
        int tile = tile_by_xy(chip, cx + c / 4, cy + c % 4);
        int p = y_pip[type_of(tile)][1 * 5 + 1];
        big[c] = p != -1 && kinds[type_of(tile)][p].big;
    }
    for (int i = 1; i <= 4; i++) {
        int length = std::max(chip->width, chip->height) + 2;
        envelope[i].assign(length + 1, INF);
        for (int c = 0; c < 16; c++)
            for (int k = 0; k < int(run[c][i].size()) && 2 * k <= length; k++)
                envelope[i][2 * k] = std::min(envelope[i][2 * k], run[c][i][k]);
        for (int d = length - 1; d >= 0; d--)
            envelope[i][d] = std::min(envelope[i][d], envelope[i][d + 1]);
    }

    // Plane changes: from an SB near the centre over SB wires, by distance on the ring, at targets that need
    // a turn and straight ahead. (Over all wires, a way through a CPE can be cheaper than several steps on
    // the ring; that is logged for comparison, but depends on the planes and the CPE too much for one term.)
    for (bool all_wires : {true, false}) {
        WireId start = WireId();
        int x0 = cx, y0 = cy;
        for (int d = 0; d < 4 && start == WireId(); d++)
            start = y_node(x0 = cx + d, y0 = cy, 1, 1);
        dict<WireId, int> dist;
        search(start, true, 9000, [&](PipId pip) { return all_wires || kind(pip).sb; },
               [](WireId, int) { return true; }, dist);
        std::vector<int> turned[7], straight[7];
        for (int dy = -20; dy <= 20; dy++)
            for (int dx = -20; dx <= 20; dx++) {
                bool is_straight = dy == 0 && dx >= 6;
                if (!is_straight && (std::abs(dx) < 6 || std::abs(dy) < 6))
                    continue;
                for (int j = 1; j <= 4; j++) {
                    if (is_straight && j != 1)
                        continue;
                    int base = get(dist, y_node(x0 + dx, y0 + dy, 1, j));
                    if (base >= INF)
                        continue;
                    for (int q = 2; q <= 12; q++) {
                        int value = get(dist, y_node(x0 + dx, y0 + dy, q, j));
                        if (value < INF)
                            (is_straight ? straight : turned)[ring_distance(1, q)].push_back(value - base);
                    }
                }
            }
        ring_turn[0] = ring_straight[0] = 0;
        for (int r = 1; r <= 6; r++) {
            ring_turn[r] = median_of(turned[r], 220 * r);
            ring_straight[r] = median_of(straight[r], ring_turn[r] + 350);
        }
        log_info("GateMate axis: plane change over %s wires by ring distance 1..6: with a turn %d %d %d %d %d %d, "
                 "straight %d %d %d %d %d %d ps\n",
                 all_wires ? "all" : "SB", ring_turn[1], ring_turn[2], ring_turn[3], ring_turn[4], ring_turn[5],
                 ring_turn[6], ring_straight[1], ring_straight[2], ring_straight[3], ring_straight[4],
                 ring_straight[5], ring_straight[6]);
    }

    // CP lines: from each line leaving a CPE near the centre, over CPE wires only
    cp_delay.assign(6 * 6 * CP_SPAN * CP_SPAN, INF);
    run_jobs(6, [&](int l) {
        WireId start = cp_node(cx, cy, l);
        if (start == WireId())
            return;
        dict<WireId, int> dist;
        search(start, true, 6000, [&](PipId pip) { return kind(pip).cpe; }, [](WireId, int) { return true; }, dist);
        for (int m = 0; m < 6; m++)
            for (int dx = 0; dx < CP_SPAN; dx++)
                for (int dy = 0; dy < CP_SPAN; dy++)
                    cp_delay[((l * 6 + m) * CP_SPAN + dx) * CP_SPAN + dy] = get(dist, cp_node(cx + dx, cy + dy, m));
    });

    // Exits and entries of every wire of a sample tile per class (x mod 4, y mod 4) near the centre: the
    // first SB outputs and CP line nodes a search from the wire reaches (forwards), and the last ones
    // before it (backwards)
    const int limit = 3000;
    int sample_tile[16];
    std::vector<std::vector<std::vector<State>>> found_exits(16), found_entries(16);
    std::vector<std::pair<int, int>> wire_jobs;
    for (int c = 0; c < 16; c++) {
        int tile = tile_by_xy(chip, cx + c / 4, cy + c % 4);
        sample_tile[c] = tile;
        classes[c] = ClassData();
        classes[c].type = type_of(tile);
        int wires = chip->tile_types[type_of(tile)].wires.ssize();
        found_exits[c].resize(wires);
        found_entries[c].resize(wires);
        for (int w = 0; w < wires; w++)
            if (anchor(ctx->normalise_wire(tile, w)) == WireId(tile, w))
                wire_jobs.emplace_back(c, w);
    }
    run_jobs(wire_jobs.size(), [&](int job) {
        auto [c, w] = wire_jobs[job];
        int tile = sample_tile[c], tx, ty;
        tile_xy(chip, tile, tx, ty);
        WireId start = ctx->normalise_wire(tile, w);
        dict<WireId, int> dist;
        for (bool forward : {true, false}) {
            dict<std::tuple<int, int, int, int>, int> best; // (tile, plane, dir, cp) -> delay
            search(start, forward, limit, [](PipId) { return true; },
                   [&](WireId wire, int cost) {
                       State s;
                       int at;
                       if (!terminal(wire, s, at))
                           return true;
                       auto key = std::make_tuple(at, int(s.plane), int(s.dir), int(s.cp));
                       auto found = best.find(key);
                       if (found == best.end() || cost < found->second)
                           best[key] = cost;
                       return wire == start; // go on only from the start
                   },
                   dist);
            // Keep the states within `prune` of the best one in the same direction: a costlier one in another
            // direction (e.g. through a neighbouring CPE, which changes direction and plane) may be the way
            int least[6] = {INF, INF, INF, INF, INF, INF};
            auto group_of = [](int dir, int cp) { return cp ? 5 : dir; };
            for (auto &entry : best) {
                int g = group_of(std::get<2>(entry.first), std::get<3>(entry.first));
                least[g] = std::min(least[g], entry.second);
            }
            std::vector<State> states;
            for (auto &entry : best) {
                auto [at, plane, dir, cp] = entry.first;
                if (entry.second > least[group_of(dir, cp)] + prune)
                    continue;
                int x, y;
                tile_xy(chip, at, x, y);
                State s;
                s.dx = x - tx;
                s.dy = y - ty;
                s.plane = plane;
                s.dir = dir;
                s.cp = cp;
                s.cost = entry.second;
                states.push_back(s);
            }
            std::sort(states.begin(), states.end(), [](const State &a, const State &b) { return a.cost < b.cost; });
            // Drop the states another, cheaper one reaches (exits) or is reached from (entries) at no more
            // cost, e.g. the SB outputs further along the lane of the first one
            std::vector<State> kept;
            for (auto &st : states) {
                if (int(kept.size()) >= keep)
                    break;
                bool dominated = false;
                for (auto &k : kept) {
                    if (k.cp || st.cp)
                        continue;
                    int cost = forward ? link(k, tx + k.dx, ty + k.dy, st, tx + st.dx, ty + st.dy)
                                       : link(st, tx + st.dx, ty + st.dy, k, tx + k.dx, ty + k.dy);
                    if (cost < INF && k.cost + cost <= st.cost) {
                        dominated = true;
                        break;
                    }
                }
                if (!dominated)
                    kept.push_back(st);
            }
            (forward ? found_exits : found_entries)[c][w] = std::move(kept);
        }
    });
    size_t total = 0, groups = 0;
    for (int c = 0; c < 16; c++) {
        auto &cd = classes[c];
        cd.exit_start.clear();
        for (auto &list : found_exits[c]) {
            cd.exit_start.push_back(cd.exits.size());
            total += list.size();
            std::vector<ExitGroup> wire_groups;
            for (auto &st : list) {
                ExitGroup *g = nullptr;
                for (auto &existing : wire_groups)
                    if (existing.dx == st.dx && existing.dy == st.dy && existing.dir == st.dir && existing.cp == st.cp)
                        g = &existing;
                if (!g) {
                    wire_groups.emplace_back();
                    g = &wire_groups.back();
                    g->dx = st.dx;
                    g->dy = st.dy;
                    g->dir = st.dir;
                    g->cp = st.cp;
                    g->least = INF;
                    for (int p = 0; p < 13; p++)
                        g->cost[p] = INF;
                }
                g->least = std::min(g->least, int(st.cost));
                g->cost[st.plane] = std::min(g->cost[st.plane], int(st.cost));
            }
            for (auto &g : wire_groups) {
                g.plane_count = 0;
                for (int p = 1; p < 13; p++)
                    if (g.cost[p] < INF)
                        g.planes[g.plane_count++] = p;
            }
            cd.exits.insert(cd.exits.end(), wire_groups.begin(), wire_groups.end());
        }
        cd.exit_start.push_back(cd.exits.size());
        groups += cd.exits.size();
        cd.exit_reach.clear();
        for (int w = 0; w + 1 < int(cd.exit_start.size()); w++) {
            int reach = 0;
            for (int e = cd.exit_start[w]; e < cd.exit_start[w + 1]; e++)
                reach = std::max(reach, std::max(std::abs(int(cd.exits[e].dx)), std::abs(int(cd.exits[e].dy))));
            cd.exit_reach.push_back(reach);
        }
        cd.entry_start.clear();
        cd.least_entry.clear();
        cd.entry_reach.clear();
        for (auto &list : found_entries[c]) {
            cd.entry_start.push_back(cd.entries.size());
            total += list.size();
            std::vector<EntryGroup> wire_groups;
            for (auto &st : list) {
                EntryGroup *g = nullptr;
                for (auto &existing : wire_groups)
                    if (existing.dx == st.dx && existing.dy == st.dy && existing.dir == st.dir && existing.cp == st.cp)
                        g = &existing;
                if (!g) {
                    wire_groups.emplace_back();
                    g = &wire_groups.back();
                    g->dx = st.dx;
                    g->dy = st.dy;
                    g->dir = st.dir;
                    g->cp = st.cp;
                    g->least = INF;
                    for (int t = 0; t < 2; t++)
                        for (int p = 0; p < 13; p++)
                            g->cost[t][p] = INF;
                }
                g->least = std::min(g->least, int(st.cost));
                for (int p = 0; p < 13; p++) {
                    if (st.cp) {
                        g->cost[0][p] = g->cost[1][p] = std::min(g->cost[0][p], int(st.cost));
                        continue;
                    }
                    if (p == 0)
                        continue;
                    int r = ring_distance(p, st.plane);
                    g->cost[0][p] = std::min(g->cost[0][p], int(st.cost) + ring_straight[r]);
                    g->cost[1][p] = std::min(g->cost[1][p], int(st.cost) + ring_turn[r]);
                }
            }
            std::sort(wire_groups.begin(), wire_groups.end(),
                      [](const EntryGroup &a, const EntryGroup &b) { return a.least < b.least; });
            cd.entries.insert(cd.entries.end(), wire_groups.begin(), wire_groups.end());
            for (int p = 0; p < 13; p++) {
                int least = INF;
                for (auto &g : wire_groups)
                    least = std::min(least, int(g.cost[1][p]));
                cd.least_entry.push_back(least);
            }
            int reach = 0;
            for (auto &g : wire_groups)
                reach = std::max(reach, std::max(std::abs(int(g.dx)), std::abs(int(g.dy))));
            cd.entry_reach.push_back(reach);
        }
        cd.entry_start.push_back(cd.entries.size());
        groups += cd.entries.size();
    }

    // The tiles the tables apply to: those of the sample's type, away from the edge
    tile_class.assign(chip->tile_insts.ssize(), -1);
    for (int tile = 0; tile < chip->tile_insts.ssize(); tile++) {
        int x, y;
        tile_xy(chip, tile, x, y);
        int c = sb_class(x, y);
        if (x >= 2 && y >= 2 && x < chip->width - 2 && y < chip->height - 2 && type_of(tile) == classes[c].type)
            tile_class[tile] = c;
    }

    int far_entries = 0, max_reach = 0;
    for (int c = 0; c < 16; c++)
        for (int r : classes[c].entry_reach) {
            far_entries += r > 4;
            max_reach = std::max(max_reach, r);
        }
    log_info("GateMate axis: %d wires have entries more than 4 tiles away (at most %d)\n", far_entries, max_reach);
    float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - start_time).count();
    log_info("Built the GateMate axis tables: %zu exit/entry states (%.1f per wire, %.1f exit and entry groups), "
             "turn %d/%d ps (small/big SB, least %d), plane step %d ps, %.1fs\n",
             total, wire_jobs.empty() ? 0.0 : double(total) / (2 * wire_jobs.size()),
             wire_jobs.empty() ? 0.0 : double(groups) / wire_jobs.size(),
             turn[sb_class(cx, cy)][1][2][0], turn[sb_class(cx + 2, cy)][1][2][0], min_turn, ring_turn[1], seconds);
    if (!key.empty()) {
        std::lock_guard<std::mutex> lock(kept_mutex);
        kept.emplace(key, *this);
    }
}

WireId GateMateAxis::anchor(WireId wire) const
{
    const ChipInfoPOD *chip = ctx->chip_info;
    auto &ts = chip_tile_shape(chip, wire.tile);
    if (wire.index >= ts.wire_to_node.ssize())
        return wire;
    auto &w2n = ts.wire_to_node[wire.index];
    if (w2n.dx_mode != RelNodeRefPOD::MODE_IS_ROOT)
        return wire;
    const Anchor &a = shape_anchor[node_shape_idx(w2n)];
    if (a.index < 0)
        return wire;
    return WireId(rel_tile(chip, wire.tile, a.dx, a.dy), a.index);
}

int GateMateAxis::lower_bound(int dx, int dy, int slack, bool turn) const
{
    int ax = std::max(0, std::abs(dx) - slack), ay = std::max(0, std::abs(dy) - slack);
    auto &ex = envelope[dx > 0 ? 1 : 3], &ey = envelope[dy > 0 ? 2 : 4];
    int bound = (ax < int(ex.size()) ? ex[ax] : ex.back()) + (ay < int(ey.size()) ? ey[ay] : ey.back());
    // and a turn where both axes are far (four diagonal steps cost more)
    return bound + (turn || (ax > 0 && ay > 0) ? min_turn : 0);
}

int GateMateAxis::run_of(int x, int y, int dir, int a) const
{
    if (a < 0 || (a & 1))
        return INF;
    auto &r = run[sb_class(x, y)][dir];
    return a / 2 < int(r.size()) ? r[a / 2] : INF;
}

int GateMateAxis::link(const State &a, int ax, int ay, const State &b, int bx, int by) const
{
    bool turned;
    int value = path(ax, ay, a.dir, bx, by, b.dir, turned);
    if (value >= INF)
        return INF;
    int r = ring_distance(a.plane, b.plane);
    return value + (turned ? ring_turn[r] : ring_straight[r]);
}

int GateMateAxis::path(int x0, int y0, int i, int x1, int y1, int j, bool &turned) const
{
    int dx = x1 - x0, dy = y1 - y0;
    turned = false;
    if (i == j) {
        if (dx * UY[i] - dy * UX[i] != 0)
            return INF;
        return run_of(x0, y0, i, dx * UX[i] + dy * UY[i]);
    }
    const bool opposite = UX[i] * UX[j] + UY[i] * UY[j] < 0;
    // A signal turns only at an SB it arrives at, or at a big SB (whose YDIAG reads the Y outputs)
    const int a_min = big[sb_class(x0, y0)] ? 0 : 2;
    turned = true;
    int best = INF;
    for (int o = 0; o < 3; o++) {
        int ex = dx - OX[o], ey = dy - OY[o];
        int a, b;
        if (opposite) {
            // A U-turn on the lane: as soon as it can, or at the target if that lies further ahead
            if (ex * UY[i] - ey * UX[i] != 0)
                continue;
            int along = ex * UX[i] + ey * UY[i];
            a = std::max(a_min, along);
            b = a - along;
        } else {
            a = ex * UX[i] + ey * UY[i];
            b = ex * UX[j] + ey * UY[j];
        }
        if (a < a_min || b < 0 || (a & 1) || (b & 1))
            continue;
        int cx = x0 + UX[i] * a, cy = y0 + UY[i] * a;
        int t = turn[sb_class(cx, cy)][i][j][o];
        if (t >= INF)
            continue;
        int first = run_of(x0, y0, i, a);
        if (first >= INF)
            continue;
        int second = run_of(cx + OX[o], cy + OY[o], j, b);
        if (second >= INF)
            continue;
        best = std::min(best, first + t + second);
    }
    return best;
}

delay_t GateMateAxis::estimate(WireId src, WireId dst) const
{
    if (!built())
        return -1;
    src = anchor(src);
    dst = anchor(dst);
    int cs = tile_class[src.tile], cd = tile_class[dst.tile];
    EstimateCache &cache = estimate_cache;
    if (stats_mode && ++cache.calls % 2000000 == 0) {
        int64_t misses = std::max<int64_t>(1, cache.calls - cache.hits - cache.outside);
        log_info("axis stats (one thread): %lld calls, %.1f%% cache hits, %.1f%% outside, %.2f us per miss; per miss "
                 "%.1f exit groups, %.1f done, %.1f fallbacks, %.1f entry steps, %.1f paths\n",
                 (long long)cache.calls, 100.0 * cache.hits / cache.calls, 100.0 * cache.outside / cache.calls,
                 cache.miss_ns / 1000.0 / misses, double(cache.w_exits) / misses, double(cache.w_done) / misses,
                 double(cache.w_fallback) / misses, double(cache.w_inner) / misses, double(cache.w_paths) / misses);
        std::vector<std::pair<int64_t, std::pair<int, int>>> top;
        for (auto &entry : cache.miss_by_name)
            top.push_back({entry.second.second, entry.first});
        std::sort(top.rbegin(), top.rend());
        for (int i = 0; i < std::min<int>(12, top.size()); i++) {
            auto &m = cache.miss_by_name[top[i].second];
            log_info("  %-24s -> %-24s %9lld misses, %.2f us each\n", IdString(top[i].second.first).c_str(ctx),
                     IdString(top[i].second.second).c_str(ctx), (long long)m.first, m.second / 1000.0 / m.first);
        }
    }
    if (cs < 0 || cd < 0) {
        cache.outside++;
        return -1;
    }
    return cached(src, cs, dst, cd);
}

delay_t GateMateAxis::cached(WireId src, int cs, WireId dst, int cd) const
{
    EstimateCache &cache = estimate_cache;
    const ChipInfoPOD *chip = ctx->chip_info;
    int sx, sy, tx, ty;
    tile_xy(chip, src.tile, sx, sy);
    tile_xy(chip, dst.tile, tx, ty);
    int dx = tx - sx + 1024, dy = ty - sy + 1024;
    if (src.index >= 4096 || dst.index >= 4096 || dx < 0 || dx >= 2048 || dy < 0 || dy >= 2048)
        return compute(src, cs, dst, cd);
    uint64_t key = (uint64_t(cs) << 59) | (uint64_t(src.index) << 47) | (uint64_t(cd) << 43) |
                   (uint64_t(dst.index) << 31) | (uint64_t(dx) << 20) | (uint64_t(dy) << 9) | 1;
    if (cache.build_id != build_id) {
        cache.build_id = build_id;
        cache.keys.assign(size_t(1) << EstimateCache::BITS, 0);
        cache.values.assign(size_t(1) << EstimateCache::BITS, 0);
    }
    uint64_t hash = key * 0x9E3779B97F4A7C15ULL;
    size_t slot = hash >> (64 - EstimateCache::BITS);
    if (cache.keys[slot] == key) {
        if (check_mode) {
            delay_t exact = compute(src, cs, dst, cd, false);
            if (exact != cache.values[slot])
                report_mismatch(src, dst, cache.values[slot], exact, "cache");
        }
        cache.hits++;
        return cache.values[slot];
    }
    auto miss_start = stats_mode ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
    delay_t value = compute(src, cs, dst, cd);
    if (stats_mode) {
        int64_t ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - miss_start)
                        .count();
        cache.miss_ns += ns;
        auto &m = cache.miss_by_name[{chip_wire_info(chip, src).name, chip_wire_info(chip, dst).name}];
        m.first++;
        m.second += ns;
    }
    if (check_mode) {
        delay_t exact = compute(src, cs, dst, cd, false);
        if (exact != value)
            report_mismatch(src, dst, value, exact, "bounds");
    }
    cache.keys[slot] = key;
    cache.values[slot] = value;
    return value;
}

void GateMateAxis::report_mismatch(WireId src, WireId dst, delay_t value, delay_t exact, const char *what) const
{
    static std::atomic<int> reported{0};
    if (reported++ < 30)
        log_info("axis check (%s): %s -> %s: %d, without bounds and cache %d\n", what,
                 ctx->nameOfWire(src), ctx->nameOfWire(dst), int(value), int(exact));
}

delay_t GateMateAxis::compute(WireId src, int cs, WireId dst, int cd, bool prune) const
{
    const ClassData &a = classes[cs], &b = classes[cd];
    int e0 = a.exit_start[src.index], e1 = a.exit_start[src.index + 1];
    int n0 = b.entry_start[dst.index], n1 = b.entry_start[dst.index + 1];
    if (e0 == e1 || n0 == n1)
        return -1;
    const ChipInfoPOD *chip = ctx->chip_info;
    int sx, sy, tx, ty;
    tile_xy(chip, src.tile, sx, sy);
    tile_xy(chip, dst.tile, tx, ty);
    int best = INF;
    // The least delay of an entry by the plane the path arrives on
    const int32_t *least_entry = &b.least_entry[13 * dst.index];
    // (a diagonal step makes up one more tile)
    const int reach = b.entry_reach[dst.index];
    // Through the plane change: the least over the exit's planes of the delay to the exit and from there on
    auto through = [](const ExitGroup &e, const int32_t *entry) {
        int value = INF;
        for (int i = 0; i < e.plane_count; i++)
            value = std::min(value, e.cost[e.planes[i]] + entry[e.planes[i]]);
        return value;
    };
    // The entries' positions, directions and least delays in a compact array (the groups are large)
    struct Hot
    {
        int bound, x, y, least, index;
        uint8_t dir, cp;
        bool operator<(const Hot &o) const { return bound < o.bound || (bound == o.bound && index < o.index); }
    };
    const int entries = n1 - n0;
    Hot hot_buf[64];
    std::vector<Hot> hot_vec;
    Hot *hot = hot_buf;
    if (entries > 64) {
        hot_vec.resize(entries);
        hot = hot_vec.data();
    }
    // (in the order of a lower bound from any exit, or any SB a first turn reaches: the exits lie within
    // exit_reach tiles of the source, a first turn two more and a diagonal step one; a CP line costs less per
    // tile than the switch boxes, so no geometric bound for those)
    const int exit_slack = a.exit_reach[src.index] + 3;
    for (int n = 0; n < entries; n++) {
        const EntryGroup &g = b.entries[n0 + n];
        int x = tx + g.dx, y = ty + g.dy;
        int bound = g.least + (g.cp ? 0 : lower_bound(x - sx, y - sy, exit_slack + 1));
        hot[n] = {bound, x, y, int(g.least), n0 + n, g.dir, g.cp};
    }
    std::sort(hot, hot + entries);
    EstimateCache &stats = estimate_cache;
    // From output `dir` of the SB at (x0, y0), `extra` beyond the exit e, to each entry; e_bound: the least
    // delay of the exit and any entry through the plane change (a lower bound of that part for each entry)
    auto from_sb = [&](const ExitGroup &e, int e_bound, int x0, int y0, int dir, int extra, bool has_turned) {
        if (prune && extra + e_bound >= best)
            return;
        for (int n = 0; n < entries; n++) {
            const Hot &h = hot[n];
            if (stats_mode)
                stats.w_inner++;
            int ends = std::max(int(e.least) + h.least, e_bound);
            if (prune && extra + e.least + h.bound >= best)
                break; // (sorted by that bound)
            if (h.cp)
                continue;
            int dx = h.x - x0, dy = h.y - y0;
            // straight on: only along the lane
            if (h.dir == dir && dx * UY[dir] - dy * UX[dir] != 0)
                continue;
            if (prune && extra + ends + lower_bound(dx, dy, 1, h.dir != dir) >= best)
                continue;
            bool turned;
            if (stats_mode)
                stats.w_paths++;
            int value = path(x0, y0, dir, h.x, h.y, h.dir, turned);
            if (value >= INF || (prune && extra + value + ends >= best))
                continue;
            best = std::min(best, extra + value + through(e, b.entries[h.index].cost[turned || has_turned]));
        }
    };
    // The exits in the order of a lower bound of the whole estimate (the entries lie within a few tiles of
    // the target)
    struct Order
    {
        int bound, exit, ends;
        bool operator<(const Order &o) const { return bound < o.bound || (bound == o.bound && exit < o.exit); }
    };
    Order order_buf[64];
    std::vector<Order> order_vec;
    Order *order = order_buf;
    int count = e1 - e0;
    if (count > 64) {
        order_vec.resize(count);
        order = order_vec.data();
    }
    for (int e = e0; e < e1; e++) {
        const ExitGroup &g = a.exits[e];
        int least = g.cp ? g.cost[0] + least_entry[0] : through(g, least_entry);
        int dx = tx - sx - g.dx, dy = ty - sy - g.dy;
        // (a turn unless an entry may lie straight ahead on the lane; no bound from the switch boxes for a
        // CP line, which is cheaper per tile)
        bool turn = !g.cp && (dx * UX[g.dir] + dy * UY[g.dir] + reach < 0 ||
                              std::abs(dx * UY[g.dir] - dy * UX[g.dir]) > reach);
        int bound = g.cp ? 0 : lower_bound(dx, dy, reach + 1, turn);
        order[e - e0] = {least + bound, e, least};
    }
    std::sort(order, order + count);
    if (stats_mode)
        stats.w_exits += count;
    for (int k = 0; k < count; k++) {
        if (prune && order[k].bound >= best)
            break;
        const ExitGroup &s = a.exits[order[k].exit];
        if (stats_mode)
            stats.w_done++;
        int x0 = sx + s.dx, y0 = sy + s.dy;
        if (s.cp) {
            for (int n = 0; n < entries; n++) {
                const Hot &h = hot[n];
                if (prune && s.least + h.bound >= best)
                    break;
                int dx = h.x - x0, dy = h.y - y0;
                if (!h.cp || dx < 0 || dy < 0 || dx >= CP_SPAN || dy >= CP_SPAN)
                    continue;
                int value = cp_delay[((s.dir * 6 + h.dir) * CP_SPAN + dx) * CP_SPAN + dy];
                if (value < INF)
                    best = std::min(best, s.least + h.least + value);
            }
            continue;
        }
        const int e_bound = order[k].ends;
        from_sb(s, e_bound, x0, y0, s.dir, 0, false);
        // Not heading towards the target: turn (or turn back) at the first SB where it can (this one if
        // big, else the next), then on with at most one more turn
        if ((tx - x0) * UX[s.dir] + (ty - y0) * UY[s.dir] < 2) {
            if (stats_mode)
                stats.w_fallback++;
            int a0 = big[sb_class(x0, y0)] ? 0 : 2;
            int first = run_of(x0, y0, s.dir, a0);
            if (first >= INF)
                continue;
            int cx = x0 + a0 * UX[s.dir], cy = y0 + a0 * UY[s.dir];
            for (int j = 1; j <= 4; j++) {
                if (j == s.dir)
                    continue;
                for (int o = 0; o < 3; o++) {
                    int t = turn[sb_class(cx, cy)][s.dir][j][o];
                    if (t >= INF)
                        continue;
                    int x1 = cx + OX[o], y1 = cy + OY[o];
                    if (!prune || first + t + e_bound + lower_bound(tx - x1, ty - y1, reach + 1) < best)
                        from_sb(s, e_bound, x1, y1, j, first + t, true);
                }
            }
        }
    }
    return best >= INF ? -1 : best;
}

NEXTPNR_NAMESPACE_END
