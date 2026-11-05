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

#ifndef GATEMATE_DISTANCE_H
#define GATEMATE_DISTANCE_H

#include <functional>
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

struct GateMateLookahead;

// A closed-form estimate of the least routing delay between two wires on the empty chip, for the
// router's lookahead and the placer (docs/distance_model.md). From its name, each wire tells where
// the signal is (which switch box or CPE), on which plane, and whether it already travels in a
// direction; the estimate then counts tiles, segments along each axis, a turn, and plane changes,
// with constants fitted to the exact delays and scaled to the chosen timing corner at start-up.
struct GateMateDistance
{
    // avail filters out pips that can never be used
    void init(Context *ctx, std::function<bool(PipId)> avail);
    bool initialised() const { return ctx != nullptr; }
    delay_t estimate(WireId src, WireId dst) const;

    // Wire kinds by name, for the evaluation
    enum Kind : uint8_t
    {
        OTHER,
        CPE_OUT,   // CPE.OUT1/2
        CPE_IN,    // CPE.IN1..8, CLK, EN, SR
        CPE_INT,   // other wires of a CPE
        IM_D,      // IM.Pp.D0..D3, from the switch box Y outputs
        IM_DIAG,   // IM.Pp.D4/D5, from a diagonal neighbour's IM
        IM_ALT,    // IM.Pp.D6/D7, from another plane's IM in the same tile
        IM_Y,      // IM.Pp.Y
        OM_D,      // OM.Pp.D0..D3
        OM_Y,      // OM.Pp.Y
        SB_D0,     // SB_*.Pp.D0 (and D0_IO)
        SB_IN,     // SB_*.Pp.Dk_i, k 2..7: from the switch box 2..20 tiles back in direction i
        SB_Y,      // SB_*.Pp.Yi (and Yi_int)
        SB_YDIAG,  // SB_*.Pp.YDIAG (and YDIAG_int)
        SB_X,      // SB_*.Pp.X12, X34, X14, X23
        DRIVE_IN,  // SB_DRIVE.Pp.Di.IN
        DRIVE_OUT, // SB_DRIVE.Pp.Di.OUT(_NOINV)
        CLOCK,     // global clock network
        KINDS
    };
    // How the estimate treats a wire
    enum State : uint8_t
    {
        FRESH,      // at a switch box, free to go anywhere (YDIAG, D0, X inputs, OM)
        SHORT,      // on a short segment or at a switch box input, travelling in direction `dir`
        LONG,       // on a long segment (SB_DRIVE) travelling in direction `dir`
        IM_INPUT,   // an input of the input mux of plane `plane`
        IM_OUTPUT,  // the output of the input mux of plane `plane`
        BRIDGE,     // in a CPE, routed on through it
        CPE_OUTPUT, // a CPE output `dir`
        SB_D0_NODE, // a CPE output node named after a switch box's D0 input of plane `plane`
        OM_D_NODE,  // a CPE output node named after an output mux input `dir` of plane `plane`
        CLOCK_NET,
    };
    // How a CPE output reaches the wire (the reverse direction, from an output pin to any wire)
    enum Reach : uint8_t
    {
        REACH_FRESH,  // a switch box (or CPE output node) at (rdx, rdy), on plane `rplane`
        REACH_TRAVEL, // the switch box at (rdx, rdy) driving a segment in direction `rdir`, on plane `rplane`
        REACH_INPUT,  // the input mux of plane `rplane` of the CPE at (rdx, rdy), as a CPE input target
    };
    struct WireInfo
    {
        uint8_t kind = OTHER, state = FRESH, plane = 0, dir = 0;
        int8_t dx = 0, dy = 0;        // from the wire's tile to where the signal is
        int16_t base = 0, credit = 0; // ps: constant term, and the term when travelling toward the target
        // The reverse direction: how, where and on which plane; constant term, the terms when the
        // source lies behind the segment (credit) or level with its switch box (level), at the same switch box
        uint8_t reach = REACH_FRESH, rplane = 0, rdir = 0;
        int8_t rdx = 0, rdy = 0;
        int16_t rbase = 0, rcredit = 0, rlevel = 0, rsame = 0;
    };
    const WireInfo &info(WireId wire) const
    {
        return types[ctx->chip_info->tile_insts[wire.tile].type].wires[wire.index];
    }

  private:
    Context *ctx = nullptr;
    struct Target
    {
        int plane, delay; // a CPE input and the delay from it to the wire
    };
    struct TypeData
    {
        std::vector<WireInfo> wires;
        // Per wire of a CPE: the CPE inputs before it, if any (start: wire index -> first, size wires + 1)
        std::vector<int> target_start;
        std::vector<Target> targets;
        // Least delay between any two wires of a CPE, -1 if unreachable (inside_id: wire index -> row or -1)
        std::vector<int> inside_id, inside_delay;
        int inside_size = 0;
    };
    std::vector<TypeData> types;
    // The constants in ps (docs/distance_model.md)
    struct Constants
    {
        int tile, segment, odd, near, turn1, turn2, east, east_tile, same_place, plane_step, rem;
        int short_level, long_mid;
        int cpe_out, out_mux, im_same, im_alt, im_diag, imy_alt, imy_diag, im_bridge, imy_bridge, ctrl_bridge;
        int reach_om;
    } k;
    int reach_planes[13][13]; // ps from a switch box plane a CPE output starts on to the plane of the wire
    int plane_steps[13][13]; // half steps between planes, from a switch box plane to a CPE input plane
    // For a CPE at (i, j) in its 2x2 block and its output o: the plane of each block switch box (at
    // offset 0 and (1, 1)) it drives directly and the two planes it reaches through the output mux
    struct Start
    {
        int direct, om1, om2;
    };
    Start starts[2][2][2][2]; // [o][i][j][switch box]

    int route(const WireInfo &w, int x, int y, int tx, int ty, int q) const;
    int core(int state, int base, int credit, int x, int y, int plane, int dir, int tx, int ty, int q) const;
    int plane_cost(int p, int q) const { return p == 0 || q == 0 ? 0 : plane_steps[p][q] * k.plane_step / 2; }
    // From the outputs in `mask` (bit 0: OUT1, bit 1: OUT2) of the CPE at (x, y)
    int cpe_output(int x, int y, int mask, int tx, int ty, int q) const;
    // The tile terms: tiles, segments and turns along both axes (d1, d2), going east by ax
    int travel(int d1, int d2, int turn, int ax) const;
    // From the outputs in `mask` of the CPE at (x, y) to the wire `d` of the tile at (tx, ty)
    int reach(int x, int y, int mask, const WireInfo &d, int tx, int ty) const;
};

// --vopt eval-estimate=N: compare the linear estimate, the delay tables and the model with the exact
// least delays on the empty chip for about N random pairs of wires, and time them
// (docs/distance_model.md); `avail` filters out pips that can never be used; `dump`, if not empty,
// receives the samples as TSV; `offset_map`, if not empty, names docs/experiments/offset_map.tsv to
// compare with as well
void evaluate_estimates(Context *ctx, const GateMateLookahead &table, const GateMateDistance &model,
                        std::function<bool(PipId)> avail, int samples, const std::string &dump,
                        const std::string &offset_map);

NEXTPNR_NAMESPACE_END

#endif
