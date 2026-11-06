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

#include "distance.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <queue>
#include <random>
#if !defined(NPNR_DISABLE_THREADS)
#include <thread>
#endif

#include "log.h"
#include "lookahead.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {
// The constants, in ps for the worst case corner in speed mode, fitted by least absolute error to
// the exact least delays on the empty chip (docs/distance_model.md); init() scales them by the
// ratio of the chosen corner's switch box mux delay to REFERENCE_STAGE.
constexpr double REFERENCE_STAGE = 218;
struct Fit
{
    int tile = 21, segment = 88, odd = 73, near = -16, turn1 = 94, turn2 = 224, east = 82, east_tile = 4,
        same_place = 486, plane_step = 214, rem = 60;
    // Constant terms by state, and with a big switch box (in_big: its D2/D3 inputs, in_far: D4..D7;
    // in_other_big: its clock inputs, clk_far: a clock input node named after its far end)
    int y = 1383, y_big = -132, y_int = 1126, in = 1418, in_big = 30, in_far = 212, in_other = 1425,
        in_other_big = 255, clk_far = 232, seg_long = 1968, drive_in = 2077, ydiag = 1015, ydiag_big = 65,
        ydiag_int = 1026, x_in = 1256, d0 = 1027, om_y = 1030, om_d = 1228, other = 1103;
    // Terms when travelling toward the target
    int y_credit = -398, y_int_credit = -192, in_credit = -278, in_other_credit = -259, long_credit = -522,
        drive_in_credit = -533;
    // Short: the target level with or one tile behind the switch box; long: 3 to 6 tiles ahead, short of
    // the segment's first landing
    int short_level = -245, long_mid = -300;
    int cpe_out = 1053, out_mux = 89;
    // ctrl_bridge: on top of the bridge for the input muxes of planes 9..12 (CLK/EN/SR), whose way through
    // the CPE to its outputs is longer
    int im_alt = 816, im_diag = 726, imy_diag = 583, im_bridge = 617, imy_bridge = 366, ctrl_bridge = 537,
        cpe_in_bridge = 367, cpe_bridge = 49, clock = 3000;
    // From routing to a carry/propagate chain input of a CPE, through its neighbour (docs/distance_model.md)
    int chain_in = 636;
    // From a CPE output to a wire (the reverse direction), by the kind of wire: constant term, and for
    // segments the terms when the source lies behind the segment's switch box (credit) or level with it
    // (level); for switch box wires the term at the switch box the output starts at (same)
    int r_short = 1053, r_short_credit = -260, r_short_level = -387, r_short_big = 1078, r_short_big_credit = -352,
        r_short_big_level = -388, r_long = 1269, r_long_credit = -384, r_long_level = -396, r_im = 1277,
        r_im_credit = -311, r_im_level = -396;
    // Input mux wires reached as the input mux they come from (as a CPE input target)
    int r_im_diag = 306, r_im_diag_node = 16, r_im_alt = 287, r_im_y = -60;
    int r_x = 809, r_x_same = -237, r_out = 1275, r_out_same = -145, r_other = 850;
    // Plane changes from where the output starts: one, two planes along the ring; through the output mux
    int r_plane1 = 206, r_plane2 = 396, r_om = 93;
};

const int DIR_X[5] = {0, 1, 0, -1, 0}, DIR_Y[5] = {0, 0, 1, 0, -1};
// Where the input mux a diagonal input (IM.Pp.D4, D5 and D5 named after its far end) comes from
const int IM_DIAG_SHIFT = -1, IM_NODE_SHIFT = 0;
const int ALT0[13] = {0, 5, 6, 7, 8, 1, 2, 3, 4, 11, 12, 9, 10};
const int ALT1[13] = {0, 9, 10, 11, 12, 9, 10, 11, 12, 12, 11, 10, 9};
// Which output of the CPEs feeds D0 of planes 1..8 at the switch box at a block's corner (odd x) and
// at the opposite corner (prjpeppercorn die.py, create_sb)
const int D0_OUT[2][8] = {{2, 1, 2, 1, 1, 2, 1, 2}, {1, 2, 1, 2, 2, 1, 2, 1}};

int prev_plane(int p) { return (p + 10) % 12 + 1; }
int next_plane(int p) { return p % 12 + 1; }
// Segments needed along an axis: one per 4 tiles up to 12, then a long one up to 20, and so on
int segments(int d) { return 3 * (d / 20) + std::min((d % 20) / 4, 3); }
// 2 or 3 tiles left over after at least one segment of 4, below a long one
int leftover(int d) { return d >= 6 && d < 20 && d % 4 >= 2; }
// The corner of the 2x2 block of CPEs around x (nextpnr coordinates, odd)
int block(int x) { return ((x - 3) & ~1) + 3; }
// The output of the CPE at block offset (i, j) that feeds output mux input i * 2 + j of plane p at
// the switch box with x parity xp (die.py, create_outmux)
int om_out(int p, int xp, int n) { return (p % 2 == xp ? n < 2 : n >= 2) ? 2 : 1; }

// Parses a decimal number at s, advancing s; -1 if there is none
int number(const char *&s)
{
    if (*s < '0' || *s > '9')
        return -1;
    int n = 0;
    while (*s >= '0' && *s <= '9')
        n = n * 10 + (*s++ - '0');
    return n;
}

bool skip(const char *&s, const char *prefix)
{
    size_t n = strlen(prefix);
    if (strncmp(s, prefix, n) != 0)
        return false;
    s += n;
    return true;
}

