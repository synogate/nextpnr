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

#ifndef GATEMATE_AXIS_H
#define GATEMATE_AXIS_H

#include <functional>
#include <vector>
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

// The factored delay estimate (docs/routing_heuristic_concept.md, --vopt lookahead=axis): a connection
// leaves its source wire on the output Yi of a switch box (SB), runs straight along its lane, turns (or
// turns back) once, and comes in on the output Yj of the SB next to its target. So the estimate is
//
//   exit cost + RUN(along i) + TURN + RUN(along j) + plane change + entry cost
//
// (and one more turn first where the exit heads away from the target)
//
// where the exits and entries are short lists of SB outputs (position, plane, direction, delay) per
// wire, from small searches around a sample tile of each position class (x mod 4, y mod 4), RUN is a
// one-dimensional table per SB class, direction and length, TURN the cost of changing direction at a
// corner SB (or at its diagonal neighbour, for the row/column parity), and the plane change a cost per
// step on the ring of 12 planes. The carry/propagate lines (CP lines), which run right and up only,
// have their own small table. Everything comes from searches on the chip database at start-up.
struct GateMateAxis
{
    // avail filters out pips that can never be used; prune: keep exits/entries within this many ps of
    // the best; keep: at most this many per wire, after dropping those that another one reaches as fast
    // key: the chip and the options the tables depend on; tables built with a key are kept for the rest of
    // the process and copied instead of built again (the tests and the GUI create many contexts)
    void build(Context *ctx, std::function<bool(PipId)> avail, int prune = 600, int keep = 1000,
               const std::string &key = "");
    bool built() const { return ctx != nullptr; }
    // Least delay estimate from src to dst, -1 where neither table knows the wires (outside the core)
    delay_t estimate(WireId src, WireId dst) const;

  private:
    static constexpr int INF = 1 << 28;
    struct State
    {
        int16_t dx, dy; // from the wire's tile to the SB (or CPE, for CP lines)
        uint8_t plane;  // 1..12 (0 for CP lines)
        uint8_t dir;    // 1..4 = E, N, W, S for SB outputs; the CP line 0..5 otherwise
        uint8_t cp;     // 1 for a CP line node
        int32_t cost;
    };
    // The entries of a wire at one SB output (or CP line node), over all planes: the least delay from
    // a path on plane p (with a turn or without) through the plane change to the wire
    struct EntryGroup
    {
        int16_t dx, dy;
        uint8_t dir, cp;
        int32_t least;
        int32_t cost[2][13]; // [turned][plane of the path]; cp: [*][0]
    };
    // The exits of a wire at one SB output (or CP line node): the delay to it on each plane
    struct ExitGroup
    {
        int16_t dx, dy;
        uint8_t dir, cp;
        int32_t least;
        int32_t cost[13]; // by plane; cp: [0]
        uint8_t planes[12], plane_count; // the planes with a delay
    };
    struct ClassData
    {
        int type = -1;
        std::vector<int> exit_start, entry_start; // wire index -> first exit / entry group, size wires + 1
        std::vector<ExitGroup> exits;
        std::vector<EntryGroup> entries;
        // Per wire: the least delay of its entries by the plane a path arrives on (13 each), and how far
        // its entries lie from it (tiles, either axis)
        std::vector<int32_t> least_entry;
        std::vector<int16_t> entry_reach;
        // Per wire: how far its exits lie from it (tiles, either axis)
        std::vector<int16_t> exit_reach;
    };

    Context *ctx = nullptr;
    // Per tile: the class (x mod 4) * 4 + (y mod 4) if its type is that of the class sample and it is
    // not at the edge, else -1
    std::vector<int8_t> tile_class;
    ClassData classes[16];
    // RUN per SB class ((x mod 4) * 4 + y mod 4), direction 1..4 and length / 2; TURN per corner SB
    // class, arriving and leaving direction and diagonal step (0, (1,1), (-1,-1))
    std::vector<int> run[16][5];
    int turn[16][5][5][3];
    // Whether the SBs of a class are big ones: their YDIAG reads the Y outputs, so a signal can turn
    // at the SB it leaves (at a small SB only where it arrives)
    bool big[16];
    // The least delay of any turn (or turn back)
    int min_turn = 0;
    // Plane change by distance on the ring, on a path with a turn (sharing its YDIAG) and without
    int ring_turn[7], ring_straight[7];
    // CP lines: least delay from line l leaving a CPE to line m leaving the CPE at (dx, dy), 0 <= dx, dy
    // < CP_SPAN
    static constexpr int CP_SPAN = 16;
    std::vector<int> cp_delay;

    // Lower envelope of RUN over all classes: the least delay of a run of at least d tiles in a direction
    std::vector<int> envelope[5];
    // A lower bound of the delay over (dx, dy) tiles, `slack` tiles of which the ends may make up
    // (turn: a turn is needed anyway; at most one turn is counted)
    int lower_bound(int dx, int dy, int slack, bool turn = false) const;
    int run_of(int x, int y, int dir, int a) const;
    // The estimate between two anchor wires of the classes cs and cd (no cache)
    // (prune false: without the lower bounds, for checking them)
    delay_t compute(WireId src, int cs, WireId dst, int cd, bool prune = true) const;
    // compute() through the per-thread cache
    delay_t cached(WireId src, int cs, WireId dst, int cd) const;
    // Identifies this build for the per-thread caches of estimate()
    uint64_t build_id = 0;
    // GATEMATE_AXIS_CHECK set (diagnosis only): compare each estimate with one without bounds and cache
    bool check_mode = false;
    // GATEMATE_AXIS_STATS set (diagnosis only): log cache hits and the time per miss now and then
    bool stats_mode = false;
    void report_mismatch(WireId src, WireId dst, delay_t value, delay_t exact, const char *what) const;
    // Where the root of a node (the wire a WireId names) lies in it is up to the chip database and may
    // differ with the position on the chip. The tables are keyed by a wire that does not: the one with
    // the least (dx, dy, index) in the node's shape. Per node shape, that wire relative to the root
    // (index -1: the root itself)
    struct Anchor
    {
        int16_t dx = 0, dy = 0;
        int32_t index = -1;
    };
    std::vector<Anchor> shape_anchor;
    WireId anchor(WireId wire) const;
    // From SB output state a at (ax, ay) to SB output state b at (bx, by): path and plane change
    int link(const State &a, int ax, int ay, const State &b, int bx, int by) const;
    int path(int x0, int y0, int i, int x1, int y1, int j, bool &turned) const;
};

NEXTPNR_NAMESPACE_END

#endif