// The wire's kind and how the estimate treats it, from its name (`sb_tile`: the tile has a switch box)
GateMateDistance::WireInfo classify(std::string name, bool sb_tile, const Fit &f)
{
    using W = GateMateDistance;
    W::WireInfo w;
    bool endpoint = false;
    if (name.size() > 2 && name.compare(name.size() - 2, 2, "_n") == 0) {
        endpoint = true;
        name.resize(name.size() - 2);
    }
    auto set = [&](W::Kind kind, W::State state, int base, int credit = 0) {
        w.kind = kind;
        w.state = state;
        w.base = base;
        w.credit = credit;
    };
    const char *s = name.c_str();
    if (skip(s, "CPE.")) {
        if (skip(s, "OUT")) {
            int k = number(s);
            if ((k == 1 || k == 2) && !*s) {
                set(W::CPE_OUT, W::CPE_OUTPUT, 0);
                w.dir = k;
                return w;
            }
        } else if (skip(s, "IN")) {
            int k = number(s);
            if (k >= 1 && k <= 8 && !*s) {
                set(W::CPE_IN, W::BRIDGE, f.cpe_in_bridge);
                w.plane = k;
                return w;
            }
            if (k >= 1 && k <= 8) {
                // INk_int: still before the LUTs, as far from the outputs as the input
                set(W::CPE_INT, W::BRIDGE, f.cpe_in_bridge);
                return w;
            }
        } else if (!strcmp(s, "CLK") || !strcmp(s, "EN") || !strcmp(s, "SR")) {
            set(W::CPE_IN, W::BRIDGE, f.cpe_in_bridge);
            w.plane = s[0] == 'C' ? 9 : s[0] == 'E' ? 10 : 11;
            return w;
        }
        set(W::CPE_INT, W::BRIDGE, f.cpe_bridge);
        return w;
    }
    if (skip(s, "IM.P")) {
        int p = number(s);
        w.plane = p;
        if (skip(s, ".Y") && !*s) {
            set(W::IM_Y, W::IM_OUTPUT, 0);
        } else if (skip(s, ".D")) {
            int k = number(s);
            set(k <= 3 ? W::IM_D : k <= 5 ? W::IM_DIAG : W::IM_ALT, W::IM_INPUT, 0);
            if (endpoint) {
                if (k <= 3) {
                    // The output Y(k + 1) of the switch box 1 tile back (2 from a switch box tile)
                    int dir = k + 1, offset = sb_tile ? 2 : 1;
                    set(W::IM_D, W::SHORT, f.y, f.y_credit);
                    w.dir = dir;
                    w.dx = -offset * DIR_X[dir];
                    w.dy = -offset * DIR_Y[dir];
                } else {
                    // The output of a diagonal neighbour's or another plane's input mux
                    w.state = W::IM_OUTPUT;
                    w.dx = w.dy = k == 4 ? -1 : k == 5 ? 1 : 0;
                    w.plane = k == 6 ? ALT0[p] : k == 7 ? ALT1[p] : p;
                }
            }
        }
        return w;
    }
    if (skip(s, "OM.P")) {
        int p = number(s);
        w.plane = p;
        if (skip(s, ".Y") && !*s) {
            set(W::OM_Y, W::FRESH, f.om_y);
        } else if (skip(s, ".D")) {
            set(W::OM_D, endpoint ? W::OM_D_NODE : W::FRESH, f.om_d);
            w.dir = number(s);
        }
        return w;
    }
    if (skip(s, "SB_DRIVE.P")) {
        w.plane = number(s);
        if (skip(s, ".D")) {
            w.dir = number(s);
            if (skip(s, ".IN"))
                set(W::DRIVE_IN, W::LONG, f.drive_in, f.drive_in_credit);
            else if (skip(s, ".OUT"))
                set(W::DRIVE_OUT, W::LONG, f.seg_long, f.long_credit);
        }
        return w;
    }
    bool big = skip(s, "SB_BIG.P");
    if (big || skip(s, "SB_SML.P")) {
        int p = number(s);
        w.plane = p;
        if (skip(s, ".YDIAG")) {
            if (*s)
                set(W::SB_YDIAG, W::FRESH, f.ydiag_int);
            else
                set(W::SB_YDIAG, W::FRESH, f.ydiag + (big ? f.ydiag_big : 0));
        } else if (skip(s, ".Y")) {
            w.dir = number(s);
            if (*s)
                set(W::SB_Y, W::SHORT, f.y_int, f.y_int_credit);
            else
                set(W::SB_Y, W::SHORT, f.y + (big ? f.y_big : 0), f.y_credit);
        } else if (skip(s, ".X")) {
            int x = number(s);
            set(W::SB_X, W::FRESH, f.x_in);
            if (endpoint) {
                // The node of another YDIAG output
                w.base = f.ydiag + (big ? f.ydiag_big : 0);
                if (x == 34)
                    w.dx = w.dy = 1;
                else if (x == 12)
                    w.dx = w.dy = -1;
                else if (x == 14)
                    w.plane = prev_plane(p);
                else if (x == 23)
                    w.plane = next_plane(p);
            }
        } else if (skip(s, ".D")) {
            int k = number(s);
            if (k == 0) {
                if (endpoint && !*s)
                    set(W::SB_D0, W::SB_D0_NODE, 0);
                else
                    set(W::SB_D0, W::FRESH, f.d0);
            } else if (k >= 2 && k <= 7 && skip(s, "_")) {
                static const int dists[] = {2, 4, 8, 12, 16, 20};
                int dir = number(s), dist = dists[k - 2];
                w.dir = dir;
                if (*s) {
                    // From the clock network or the other die
                    bool clock = !strcmp(s, "_CLK");
                    set(W::SB_IN, W::SHORT,
                        f.in_other + (big && clock ? f.in_other_big : 0) + (clock && endpoint ? f.clk_far : 0),
                        f.in_other_credit);
                } else if (!endpoint) {
                    // (D4..D7, from the long segments, only at big switch boxes)
                    set(W::SB_IN, W::SHORT, f.in + (big ? (k >= 4 ? f.in_far : f.in_big) : 0), f.in_credit);
                } else {
                    // The segment from the switch box `dist` tiles back, of the other size for 2
                    if (dist <= 4)
                        set(W::SB_IN, W::SHORT, f.y + (big == (dist == 4) ? f.y_big : 0), f.y_credit);
                    else
                        set(W::SB_IN, W::LONG, f.seg_long, f.long_credit);
                    w.dx = -dist * DIR_X[dir];
                    w.dy = -dist * DIR_Y[dir];
                }
            }
        }
        return w;
    }
    if (name.find("CLOCK") != std::string::npos) {
        set(W::CLOCK, W::CLOCK_NET, f.clock);
        return w;
    }
    set(W::OTHER, W::FRESH, f.other);
    return w;
}
// How a CPE output reaches the wire (the reverse direction): fills the r* fields of w, classified by
// classify() before
void classify_reverse(std::string name, bool sb_tile, const Fit &f, GateMateDistance::WireInfo &w)
{
    using W = GateMateDistance;
    bool endpoint = false;
    if (name.size() > 2 && name.compare(name.size() - 2, 2, "_n") == 0) {
        endpoint = true;
        name.resize(name.size() - 2);
    }
    auto set = [&](W::Reach reach, int dx, int dy, int plane, int dir, int base, int credit = 0, int level = 0,
                   int same = 0) {
        w.reach = reach;
        w.rdx = dx;
        w.rdy = dy;
        w.rplane = plane;
        w.rdir = dir;
        w.rbase = base;
        w.rcredit = credit;
        w.rlevel = level;
        w.rsame = same;
    };
    // The digit after ".D" (input number)
    auto input = [&]() { return name[name.find(".D") + 2] - '0'; };
    int p = w.plane, d = w.dir;
    switch (w.kind) {
    case W::SB_Y:
        // A Y output: reach its switch box, leave in direction d
        if (name.rfind("SB_BIG", 0) == 0 && name.find("_int") == std::string::npos)
            set(W::REACH_TRAVEL, 0, 0, p, d, f.r_short_big, f.r_short_big_credit, f.r_short_big_level);
        else
            set(W::REACH_TRAVEL, 0, 0, p, d, f.r_short, f.r_short_credit, f.r_short_level);
        return;
    case W::SB_IN: {
        // An input Dk_d: the segment from the switch box 2..20 tiles back in direction d
        static const int dists[] = {2, 4, 8, 12, 16, 20};
        int k = input(), dist = dists[k - 2], dx = -dist * DIR_X[d], dy = -dist * DIR_Y[d];
        if (k >= 4)
            set(W::REACH_TRAVEL, dx, dy, p, d, f.r_long, f.r_long_credit, f.r_long_level);
        else if (name.rfind("SB_BIG", 0) == 0)
            set(W::REACH_TRAVEL, dx, dy, p, d, f.r_short_big, f.r_short_big_credit, f.r_short_big_level);
        else
            set(W::REACH_TRAVEL, dx, dy, p, d, f.r_short, f.r_short_credit, f.r_short_level);
        return;
    }
    case W::DRIVE_IN:
    case W::DRIVE_OUT:
        set(W::REACH_TRAVEL, 0, 0, p, d, f.r_long, f.r_long_credit, f.r_long_level);
        return;
    case W::IM_D: {
        // IM.Pp.Dk, k 0..3: the output Y(k + 1) of the switch box 1 tile back (2 from a switch box tile)
        int dir = input() + 1, offset = sb_tile ? 2 : 1;
        set(W::REACH_TRAVEL, -offset * DIR_X[dir], -offset * DIR_Y[dir], p, dir, f.r_im, f.r_im_credit,
            f.r_im_level);
        return;
    }
    case W::IM_DIAG:
    case W::IM_ALT: {
        // From the input mux output of a diagonal neighbour or of another plane: reach that input mux
        int k = input();
        if (endpoint)
            set(W::REACH_INPUT, w.dx + IM_NODE_SHIFT, w.dy + IM_NODE_SHIFT, w.plane, 0, f.r_im_diag_node);
        else if (k <= 5)
            set(W::REACH_INPUT, k == 4 ? IM_DIAG_SHIFT : -IM_DIAG_SHIFT, k == 4 ? IM_DIAG_SHIFT : -IM_DIAG_SHIFT, p, 0,
                f.r_im_diag);
        else
            set(W::REACH_INPUT, 0, 0, k == 6 ? ALT0[p] : ALT1[p], 0, f.r_im_alt);
        return;
    }
    case W::IM_Y:
        set(W::REACH_INPUT, 0, 0, p, 0, f.r_im_y);
        return;
    case W::SB_YDIAG:
        set(W::REACH_FRESH, 0, 0, p, 0, f.r_x, 0, 0, f.r_x_same);
        return;
    case W::SB_X:
        if (endpoint) {
            // The node of another YDIAG output (as classify())
            set(W::REACH_FRESH, w.dx, w.dy, w.plane, 0, f.r_x, 0, 0, f.r_x_same);
        } else {
            // Driven by the YDIAG of the diagonal neighbour (X12, X34) or of the neighbouring plane (X14, X23)
            int x = atoi(name.c_str() + name.find(".X") + 2), o = x == 34 ? 1 : x == 12 ? -1 : 0;
            int plane = x == 14 ? prev_plane(p) : x == 23 ? next_plane(p) : p;
            set(W::REACH_FRESH, o, o, plane, 0, f.r_x, 0, 0, f.r_x_same);
        }
        return;
    case W::SB_D0:
    case W::OM_D:
    case W::OM_Y:
        // CPE output nodes and the output mux: through another CPE
        set(W::REACH_FRESH, 0, 0, p, 0, f.r_out, 0, 0, f.r_out_same);
        return;
    case W::CLOCK:
        set(W::REACH_FRESH, 0, 0, 0, 0, f.clock);
        return;
    default:
        set(W::REACH_FRESH, 0, 0, 0, 0, f.r_other);
        return;
    }
}
} // namespace

void GateMateDistance::init(Context *ctx, std::function<bool(PipId)> avail)
{
    this->ctx = ctx;
    const ChipInfoPOD *chip = ctx->chip_info;

    // Scale to the timing corner and speed mode: the mean delay of the Y output muxes of a big switch
    // box from their inputs from 2 and 4 tiles away, including those inputs' wires
    double sum = 0;
    int count = 0;
    for (int tile = 0; tile < chip->tile_insts.ssize() && count == 0; tile++) {
        auto &type = chip_tile_info(chip, tile);
        for (int i = 0; i < type.pips.ssize(); i++) {
            std::string dst = IdString(type.wires[type.pips[i].dst_wire].name).str(ctx);
            std::string src = IdString(type.wires[type.pips[i].src_wire].name).str(ctx);
            if (dst.rfind("SB_BIG.P", 0) != 0 || dst.find(".Y") == std::string::npos ||
                dst.find("YDIAG") != std::string::npos || src.find(".D2_") == std::string::npos)
                continue;
            int delay = ctx->getPipDelay(PipId(tile, i)).maxDelay();
            // ...and the delay into the input
            for (PipId pip : ctx->getPipsUphill(ctx->normalise_wire(tile, type.pips[i].src_wire)))
                delay += ctx->getPipDelay(pip).maxDelay();
            sum += delay;
            count++;
        }
    }
    double stage = count ? sum / count : REFERENCE_STAGE;
    Fit f;
    static_assert(sizeof(Fit) % sizeof(int) == 0, "Fit holds ints only");
    int *values = reinterpret_cast<int *>(&f);
    for (size_t i = 0; i < sizeof(Fit) / sizeof(int); i++)
        values[i] = int(std::lround(values[i] * stage / REFERENCE_STAGE));
    log_info("GateMate distance model: switch box stage %.0f ps, constants scaled by %.3f\n", stage,
             stage / REFERENCE_STAGE);
    k.tile = f.tile;
    k.segment = f.segment;
    k.odd = f.odd;
    k.near = f.near;
    k.turn1 = f.turn1;
    k.turn2 = f.turn2;
    k.east = f.east;
    k.east_tile = f.east_tile;
    k.same_place = f.same_place;
    k.plane_step = f.plane_step;
    k.rem = f.rem;
    k.short_level = f.short_level;
    k.long_mid = f.long_mid;
    k.cpe_out = f.cpe_out;
    k.out_mux = f.out_mux;
    // From an input of the input mux: one mux stage to its output and on to the CPE input
    k.im_same = f.im_diag - f.imy_diag;
    k.im_alt = f.im_alt;
    k.im_diag = f.im_diag;
    k.imy_alt = f.im_alt - k.im_same;
    k.imy_diag = f.imy_diag;
    k.im_bridge = f.im_bridge;
    k.imy_bridge = f.imy_bridge;
    k.ctrl_bridge = f.ctrl_bridge;
    k.reach_om = f.r_om;

    // Half steps between planes: one per neighbouring plane at a switch box (YDIAG to X14/X23), 2.5
    // for an alternative plane at the input mux (D6/D7), at most 3 steps (a detour costs no more)
    for (int q = 0; q <= 12; q++)
        for (int p = 0; p <= 12; p++) {
            if (p == 0 || q == 0) {
                plane_steps[p][q] = 0;
                continue;
            }
            auto ring = [](int a, int b) { return std::min((a - b + 12) % 12, (b - a + 12) % 12); };
            int best = 2 * ring(p, q);
            for (int a : {ALT0[q], ALT1[q]})
                if (a != q)
                    best = std::min(best, 2 * ring(p, a) + 5);
            plane_steps[p][q] = std::min(best, 6);
        }
    // From a switch box plane to another, for the reverse direction: by steps along the ring
    for (int q = 0; q <= 12; q++)
        for (int p = 0; p <= 12; p++) {
            int r = std::min((p - q + 12) % 12, (q - p + 12) % 12);
            reach_planes[p][q] = p == 0 || q == 0 || r == 0 ? 0
                                 : r == 1                   ? f.r_plane1
                                                            : f.r_plane2 + (r - 2) * f.plane_step;
        }
    // The planes each CPE output starts on (die.py, create_sb and create_outmux)
    for (int o = 1; o <= 2; o++)
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 2; j++)
                for (int sb = 0; sb < 2; sb++) {
                    Start &start = starts[o - 1][i][j][sb];
                    start = Start{0, 0, 0};
                    for (int p : {1 + 2 * i + j, 5 + 2 * i + j})
                        if (D0_OUT[sb][p - 1] == o)
                            start.direct = p;
                    // The corner switch box has odd x, the other even
                    int xp = sb == 0 ? 1 : 0;
                    for (int p = 9; p <= 12; p++)
                        if (om_out(p, xp, 2 * i + j) == o)
                            (start.om1 ? start.om2 : start.om1) = p;
                }

    types.resize(chip->tile_types.ssize());
    for (int t = 0; t < chip->tile_types.ssize(); t++) {
        auto &type = chip->tile_types[t];
        auto &data = types[t];
        const int wires = type.wires.ssize();
        bool sb_tile = false;
        for (int i = 0; i < wires && !sb_tile; i++)
            sb_tile = IdString(type.wires[i].name).str(ctx).rfind("SB_", 0) == 0;
        data.wires.resize(wires);
        for (int i = 0; i < wires; i++) {
            std::string name = IdString(type.wires[i].name).str(ctx);
            data.wires[i] = classify(name, sb_tile, f);
            classify_reverse(name, sb_tile, f, data.wires[i]);
        }

        // Delays inside a CPE, over the tile type's own pips, as for the delay tables
        data.inside_id.assign(wires, -1);
        for (int i = 0; i < wires; i++)
            if (data.wires[i].kind == CPE_IN || data.wires[i].kind == CPE_OUT || data.wires[i].kind == CPE_INT)
                data.inside_id[i] = data.inside_size++;
        data.target_start.assign(wires + 1, 0);
        if (data.inside_size == 0)
            continue;
        int tile = -1;
        for (int i = 0; i < chip->tile_insts.ssize() && tile == -1; i++)
            if (chip->tile_insts[i].type == t)
                tile = i;
        const int n = data.inside_size;
        std::vector<std::vector<std::pair<int, int>>> downhill(n);
        for (int i = 0; i < type.pips.ssize(); i++) {
            auto &pip = type.pips[i];
            int a = data.inside_id[pip.src_wire], b = data.inside_id[pip.dst_wire];
            if (a != -1 && b != -1 && avail(PipId(tile, i)))
                downhill[a].emplace_back(b, ctx->getPipDelay(PipId(tile, i)).maxDelay());
        }
        data.inside_delay.assign(n * n, -1);
        for (int from = 0; from < n; from++) {
            std::vector<int> dist(n, INT_MAX);
            using Item = std::pair<int, int>;
            std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
            dist[from] = 0;
            queue.push({0, from});
            while (!queue.empty()) {
                auto [cost, wire] = queue.top();
                queue.pop();
                if (cost > dist[wire])
                    continue;
                data.inside_delay[from * n + wire] = cost;
                for (auto [next, delay] : downhill[wire])
                    if (cost + delay < dist[next]) {
                        dist[next] = cost + delay;
                        queue.push({dist[next], next});
                    }
            }
        }
        // The CPE inputs before each wire of the CPE
        for (int i = 0; i < wires; i++) {
            data.target_start[i] = data.targets.size();
            if (data.inside_id[i] == -1 || data.wires[i].kind == CPE_IN)
                continue;
            for (int a = 0; a < wires; a++)
                if (data.wires[a].kind == CPE_IN) {
                    int delay = data.inside_delay[data.inside_id[a] * n + data.inside_id[i]];
                    if (delay != -1)
                        data.targets.push_back(Target{data.wires[a].plane, delay});
                }
            if (data.target_start[i] != int(data.targets.size()))
                continue;
            // None: a wire after the carry/propagate chain inputs (CINX, CINY1/2, PINX, PINY1/2), reached
            // through the neighbouring CPE (chain_in: from routing to the chain input, through that CPE)
            for (int a = 0; a < wires; a++) {
                std::string name = IdString(type.wires[a].name).str(ctx);
                if (data.inside_id[a] == -1 || (name.rfind("CPE.CIN", 0) != 0 && name.rfind("CPE.PIN", 0) != 0))
                    continue;
                int delay = data.inside_delay[data.inside_id[a] * n + data.inside_id[i]];
                if (delay != -1)
                    data.targets.push_back(Target{0, delay + f.chain_in});
            }
        }
        data.target_start[wires] = data.targets.size();
    }
}

int GateMateDistance::core(int state, int base, int credit, int x, int y, int plane, int dir, int tx, int ty,
                           int q) const
{
    int ax = tx - x, ay = ty - y, d1, d2, turn;
    int cost = base + plane_cost(plane, q);
    if (state == FRESH) {
        d1 = std::abs(ax);
        d2 = std::abs(ay);
        turn = std::min(d1, d2);
        if (d1 + d2 == 0)
            cost += k.same_place;
    } else {
        int hx = DIR_X[dir], hy = DIR_Y[dir];
        int along = ax * hx + ay * hy;
        d1 = std::abs(along);
        d2 = turn = std::abs(ay * hx - ax * hy);
        if (state == SHORT && along >= 1) {
            cost += credit;
        } else if (state == LONG && along >= 7) {
            // The segment reaches 8 to 20 tiles ahead
            cost += credit;
            d1 = std::max(along - 20, 0);
        } else if (state == SHORT ? along >= -1 : along >= 3) {
            cost += state == SHORT ? k.short_level : k.long_mid;
        }
    }
    return cost + travel(d1, d2, turn, ax);
}

int GateMateDistance::travel(int d1, int d2, int turn, int ax) const
{
    int cost = k.tile * (d1 + d2) + k.segment * (segments(d1) + segments(d2)) + k.odd * ((d1 & 1) + (d2 & 1)) +
               k.near * std::max(0, 12 - d1 - d2) + k.rem * (leftover(d1) + leftover(d2));
    if (turn == 1)
        cost += k.turn1;
    else if (turn >= 2)
        cost += k.turn2;
    if (ax > 0)
        cost += k.east + k.east_tile * ax;
    return cost;
}

int GateMateDistance::reach(int x, int y, int mask, const WireInfo &d, int tx, int ty) const
{
    tx += d.rdx;
    ty += d.rdy;
    if (d.reach == REACH_INPUT)
        return d.rbase + cpe_output(x, y, mask, tx, ty, d.rplane);
    int bx = block(x), by = block(y), i = x - bx, j = y - by;
    if (i < 0 || i > 1 || j < 0 || j > 1)
        return d.rbase + k.cpe_out + travel(std::abs(tx - x), std::abs(ty - y), 0, tx - x);
    // From either switch box of the CPE's block, on a plane the output starts on there
    int best = INT_MAX;
    for (int sb = 0; sb < 2; sb++) {
        int ax = tx - bx - sb, ay = ty - by - sb, cost = d.rbase, d1, d2, turn;
        if (d.reach == REACH_FRESH) {
            d1 = std::abs(ax);
            d2 = std::abs(ay);
            turn = std::min(d1, d2);
            if (d1 + d2 == 0)
                cost += d.rsame;
        } else {
            // Leaving the switch box in direction rdir: from behind it, level with it, or else
            int hx = DIR_X[d.rdir], hy = DIR_Y[d.rdir];
            int along = ax * hx + ay * hy;
            d1 = std::abs(along);
            d2 = turn = std::abs(ay * hx - ax * hy);
            if (along >= 1)
                cost += d.rcredit;
            else if (along >= -1)
                cost += d.rlevel;
        }
        cost += travel(d1, d2, turn, ax);
        for (int o = 0; o < 2; o++) {
            if (!(mask & (1 << o)))
                continue;
            const Start &start = starts[o][i][j][sb];
            int planes = std::min(reach_planes[start.direct][d.rplane],
                                  k.reach_om +
                                          std::min(reach_planes[start.om1][d.rplane], reach_planes[start.om2][d.rplane]));
            best = std::min(best, cost + planes);
        }
    }
    return best;
}

int GateMateDistance::cpe_output(int x, int y, int mask, int tx, int ty, int q) const
{
    int bx = block(x), by = block(y), i = x - bx, j = y - by;
    if (i < 0 || i > 1 || j < 0 || j > 1)
        return core(FRESH, k.cpe_out, 0, x, y, 0, 0, tx, ty, 0);
    int best = INT_MAX;
    for (int sb = 0; sb < 2; sb++) {
        int at = core(FRESH, k.cpe_out, 0, bx + sb, by + sb, 0, 0, tx, ty, 0);
        for (int o = 0; o < 2; o++) {
            if (!(mask & (1 << o)))
                continue;
            const Start &start = starts[o][i][j][sb];
            int planes = std::min(plane_cost(start.direct, q),
                                  k.out_mux + std::min(plane_cost(start.om1, q), plane_cost(start.om2, q)));
            best = std::min(best, at + planes);
        }
    }
    return best;
}

int GateMateDistance::route(const WireInfo &w, int x, int y, int tx, int ty, int q) const
{
    x += w.dx;
    y += w.dy;
    switch (w.state) {
    case FRESH:
    case SHORT:
    case LONG:
        return core(w.state, w.base, w.credit, x, y, w.plane, w.dir, tx, ty, q);
    case CPE_OUTPUT:
        return cpe_output(x, y, w.dir, tx, ty, q);
    case SB_D0_NODE: {
        // The node of the output of a CPE in the block, named after a switch box's D0 input
        int p = w.plane, o = D0_OUT[(x & 1) ? 0 : 1][p - 1];
        return cpe_output(block(x) + (((p - 1) & 2) ? 1 : 0), block(y) + ((p - 1) & 1), o, tx, ty, q);
    }
    case OM_D_NODE: {
        int n = w.dir, o = om_out(w.plane, x & 1, n);
        return cpe_output(block(x) + (n >> 1), block(y) + (n & 1), o, tx, ty, q);
    }
    case IM_INPUT:
    case IM_OUTPUT: {
        bool input = w.state == IM_INPUT;
        int p = w.plane, best = INT_MAX;
        if (q != 0 && x == tx && y == ty) {
            if (p == q)
                best = input ? k.im_same : 0;
            else if (p == ALT0[q] || p == ALT1[q])
                best = input ? k.im_alt : k.imy_alt;
        } else if (q == p && std::abs(x - tx) == 1 && x - tx == y - ty) {
            best = input ? k.im_diag : k.imy_diag;
        }
        int bridge = (input ? k.im_bridge : k.imy_bridge) + (p >= 9 ? k.ctrl_bridge : 0);
        return std::min(best, bridge + cpe_output(x, y, 3, tx, ty, q));
    }
    case BRIDGE:
        return w.base + cpe_output(x, y, 3, tx, ty, q);
    default:
        return w.base;
    }
}

delay_t GateMateDistance::estimate(WireId src, WireId dst) const
{
    const ChipInfoPOD *chip = ctx->chip_info;
    const TypeData &src_type = types[chip->tile_insts[src.tile].type];
    const TypeData &dst_type = types[chip->tile_insts[dst.tile].type];
    if (src.tile == dst.tile && src_type.inside_size > 0) {
        int a = src_type.inside_id[src.index], b = src_type.inside_id[dst.index];
        if (a != -1 && b != -1 && src_type.inside_delay[a * src_type.inside_size + b] != -1)
            return src_type.inside_delay[a * src_type.inside_size + b];
    }
    int sx, sy, tx, ty;
    tile_xy(chip, src.tile, sx, sy);
    tile_xy(chip, dst.tile, tx, ty);
    const WireInfo &w = src_type.wires[src.index];
    const WireInfo &d = dst_type.wires[dst.index];
    if (d.kind == CPE_IN)
        return route(w, sx, sy, tx, ty, d.plane);
    int first = dst_type.target_start[dst.index], last = dst_type.target_start[dst.index + 1];
    if (first == last) {
        // Any other wire: from a CPE output the reverse direction (router2's backward search asks this)
        int x = sx + w.dx, y = sy + w.dy;
        switch (w.state) {
        case CPE_OUTPUT:
            return reach(x, y, w.dir, d, tx, ty);
        case SB_D0_NODE: {
            int p = w.plane, o = D0_OUT[(x & 1) ? 0 : 1][p - 1];
            return reach(block(x) + (((p - 1) & 2) ? 1 : 0), block(y) + ((p - 1) & 1), o, d, tx, ty);
        }
        case OM_D_NODE: {
            int n = w.dir, o = om_out(w.plane, x & 1, n);
            return reach(block(x) + (n >> 1), block(y) + (n & 1), o, d, tx, ty);
        }
        default:
            return route(w, sx, sy, tx, ty, 0);
        }
    }
    int best = INT_MAX;
    for (int i = first; i < last; i++)
        best = std::min(best, route(w, sx, sy, tx, ty, dst_type.targets[i].plane) + dst_type.targets[i].delay);
    return best;
}

namespace {
struct Stats
{
    std::vector<double> ref, est;
    std::vector<int> dist;
    void add(double r, double e, int d)
    {
        ref.push_back(r);
        est.push_back(e);
        dist.push_back(d);
    }
};

void report(const char *title, const Stats &s)
{
    int n = int(s.ref.size());
    if (n == 0)
        return;
    std::vector<double> err(n), abs_err(n);
    double sum_abs = 0, sum = 0, mr = 0, me = 0;
    int within = 0;
    for (int i = 0; i < n; i++) {
        err[i] = s.est[i] - s.ref[i];
        abs_err[i] = std::abs(err[i]);
        sum_abs += abs_err[i];
        sum += err[i];
        mr += s.ref[i];
        me += s.est[i];
        within += abs_err[i] <= 150;
    }
    mr /= n;
    me /= n;
    double cov = 0, vr = 0, ve = 0;
    for (int i = 0; i < n; i++) {
        cov += (s.ref[i] - mr) * (s.est[i] - me);
        vr += (s.ref[i] - mr) * (s.ref[i] - mr);
        ve += (s.est[i] - me) * (s.est[i] - me);
    }
    std::nth_element(abs_err.begin(), abs_err.begin() + n / 2, abs_err.end());
    log_info("  %-9s n=%7d  mean|err| %5.0f  median|err| %5.0f  mean err %+5.0f  r %.3f  within 150ps %5.1f%%\n", title,
             n, sum_abs / n, abs_err[n / 2], sum / n, cov / std::sqrt(vr * ve), 100.0 * within / n);
    static const int bounds[] = {0, 2, 4, 8, 16, 32, 64, 1 << 30};
    std::string line;
    for (int b = 0; b < 7; b++) {
        double a = 0, e = 0;
        int c = 0;
        for (int i = 0; i < n; i++)
            if (s.dist[i] >= bounds[b] && s.dist[i] < bounds[b + 1]) {
                a += std::abs(err[i]);
                e += err[i];
                c++;
            }
        if (c)
            line += stringf(" %d-%s:%4.0f/%+4.0f", bounds[b], b == 6 ? "" : std::to_string(bounds[b + 1] - 1).c_str(),
                            a / c, e / c);
    }
    log_info("            |err|/err by distance:%s\n", line.c_str());
}
} // namespace

void evaluate_estimates(Context *ctx, const GateMateLookahead &table, const GateMateDistance &model,
                        std::function<bool(PipId)> avail, int samples, const std::string &dump,
                        const std::string &offset_map)
{
    auto start_time = std::chrono::steady_clock::now();
    const ChipInfoPOD *chip = ctx->chip_info;
    IdString anchor_names[] = {ctx->id("CPE.IN1"), ctx->id("CPE.IN2"), ctx->id("CPE.IN3"), ctx->id("CPE.IN4"),
                               ctx->id("CPE.IN5"), ctx->id("CPE.IN6"), ctx->id("CPE.IN7"), ctx->id("CPE.IN8"),
                               ctx->id("CPE.CLK"), ctx->id("CPE.EN"),  ctx->id("CPE.SR"),  ctx->id("CPE.OUT1"),
                               ctx->id("CPE.OUT2")};
    const int inputs = 11, anchor_count = 13;
    // The anchors' wire index per tile type, if it has them all
    std::vector<std::vector<int>> anchors(chip->tile_types.ssize());
    for (int t = 0; t < chip->tile_types.ssize(); t++) {
        dict<IdString, int> by_name;
        auto &wires = chip->tile_types[t].wires;
        for (int i = 0; i < wires.ssize(); i++)
            by_name.emplace(IdString(wires[i].name), i);
        for (auto name : anchor_names)
            if (by_name.count(name))
                anchors[t].push_back(by_name.at(name));
        if (int(anchors[t].size()) != anchor_count)
            anchors[t].clear();
    }
    auto inside = [&](int x, int y) { return x >= 0 && y >= 0 && x < chip->width && y < chip->height; };
    auto anchor = [&](int x, int y, int k) {
        if (!inside(x, y))
            return WireId();
        int tile = tile_by_xy(chip, x, y);
        auto &a = anchors[chip->tile_insts[tile].type];
        return a.empty() ? WireId() : ctx->normalise_wire(tile, a[k]);
    };
    std::vector<int64_t> tile_base(chip->tile_insts.ssize() + 1, 0);
    for (int i = 0; i < chip->tile_insts.ssize(); i++)
        tile_base[i + 1] = tile_base[i] + chip_tile_info(chip, i).wires.ssize();
    auto index = [&](WireId w) { return tile_base[w.tile] + w.index; };

    // Exact least delays by Dijkstra over the whole chip: to each input and from each output of 16
    // CPEs near the centre (all positions modulo 4)
    struct Sample
    {
        int set; // 0: wire to input pin, 1: output pin to wire, 2: output pin to input pin
        WireId src, dst;
        int truth;
    };
    const int cx = chip->width / 2, cy = chip->height / 2, targets = 16;
    const int jobs = targets * anchor_count;
    const int per_job = std::max(1, samples / (targets * inputs));
    std::vector<std::vector<Sample>> found(jobs);
    auto job_fn = [&](int job, std::vector<int> &dist) {
        int t = job / anchor_count, k = job % anchor_count;
        int x = cx + t % 4, y = cy + t / 4;
        WireId pin = anchor(x, y, k);
        NPNR_ASSERT(pin != WireId());
        bool forward = k >= inputs;
        dist.assign(tile_base.back(), INT_MAX);
        using Item = std::pair<int, WireId>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
        dist[index(pin)] = 0;
        queue.push({0, pin});
        while (!queue.empty()) {
            auto [cost, wire] = queue.top();
            queue.pop();
            if (cost > dist[index(wire)])
                continue;
            auto relax = [&](PipId pip, WireId next) {
                if (!avail(pip))
                    return;
                int next_cost = cost + ctx->getPipDelay(pip).maxDelay();
                if (next_cost < dist[index(next)]) {
                    dist[index(next)] = next_cost;
                    queue.push({next_cost, next});
                }
            };
            if (forward) {
                for (PipId pip : ctx->getPipsDownhill(wire))
                    relax(pip, ctx->getPipDstWire(pip));
            } else {
                for (PipId pip : ctx->getPipsUphill(wire))
                    relax(pip, ctx->getPipSrcWire(pip));
            }
        }
        std::mt19937 rng(job + 1);
        // Offsets: 40% within the tables' radius of 12, 35% within 40 tiles, 25% within 80
        auto offset = [&](int &ox, int &oy) {
            int r = rng() % 20 < 8 ? 12 : rng() % 7 < 4 ? 40 : 80;
            ox = int(rng() % (2 * r + 1)) - r;
            oy = int(rng() % (2 * r + 1)) - r;
        };
        auto &out = found[job];
        int wanted = forward ? per_job * inputs / 2 : per_job;
        for (int tries = 0, got = 0; got < wanted && tries < 50 * wanted; tries++) {
            int ox, oy;
            offset(ox, oy);
            if (!inside(x + ox, y + oy))
                continue;
            int tile = tile_by_xy(chip, x + ox, y + oy);
            int count = chip_tile_info(chip, tile).wires.ssize();
            if (count == 0)
                continue;
            WireId w = ctx->normalise_wire(tile, std::uniform_int_distribution<int>(0, count - 1)(rng));
            if (w == pin || dist[index(w)] == INT_MAX)
                continue;
            out.push_back(Sample{forward ? 1 : 0, forward ? pin : w, forward ? w : pin, dist[index(w)]});
            got++;
        }
        if (!forward)
            for (int tries = 0, got = 0; got < per_job / 4 && tries < 50 * per_job; tries++) {
                int ox, oy;
                offset(ox, oy);
                WireId w = anchor(x + ox, y + oy, inputs + rng() % 2);
                if (w == WireId() || dist[index(w)] == INT_MAX)
                    continue;
                out.push_back(Sample{2, w, pin, dist[index(w)]});
                got++;
            }
    };
    std::atomic<int> next_job{0};
    auto worker = [&]() {
        std::vector<int> dist;
        for (int job = next_job++; job < jobs; job = next_job++)
            job_fn(job, dist);
    };
#if !defined(NPNR_DISABLE_THREADS)
    std::vector<std::thread> threads;
    for (int i = 0; i < std::max(1, std::min(8, int(std::thread::hardware_concurrency()))); i++)
        threads.emplace_back(worker);
    for (auto &thread : threads)
        thread.join();
#else
    worker();
#endif
    std::vector<Sample> all;
    for (auto &f : found)
        all.insert(all.end(), f.begin(), f.end());
    log_info("Exact delays of %d pairs from %d searches over the whole chip in %.0fs\n", int(all.size()), jobs,
             std::chrono::duration<float>(std::chrono::steady_clock::now() - start_time).count());

    auto linear = [&](WireId a, WireId b) {
        int ax, ay, bx, by;
        tile_xy(chip, a.tile, ax, ay);
        tile_xy(chip, b.tile, bx, by);
        return 100 + 100 * (std::abs(ax - bx) + std::abs(ay - by));
    };
    std::ofstream out;
    if (!dump.empty())
        out.open(dump);
    if (out)
        out << "set\tsrc\tsx\tsy\tdst\tdx\tdy\ttruth\ttable\tlinear\tmodel\tskind\tsstate\tdkind\tdstate\tdplane\tddir"
               "\tdpx\tdpy\n";
    const char *set_names[] = {"wire to input pin", "output pin to wire", "output pin to input pin"};
    Stats lin[3], tab[3], mod[3];
    int no_table[3] = {0, 0, 0};
    std::vector<Stats> mod_kind(GateMateDistance::KINDS), mod_dst_kind(GateMateDistance::KINDS);
    for (auto &s : all) {
        int sx, sy, tx, ty;
        tile_xy(chip, s.src.tile, sx, sy);
        tile_xy(chip, s.dst.tile, tx, ty);
        int d = std::abs(sx - tx) + std::abs(sy - ty);
        delay_t l = linear(s.src, s.dst), m = model.estimate(s.src, s.dst), t = table.estimate(s.src, s.dst);
        lin[s.set].add(s.truth, l, d);
        mod[s.set].add(s.truth, m, d);
        // (The tables know nothing of some pairs, and give 20000 for those they cannot reach in their radius)
        if (t >= 0 && t < 20000)
            tab[s.set].add(s.truth, t, d);
        else
            no_table[s.set]++;
        if (s.set == 0)
            mod_kind[model.info(s.src).kind].add(s.truth, m, d);
        else if (s.set == 1)
            mod_dst_kind[model.info(s.dst).kind].add(s.truth, m, d);
        if (out) {
            // and the classification: kinds and states, the destination's plane, direction and position
            const auto &a = model.info(s.src), &b = model.info(s.dst);
            out << s.set << "\t" << IdString(chip_wire_info(chip, s.src).name).str(ctx) << "\t" << sx << "\t" << sy
                << "\t" << IdString(chip_wire_info(chip, s.dst).name).str(ctx) << "\t" << tx << "\t" << ty << "\t"
                << s.truth << "\t" << t << "\t" << l << "\t" << m << "\t" << int(a.kind) << "\t" << int(a.state)
                << "\t" << int(b.kind) << "\t" << int(b.state) << "\t" << int(b.plane) << "\t" << int(b.dir) << "\t"
                << tx + b.dx << "\t" << ty + b.dy << "\n";
        }
    }
    static const char *kind_names[] = {"OTHER",  "CPE_OUT",  "CPE_IN", "CPE_INT",  "IM_D",      "IM_DIAG",
                                       "IM_ALT", "IM_Y",     "OM_D",   "OM_Y",     "SB_D0",     "SB_IN",
                                       "SB_Y",   "SB_YDIAG", "SB_X",   "DRIVE_IN", "DRIVE_OUT", "CLOCK"};
    log_info("Errors against the exact delays (estimate - exact, ps):\n");
    for (int set = 0; set < 3; set++) {
        log_info("%s:\n", set_names[set]);
        report("linear", lin[set]);
        report("table", tab[set]);
        log_info("            (table: %d pairs left out, unknown or unreachable in the tables)\n", no_table[set]);
        report("model", mod[set]);
    }
    // The least delay from a CPE output to a CPE input by offset, measured from the CPE at (86, 68)
    if (!offset_map.empty()) {
        std::ifstream in(offset_map);
        if (!in)
            log_error("Cannot read '%s'.\n", offset_map.c_str());
        Stats lin_map, tab_map, mod_map;
        const int x0 = 86, y0 = 68;
        int ox, oy, truth;
        while (in >> ox >> oy >> truth) {
            int best_model = INT_MAX, best_table = INT_MAX;
            for (int o = inputs; o < anchor_count; o++)
                for (int i = 0; i < inputs; i++) {
                    WireId src = anchor(x0, y0, o), dst = anchor(x0 + ox, y0 + oy, i);
                    if (src == WireId() || dst == WireId())
                        continue;
                    best_model = std::min(best_model, int(model.estimate(src, dst)));
                    int t = table.estimate(src, dst);
                    if (t >= 0 && t < 20000)
                        best_table = std::min(best_table, t);
                }
            if (best_model == INT_MAX)
                continue;
            int d = std::abs(ox) + std::abs(oy);
            lin_map.add(truth, 100 + 100 * d, d);
            mod_map.add(truth, best_model, d);
            if (best_table != INT_MAX)
                tab_map.add(truth, best_table, d);
        }
        log_info("least delay from a CPE output to a CPE input by offset (%s):\n", offset_map.c_str());
        report("linear", lin_map);
        report("table", tab_map);
        report("model", mod_map);
    }
    log_info("wire to input pin by the kind of the wire, model:\n");
    for (int k = 0; k < GateMateDistance::KINDS; k++)
        report(kind_names[k], mod_kind[k]);
    log_info("output pin to wire by the kind of the wire, model:\n");
    for (int k = 0; k < GateMateDistance::KINDS; k++)
        report(kind_names[k], mod_dst_kind[k]);

    // Time per call, over the wire to input pin pairs
    std::vector<std::pair<WireId, WireId>> pairs;
    for (auto &s : all)
        if (s.set == 0)
            pairs.emplace_back(s.src, s.dst);
    const int calls = 1000000;
    auto time = [&](const char *name, auto fn) {
        int64_t sum = 0;
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < calls; i++) {
            auto &p = pairs[i % pairs.size()];
            sum += fn(p.first, p.second);
        }
        double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
        log_info("  %-7s %6.1f ns per call (checksum %lld)\n", name, ns / calls, (long long)sum);
    };
    if (!pairs.empty()) {
        log_info("Time per call:\n");
        time("table", [&](WireId a, WireId b) { return table.estimate(a, b); });
        time("model", [&](WireId a, WireId b) { return model.estimate(a, b); });
        time("linear", [&](WireId a, WireId b) { return linear(a, b); });
    }

    // GATEMATE_EVAL_PINS=K (diagnosis only): the estimates between bel pins of all kinds, as predictDelay
    // sees them: forward searches from K output pins per pin kind near the centre, to the input pins of
    // all kinds within 40 tiles. "tbl+lin" is what predictDelay gives with place-delay=table (linear
    // where the tables have no value).
    const char *pins_env = getenv("GATEMATE_EVAL_PINS");
    if (!pins_env)
        return;
    const int per_kind = std::max(1, atoi(pins_env));
    struct Pin
    {
        WireId wire;
        std::string kind;
        int x, y;
    };
    std::vector<Pin> outs, ins;
    auto pin_kind = [&](BelId bel, IdString pin) {
        std::string name = pin.str(ctx), k;
        for (char c : name)
            if (!(c >= '0' && c <= '9'))
                k += c;
        return ctx->getBelType(bel).str(ctx) + "." + k;
    };
    for (BelId bel : ctx->getBels()) {
        Loc loc = ctx->getBelLocation(bel);
        for (IdString pin : ctx->getBelPins(bel)) {
            WireId w = ctx->getBelPinWire(bel, pin);
            if (w == WireId())
                continue;
            PortType t = ctx->getBelPinType(bel, pin);
            Pin p{w, pin_kind(bel, pin), loc.x, loc.y};
            if (t == PORT_OUT)
                outs.push_back(p);
            else if (t == PORT_IN)
                ins.push_back(p);
        }
    }
    std::map<std::string, std::vector<Pin>> by_kind;
    for (auto &p : outs)
        by_kind[p.kind].push_back(p);
    std::vector<Pin> sources;
    for (auto &kv : by_kind) {
        auto &v = kv.second;
        std::sort(v.begin(), v.end(), [&](const Pin &a, const Pin &b) {
            return std::abs(a.x - cx) + std::abs(a.y - cy) < std::abs(b.x - cx) + std::abs(b.y - cy);
        });
        for (int i = 0; i < per_kind && i * 7 < int(v.size()); i++)
            sources.push_back(v[i * 7]);
    }
    log_info("Pin evaluation: %d output pins, %d input pins, %d sources over %d kinds\n", int(outs.size()),
             int(ins.size()), int(sources.size()), int(by_kind.size()));
    struct Acc
    {
        int n = 0, no_table = 0;
        double model = 0, model_abs = 0, tl = 0, tl_abs = 0, lin = 0, truth = 0;
        void add(const Acc &o)
        {
            n += o.n;
            no_table += o.no_table;
            model += o.model;
            model_abs += o.model_abs;
            tl += o.tl;
            tl_abs += o.tl_abs;
            lin += o.lin;
            truth += o.truth;
        }
    };
    std::map<std::pair<std::string, std::string>, Acc> acc;
    std::mutex acc_mutex;
    std::atomic<int> next_src{0};
    auto pin_worker = [&]() {
        std::vector<int> dist;
        for (int si = next_src++; si < int(sources.size()); si = next_src++) {
            const Pin &src = sources[si];
            dist.assign(tile_base.back(), INT_MAX);
            using Item = std::pair<int, WireId>;
            std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
            dist[index(src.wire)] = 0;
            queue.push({0, src.wire});
            while (!queue.empty()) {
                auto [cost, wire] = queue.top();
                queue.pop();
                if (cost > dist[index(wire)])
                    continue;
                for (PipId pip : ctx->getPipsDownhill(wire)) {
                    if (!avail(pip))
                        continue;
                    WireId next = ctx->getPipDstWire(pip);
                    int next_cost = cost + ctx->getPipDelay(pip).maxDelay();
                    if (next_cost < dist[index(next)]) {
                        dist[index(next)] = next_cost;
                        queue.push({next_cost, next});
                    }
                }
            }
            std::map<std::pair<std::string, std::string>, Acc> local;
            for (auto &dst : ins) {
                if (std::abs(dst.x - src.x) + std::abs(dst.y - src.y) > 40 || dst.wire == src.wire)
                    continue;
                int truth = dist[index(dst.wire)];
                if (truth == INT_MAX)
                    continue;
                int m = model.estimate(src.wire, dst.wire), t = table.estimate(src.wire, dst.wire),
                    l = linear(src.wire, dst.wire);
                bool has_table = t >= 0 && t < 20000;
                int tl = has_table ? t : l;
                auto &a = local[{src.kind, dst.kind}];
                a.n++;
                a.model += m - truth;
                a.model_abs += std::abs(m - truth);
                a.tl += tl - truth;
                a.tl_abs += std::abs(tl - truth);
                a.lin += l - truth;
                a.truth += truth;
                a.no_table += !has_table;
            }
            std::lock_guard<std::mutex> lock(acc_mutex);
            for (auto &kv : local)
                acc[kv.first].add(kv.second);
        }
    };
#if !defined(NPNR_DISABLE_THREADS)
    {
        std::vector<std::thread> pin_threads;
        for (int i = 0; i < 4; i++)
            pin_threads.emplace_back(pin_worker);
        for (auto &thread : pin_threads)
            thread.join();
    }
#else
    pin_worker();
#endif
    log_info("src kind -> dst kind: pairs, mean exact, model err/|err|, tbl+lin err/|err|, linear err, "
             "share without table value\n");
    std::map<std::string, Acc> by_src, by_dst;
    for (auto &kv : acc) {
        by_src[kv.first.first].add(kv.second);
        by_dst[kv.first.second].add(kv.second);
    }
    auto line = [&](const std::string &a, const std::string &b, const Acc &x) {
        log_info("  %-24s -> %-24s n=%7d exact %6.0f  model %+6.0f/%5.0f  tbl+lin %+6.0f/%5.0f  linear %+6.0f  "
                 "notbl %3.0f%%\n",
                 a.c_str(), b.c_str(), x.n, x.truth / x.n, x.model / x.n, x.model_abs / x.n, x.tl / x.n,
                 x.tl_abs / x.n, x.lin / x.n, 100.0 * x.no_table / x.n);
    };
    for (auto &kv : by_src)
        line(kv.first, "*", kv.second);
    for (auto &kv : by_dst)
        line("*", kv.first, kv.second);
    for (auto &kv : acc)
        if (kv.second.n >= 10)
            line(kv.first.first, kv.first.second, kv.second);
}

NEXTPNR_NAMESPACE_END
