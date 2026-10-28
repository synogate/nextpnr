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
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <set>

#include "gatemate.h"
#include "nextpnr.h"

#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#define HIMBAECHEL_GFXIDS "uarch/gatemate/gfxids.inc"
#define HIMBAECHEL_UARCH gatemate

#include "himbaechel_constids.h"
#include "himbaechel_gfxids.h"

NEXTPNR_NAMESPACE_BEGIN

// Layout of a tile in the GUI, in tile-local coordinates with y pointing north (up on screen).
//
// Every tile has a routing channel along its bottom edge and one along its left edge. The rest
// of the tile holds its entities, side by side, with a free lane beside each: the 12 routing
// planes are rows (plane 1 at the top), and from left to right a row has the plane's switch box
// (in tiles that have one), its input mux and, for planes 9 to 12, its output mux. The CPE takes
// the right half of the tile and follows the Project Peppercorn CPE schematic: inputs IN1..IN8
// on the left (one per plane), the upper LUT tree (IN1..IN4) above the lower one (IN5..IN8), the
// registers and output muxes to their right, and the carry and propagate lines in a column at
// the right edge (carry in at the bottom, carry out at the top).
//
// Only the routing that nets use is drawn (guiDrawsUnusedRouting() is false). A used pip inside
// an entity (a mux) is a line from the input it selects to the output. A connection between
// entities runs from its source into the source's lane, down to the bottom channel, along the
// channels on the track of its plane and direction, and up the lane of its destination: lines
// never cross an entity. The exceptions are the carry and propagate lines between CPEs, which
// have their own band, and the CPE inputs, which are next to the input muxes.

namespace {

// Channels: x < CHAN (vertical) and y < CHAN (horizontal), 24 tracks each
constexpr float CHAN = 0.16f;
float track(int slot) { return 0.006f + slot * 0.0063f; }
int slot(int plane, int dir) { return ((plane - 1) % 12) * 2 + dir; }

float row(int plane) { return 0.972f - (plane - 1) * 0.0645f; }

// Switch box plane: inputs on the left edge, output drivers on the right edge. Inside, two stages:
// the diagonal mux (YDIAG_int, from the diagonal inputs X* at the bottom of the left edge, or from
// the Y muxes) at the bottom left, its output along the bottom to its driver and up a bus beside
// the four Y muxes (Y1_int..Y4_int, from the direct inputs D* or the diagonal mux) on the right.
constexpr float SB_IN_LANE = 0.166f, SB_X0 = 0.20f, SB_X1 = 0.265f, SB_H = 0.024f, SB_OUT_LANE = 0.27f;
constexpr float SB_DIAG_X0 = 0.207f, SB_DIAG_X1 = 0.217f, SB_DIAG_Y = -0.016f; // relative to the row
constexpr float SB_BUS_X = 0.236f, SB_Y_X0 = 0.24f, SB_Y_X1 = 0.25f;
constexpr float SB_BACK_X = 0.2545f, SB_BACK_Y = -0.0232f, SB_BACK_IN_X = 0.2035f, SB_BACK_IN_Y = -0.0105f;
// Input mux (8:1): inputs on the left, output on the right
constexpr float IM_IN_LANE = 0.30f, IM_X0 = 0.33f, IM_X1 = 0.368f, IM_OUT_LANE = 0.371f;
// Output mux (4:1) of planes 9 to 12, between the plane rows: output on the left, inputs on the right
constexpr float OM_OUT_LANE = 0.399f, OM_X0 = 0.403f, OM_X1 = 0.433f, OM_IN_LANE = 0.437f;
float om_centre(int plane) { return row(plane) - 0.032f; }
// The CPE, from left to right:
//  - its pins (IN1..IN8 in the rows of planes 1 to 8, CLK, EN and SR in rows 9 to 11) and their
//    internal side;
//  - vertical lanes: the bridge lane, down from the inputs to the bridge mux below the LUT2s, and
//    one lane for each carry or propagate line that can drive a 2:1 mux on the left;
//  - the 2:1 muxes TI2..TI8 in front of the second input of each first-level LUT2, and those of
//    the clock and enable (in rows 9 and 10);
//  - the LUT2s: L00..L03, L10 and L11, L20 (in the lower half);
//  - the lanes of the register's clock, enable and set/reset and of its data input;
//  - the registers, with their data input in the middle of their half;
//  - the output muxes (inputs: the LUT2 tree from above, the register, the carry output and the
//    bridge from below), the outputs and their lanes, and the carry column.
constexpr float CPE_PIN = 0.455f, CPE_INT = 0.46f, BRIDGE_LANE = 0.4625f;
constexpr float TI_X0 = 0.481f, TI_X1 = 0.49f;
constexpr float L1_X0 = 0.502f, L1_X1 = 0.544f, L2_X0 = 0.558f, L2_X1 = 0.6f, L20_X0 = 0.614f, L20_X1 = 0.652f;
constexpr float COMBIN_X = 0.607f, BYPASS_X = 0.661f;
constexpr float BRIDGE_X0 = 0.565f, BRIDGE_X1 = 0.585f, BRIDGE_Y = 0.478f;
constexpr float CLK_LANE = 0.668f, EN_LANE = 0.674f, SR_LANE = 0.68f, D_JOG = 0.686f;
constexpr float FF_X0 = 0.692f, FF_X1 = 0.737f;
constexpr float CPOUT_JOG = 0.744f, MUXOUT_JOG = 0.749f, BYPASS_JOG = 0.753f;
constexpr float OMUX_X0 = 0.758f, OMUX_X1 = 0.783f, OUT_X = 0.8f;
// The RAM I/O port above the output mux: its input I is the output mux's output (coming up at
// RAMIO_IN_X), its output OUT drives the CPE output instead (going down at RAMIO_OUT_X)
constexpr float RAMIO_X0 = 0.758f, RAMIO_X1 = 0.795f, RAMIO_IN_X = 0.786f, RAMIO_OUT_X = 0.792f;
constexpr float RAMIO_Y0 = 0.07f, RAMIO_Y1 = 0.105f, RAMIO_O_Y = 0.098f, RAMIO_I_Y = 0.084f;
// The comparator, between the output muxes (above the lower RAM I/O port) and left of the carry
// logic: its inputs (the LUT2 trees' results, COMBIN1/2) on its left, its output on its right
// (across into the carry logic, see CL_RAIL_OUT2)
constexpr float CMP_X0 = 0.765f, CMP_X1 = 0.79f, CMP_Y0 = 0.745f, CMP_Y1 = 0.775f;
constexpr float CMP_IN1_Y = 0.75f, CMP_IN2_Y = 0.77f, CMP_OUT_Y = 0.76f;
// The lanes of the CPE's outputs and RAM port pins, each its own
constexpr float OUT2_LANE = 0.818f, OUT1_LANE = 0.822f, RAM_O2_LANE = 0.826f, RAM_O1_LANE = 0.83f;
constexpr float RAM_I2_LANE = 0.834f, RAM_I1_LANE = 0.838f;
// Register inputs on the left edge, relative to the middle of the half: data, clock, enable, set/reset
constexpr float FF_D = 0, FF_CLK = -0.012f, FF_EN = -0.024f, FF_SR = -0.034f, FF_TOP = 0.032f, FF_BOTTOM = -0.04f;
// Output mux inputs, relative to the middle of the half
constexpr float OMUX_COMB = 0.03f, OMUX_DOUT = 0, OMUX_CPOUT = -0.02f, OMUX_MUXOUT = -0.035f;
// Where the LUT2 tree's output passes above the register, and where the carry output leaves it
constexpr float BYPASS_DY = 0.045f, CPOUT_DY = -0.05f;
// Carry column: x of each line; the carry in at the bottom, out at the top; X lines in a band
// between the bottom channel and the entities
constexpr float CY1 = 0.944f, PY1 = 0.958f, CY2 = 0.972f, PY2 = 0.986f;
constexpr float CARRY_IN_Y = 0.205f, CARRY_OUT_Y = 0.995f, CX_BAND = 0.188f, PX_BAND = 0.176f;
// The track below the CPE along which CINY1 goes to a LUT2 in a carry-in mode (see drawBel)
constexpr float CIN_TRACK_Y = 0.228f;
// The 2:1 mux where a Y line leaves, below its end: its output is where the line leaves (COUTY1, ...,
// CMUX_OUT_DY below CARRY_OUT_Y, where the line to the next CPE starts), its inputs CMUX_IN_DY
// below that; its width each side of the line (reaching the line's _VAL) and the half width of its
// output
constexpr float CMUX_OUT_DY = 0.017f, CMUX_IN_DY = 0.007f, CMUX_W = 0.005f, CMUX_PAD = 0.0015f;
// The carry logic (CPE_CPLINES), right of the output muxes, between OUT2 above and OUT1 below and
// left of the Y lines: a row for each line, from the bottom PX, CX, PY2, CY2, PY1, CY1, so that the
// ways out to the lines do not cross (right, and the X lines' down to their band, the Y lines' up
// beside their line). In a row, two 2:1 muxes (_OUT: OUT1 above or OUT2; _OUT2: the comparator
// above or a carry or propagate input) and the 2:1 mux choosing between them (_VAL). Their inputs
// come along rails on the left: OUT2 from above, OUT1, the comparator's output, CINX and PINX from
// below, and CIY12 and PIY12 from their muxes below the rows (C_Y12: CINY1 above or CINY2, PINY1
// above or PINY2).
constexpr float CL_RAIL_OUT2 = 0.846f, CL_RAIL_OUT1 = 0.85f, CL_RAIL_COMP = 0.854f, CL_RAIL_CINX = 0.858f,
                CL_RAIL_PINX = 0.862f, CL_RAIL_CIY12 = 0.866f, CL_RAIL_PIY12 = 0.87f;
constexpr float CL_SEL_X0 = 0.876f, CL_SEL_X1 = 0.884f, CL_VAL_X0 = 0.89f, CL_VAL_X1 = 0.898f;
constexpr float CL_ROW0 = 0.65f, CL_ROW_DY = 0.03f, CL_SEL_DY = 0.008f, CL_IN_DY = 0.003f;
constexpr float CL_X_DOWN_PX = 0.92f, CL_X_DOWN_CX = 0.93f; // where the X lines' ways go down
constexpr float CIY12_X0 = 0.892f, CIY12_X1 = 0.882f, CIY12_Y = 0.6f, PIY12_X0 = 0.892f, PIY12_X1 = 0.882f,
                PIY12_Y = 0.585f;
// The 2:1 muxes where the X lines leave, on their band near the right edge (COUTX, POUTX at their
// output); the carry logic comes in above the band
constexpr float COUTX_X0 = 0.945f, COUTX_X1 = 0.955f, POUTX_X0 = 0.925f, POUTX_X1 = 0.935f, XMUX_IN_DY = 0.004f;
// Block RAMs: each RAM column of the chip (beside a CPE column) is drawn as a column of its own,
// right of that CPE column (see gui_column). In it, a RAM is a block 16 tiles high with its pins
// on its left and their lanes in front of it.
constexpr float RAM_LANE = 0.35f, RAM_X0 = 0.55f, RAM_X1 = 0.9f;
// IO tiles: the pad (GPIO) and its IO cell (IOSEL) side by side; the IO cell's two inputs as a bus
// down to the edge switch (IOES), which has one mux per plane (SB_IN_pp) in the plane's row,
// choosing one of the inputs or what the switch box next to it sends back (ALTIN_pp), for that
// switch box. The edge's other logic (LES, BES, RES, TES) below the boxes.
constexpr float GPIO_X0 = 0.2f, GPIO_X1 = 0.4f, GPIO_Y0 = 0.58f, GPIO_Y1 = 0.88f;
constexpr float IOSEL_X0 = 0.47f, IOSEL_X1 = 0.67f, IOSEL_Y0 = 0.5f, IOSEL_Y1 = 0.92f;
constexpr float IO_IN1_X = 0.73f, IO_IN2_X = 0.745f, ALTIN_X0 = 0.757f, IOES_X0 = 0.8f, IOES_X1 = 0.83f;
constexpr float IO_IN1_Y = 0.88f, IO_IN2_Y = 0.85f, IO_PAD_Y = 0.82f, IO_OUT_Y = 0.74f, IO_EN_Y = 0.66f;
// The edge's other logic (LES, RES, BES, TES: clock selection, die-to-die links, the switch box's
// returns into the carry lines): its muxes (the _int wires) in one column and their inputs in
// another, in the order of their names; in IO tiles below the pad and its IO cell
constexpr float EDGE_IN_X = 0.34f, EDGE_IN_LANE = 0.31f, EDGE_MUX_X = 0.62f, EDGE_MUX_LANE = 0.645f;

// Levels of detail, in pixels per tile on screen (see GraphicElement::min_scale): the whole chip
// is drawn at about 8. Zoomed out, a used CPE is an outline and a connection between tiles a
// simple path; the inside of the tiles appears when zooming in.
constexpr float LOD_TILE = 25;    // connections inside a tile, outlines of unused CPEs
constexpr float LOD_ENTITY = 60;  // switch boxes, input and output muxes, and the pips in them
constexpr float LOD_DETAIL = 150; // the inside of the CPE
constexpr float LOD_TEXT = 400;   // labels of LUT2 functions and registers
constexpr float LOD_PORTS = 1000; // names of ports in the CPE
constexpr float NO_LIMIT = std::numeric_limits<float>::infinity();

float mid(float a, float b) { return (a + b) / 2; }
// Centres of the LUT2 outputs and of the two halves
float m12() { return mid(row(1), row(2)); }
float m34() { return mid(row(3), row(4)); }
float m56() { return mid(row(5), row(6)); }
float m78() { return mid(row(7), row(8)); }
float mu() { return mid(m12(), m34()); }
float ml() { return mid(m56(), m78()); }

struct Pt
{
    float x = 0.5f, y = 0.5f;
};

// What a tile wire is part of: wires of the same entity in a tile connect directly, others
// through lanes and channels
enum Entity
{
    E_OTHER = 0,
    E_CPE = 1,   // the CPE, its pins and internal wires
    E_CARRY = 2, // ends of the carry and propagate lines between CPEs (and edge tiles)
    E_IO = 3,    // GPIO and IOSEL of a pad
    E_CLOCK = 4, // clock and PLL tile
    E_RAM = 5,
    E_IOES = 6, // the edge switch of an IO tile, with the IO cell's input bus
    E_EDGE = 7, // the other logic of the chip's edge (clock selection, die-to-die links, ...)
    E_SB = 100, // + plane
    E_IM = 200, // + plane
    E_OM = 300, // + plane
};

struct Anchor
{
    Pt p;
    float lane = 0.5f; // x of the vertical lane the wire uses to reach the channels
    int plane = 0;     // routing plane, or 0
    int entity = E_OTHER;
};

bool starts_with(const std::string &s, const char *prefix) { return s.compare(0, strlen(prefix), prefix) == 0; }
bool ends_with(const std::string &s, const char *suffix)
{
    size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// The row of a line's carry logic (n starts with the line's name, see CL_RAIL_OUT2)
float carry_row(const std::string &n)
{
    const char *order[] = {"PX", "CX", "PY2", "CY2", "PY1", "CY1"};
    for (int i = 0; i < 6; i++)
        if (starts_with(n, order[i]))
            return CL_ROW0 + i * CL_ROW_DY;
    return CL_ROW0;
}

// Number after a prefix, e.g. num("P09.Y", 1) == 9
int num(const std::string &s, size_t pos)
{
    int v = 0;
    bool any = false;
    while (pos < s.size() && isdigit(s[pos])) {
        v = v * 10 + (s[pos++] - '0');
        any = true;
    }
    return any ? v : -1;
}

// A position for wires with no particular place: inside the entity area of the tile
Pt spread(const std::string &name)
{
    size_t h = std::hash<std::string>{}(name);
    return Pt{0.22f + 0.72f * float(h % 997) / 997, 0.24f + 0.70f * float((h / 997) % 991) / 991};
}

Anchor make(Pt p, float lane, int plane, int entity)
{
    Anchor a;
    a.p = p;
    a.lane = lane;
    a.plane = plane;
    a.entity = entity;
    return a;
}

// Switch box wire, `rest` after "SB_BIG.Pxx." or "SB_SML.Pxx."
Anchor sb_anchor(int plane, const std::string &rest)
{
    float r = row(plane);
    float in_lane = SB_IN_LANE + (plane - 1) * 0.0026f, out_lane = SB_OUT_LANE + (plane - 1) * 0.0024f;
    int entity = E_SB + plane;
    if (rest.size() >= 2 && rest[0] == 'Y') {
        // Output drivers Y1..Y4 and YDIAG on the right edge; the Y muxes' outputs (Y1_int..Y4_int)
        // in their column, the diagonal mux's output (YDIAG_int) at the bottom left
        bool diagonal = starts_with(rest, "YDIAG");
        int i = diagonal ? 4 : rest[1] - '1';
        Pt p{SB_X1, r + 0.016f - i * 0.008f};
        if (ends_with(rest, "_int"))
            p = diagonal ? Pt{SB_DIAG_X1, r + SB_DIAG_Y} : Pt{SB_Y_X1, p.y};
        return make(p, out_lane, plane, entity);
    }
    // Inputs on the left edge: D0 and D{2..7}_{1..4} for the Y muxes from the top, the diagonal
    // inputs X* for the diagonal mux at the bottom, in front of it
    int i = -1;
    float y = 0;
    if (starts_with(rest, "D0"))
        i = 0;
    else if (rest.size() >= 4 && rest[0] == 'D' && rest[2] == '_')
        i = 1 + (rest[3] - '1') * 6 + (rest[1] - '2');
    if (i >= 0) {
        y = r + SB_H - 0.002f - i * 0.028f / 24;
    } else {
        const char *diagonal[] = {"X12", "X14", "X23", "X34"};
        for (int k = 0; k < 4; k++)
            if (starts_with(rest, diagonal[k]))
                i = k, y = r - 0.0173f - k * 0.0016f;
    }
    if (i < 0)
        return make(spread(rest), in_lane, plane, entity);
    Pt p{SB_X0, y};
    if (ends_with(rest, "_IO") || ends_with(rest, "_D2D") || ends_with(rest, "_CLK"))
        p.x -= 0.004f;
    return make(p, in_lane, plane, entity);
}

Anchor cpe_anchor(const std::string &n)
{
    // n is the part after "CPE."
    auto cpe = [](float x, float y) { return make(Pt{x, y}, x, 0, E_CPE); };
    auto carry = [](float x, float y) { return make(Pt{x, y}, x, 0, E_CARRY); };
    if (starts_with(n, "IN") && n.size() >= 3 && isdigit(n[2])) {
        int i = num(n, 2);
        return cpe(ends_with(n, "_int") ? CPE_INT : CPE_PIN, row(i));
    }
    // Outputs of the 2:1 muxes on the left
    if (starts_with(n, "TI"))
        return cpe(TI_X1, row(num(n, 2)));
    if (n == "CLK_int")
        return cpe(TI_X1, row(9));
    if (n == "EN_int")
        return cpe(TI_X1, row(10));
    if (n.size() >= 7 && n[0] == 'D' && n[2] == '_' && ends_with(n, "_int") && isdigit(n[1])) {
        // LUT2 inputs D{0,1}_{lut}_int
        int d = n[1] - '0';
        std::string lut = n.substr(3, 2);
        if (lut == "00")
            return cpe(L1_X0, d ? row(2) : row(1));
        if (lut == "01")
            return cpe(L1_X0, d ? row(4) : row(3));
        if (lut == "02")
            return cpe(L1_X0, d ? row(6) : row(5));
        if (lut == "03")
            return cpe(L1_X0, d ? row(8) : row(7));
        if (lut == "10")
            return cpe(L2_X0, d ? m34() : m12());
        if (lut == "11")
            return cpe(L2_X0, d ? m78() : m56());
    }
    if (n == "COMBOUT2_int")
        return cpe(L2_X1, mu());
    if (n == "COMBOUT1_int")
        return cpe(L20_X1, ml());
    if (n == "COMBIN_int")
        return cpe(L20_X0, ml() + 0.045f);
    if (n == "COMBIN2_int")
        return cpe(CMP_X0, CMP_IN2_Y);
    if (n == "COMBIN1_int")
        return cpe(CMP_X0, CMP_IN1_Y);
    if (n == "MUXOUT_int")
        return cpe(BRIDGE_X1, BRIDGE_Y);
    if (n == "DIN2_int")
        return cpe(FF_X0, mu() + FF_D);
    if (n == "DIN1_int")
        return cpe(FF_X0, ml() + FF_D);
    if (n == "DOUT2_int")
        return cpe(FF_X1, mu() + FF_D);
    if (n == "DOUT1_int")
        return cpe(FF_X1, ml() + FF_D);
    if (n == "CPOUT2_int")
        return cpe(L2_X1, mu() + CPOUT_DY);
    if (n == "CPOUT1_int")
        return cpe(L20_X1, ml() + CPOUT_DY);
    if (n == "OUT2_int")
        return cpe(OMUX_X1, mu());
    if (n == "OUT1_int")
        return cpe(OMUX_X1, ml());
    // The CPE outputs leave through lanes on their right to the switch boxes and output muxes
    if (n == "OUT2")
        return make(Pt{OUT_X, mu()}, OUT2_LANE, 0, E_CPE);
    if (n == "OUT1")
        return make(Pt{OUT_X, ml()}, OUT1_LANE, 0, E_CPE);
    // The outputs back into the carry logic
    if (n == "OUT2_IN_int")
        return cpe(CL_RAIL_OUT2, mu());
    if (n == "OUT1_IN_int")
        return cpe(CL_RAIL_OUT1, ml());
    if (n == "COMPOUT_int")
        return cpe(CMP_X1, CMP_OUT_Y);
    if (n == "COMPOUT_IN_int")
        return cpe(CL_RAIL_COMP, CMP_OUT_Y);
    // The RAM pin paths: RAM_O drives a RAM input, RAM_I takes a RAM output
    if (n == "RAM_O2" || n == "RAM_O1")
        return make(Pt{RAMIO_X1, (n == "RAM_O2" ? mu() : ml()) + RAMIO_O_Y}, n == "RAM_O2" ? RAM_O2_LANE : RAM_O1_LANE,
                    0, E_CPE);
    if (n == "RAM_I2" || n == "RAM_I1")
        return make(Pt{RAMIO_X1, (n == "RAM_I2" ? mu() : ml()) + RAMIO_I_Y}, n == "RAM_I2" ? RAM_I2_LANE : RAM_I1_LANE,
                    0, E_CPE);
    if (n == "CLK")
        return cpe(CPE_PIN, row(9));
    if (n == "EN")
        return cpe(CPE_PIN, row(10));
    if (n == "SR")
        return cpe(CPE_PIN, row(11));
    // Carry and propagate lines between CPEs
    if (n == "CINX")
        return carry(CPE_PIN, CX_BAND);
    if (n == "PINX")
        return carry(CPE_PIN, PX_BAND);
    if (n == "COUTX")
        return carry(COUTX_X1, CX_BAND);
    if (n == "POUTX")
        return carry(POUTX_X1, PX_BAND);
    auto column = [](const std::string &s) {
        // CY1, PY1, CY2, PY2 at the start of s
        if (starts_with(s, "CY1"))
            return CY1;
        if (starts_with(s, "PY1"))
            return PY1;
        if (starts_with(s, "CY2"))
            return CY2;
        return PY2;
    };
    if (starts_with(n, "CINY") || starts_with(n, "PINY"))
        return carry(column(std::string(1, n[0]) + "Y" + n.substr(4)), CARRY_IN_Y);
    if (starts_with(n, "COUTY") || starts_with(n, "POUTY"))
        return carry(column(std::string(1, n[0]) + "Y" + n.substr(5)), CARRY_OUT_Y - CMUX_OUT_DY);
    // The carry logic (see CL_RAIL_OUT2): the outputs of its muxes
    if (n == "CIY12")
        return cpe(CIY12_X1, CIY12_Y);
    if (n == "PIY12")
        return cpe(PIY12_X1, PIY12_Y);
    if (ends_with(n, "_OUT2"))
        return cpe(CL_SEL_X1, carry_row(n) - CL_SEL_DY);
    if (ends_with(n, "_OUT"))
        return cpe(CL_SEL_X1, carry_row(n) + CL_SEL_DY);
    if (ends_with(n, "_VAL"))
        return cpe(CL_VAL_X1, carry_row(n));
    Pt p = spread("CPE." + n);
    return cpe(p.x, p.y);
}

// Wires of the edge tiles: along the side that faces the fabric. The carry and propagate lines
// into the first CPE (and out of the last one) are in line with the CPE's.
Anchor edge_anchor(const std::string &tile_type, const std::string &name)
{
    auto carry = [](float x, float y) { return make(Pt{x, y}, x, 0, E_CARRY); };
    if (name == "LES.CPE_CINX")
        return carry(0.95f, CX_BAND);
    if (name == "LES.CPE_PINX")
        return carry(0.95f, PX_BAND);
    if (name == "RES.CPE_COUTX")
        return carry(0.25f, CX_BAND);
    if (name == "RES.CPE_POUTX")
        return carry(0.25f, PX_BAND);
    for (auto &c : {std::make_pair("CINY1", CY1), std::make_pair("PINY1", PY1), std::make_pair("CINY2", CY2),
                    std::make_pair("PINY2", PY2)})
        if (name == std::string("BES.CPE_") + c.first)
            return carry(c.second, 0.95f);
    for (auto &c : {std::make_pair("COUTY1", CY1), std::make_pair("POUTY1", PY1), std::make_pair("COUTY2", CY2),
                    std::make_pair("POUTY2", PY2)})
        if (name == std::string("TES.CPE_") + c.first)
            return carry(c.second, 0.25f);
    // The rest is edge logic, laid out by gfx_data (see edge_logic_anchor)
    return make(Pt{EDGE_IN_X, 0.5f}, EDGE_IN_LANE, 0, E_EDGE);
}

// Wires of the pad, its IO cell and the edge switch (see GPIO_X0)
Anchor io_anchor(const std::string &kind, const std::string &rest)
{
    auto io = [](float x, float y) { return make(Pt{x, y}, x, 0, E_IO); };
    auto bus = [](float x, float y) { return make(Pt{x, y}, x, 0, E_IOES); };
    if (kind == "GPIO") {
        // The pad's connections to the IO cell on its right; the pad side on its left
        if (rest == "Y")
            return io(GPIO_X1, IO_PAD_Y);
        if (rest == "A")
            return io(GPIO_X1, IO_OUT_Y);
        if (rest == "T")
            return io(GPIO_X1, IO_EN_Y);
        Pt f = spread("GPIO." + rest);
        return io(GPIO_X0, GPIO_Y0 + 0.02f + (GPIO_Y1 - GPIO_Y0 - 0.04f) * (f.y - 0.24f) / 0.70f);
    }
    if (kind == "IOSEL") {
        if (rest == "GPIO_IN")
            return io(IOSEL_X0, IO_PAD_Y);
        if (rest == "GPIO_OUT")
            return io(IOSEL_X0, IO_OUT_Y);
        if (rest == "GPIO_EN")
            return io(IOSEL_X0, IO_EN_Y);
        if (rest == "IN1")
            return io(IOSEL_X1, IO_IN1_Y);
        if (rest == "IN2")
            return io(IOSEL_X1, IO_IN2_Y);
        // Outputs from the fabric on the right edge, each with its own lane down to the channel
        if (starts_with(rest, "OUT")) {
            int k = num(rest, 3);
            return make(Pt{IOSEL_X1, 0.80f - 0.03f * k}, IOSEL_X1 + 0.012f + 0.005f * k, 0, E_IO);
        }
        // Clocks and the DDR clock from below
        if (starts_with(rest, "CLOCK"))
            return make(Pt{IOSEL_X0 + 0.03f * num(rest, 5), IOSEL_Y0}, IOSEL_X0 + 0.03f * num(rest, 5), 0, E_IO);
        if (rest == "DDR")
            return make(Pt{IOSEL_X0 + 0.15f, IOSEL_Y0}, IOSEL_X0 + 0.15f, 0, E_IO);
        Pt f = spread("IOSEL." + rest);
        return io(IOSEL_X0 + 0.02f + 0.16f * (f.x - 0.22f) / 0.72f, IOSEL_Y0 + 0.02f);
    }
    // The edge switch
    if (rest == "IO_IN1")
        return bus(IO_IN1_X, IO_IN1_Y);
    if (rest == "IO_IN2")
        return bus(IO_IN2_X, IO_IN2_Y);
    int plane = num(rest, rest.find_last_of('_') + 1);
    if (plane >= 1 && plane <= 12) {
        if (starts_with(rest, "SB_IN"))
            return make(Pt{IOES_X1, row(plane)}, 0.85f + 0.007f * (plane - 1), plane, E_IOES);
        if (starts_with(rest, "ALTIN"))
            return make(Pt{ALTIN_X0 + 0.003f * (plane - 1), row(plane) - 0.006f}, ALTIN_X0 + 0.003f * (plane - 1),
                        plane, E_IOES);
    }
    Pt f = spread("IOES." + rest);
    return make(f, f.x, 0, E_IOES);
}

Anchor edge_logic_anchor(int slot, int count, bool mux, bool io_tile)
{
    float y0 = 0.2f, y1 = io_tile ? 0.47f : 0.95f;
    float y = y1 - (y1 - y0) * (slot + 0.5f) / count;
    return mux ? make(Pt{EDGE_MUX_X, y}, EDGE_MUX_LANE, 0, E_EDGE) : make(Pt{EDGE_IN_X, y}, EDGE_IN_LANE, 0, E_EDGE);
}

Anchor wire_anchor(const std::string &tile_type, std::string name)
{
    if (ends_with(name, "_n"))
        name = name.substr(0, name.size() - 2);
    size_t dot = name.find('.');
    std::string kind = dot == std::string::npos ? name : name.substr(0, dot);
    std::string rest = dot == std::string::npos ? "" : name.substr(dot + 1);
    if (kind == "SB_BIG" || kind == "SB_SML") {
        int plane = num(rest, 1);
        size_t d = rest.find('.');
        if (plane > 0 && d != std::string::npos) {
            std::string port = rest.substr(d + 1);
            // A big switch box's Y1..Y4 and YDIAG are its muxes' outputs (a small one's Y*_int): at
            // the mux, where the pips into them end (see sb_mux_output)
            if (kind == "SB_BIG" && starts_with(port, "Y") && !ends_with(port, "_int"))
                port += "_int";
            return sb_anchor(plane, port);
        }
    }
    if (kind == "SB_DRIVE") {
        // SB_DRIVE.Pxx.D{i}.{IN,OUT,OUT_NOINV}: the driver behind output Y{i}, in its output lane
        int plane = num(rest, 1);
        size_t d = rest.find(".D");
        if (plane > 0 && d != std::string::npos) {
            Anchor a = sb_anchor(plane, "Y" + std::to_string(num(rest, d + 2)));
            a.p.x += ends_with(rest, ".IN") ? 0 : (ends_with(rest, "NOINV") ? 0.004f : 0.002f);
            return a;
        }
    }
    if (kind == "IM") {
        int plane = num(rest, 1);
        size_t d = rest.find('.');
        if (plane > 0 && d != std::string::npos) {
            std::string pin = rest.substr(d + 1);
            float r = row(plane);
            if (pin == "Y")
                return make(Pt{IM_X1, r}, IM_OUT_LANE + (plane - 1) * 0.0022f, plane, E_IM + plane);
            return make(Pt{IM_X0, r + 0.022f - num(pin, 1) * 0.0063f}, IM_IN_LANE + (plane - 1) * 0.0024f, plane,
                        E_IM + plane);
        }
    }
    if (kind == "OM") {
        int plane = num(rest, 1);
        size_t d = rest.find('.');
        if (plane > 0 && d != std::string::npos) {
            std::string pin = rest.substr(d + 1);
            float c = om_centre(plane);
            if (pin == "Y")
                return make(Pt{OM_X0, c}, OM_OUT_LANE, plane, E_OM + plane);
            return make(Pt{OM_X1, c + 0.009f - num(pin, 1) * 0.006f}, OM_IN_LANE + (plane - 9) * 0.004f, plane,
                        E_OM + plane);
        }
    }
    if (kind == "CPE")
        return cpe_anchor(rest);
    if (kind == "GPIO" || kind == "IOSEL" || kind == "IOES")
        return io_anchor(kind, rest);
    if (kind == "RAM")
        // Along the RAM bar, laid out by gfx_data (each of the RAM's two tiles has its pins along
        // its own 8 rows of the bar)
        return make(Pt{RAM_X0, 0.5f}, RAM_LANE, 0, E_RAM);
    if (kind == "IOES" || kind == "LES" || kind == "RES" || kind == "BES" || kind == "TES")
        return edge_anchor(tile_type, name);
    Pt f = spread(name);
    bool clock = kind == "GLBOUT" || kind == "CLKIN" || starts_with(kind, "PLL");
    return make(f, f.x, 0, clock ? E_CLOCK : E_OTHER);
}

void line(std::vector<GraphicElement> &g, GraphicElement::style_t style, float x1, float y1, float x2, float y2)
{
    GraphicElement el(GraphicElement::TYPE_LINE, style, x1, y1, x2, y2, 0);
    g.push_back(el);
}

void polyline(std::vector<GraphicElement> &g, GraphicElement::style_t style, const std::vector<Pt> &pts)
{
    for (size_t i = 1; i < pts.size(); i++)
        if (pts[i - 1].x != pts[i].x || pts[i - 1].y != pts[i].y)
            line(g, style, pts[i - 1].x, pts[i - 1].y, pts[i].x, pts[i].y);
}

void box(std::vector<GraphicElement> &g, GraphicElement::style_t style, float x1, float y1, float x2, float y2)
{
    GraphicElement el(GraphicElement::TYPE_BOX, style, x1, y1, x2, y2, 0);
    g.push_back(el);
}

// Text centred in a box, as large as fits, shown from the zoom min_scale on
void label(std::vector<GraphicElement> &g, GraphicElement::style_t style, float x1, float y1, float x2, float y2,
           const std::string &text, float min_scale = LOD_TEXT)
{
    if (text.empty())
        return;
    GraphicElement el(GraphicElement::TYPE_LABEL, style, x1, y1, x2, y2, 0);
    el.text = text;
    el.min_scale = min_scale;
    g.push_back(el);
}

// Show the elements added since g had `from` elements only from the zoom min_scale up to max_scale
void lod(std::vector<GraphicElement> &g, size_t from, float min_scale, float max_scale = NO_LIMIT)
{
    for (size_t i = from; i < g.size(); i++) {
        g[i].min_scale = min_scale;
        g[i].max_scale = max_scale;
    }
}

// A mux symbol: a trapezoid from inputs at x1 to the output at x2, centred on y
void mux(std::vector<GraphicElement> &g, GraphicElement::style_t style, float x1, float x2, float y, float h_in,
         float h_out)
{
    line(g, style, x1, y - h_in, x1, y + h_in);
    line(g, style, x2, y - h_out, x2, y + h_out);
    line(g, style, x1, y + h_in, x2, y + h_out);
    line(g, style, x1, y - h_in, x2, y - h_out);
}

// Connections longer than the longest switch box span (20 tiles) belong to the global clock
// networks, which reach every tile: drawn as a short stub with a bar where they arrive, not as a
// line from the clock buffer.
bool is_global(Pt from, Pt to) { return std::abs(from.x - to.x) + std::abs(from.y - to.y) > 22; }

void global_stub(std::vector<GraphicElement> &g, GraphicElement::style_t style, Pt to)
{
    line(g, style, to.x - 0.025f, to.y + 0.008f, to.x, to.y);
    line(g, style, to.x - 0.025f, to.y + 0.002f, to.x - 0.025f, to.y + 0.014f);
}

} // namespace

// Where tile column x is drawn: one column further right for each RAM column left of it, which is
// drawn in its own column right of the CPE column it belongs to
static int gui_column(const Context *ctx, int x)
{
    static std::once_flag once;
    static std::vector<int> ram_columns;
    std::call_once(once, [&]() {
        std::set<int> columns;
        for (int t = 0; t < ctx->chip_info->tile_insts.ssize(); t++)
            for (auto &bel : chip_tile_info(ctx->chip_info, t).bels)
                if (IdString(bel.bel_type) == id_RAM) {
                    int tx, ty;
                    tile_xy(ctx->chip_info, t, tx, ty);
                    columns.insert(tx);
                }
        ram_columns.assign(columns.begin(), columns.end());
    });
    int shift = 0;
    for (int c : ram_columns)
        shift += c < x;
    return x + shift;
}

// Anchor of every tile wire of every tile type, in tile-local coordinates
struct GateMateGfx
{
    std::vector<std::vector<Anchor>> anchors;
    std::vector<std::vector<std::string>> names;
};

static GateMateGfx &gfx_data(const Context *ctx)
{
    static std::once_flag once;
    static GateMateGfx data;
    std::call_once(once, [&]() {
        const ChipInfoPOD *chip = ctx->chip_info;
        data.anchors.resize(chip->tile_types.ssize());
        data.names.resize(chip->tile_types.ssize());
        for (int t = 0; t < chip->tile_types.ssize(); t++) {
            const auto &tt = chip->tile_types[t];
            std::string type_name = IdString(tt.type_name).str(ctx);
            for (const auto &w : tt.wires) {
                std::string name = IdString(w.name).str(ctx);
                data.names[t].push_back(name);
                data.anchors[t].push_back(wire_anchor(type_name, name));
            }
            // The edge logic, by name (a wire arriving from another tile where the wire it feeds is)
            std::map<std::string, std::vector<int>> edge;
            for (int i = 0; i < tt.wires.ssize(); i++)
                if (data.anchors[t][i].entity == E_EDGE) {
                    const std::string &name = data.names[t][i];
                    edge[ends_with(name, "_n") ? name.substr(0, name.size() - 2) : name].push_back(i);
                }
            // The RAM's pins, by name (numbers by value: DO[2] before DO[10]), along the tile's 8
            // rows of the RAM bar
            auto by_value = [](const std::string &n) {
                std::string key;
                for (size_t i = 0; i < n.size();) {
                    size_t j = i;
                    while (j < n.size() && isdigit(n[j]))
                        j++;
                    if (j > i) {
                        key += std::string(8 - std::min<size_t>(8, j - i), '0') + n.substr(i, j - i);
                        i = j;
                    } else {
                        key += n[i++];
                    }
                }
                return key;
            };
            std::map<std::string, std::vector<int>> ram;
            for (int i = 0; i < tt.wires.ssize(); i++)
                if (data.anchors[t][i].entity == E_RAM) {
                    const std::string &name = data.names[t][i];
                    ram[by_value(ends_with(name, "_n") ? name.substr(0, name.size() - 2) : name)].push_back(i);
                }
            int pin = 0;
            for (auto &r : ram) {
                float y = 7.95f - 7.7f * (pin++ + 0.5f) / ram.size();
                for (int i : r.second)
                    data.anchors[t][i] = make(Pt{RAM_X0, y}, RAM_LANE, 0, E_RAM);
            }
            int slot = 0;
            for (auto &e : edge) {
                Anchor a = edge_logic_anchor(slot++, int(edge.size()), ends_with(e.first, "_int"),
                                             type_name.find("GPIO") != std::string::npos);
                for (int i : e.second)
                    data.anchors[t][i] = a;
            }
        }
    });
    return data;
}

static int tile_type_of(const Context *ctx, int tile) { return ctx->chip_info->tile_insts[tile].type; }

// A tile wire's anchor, with its position and lane in GUI coordinates
struct PlacedAnchor
{
    Anchor a;
    const std::string *name; // of the tile wire
    int tile, tx, ty;
    Pt pos() const { return Pt{tx + a.p.x, ty + a.p.y}; }
    float lane() const { return tx + a.lane; }
};

static PlacedAnchor placed(const Context *ctx, int tile, int index)
{
    PlacedAnchor pa;
    pa.a = gfx_data(ctx).anchors.at(tile_type_of(ctx, tile)).at(index);
    pa.name = &gfx_data(ctx).names.at(tile_type_of(ctx, tile)).at(index);
    pa.tile = tile;
    tile_xy(ctx->chip_info, tile, pa.tx, pa.ty);
    // (a RAM's pins are in the RAM's own column)
    pa.tx = gui_column(ctx, pa.tx) + (pa.a.entity == E_RAM ? 1 : 0);
    return pa;
}

static const std::string &tile_wire_name(const Context *ctx, int tile, int index)
{
    return gfx_data(ctx).names.at(tile_type_of(ctx, tile)).at(index);
}

// The index of the tile wire called name in a tile, or -1
static int tile_wire_index(const Context *ctx, int tile, const std::string &name)
{
    const auto &names = gfx_data(ctx).names.at(tile_type_of(ctx, tile));
    auto it = std::find(names.begin(), names.end(), name);
    return it == names.end() ? -1 : int(it - names.begin());
}

// The tile wire that drives a node: the one with pips uphill in its own tile, or for a node driven
// by a bel pin, the one that does not arrive from another tile
static WireId node_driver(const Context *ctx, WireId node, int except_tile, int except_index)
{
    WireId first, local;
    for (WireId tw : ctx->get_tile_wire_range(node)) {
        if (first == WireId())
            first = tw;
        if (tw.tile == except_tile && tw.index == except_index)
            continue;
        if (chip_wire_info(ctx->chip_info, tw).pips_uphill.ssize() > 0)
            return tw;
        if (local == WireId() && !ends_with(tile_wire_name(ctx, tw.tile, tw.index), "_n"))
            local = tw;
    }
    return local != WireId() ? local : first;
}

// The carry and propagate lines that can drive a 2:1 mux on the CPE's left (the second LUT2
// inputs TI2..TI8, the clock and the enable): each has a lane there, the leftmost reaching
// furthest up. Those from the CPE below (Y) come along a track below the CPE; those from the left
// (X) arrive on their band.
struct CarryLane
{
    const char *name;
    float x, track_y;
};
static const CarryLane carry_lanes[] = {{"PINY1", 0.465f, 0.21f},
                                        {"CINX", 0.468f, 0},
                                        {"PINX", 0.471f, 0},
                                        {"CINY2", 0.474f, 0.216f},
                                        {"PINY2", 0.477f, 0.222f}};

// The output of the first-level LUT2 L0k
static float lut_output_y(int k)
{
    const float outputs[4] = {m12(), m34(), m56(), m78()};
    return outputs[std::min(std::max(k, 0), 3)];
}

// A connection inside the CPE, following its schematic: a pip of a mux runs from the input it
// selects through the mux symbol to the output; connections between the parts run in their own
// lanes, horizontally and vertically.
static void cpe_internal(std::vector<GraphicElement> &g, GraphicElement::style_t style, const PlacedAnchor &s,
                         const PlacedAnchor &d)
{
    const std::string sn = s.name->substr(4), dn = d.name->substr(4); // after "CPE."
    const Pt S = s.a.p, D = d.a.p;                                    // tile-local
    auto path = [&](std::vector<Pt> pts) {
        for (auto &p : pts)
            p = Pt{s.tx + p.x, s.ty + p.y};
        polyline(g, style, pts);
    };
    // Into the input of a mux at (x0, in_y), and across the mux to its output D
    auto through_mux = [&](std::vector<Pt> pts, float x0, float in_y) {
        pts.push_back(Pt{x0, in_y});
        pts.push_back(D);
        path(pts);
    };
    auto lut_input = [](const std::string &n) {
        return n.size() == 9 && n[0] == 'D' && n[2] == '_' && ends_with(n, "_int");
    };
    // A second-level LUT2 input from a first-level one: the signal passes through that LUT2 (a
    // LUT2 may take either input; postRoute permutes its function to match), so from its output
    if (lut_input(dn) && dn[3] == '1' && lut_input(sn) && sn[3] == '0') {
        float m = lut_output_y(sn[4] - '0');
        path({Pt{L1_X1, m}, Pt{L2_X0, m}});
        return;
    }
    // A first-level LUT2 input: straight in, in the source's row (each input has its own row)
    if (lut_input(dn)) {
        path({S, Pt{L1_X0, S.y}});
        return;
    }
    // The 2:1 muxes on the left: from the input in the same row, or from a carry line up its lane
    if (starts_with(dn, "TI") || dn == "CLK_int" || dn == "EN_int") {
        for (const auto &lane : carry_lanes) {
            if (sn != lane.name)
                continue;
            float in_y = D.y - 0.006f;
            std::vector<Pt> pts{S};
            if (lane.track_y > 0) {
                pts.push_back(Pt{S.x, lane.track_y});
                pts.push_back(Pt{lane.x, lane.track_y});
            } else {
                pts.push_back(Pt{lane.x, S.y});
            }
            pts.push_back(Pt{lane.x, in_y});
            through_mux(pts, TI_X0, in_y);
            return;
        }
        through_mux({S}, TI_X0, D.y);
        return;
    }
    // The bridge mux: an input down the bridge lane, along below the LUT2s
    if (dn == "MUXOUT_int") {
        through_mux({S, Pt{BRIDGE_LANE, S.y}, Pt{BRIDGE_LANE, BRIDGE_Y}}, BRIDGE_X0, BRIDGE_Y);
        return;
    }
    // A register's data input: from its own half straight in, otherwise up beside the registers
    if (dn == "DIN1_int" || dn == "DIN2_int") {
        if (std::abs(S.y - D.y) < 1e-4f)
            path({S, D});
        else
            path({S, Pt{D_JOG, S.y}, Pt{D_JOG, D.y}, D});
        return;
    }
    // The output muxes: the register straight in, the LUT2 tree from above (around the register),
    // the carry output and the bridge from below
    if (dn == "OUT1_int" || dn == "OUT2_int") {
        float m = D.y;
        if (starts_with(sn, "DOUT"))
            through_mux({S}, OMUX_X0, m + OMUX_DOUT);
        else if (starts_with(sn, "COMBOUT"))
            through_mux({S, Pt{BYPASS_X, S.y}, Pt{BYPASS_X, m + BYPASS_DY}, Pt{BYPASS_JOG, m + BYPASS_DY},
                         Pt{BYPASS_JOG, m + OMUX_COMB}},
                        OMUX_X0, m + OMUX_COMB);
        else if (starts_with(sn, "CPOUT"))
            through_mux({S, Pt{CPOUT_JOG, S.y}, Pt{CPOUT_JOG, m + OMUX_CPOUT}}, OMUX_X0, m + OMUX_CPOUT);
        else
            through_mux({S, Pt{MUXOUT_JOG, S.y}, Pt{MUXOUT_JOG, m + OMUX_MUXOUT}}, OMUX_X0, m + OMUX_MUXOUT);
        return;
    }
    // L20's upper input: the upper half's result, down beside the lower LUT2s
    if (dn == "COMBIN_int") {
        path({S, Pt{COMBIN_X, S.y}, Pt{COMBIN_X, D.y}, D});
        return;
    }
    // The comparator's inputs: down beside the LUT2s to below them, and across to it
    if (dn == "COMBIN2_int" || dn == "COMBIN1_int") {
        float down = dn == "COMBIN2_int" ? COMBIN_X : BYPASS_X;
        path({S, Pt{down, S.y}, Pt{down, D.y}, D});
        return;
    }
    // The carry logic (see CL_RAIL_OUT2). OUT1, OUT2 and the comparator into it: straight to their
    // rails
    if (dn == "OUT1_IN_int" || dn == "OUT2_IN_int" || dn == "COMPOUT_IN_int") {
        path({S, D});
        return;
    }
    // Into the first muxes of a row: down or up the input's rail (CINX and PINX along their band to
    // it first, CIY12 and PIY12 from their muxes) and across to the mux input
    if (ends_with(dn, "_OUT") || ends_with(dn, "_OUT2")) {
        bool upper = ends_with(dn, "_OUT2") ? sn == "COMPOUT_IN_int" : sn == "OUT1_IN_int";
        float in_y = D.y + (upper ? CL_IN_DY : -CL_IN_DY);
        std::vector<Pt> pts{S};
        if (sn == "CINX" || sn == "PINX" || sn == "CIY12" || sn == "PIY12") {
            float rail = sn == "CINX"    ? CL_RAIL_CINX
                         : sn == "PINX"  ? CL_RAIL_PINX
                         : sn == "CIY12" ? CL_RAIL_CIY12
                                         : CL_RAIL_PIY12;
            pts.push_back(Pt{rail, S.y});
            pts.push_back(Pt{rail, in_y});
        } else {
            pts.push_back(Pt{S.x, in_y});
        }
        through_mux(pts, CL_SEL_X0, in_y);
        return;
    }
    // From a first mux straight into the row's _VAL mux, whose inputs are at their heights
    if (ends_with(dn, "_VAL")) {
        through_mux({S}, CL_VAL_X0, S.y);
        return;
    }
    // The C_Y12 muxes: up the Y line from its start, and along to the mux input
    if (dn == "CIY12" || dn == "PIY12") {
        bool upper = sn == "CINY1" || sn == "PINY1";
        float in_y = D.y + (upper ? CL_IN_DY : -CL_IN_DY);
        through_mux({S, Pt{S.x, in_y}}, dn == "CIY12" ? CIY12_X0 : PIY12_X0, in_y);
        return;
    }
    // A Y line's carry logic out to its line, up beside it (left) into the 2:1 mux where it leaves,
    // and across it
    if (ends_with(sn, "_VAL") && (starts_with(dn, "COUTY") || starts_with(dn, "POUTY"))) {
        float up = D.x - 0.0035f;
        path({S, Pt{up, S.y}, Pt{up, D.y - CMUX_IN_DY}, D});
        return;
    }
    // An X line's: along to the right, down to just above its band and into the 2:1 mux there
    if (ends_with(sn, "_VAL") && (dn == "COUTX" || dn == "POUTX")) {
        float x0 = dn == "COUTX" ? COUTX_X0 : POUTX_X0, in_y = D.y + XMUX_IN_DY;
        float down = dn == "COUTX" ? CL_X_DOWN_CX : CL_X_DOWN_PX;
        path({S, Pt{down, S.y}, Pt{down, in_y}, Pt{x0, in_y}, D});
        return;
    }
    // The carry logic: along the band below the CPE, and up and down the carry column
    bool sc = S.x >= CY1 - 0.01f, dc = D.x >= CY1 - 0.01f;
    bool sb = S.y < CARRY_IN_Y - 0.001f, db = D.y < CARRY_IN_Y - 0.001f;
    if (sb && (dc || db)) {
        path({S, Pt{D.x, S.y}, D});
        return;
    }
    if ((sc || sb) && (dc || db)) {
        path({S, Pt{S.x, D.y}, D});
        return;
    }
    if (sc != dc) {
        // from the carry column horizontally, or vertically to it
        path({S, sc ? Pt{D.x, S.y} : Pt{S.x, D.y}, D});
        return;
    }
    if (std::abs(S.y - D.y) < 1e-4f) {
        path({S, D});
        return;
    }
    float turn = S.x < D.x ? D.x - 0.006f : D.x + 0.006f;
    path({S, Pt{turn, S.y}, Pt{turn, D.y}, D});
}

// The output of a first-stage mux of a switch box (Y1..Y4, YDIAG), where its pips end: in a small
// switch box the *_int wire, which drives Y1..Y4 or YDIAG on the right edge. A big switch box has
// no such stage, its Y1..Y4 and YDIAG are the mux outputs themselves, placed where a small one's
// *_int wires are (see wire_anchor); its output drivers (SB_DRIVE) are on the right edge.
static bool sb_mux_output(const PlacedAnchor &pa, Pt &p)
{
    const std::string &n = *pa.name;
    bool big = starts_with(n, "SB_BIG.");
    if (!big && !starts_with(n, "SB_SML."))
        return false;
    std::string port = n.substr(n.find('.', 7) + 1);
    if (port.empty() || port[0] != 'Y' || (!big && !ends_with(port, "_int")))
        return false;
    p = pa.pos();
    return true;
}

// A pip inside a switch box, input mux or output mux: across the mux, from the input it selects
// to the output. In a switch box, the first stage (Y1_int..Y4_int, YDIAG_int) feeds the output
// drivers on the right edge, and the diagonal mux and the others feed each other beside them.
static void mux_internal(std::vector<GraphicElement> &g, GraphicElement::style_t style, const PlacedAnchor &s,
                         const PlacedAnchor &d)
{
    Pt S = s.pos(), D = d.pos();
    if (sb_mux_output(d, D)) {
        if (sb_mux_output(s, S)) {
            float x = s.tx, r = s.ty + row(s.a.plane);
            if (s.name->find("YDIAG") != std::string::npos)
                // The diagonal mux into a Y mux: up the bus beside them, and across the Y mux
                polyline(g, style, {S, Pt{x + SB_BUS_X, S.y}, Pt{x + SB_BUS_X, D.y - 0.002f}, D});
            else
                // A Y mux back into the diagonal mux: round the bottom of the box, into the diagonal
                // mux's input side and across it
                polyline(g, style,
                         {S, Pt{x + SB_BACK_X, S.y}, Pt{x + SB_BACK_X, r + SB_BACK_Y},
                          Pt{x + SB_BACK_IN_X, r + SB_BACK_Y}, Pt{x + SB_BACK_IN_X, r + SB_BACK_IN_Y},
                          Pt{x + SB_DIAG_X0, r + SB_BACK_IN_Y}, D});
            return;
        }
    }
    line(g, style, S.x, S.y, D.x, D.y);
}

// Between the routing muxes of a tile (switch boxes, input and output muxes of different planes:
// IM5.Y to IM1.D6, a switch box's diagonal output to another plane's, an output mux to its switch
// box): along the gap below the source's row (an output mux sits in that gap), not down to the
// channel and back
static void within_tile(std::vector<GraphicElement> &g, GraphicElement::style_t style, const PlacedAnchor &s,
                        const PlacedAnchor &d)
{
    Pt S = s.pos(), D = d.pos();
    // (the output mux of the plane sits in the middle of the gap: switch boxes pass above it,
    // input muxes below it)
    bool om = s.a.entity >= E_OM && s.a.entity <= E_OM + 12, im = s.a.entity >= E_IM && s.a.entity <= E_IM + 12;
    float gap = om ? S.y : s.ty + row(s.a.plane) - 0.032f + (im ? -0.004f : 0.004f);
    float out_lane = s.lane(), in_lane = d.lane();
    if (om)
        polyline(g, style, {S, Pt{in_lane, S.y}, Pt{in_lane, D.y}, D});
    else
        polyline(g, style, {S, Pt{out_lane, S.y}, Pt{out_lane, gap}, Pt{in_lane, gap}, Pt{in_lane, D.y}, D});
}

// Inside an IO tile (and from the IO cell to the edge switch of the tile above): the pad and the
// IO cell side by side; the IO cell's inputs across to their bus; into an edge switch mux from the
// bus or the switch box's return at the mux input's own height
static void io_internal(std::vector<GraphicElement> &g, GraphicElement::style_t style, const PlacedAnchor &s,
                        const PlacedAnchor &d)
{
    Pt S = s.pos(), D = d.pos();
    const std::string &sn = *s.name, &dn = *d.name;
    if (starts_with(dn, "IOES.SB_IN")) {
        float in_y = D.y + (ends_with(sn, "IO_IN1") ? 0.006f : (ends_with(sn, "IO_IN2") ? 0 : -0.006f));
        polyline(g, style, {S, Pt{S.x, in_y}, Pt{d.tx + IOES_X0, in_y}, D});
        return;
    }
    polyline(g, style, {S, Pt{D.x, S.y}, D});
}

// The tracks of a signal in the channels: (channel, default offset) -> offset in the channel. A
// channel is a tile row's bottom channel (row * 2) or a tile column's left channel (column * 2 + 1).
using TrackFn = std::function<float(int, float)>;
static int channel(bool vertical, int coordinate) { return coordinate * 2 + (vertical ? 1 : 0); }

// Draw the connection from wire s to wire d
static void connect(std::vector<GraphicElement> &g, GraphicElement::style_t style, const PlacedAnchor &s,
                    const PlacedAnchor &d, const TrackFn &tracks = nullptr)
{
    Pt S = s.pos(), D = d.pos();
    bool same_tile = s.tile == d.tile;
    size_t from = g.size();
    auto cpe_part = [](int e) { return e == E_CPE || e == E_CARRY; };
    if (same_tile && cpe_part(s.a.entity) && cpe_part(d.a.entity)) {
        cpe_internal(g, style, s, d);
        lod(g, from, LOD_DETAIL);
        return;
    }
    // The edge logic: horizontally into a mux and across it; to and from the carry lines along them
    if (same_tile && s.a.entity == E_EDGE && d.a.entity == E_EDGE) {
        polyline(g, style, {S, Pt{D.x + (S.x < D.x ? -0.02f : 0.02f), S.y}, D});
        lod(g, from, LOD_ENTITY);
        return;
    }
    if (same_tile && (s.a.entity == E_EDGE || d.a.entity == E_EDGE) &&
        (s.a.entity == E_CARRY || d.a.entity == E_CARRY)) {
        // A carry band (X lines): along it to a column of its own beside the edge logic
        const Pt &c = s.a.entity == E_CARRY ? S : D, &e = s.a.entity == E_CARRY ? D : S;
        if (c.y - std::floor(c.y) < CARRY_IN_Y - 0.001f) {
            float column = e.x + 0.006f * (std::abs(c.y - std::floor(c.y) - PX_BAND) < 0.001f ? 2 : 1);
            polyline(g, style, {S, Pt{column, S.y}, Pt{column, D.y}, D});
        } else {
            polyline(g, style, {S, Pt{S.x, D.y}, D});
        }
        lod(g, from, LOD_ENTITY);
        return;
    }
    auto io_part = [](int e) { return e == E_IO || e == E_IOES; };
    if (io_part(s.a.entity) && io_part(d.a.entity) && s.tx == d.tx && std::abs(s.ty - d.ty) <= 1) {
        io_internal(g, style, s, d);
        lod(g, from, same_tile ? LOD_ENTITY : 0);
        return;
    }
    // Within a mux; along the carry lines; from an input mux to the CPE input next to it
    if (same_tile && s.a.entity >= E_SB && s.a.entity == d.a.entity) {
        mux_internal(g, style, s, d);
        lod(g, from, LOD_ENTITY);
        return;
    }
    if (same_tile && s.a.entity != E_OTHER && s.a.entity == d.a.entity) {
        line(g, style, S.x, S.y, D.x, D.y);
        lod(g, from, LOD_ENTITY);
        return;
    }
    auto routing = [](int e) { return e >= E_SB && e <= E_OM + 12; };
    if (same_tile && routing(s.a.entity) && routing(d.a.entity) && d.a.entity < E_OM && s.a.plane > 0) {
        within_tile(g, style, s, d);
        lod(g, from, LOD_TILE);
        return;
    }
    if (s.a.entity == E_CARRY && d.a.entity == E_CARRY) {
        line(g, style, S.x, S.y, D.x, D.y);
        return;
    }
    if (same_tile && s.a.entity > E_IM && s.a.entity <= E_IM + 12 && d.a.entity == E_CPE) {
        line(g, style, S.x, S.y, D.x, D.y);
        lod(g, from, LOD_TILE);
        return;
    }
    // Zoomed out, used routing between tiles is drawn less prominently than the tiles' contents, so
    // that the used CPEs stand out; from the zoom that shows the connections inside the tiles, as
    // used
    auto routing_style = style == GraphicElement::STYLE_ACTIVE && !same_tile ? GraphicElement::STYLE_ROUTING : style;
    if (is_global(S, D)) {
        global_stub(g, routing_style, D);
        lod(g, from, 0, LOD_TILE);
        size_t near = g.size();
        global_stub(g, style, D);
        lod(g, near, LOD_TILE);
        return;
    }
    // Through the channels: down the source's lane to the bottom channel of its row, along it to
    // the destination's lane (same row), or to the left channel of the destination's column and
    // along that to the destination's row; then up the destination's lane.
    int plane = s.a.plane > 0 ? s.a.plane : (d.a.plane > 0 ? d.a.plane : 1);
    float ls = s.lane(), ld = d.lane();
    // The channels of the rows the wires are in (a RAM pin may be in a row above its tile's)
    int s_row = s.ty + int(std::floor(s.a.p.y)), d_row = d.ty + int(std::floor(d.a.p.y));
    auto offset = [&](bool vertical, int coordinate, float def) {
        return tracks ? tracks(channel(vertical, coordinate), def) : def;
    };
    float hs = s_row + offset(false, s_row, track(slot(plane, ld >= ls ? 0 : 1)));
    std::vector<Pt> pts{S, Pt{ls, S.y}, Pt{ls, hs}};
    if (s_row == d_row) {
        pts.push_back(Pt{ld, hs});
    } else {
        float vx = d.tx + offset(true, d.tx, track(slot(plane, d_row > s_row ? 0 : 1)));
        float hd = d_row + offset(false, d_row, track(slot(plane, 0)));
        pts.push_back(Pt{vx, hs});
        pts.push_back(Pt{vx, hd});
        pts.push_back(Pt{ld, hd});
    }
    pts.push_back(Pt{ld, D.y});
    pts.push_back(D);
    polyline(g, style, pts);
    lod(g, from, LOD_TILE);
    if (!same_tile) {
        // Zoomed out, where the channels are too close to tell apart: the shortest such path
        size_t simple = g.size();
        polyline(g, routing_style, {S, Pt{D.x, S.y}, D});
        lod(g, simple, 0, LOD_TILE);
    }
}

// The symbol of a first-stage mux of a switch box (the wire after "SB_xxx.", a *_int), in the
// tile at x, for the plane's row at r
static void sb_mux_symbol(std::vector<GraphicElement> &g, GraphicElement::style_t style, const std::string &rest,
                          float x, float r)
{
    if (rest.find("YDIAG") != std::string::npos) {
        mux(g, style, x + SB_DIAG_X0, x + SB_DIAG_X1, r + SB_DIAG_Y, 0.0065f, 0.002f);
    } else {
        int k = rest[rest.find(".Y") + 2] - '1';
        mux(g, style, x + SB_Y_X0, x + SB_Y_X1, r + 0.016f - k * 0.008f, 0.0035f, 0.0015f);
    }
}

// The symbol of a mux in the CPE whose output is the wire n (after "CPE."), in the tile at (x, y);
// false if n is not a mux output
static bool cpe_mux_symbol(std::vector<GraphicElement> &g, GraphicElement::style_t style, const std::string &n, float x,
                           float y)
{
    float my = y + cpe_anchor(n).p.y;
    if (starts_with(n, "TI") || n == "CLK_int" || n == "EN_int")
        mux(g, style, x + TI_X0, x + TI_X1, my, 0.01f, 0.005f);
    else if (n == "OUT1_int" || n == "OUT2_int")
        mux(g, style, x + OMUX_X0, x + OMUX_X1, my, 0.045f, 0.015f);
    else if (n == "MUXOUT_int")
        mux(g, style, x + BRIDGE_X0, x + BRIDGE_X1, my, 0.011f, 0.005f);
    else if (ends_with(n, "_OUT") || ends_with(n, "_OUT2"))
        // the carry logic's first muxes, and its _VAL muxes (see CL_RAIL_OUT2)
        mux(g, style, x + CL_SEL_X0, x + CL_SEL_X1, my, CL_IN_DY + 0.0015f, 0.0015f);
    else if (ends_with(n, "_VAL"))
        mux(g, style, x + CL_VAL_X0, x + CL_VAL_X1, my, CL_SEL_DY + 0.0015f, 0.002f);
    else if (n == "CIY12" || n == "PIY12")
        // inputs on the right, from the Y lines
        mux(g, style, x + (n == "CIY12" ? CIY12_X0 : PIY12_X0), x + cpe_anchor(n).p.x, my, CL_IN_DY + 0.0015f,
            0.0015f);
    else if (n == "COUTX" || n == "POUTX")
        // where an X line leaves: the line passing through along its band, or its carry logic from above
        mux(g, style, x + (n == "COUTX" ? COUTX_X0 : POUTX_X0), x + cpe_anchor(n).p.x, my, XMUX_IN_DY + 0.0015f,
            0.0015f);
    else if (starts_with(n, "COUTY") || starts_with(n, "POUTY")) {
        // The 2:1 mux where a Y line leaves (C_CY1_I, ...): the line passing through from below, or
        // its carry logic (_VAL, right of it), up into the line; upright and centred on the line
        float c = x + cpe_anchor(n).p.x, in_y = my - CMUX_IN_DY, out_y = my;
        line(g, style, c - CMUX_W, in_y, c + CMUX_W, in_y);
        line(g, style, c - CMUX_PAD, out_y, c + CMUX_PAD, out_y);
        line(g, style, c - CMUX_W, in_y, c - CMUX_PAD, out_y);
        line(g, style, c + CMUX_W, in_y, c + CMUX_PAD, out_y);
    } else
        return false;
    return true;
}

// Replace the horizontal and vertical lines of g that overlap by one line per stretch (the pips of
// a mux share the way to it)
static void merge_lines(std::vector<GraphicElement> &g)
{
    std::map<std::pair<bool, float>, std::vector<std::pair<float, float>>> runs;
    std::vector<GraphicElement> rest;
    for (auto &e : g) {
        bool vertical = e.x1 == e.x2, horizontal = e.y1 == e.y2;
        if (e.type != GraphicElement::TYPE_LINE || vertical == horizontal) {
            rest.push_back(e);
            continue;
        }
        float a = vertical ? e.y1 : e.x1, b = vertical ? e.y2 : e.x2;
        runs[{vertical, vertical ? e.x1 : e.y1}].push_back({std::min(a, b), std::max(a, b)});
    }
    if (g.empty())
        return;
    GraphicElement proto = g.front();
    proto.type = GraphicElement::TYPE_LINE;
    proto.text.clear();
    g = rest;
    for (auto &run : runs) {
        auto &spans = run.second;
        std::sort(spans.begin(), spans.end());
        for (size_t i = 0; i < spans.size();) {
            float a = spans[i].first, b = spans[i].second;
            for (i++; i < spans.size() && spans[i].first <= b; i++)
                b = std::max(b, spans[i].second);
            GraphicElement e = proto;
            float c = run.first.second;
            if (run.first.first)
                e.x1 = e.x2 = c, e.y1 = a, e.y2 = b;
            else
                e.y1 = e.y2 = c, e.x1 = a, e.x2 = b;
            g.push_back(e);
        }
    }
}

// The names of the ports in the CPE schematic
static void cpe_port_labels(std::vector<GraphicElement> &g)
{
    // In the frame colour, lighter than the unused parts' (and not picked)
    auto port = [&](float x1, float y1, float x2, float y2, const std::string &text) {
        label(g, GraphicElement::STYLE_FRAME, x1, y1, x2, y2, text, LOD_PORTS);
    };
    const float h = 0.007f;
    // The pins, left of their row
    for (int i = 1; i <= 11; i++) {
        std::string name = i <= 8 ? "IN" + std::to_string(i) : (i == 9 ? "CLK" : (i == 10 ? "EN" : "SR"));
        port(CPE_PIN - 0.017f, row(i) + 0.001f, CPE_PIN - 0.001f, row(i) + 0.001f + h, name);
    }
    // The 2:1 muxes on the left: their configuration, above them
    const char *ti[] = {"C_I1", "C_I2", "C_I3", "C_I4"};
    for (int k = 0; k < 4; k++) {
        int r = 2 + 2 * k;
        port(TI_X0 - 0.004f, row(r) + 0.011f, TI_X1 + 0.004f, row(r) + 0.016f, ti[k]);
    }
    port(TI_X0 - 0.006f, row(9) + 0.011f, TI_X1 + 0.006f, row(9) + 0.016f, "C_CLKSEL");
    port(TI_X0 - 0.006f, row(10) + 0.011f, TI_X1 + 0.006f, row(10) + 0.016f, "C_ENSEL");
    // The LUT2s: their name at the top, their inputs A and B
    struct
    {
        const char *name;
        float x0, x1, top, a, b;
    } luts[] = {{"L00", L1_X0, L1_X1, row(1) + 0.012f, row(1), row(2)},
                {"L01", L1_X0, L1_X1, row(3) + 0.012f, row(3), row(4)},
                {"L02", L1_X0, L1_X1, row(5) + 0.012f, row(5), row(6)},
                {"L03", L1_X0, L1_X1, row(7) + 0.012f, row(7), row(8)},
                {"L10", L2_X0, L2_X1, m12() + 0.018f, m12(), m34()},
                {"L11", L2_X0, L2_X1, m56() + 0.018f, m56(), m78()},
                {"L20", L20_X0, L20_X1, ml() + 0.06f, ml() + 0.045f, ml()}};
    for (auto &l : luts) {
        port(l.x0 + 0.002f, l.top - 0.008f, l.x1 - 0.002f, l.top - 0.002f, l.name);
        port(l.x0 + 0.001f, l.a - 0.003f, l.x0 + 0.006f, l.a + 0.003f, "A");
        port(l.x0 + 0.001f, l.b - 0.003f, l.x0 + 0.006f, l.b + 0.003f, "B");
    }
    // The registers, their output muxes (inputs: 0 register, 1 bridge, 2 carry output, 3 LUT2
    // tree) and the outputs
    for (bool upper : {true, false}) {
        float m = upper ? mu() : ml();
        port(FF_X0 + 0.0015f, m + FF_D - 0.0035f, FF_X0 + 0.0075f, m + FF_D + 0.0035f, "D");
        port(FF_X0 + 0.0015f, m + FF_EN - 0.0035f, FF_X0 + 0.0105f, m + FF_EN + 0.0035f, "EN");
        port(FF_X0 + 0.0015f, m + FF_SR - 0.0035f, FF_X0 + 0.0105f, m + FF_SR + 0.0035f, "SR");
        port(FF_X1 - 0.0075f, m + FF_D - 0.0035f, FF_X1 - 0.0015f, m + FF_D + 0.0035f, "Q");
        const std::pair<float, const char *> inputs[] = {
                {OMUX_DOUT, "0"}, {OMUX_MUXOUT, "1"}, {OMUX_CPOUT, "2"}, {OMUX_COMB, "3"}};
        for (auto &in : inputs)
            port(OMUX_X0 + 0.001f, m + in.first + 0.001f, OMUX_X0 + 0.005f, m + in.first + 0.0065f, in.second);
        port(OMUX_X0, m + 0.047f, OMUX_X1, m + 0.054f, upper ? "C_O2" : "C_O1");
        port(OUT_X + 0.001f, m + 0.001f, OUT_X + 0.016f, m + 0.001f + h, upper ? "OUT2" : "OUT1");
    }
    port(BRIDGE_X0 - 0.004f, BRIDGE_Y + 0.012f, BRIDGE_X1 + 0.004f, BRIDGE_Y + 0.018f, "C_SN");
    // The carry and propagate pins: X on their band, left and right; Y right of their line, at the
    // bottom and top
    for (auto &band : {std::make_pair(CX_BAND, "X"), std::make_pair(PX_BAND, "X")}) {
        std::string c = band.first == CX_BAND ? "C" : "P";
        port(CPE_PIN - 0.022f, band.first + 0.001f, CPE_PIN - 0.001f, band.first + 0.0065f, c + "IN" + band.second);
        port(0.975f, band.first + 0.001f, 0.999f, band.first + 0.0065f, c + "OUT" + band.second);
    }
    for (auto &col : {std::make_pair(CY1, "CINY1"), std::make_pair(PY1, "PINY1"), std::make_pair(CY2, "CINY2"),
                      std::make_pair(PY2, "PINY2")}) {
        std::string in = col.second, out = in.substr(0, 1) + "OUT" + in.substr(3);
        port(col.first + 0.0008f, CARRY_IN_Y - 0.0095f, col.first + 0.0128f, CARRY_IN_Y - 0.0015f, in);
        port(col.first + 0.0008f, CARRY_OUT_Y - 0.0095f, col.first + 0.0128f, CARRY_OUT_Y - 0.0015f, out);
    }
}

// Where the routing input tile wire `index` of a tile comes from, in short: the kind of source (SB,
// IM, OM, CPE, IO) and its direction and distance in tiles (W2: two tiles west), or its plane in the
// same tile; for a CPE output, which one
static std::string source_of(const Context *ctx, int tile, int index)
{
    const auto &uphill = chip_wire_info(ctx->chip_info, WireId(tile, index)).pips_uphill;
    if (uphill.ssize() != 1)
        return "";
    int src = chip_pip_info(ctx->chip_info, PipId(tile, uphill[0])).src_wire;
    WireId from(tile, src);
    if (ends_with(tile_wire_name(ctx, tile, src), "_n")) {
        from = node_driver(ctx, ctx->normalise_wire(tile, src), tile, src);
        if (from == WireId() || from == WireId(tile, src))
            return "";
    }
    const std::string &name = tile_wire_name(ctx, from.tile, from.index);
    std::string kind = name.substr(0, name.find('.'));
    if (starts_with(kind, "SB_"))
        kind = "SB";
    else if (kind == "IOES" || kind == "IOSEL" || kind == "GPIO")
        kind = "IO";
    int x0, y0, x1, y1;
    tile_xy(ctx->chip_info, tile, x0, y0);
    tile_xy(ctx->chip_info, from.tile, x1, y1);
    int dx = x1 - x0, dy = y1 - y0;
    std::string where;
    if (dx == 0 && dy == 0) {
        size_t p = name.find(".P");
        if (p != std::string::npos)
            where = name.substr(p + 1, 3);
    } else {
        where = std::string(dy > 0 ? "N" : (dy < 0 ? "S" : "")) + (dx > 0 ? "E" : (dx < 0 ? "W" : ""));
        int distance = std::max(std::abs(dx), std::abs(dy));
        if (distance > 1)
            where += std::to_string(distance);
    }
    if (kind == "CPE" && starts_with(name, "CPE.OUT"))
        where += where.empty() ? name.substr(7, 1) : " " + name.substr(7, 1);
    return where.empty() ? kind : kind + " " + where;
}

// Where the outputs of a switch box go: Y1 east, Y2 north, Y3 west, Y4 south, YDIAG diagonally
static const char *sb_output_label(const std::string &out)
{
    if (out == "Y1")
        return "Y1 E";
    if (out == "Y2")
        return "Y2 N";
    if (out == "Y3")
        return "Y3 W";
    if (out == "Y4")
        return "Y4 S";
    return "YD";
}

// The schematic of a tile type's CPE and routing muxes, in dark grey under what is used: every pip
// in them (with the area it covers, for selecting it), every mux, the names of the ports, and
// where each routing input comes from. Shown only when zoomed in, for the tiles in view.
struct TileSchematic
{
    std::vector<GraphicElement> graphics; // in the tile
    std::vector<std::pair<int, std::array<float, 4>>> pips;
};

static const TileSchematic &tile_schematic(const Context *ctx, int tile_type)
{
    static std::once_flag once;
    static std::vector<TileSchematic> schematics;
    std::call_once(once, [&]() {
        const ChipInfoPOD *chip = ctx->chip_info;
        const GateMateGfx &data = gfx_data(ctx);
        // For where the routing inputs come from: a tile of each type near the middle of the chip
        std::vector<int> example(chip->tile_types.ssize(), -1);
        std::vector<int> example_distance(chip->tile_types.ssize(), 1 << 30);
        for (int t = 0; t < chip->tile_insts.ssize(); t++) {
            int x, y;
            tile_xy(chip, t, x, y);
            int type = tile_type_of(ctx, t);
            int distance = std::abs(x - ctx->getGridDimX() / 2) + std::abs(y - ctx->getGridDimY() / 2);
            if (distance < example_distance[type])
                example[type] = t, example_distance[type] = distance;
        }
        auto drawn = [](int e) {
            return e == E_CPE || e == E_CARRY || e == E_IO || e == E_IOES || e == E_EDGE ||
                   (e >= E_SB && e <= E_OM + 12);
        };
        schematics.resize(chip->tile_types.ssize());
        for (int t = 0; t < chip->tile_types.ssize(); t++) {
            const auto &tt = chip->tile_types[t];
            TileSchematic &sch = schematics[t];
            auto anchor = [&](int w) {
                PlacedAnchor pa;
                pa.a = data.anchors[t][w];
                pa.name = &data.names[t][w];
                pa.tile = -1;
                pa.tx = pa.ty = 0;
                return pa;
            };
            for (int i = 0; i < tt.pips.ssize(); i++) {
                int src = tt.pips[i].src_wire, dst = tt.pips[i].dst_wire;
                if (ends_with(data.names[t][src], "_n") || ends_with(data.names[t][dst], "_n") ||
                    !drawn(data.anchors[t][src].entity) || !drawn(data.anchors[t][dst].entity))
                    continue;
                std::vector<GraphicElement> g;
                connect(g, GraphicElement::STYLE_INACTIVE, anchor(src), anchor(dst));
                if (g.empty())
                    continue;
                std::array<float, 4> bb{1e9f, 1e9f, -1e9f, -1e9f};
                for (auto &e : g) {
                    bb[0] = std::min({bb[0], e.x1, e.x2}), bb[1] = std::min({bb[1], e.y1, e.y2});
                    bb[2] = std::max({bb[2], e.x1, e.x2}), bb[3] = std::max({bb[3], e.y1, e.y2});
                }
                sch.pips.push_back({i, bb});
                for (auto &e : g)
                    sch.graphics.push_back(e);
            }
            if (sch.pips.empty())
                continue;
            // The muxes, and the names and sources of the routing ports
            std::vector<GraphicElement> labels;
            auto port = [&](float x1, float y1, float x2, float y2, const std::string &text) {
                label(labels, GraphicElement::STYLE_FRAME, x1, y1, x2, y2, text, LOD_PORTS);
            };
            pool<int> sb_planes;
            bool cpe = false, edge = false;
            for (int w = 0; w < tt.wires.ssize(); w++) {
                const std::string &name = data.names[t][w];
                if (ends_with(name, "_n"))
                    continue;
                size_t dot = name.find('.');
                std::string kind = name.substr(0, dot), rest = name.substr(dot + 1);
                const Pt &p = data.anchors[t][w].p;
                std::string from = example[t] >= 0 ? source_of(ctx, example[t], w) : "";
                if (kind == "CPE") {
                    cpe = true;
                    cpe_mux_symbol(sch.graphics, GraphicElement::STYLE_INACTIVE, rest, 0, 0);
                } else if (kind == "IM") {
                    std::string port_name = rest.substr(rest.find('.') + 1);
                    if (port_name == "Y")
                        mux(sch.graphics, GraphicElement::STYLE_INACTIVE, IM_X0, IM_X1, p.y, 0.026f, 0.01f);
                    else
                        port(IM_X0 - 0.029f, p.y - 0.0025f, IM_X0 - 0.001f, p.y + 0.0025f, port_name + " " + from);
                } else if (kind == "OM") {
                    if (ends_with(rest, ".Y"))
                        mux(sch.graphics, GraphicElement::STYLE_INACTIVE, OM_X1, OM_X0, p.y, 0.014f, 0.006f);
                    else
                        port(OM_X1 + 0.0005f, p.y - 0.002f, OM_X1 + 0.0175f, p.y + 0.002f, from);
                } else if (kind == "SB_BIG" || kind == "SB_SML") {
                    int plane = num(rest, 1);
                    std::string port_name = rest.substr(rest.find('.') + 1);
                    if (!sb_planes.count(plane)) {
                        sb_planes.insert(plane);
                        box(sch.graphics, GraphicElement::STYLE_INACTIVE, SB_X0, row(plane) - SB_H, SB_X1,
                            row(plane) + SB_H);
                        port(SB_X1 + 0.0015f, row(plane) + SB_H - 0.006f, SB_X1 + 0.03f, row(plane) + SB_H,
                             "SB P" + std::string(plane < 10 ? "0" : "") + std::to_string(plane));
                    }
                    Pt m;
                    if (sb_mux_output(anchor(w), m)) {
                        sb_mux_symbol(sch.graphics, GraphicElement::STYLE_INACTIVE, rest, 0, row(plane));
                        if (kind == "SB_BIG") {
                            // (on to the right edge, which a small switch box's Y*_int -> Y* pip draws)
                            Pt e = sb_anchor(plane, port_name).p;
                            line(sch.graphics, GraphicElement::STYLE_INACTIVE, m.x, m.y, e.x, e.y);
                        }
                    }
                    if (port_name[0] == 'Y' && !ends_with(port_name, "_int"))
                        port(SB_X1 - 0.0135f, p.y + 0.0004f, SB_X1 - 0.0005f, p.y + 0.0038f,
                             sb_output_label(port_name));
                    else if (port_name[0] == 'X')
                        port(SB_X0 + 0.0003f, p.y - 0.0006f, SB_DIAG_X0 - 0.0003f, p.y + 0.0006f,
                             port_name + " " + from);
                    else if (!ends_with(port_name, "_IO") && !ends_with(port_name, "_D2D") &&
                             !ends_with(port_name, "_CLK"))
                        port(SB_X0 + 0.001f, p.y - 0.00055f, SB_X0 + 0.028f, p.y + 0.00055f, port_name + " " + from);
                } else if (kind == "IOES" && starts_with(rest, "SB_IN") && num(rest, 6) == 1) {
                    port(IOES_X0 - 0.01f, p.y + 0.011f, IOES_X1 + 0.01f, p.y + 0.019f, "IOES");
                }
                // The RAM's pins along its bar, and the edge logic's wires, by name
                if (kind == "RAM")
                    port(1 + RAM_X0 + 0.005f, p.y - 0.004f, 1 + RAM_X0 + 0.15f, p.y + 0.004f, rest);
                if (data.anchors[t][w].entity == E_EDGE) {
                    std::string short_name = rest.substr(0, rest.size() - (ends_with(rest, "_int") ? 4 : 0));
                    if (p.x > EDGE_IN_X + 0.01f)
                        port(EDGE_MUX_X + 0.002f, p.y - 0.0025f, EDGE_MUX_X + 0.1f, p.y + 0.0025f, short_name);
                    else
                        port(EDGE_IN_X - 0.1f, p.y - 0.0025f, EDGE_IN_X - 0.002f, p.y + 0.0025f, short_name);
                }
                if (data.anchors[t][w].entity == E_EDGE && !edge) {
                    // The edge logic, by the edge it is on (LES: left, RES: right, BES, TES)
                    edge = true;
                    bool io = data.anchors[t][w].p.y < 0.48f &&
                              IdString(tt.type_name).str(ctx).find("GPIO") != std::string::npos;
                    port(EDGE_IN_X, io ? 0.473f : 0.953f, EDGE_MUX_X, io ? 0.49f : 0.97f, kind + " logic");
                }
                if (kind == "IOES" && starts_with(rest, "SB_IN")) {
                    mux(sch.graphics, GraphicElement::STYLE_INACTIVE, IOES_X0, IOES_X1, p.y, 0.009f, 0.004f);
                    const std::pair<float, const char *> inputs[] = {{0.006f, "IN1"}, {0, "IN2"}, {-0.006f, "ALT"}};
                    for (auto &in : inputs)
                        port(IOES_X0 - 0.014f, p.y + in.first - 0.0025f, IOES_X0 - 0.001f, p.y + in.first + 0.0025f,
                             in.second);
                }
            }
            merge_lines(sch.graphics);
            lod(sch.graphics, 0, LOD_DETAIL);
            if (cpe)
                cpe_port_labels(labels);
            for (auto &l : labels)
                sch.graphics.push_back(l);
        }
    });
    return schematics.at(tile_type);
}

std::vector<std::pair<DecalXY, BoundingBox>> GateMateImpl::getDetailDecals() const
{
    static std::once_flag once;
    static std::vector<std::pair<DecalXY, BoundingBox>> decals;
    std::call_once(once, [&]() {
        for (int t = 0; t < ctx->chip_info->tile_insts.ssize(); t++) {
            const TileSchematic &sch = tile_schematic(ctx, tile_type_of(ctx, t));
            if (sch.pips.empty())
                continue;
            // (a RAM's pins reach into the RAM column, and up to 16 tiles away)
            float x1 = 1, y0 = 0, y1 = 1;
            for (auto &e : sch.graphics)
                x1 = std::max({x1, e.x1, e.x2}), y0 = std::min({y0, e.y1, e.y2}), y1 = std::max({y1, e.y1, e.y2});
            int x, y;
            tile_xy(ctx->chip_info, t, x, y);
            DecalXY d;
            d.decal = DecalId(t, 0, DecalId::TYPE_DETAIL);
            decals.push_back({d, BoundingBox(x, y + int(std::floor(y0)), x + int(std::ceil(x1)) - 1,
                                             y + int(std::ceil(y1)) - 1)});
        }
    });
    return decals;
}

void GateMateImpl::drawDetail(std::vector<GraphicElement> &g, DecalId decal)
{
    int x, y;
    tile_xy(ctx->chip_info, decal.tile, x, y);
    x = gui_column(ctx, x);
    for (GraphicElement e : tile_schematic(ctx, tile_type_of(ctx, decal.tile)).graphics) {
        e.x1 += x, e.x2 += x, e.y1 += y, e.y2 += y;
        g.push_back(e);
    }
}

std::vector<PipId> GateMateImpl::getDetailPips(DecalId decal, float x, float y) const
{
    int tx, ty;
    tile_xy(ctx->chip_info, decal.tile, tx, ty);
    tx = gui_column(ctx, tx);
    float lx = x - tx, ly = y - ty, margin = 0.004f;
    std::vector<PipId> pips;
    for (auto &p : tile_schematic(ctx, tile_type_of(ctx, decal.tile)).pips) {
        const auto &bb = p.second;
        if (lx >= bb[0] - margin && lx <= bb[2] + margin && ly >= bb[1] - margin && ly <= bb[3] + margin)
            pips.push_back(PipId(decal.tile, p.first));
    }
    return pips;
}

// The connections in a CPE that its configuration makes rather than its routing: where a cell's
// pin is a carry or propagate line (PINY1 into a LUT2 through its 2:1 mux), or the cell takes the
// whole CPE (IN1..IN8 as its pins), or the carry logic (CPE_CPLINES) sends the outputs on along the
// carry and propagate lines. These muxes are set by the cells' parameters (C_I1, C_SELY1, C_CY1_I,
// ...; a first-level LUT2's inputs are not swapped): their selected pips that are not routed and
// lie on a way from a used wire to another or to a pin of a used bel are drawn, as used.
void GateMateImpl::configured_cpe_paths(std::vector<GraphicElement> &g, int tile, float x, float y) const
{
    // The parameters of the cells in the CPE, and the wires of their bels' pins
    dict<IdString, int> params;
    pool<int> live;
    const auto &tdata = chip_tile_info(ctx->chip_info, tile);
    for (int b = 0; b < tdata.bels.ssize(); b++) {
        BelId bel(tile, b);
        const CellInfo *cell = ctx->getBoundBelCell(bel);
        if (cell == nullptr || !starts_with(ctx->getBelType(bel).str(ctx), "CPE_"))
            continue;
        // (the carry logic's muxes are 0 where its cell does not set them, as in the bitstream)
        if (cell->type == id_CPE_CPLINES)
            for (IdString p : {id_C_SELX, id_C_SELY1, id_C_SELY2, id_C_SEL_C, id_C_SEL_P, id_C_Y12, id_C_CX_I,
                               id_C_CY1_I, id_C_CY2_I, id_C_PX_I, id_C_PY1_I, id_C_PY2_I})
                if (!params.count(p))
                    params[p] = 0;
        for (auto &p : cell->params)
            params[p.first] = int_or_default(cell->params, p.first, 0);
        for (IdString pin : ctx->getBelPins(bel)) {
            WireId w = ctx->getBelPinWire(bel, pin);
            if (w.tile == tile)
                live.insert(w.index);
        }
    }
    if (params.empty())
        return;
    // The configurable pips of the CPE in tiles of this type: (pip, parameter, value), the parameter
    // empty for the first-level LUT2s' inputs (not swapped: value 0)
    struct ConfigPip
    {
        int pip;
        IdString param;
        int value;
    };
    static std::mutex cache_mutex;
    static std::map<int, std::vector<ConfigPip>> cache;
    std::vector<ConfigPip> candidates;
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        int type = tile_type_of(ctx, tile);
        auto it = cache.find(type);
        if (it == cache.end()) {
            std::vector<ConfigPip> found;
            for (int i = 0; i < tdata.pips.ssize(); i++) {
                const auto &extra = *pip_extra_data(PipId(tile, i));
                if (extra.type != PipExtra::PIP_EXTRA_MUX || extra.bits == 0 || extra.name == 0)
                    continue;
                const std::string &src = tile_wire_name(ctx, tile, tdata.pips[i].src_wire);
                const std::string &dst = tile_wire_name(ctx, tile, tdata.pips[i].dst_wire);
                if (!starts_with(src, "CPE.") || !starts_with(dst, "CPE.") || ends_with(src, "_n"))
                    continue;
                std::string name = IdString(extra.name).str(ctx);
                if (starts_with(name, "LUT2_0"))
                    found.push_back({i, IdString(), extra.value});
                else if (!starts_with(name, "LUT2_"))
                    found.push_back({i, ctx->id(starts_with(name, "CPE.") ? name.substr(4) : name), extra.value});
            }
            it = cache.emplace(type, std::move(found)).first;
        }
        candidates = it->second;
    }
    // The selected pips, not routed
    std::map<int, std::vector<int>> from; // source wire -> selected pips
    for (auto &c : candidates) {
        PipId pip(tile, c.pip);
        if (ctx->getBoundPipNet(pip) != nullptr)
            continue;
        if (c.param == IdString() ? c.value != 0 : (!params.count(c.param) || params.at(c.param) != c.value))
            continue;
        from[tdata.pips[c.pip].src_wire].push_back(c.pip);
    }
    // Ways start at a used wire, and end at one or at a pin of a used bel
    auto is_used = [&](int w) { return ctx->getBoundWireNet(ctx->normalise_wire(tile, w)) != nullptr; };
    auto is_live = [&](int w) { return live.count(w) || is_used(w); };
    // The wires from which a live wire is reached through selected pips (the muxes form no loops)
    dict<int, bool> reaches;
    std::function<bool(int)> leads_on = [&](int w) {
        auto it = reaches.find(w);
        if (it != reaches.end())
            return it->second;
        reaches[w] = false;
        bool r = false;
        auto out = from.find(w);
        if (out != from.end())
            for (int p : out->second) {
                int d = tdata.pips[p].dst_wire;
                r |= is_live(d) || leads_on(d);
            }
        reaches[w] = r;
        return r;
    };
    // From each live wire, along the selected pips that lead on to a live wire
    pool<int> drawn, visited;
    std::function<void(int)> walk = [&](int w) {
        if (!visited.insert(w).second)
            return;
        auto out = from.find(w);
        if (out == from.end())
            return;
        for (int p : out->second) {
            int d = tdata.pips[p].dst_wire;
            if (!is_live(d) && !leads_on(d))
                continue;
            drawn.insert(p);
            if (!is_live(d))
                walk(d);
        }
    };
    for (auto &f : from)
        if (is_used(f.first))
            walk(f.first);
    for (int p : drawn) {
        int s = tdata.pips[p].src_wire, d = tdata.pips[p].dst_wire;
        connect(g, GraphicElement::STYLE_ACTIVE, placed_routed(tile, s), placed_routed(tile, d));
        size_t first = g.size();
        if (cpe_mux_symbol(g, GraphicElement::STYLE_ACTIVE, tile_wire_name(ctx, tile, d).substr(4), x, y))
            lod(g, first, LOD_DETAIL);
    }
}

// The outputs of a CPE used as a multiplier cell are the carry and propagate lines, driven by its
// MULT mode, which the routing graph does not describe: the used ones are only named, by what the
// multiplier sends on them (see pack_mult.cc), the Y lines at the top of their columns (with a faint
// line from the column), the X lines at the right edge, just above their band.
void GateMateImpl::mult_cpe_outputs(std::vector<GraphicElement> &g, GraphicElement::style_t style, BelId bel,
                                    float x, float y) const
{
    const CellInfo *cell = ctx->getBoundBelCell(bel);
    if (cell == nullptr || int_or_default(cell->params, id_C_FUNCTION, 0) != C_MULT)
        return;
    int tile = bel.tile;
    auto used = [&](const char *name) {
        int index = tile_wire_index(ctx, tile, std::string("CPE.") + name);
        return index >= 0 && ctx->getBoundWireNet(ctx->normalise_wire(tile, index)) != nullptr;
    };
    // (left of the lines, which are at the right edge)
    constexpr float TEXT_X0 = 0.88f, TEXT_X1 = 0.936f, TEXT_H = 0.008f;
    struct
    {
        const char *name;
        float column;
        std::string text;
    } y_lines[] = {
            {"COUTY1", CY1, "COUTY1 carry"},
            {"POUTY1", PY1, int_or_default(cell->params, id_C_PY1_I, 0) ? "POUTY1" : "POUTY1 = PINY1"},
            {"COUTY2", CY2, "COUTY2 carry"},
            {"POUTY2", PY2, int_or_default(cell->params, id_C_PY2_I, 0) ? "POUTY2" : "POUTY2 = PINY2"}};
    // (below the names of the lines at the top and the muxes where they leave)
    float ty = CARRY_OUT_Y - 0.032f;
    for (auto &l : y_lines) {
        if (!used(l.name))
            continue;
        line(g, GraphicElement::STYLE_FRAME, x + l.column, y + ty, x + TEXT_X1, y + ty);
        label(g, style, x + TEXT_X0, y + ty - TEXT_H / 2, x + TEXT_X1, y + ty + TEXT_H / 2, l.text, LOD_PORTS);
        ty -= TEXT_H + 0.002f;
    }
    // (between the 2:1 mux where the line leaves and its name at the right edge)
    const std::pair<const char *, float> x_lines[] = {{"COUTX", CX_BAND}, {"POUTX", PX_BAND}};
    for (auto &l : x_lines)
        if (used(l.first))
            label(g, style, x + COUTX_X1 + 0.002f, y + l.second + 0.001f, x + 0.974f, y + l.second + 0.001f + TEXT_H,
                  "sum", LOD_PORTS);
}

void GateMateImpl::drawBel(std::vector<GraphicElement> &g, GraphicElement::style_t style, IdString bel_type, Loc loc)
{
    float x = gui_column(ctx, loc.x), y = loc.y;
    bool used = style == GraphicElement::STYLE_ACTIVE;
    // The configuration of a used bel, as labels (see update_gui_labels)
    std::vector<std::string> labels;
    if (used)
        labels = gui_labels(ctx->getBelByLocation(loc));
    auto text = [&](size_t i) { return i < labels.size() ? labels[i] : std::string(); };
    // A LUT2 with its function, its inputs A and B and its output at the heights a, b and out. One
    // that does nothing (a constant, or passing an input through) is grey: a constant shows its
    // value, one passing an input through a line from that input to the output.
    auto idle = [&](size_t index) {
        std::string f = text(index);
        return used && (f == "0" || f == "1" || f == "A" || f == "B");
    };
    auto lut = [&](float x0, float x1, float y0, float y1, size_t index, float a, float b, float out,
                   float label_y = -1) {
        std::string f = text(index);
        auto s = idle(index) ? GraphicElement::STYLE_INACTIVE : style;
        box(g, s, x + x0, y + y0, x + x1, y + y1);
        if (used && (f == "A" || f == "B")) {
            float in = f == "A" ? a : b, xm = mid(x0, x1);
            polyline(g, style, {Pt{x + x0, y + in}, Pt{x + xm, y + in}, Pt{x + xm, y + out}, Pt{x + x1, y + out}});
            return;
        }
        float ly = label_y < 0 ? mid(y0, y1) : label_y;
        label(g, s, x + x0 + 0.003f, y + ly - 0.012f, x + x1 - 0.003f, y + ly + 0.012f, f);
    };
    // A lower half in a carry-in mode takes the carry input (CINY1, or CINX with C_HORIZ) in place
    // of a LUT2's output (see pack_mult.cc): C_ADDCIN in place of L11's into L20, C_EN_CIN in place
    // of L03's into L11. Which one (5: L11, 3: L03, -1: none), and whether the carry input is used.
    int cin_into = -1;
    bool cin_x = false, cin_used = false;
    if (used && (bel_type == id_CPE_LT_L || bel_type == id_CPE_LT_FULL)) {
        BelId bel = ctx->getBelByLocation(loc);
        if (const CellInfo *cell = ctx->getBoundBelCell(bel)) {
            int function = int_or_default(cell->params, id_C_FUNCTION, 0);
            cin_into = function == C_ADDCIN ? 5 : function == C_EN_CIN ? 3 : -1;
            cin_x = int_or_default(cell->params, id_C_HORIZ, 0) != 0;
            int w = tile_wire_index(ctx, bel.tile, cin_x ? "CPE.CINX" : "CPE.CINY1");
            cin_used = w >= 0 && ctx->getBoundWireNet(ctx->normalise_wire(bel.tile, w)) != nullptr;
        }
    }
    // From the output of a LUT2 to the next one: grey from a constant, or where the carry input
    // takes its place
    auto link = [&](size_t index, float x0, float x1, float h) {
        std::string f = text(index);
        bool idle_link = used && (f == "0" || f == "1" || int(index) == cin_into);
        line(g, idle_link ? GraphicElement::STYLE_INACTIVE : style, x + x0, y + h, x + x1, y + h);
    };
    // The carry input into the LUT2 input it takes: from the start of CINY1 along a track below
    // the CPE (CINX along its band), and up a lane just left of the LUT2
    auto carry_in = [&]() {
        if (cin_into < 0)
            return;
        float to_x = cin_into == 5 ? L20_X0 : L2_X0, to_y = cin_into == 5 ? ml() : m78(), lane = to_x - 0.004f;
        std::vector<Pt> pts;
        if (cin_x) {
            pts = {Pt{CPE_PIN, CX_BAND}, Pt{lane, CX_BAND}};
        } else {
            pts = {Pt{CY1, CARRY_IN_Y}, Pt{CY1, CIN_TRACK_Y}, Pt{lane, CIN_TRACK_Y}};
        }
        pts.push_back(Pt{lane, to_y});
        pts.push_back(Pt{to_x, to_y});
        for (auto &p : pts)
            p = Pt{x + p.x, y + p.y};
        polyline(g, cin_used ? style : GraphicElement::STYLE_INACTIVE, pts);
    };
    // LUT2 L00 (IN1, IN2) and L01 (IN3, IN4) into L10
    auto upper_tree = [&]() {
        size_t from = g.size();
        lut(L1_X0, L1_X1, row(2) - 0.012f, row(1) + 0.012f, 0, row(1), row(2), m12());
        lut(L1_X0, L1_X1, row(4) - 0.012f, row(3) + 0.012f, 1, row(3), row(4), m34());
        lut(L2_X0, L2_X1, m34() - 0.018f, m12() + 0.018f, 4, m12(), m34(), mu());
        link(0, L1_X1, L2_X0, m12());
        link(1, L1_X1, L2_X0, m34());
        lod(g, from, LOD_DETAIL);
    };
    // LUT2 L02 (IN5, IN6) and L03 (IN7, IN8) into L11, and L20 combining L11 with the upper half
    auto lower_tree = [&]() {
        size_t from = g.size();
        lut(L1_X0, L1_X1, row(6) - 0.012f, row(5) + 0.012f, 2, row(5), row(6), m56());
        lut(L1_X0, L1_X1, row(8) - 0.012f, row(7) + 0.012f, 3, row(7), row(8), m78());
        lut(L2_X0, L2_X1, m78() - 0.018f, m56() + 0.018f, 5, m56(), m78(), ml());
        lut(L20_X0, L20_X1, ml() + CPOUT_DY - 0.005f, ml() + 0.06f, 6, ml() + 0.045f, ml(), ml(), ml() + 0.0225f);
        link(2, L1_X1, L2_X0, m56());
        link(3, L1_X1, L2_X0, m78());
        link(5, L2_X1, L20_X0, ml());
        carry_in();
        lod(g, from, LOD_DETAIL);
    };
    // Zoomed out, a used half of the CPE logic is a box in place of its LUT2s
    auto outline = [&](float x0, float x1, float y0, float y1) {
        if (!used)
            return;
        size_t from = g.size();
        box(g, style, x + x0, y + y0, x + x1, y + y1);
        lod(g, from, 0, LOD_DETAIL);
    };
    // What a bel is, in its box: lighter than an unused bel's lines
    auto name = [&](float x0, float y0, float x1, float y1, const std::string &text, float min_scale) {
        label(g, used ? style : GraphicElement::STYLE_FRAME, x + x0, y + y0, x + x1, y + y1, text, min_scale);
    };
    // The name of one of its ports, when zoomed in closely
    auto port_name = [&](float x0, float y0, float x1, float y1, const std::string &text) {
        label(g, GraphicElement::STYLE_FRAME, x + x0, y + y0, x + x1, y + y1, text, LOD_PORTS);
    };
    // A bel shown when used, or when zoomed in
    auto simple = [&](float x0, float y0, float x1, float y1) {
        size_t from = g.size();
        box(g, style, x + x0, y + y0, x + x1, y + y1);
        lod(g, from, used ? 0 : LOD_DETAIL);
    };
    switch (bel_type.index) {
    case id_CPE_LT_FULL.index: {
        // The outline of the CPE logic: shown for unused CPEs as well once zoomed in a bit
        size_t from = g.size();
        box(g, style, x + L1_X0 - 0.006f, y + row(8) - 0.022f, x + L20_X1 + 0.003f, y + row(1) + 0.022f);
        lod(g, from, used ? 0 : LOD_TILE);
        name(L1_X0 - 0.005f, row(1) + 0.013f, L1_X0 + 0.02f, row(1) + 0.021f, "CPE", LOD_TEXT);
        if (used) {
            upper_tree();
            lower_tree();
            mult_cpe_outputs(g, style, ctx->getBelByLocation(loc), x, y);
        }
        // (for every CPE, used as a whole or not: drawn once per CPE, with the decal of this bel)
        configured_cpe_paths(g, ctx->getBelByLocation(loc).tile, x, y);
        break;
    }
    case id_CPE_LT_U.index:
        outline(L1_X0, L2_X1, row(4) - 0.012f, row(1) + 0.012f);
        upper_tree();
        break;
    case id_CPE_LT_L.index:
        outline(L1_X0, L20_X1, row(8) - 0.012f, row(5) + 0.012f);
        lower_tree();
        break;
    case id_CPE_FF_U.index:
    case id_CPE_FF_L.index: {
        // The register, in the middle of its half: data input, clock (with the triangle), enable and
        // set/reset on its left. Clock, enable and set/reset come from the CPE's rows 9 to 11 (clock
        // and enable through their 2:1 muxes), each up its own lane, shared by both registers.
        float m = bel_type == id_CPE_FF_U ? mu() : ml();
        simple(FF_X0, m + FF_BOTTOM, FF_X1, m + FF_TOP);
        size_t from = g.size();
        float x0 = x + FF_X0, ym = y + m;
        line(g, style, x0, ym + FF_CLK - 0.006f, x0 + 0.008f, ym + FF_CLK);
        line(g, style, x0 + 0.008f, ym + FF_CLK, x0, ym + FF_CLK + 0.006f);
        // A used register shows only the inputs it uses (see the flags)
        std::string flags = text(1);
        bool en = !used || flags.find('E') != std::string::npos;
        bool sr = !used || flags.find_first_of("RS") != std::string::npos;
        struct
        {
            bool shown;
            float start_x, row_y, lane, input;
        } controls[] = {{true, TI_X1, row(9), CLK_LANE, FF_CLK},
                        {en, TI_X1, row(10), EN_LANE, FF_EN},
                        {sr, CPE_PIN, row(11), SR_LANE, FF_SR}};
        for (auto &c : controls)
            if (c.shown)
                polyline(g, style,
                         {Pt{x + c.start_x, y + c.row_y}, Pt{x + c.lane, y + c.row_y}, Pt{x + c.lane, ym + c.input},
                          Pt{x0, ym + c.input}});
        lod(g, from, LOD_DETAIL);
        // FF or LATCH, and below it the flags (~ falling edge, Enable, Reset, Set)
        if (used)
            label(g, style, x0 + 0.004f, ym + 0.004f, x + FF_X1 - 0.004f, ym + FF_TOP - 0.004f, text(0));
        else
            name(FF_X0 + 0.004f, m + 0.004f, FF_X1 - 0.004f, m + FF_TOP - 0.004f, "FF", LOD_TEXT);
        label(g, style, x0 + 0.012f, ym + FF_BOTTOM + 0.004f, x + FF_X1 - 0.004f, ym - 0.006f, text(1));
        break;
    }
    case id_CPE_BRIDGE.index: {
        // The bridge mux (C_SN): one of IN1..IN8 to the register or the outputs
        size_t from = g.size();
        cpe_mux_symbol(g, style, "MUXOUT_int", x, y);
        lod(g, from, LOD_DETAIL);
        name(BRIDGE_X0 - 0.004f, BRIDGE_Y - 0.018f, BRIDGE_X1 + 0.004f, BRIDGE_Y - 0.012f, "BRIDGE", LOD_PORTS);
        break;
    }
    case id_CPE_RAMIO_U.index:
    case id_CPE_RAMIO_L.index: {
        // The RAM I/O port: to a block RAM or an IO cell (RAM_O) from the output mux, or from them
        // (RAM_I) to the CPE output. The directions in use (see the flags) as used, the others grey.
        float m = bel_type == id_CPE_RAMIO_U ? mu() : ml();
        simple(RAMIO_X0, m + RAMIO_Y0, RAMIO_X1, m + RAMIO_Y1);
        std::string dirs = text(0);
        size_t from = g.size();
        auto way = [&](bool in_use) { return used && in_use ? style : GraphicElement::STYLE_INACTIVE; };
        polyline(g, way(dirs.find('O') != std::string::npos),
                 {Pt{x + OMUX_X1, y + m}, Pt{x + RAMIO_IN_X, y + m}, Pt{x + RAMIO_IN_X, y + m + RAMIO_Y0},
                  Pt{x + RAMIO_X1, y + m + RAMIO_O_Y}});
        polyline(g, way(dirs.find('I') != std::string::npos),
                 {Pt{x + RAMIO_X1, y + m + RAMIO_I_Y}, Pt{x + RAMIO_OUT_X, y + m + RAMIO_Y0},
                  Pt{x + RAMIO_OUT_X, y + m}, Pt{x + OUT_X, y + m}});
        lod(g, from, LOD_DETAIL);
        name(RAMIO_X0 + 0.001f, m + RAMIO_Y1 - 0.011f, RAMIO_X0 + 0.026f, m + RAMIO_Y1 - 0.002f, "RAM I/O", LOD_PORTS);
        port_name(RAMIO_IN_X - 0.0065f, m + RAMIO_Y0 + 0.0008f, RAMIO_IN_X - 0.0005f, m + RAMIO_Y0 + 0.0058f, "I");
        port_name(RAMIO_X1 + 0.0008f, m + RAMIO_Y0 + 0.0005f, RAMIO_X1 + 0.0128f, m + RAMIO_Y0 + 0.0045f, "OUT");
        port_name(RAMIO_X1 + 0.0008f, m + RAMIO_O_Y + 0.0005f, RAMIO_X1 + 0.0168f, m + RAMIO_O_Y + 0.0055f, "RAM_O");
        port_name(RAMIO_X1 + 0.0008f, m + RAMIO_I_Y + 0.0005f, RAMIO_X1 + 0.0168f, m + RAMIO_I_Y + 0.0055f, "RAM_I");
        break;
    }
    case id_CPE_CPLINES.index: {
        // The carry logic (see CL_RAIL_OUT2), and the name of each row's line
        float top = CL_ROW0 + 5 * CL_ROW_DY + 0.016f;
        simple(CL_RAIL_OUT2 - 0.004f, CL_ROW0 - 0.016f, CL_VAL_X1 + 0.016f, top);
        name(CL_RAIL_OUT2, top + 0.002f, CL_VAL_X1 + 0.012f, top + 0.01f, "CARRY", LOD_PORTS);
        for (const char *line : {"PX", "CX", "PY2", "CY2", "PY1", "CY1"}) {
            float r = carry_row(line);
            port_name(CL_VAL_X1 + 0.002f, r + 0.001f, CL_VAL_X1 + 0.014f, r + 0.006f, line);
        }
        break;
    }
    case id_CPE_COMP.index: {
        // The comparator, a LUT2 behind L20 (A: COMB1 from L20, B: COMB2 from L10): what it computes
        // when used (see update_gui_labels), grey when it is a constant or passes an input through,
        // with a line from that input to its output
        std::string f = text(0);
        bool through = used && (f == "A" || f == "B"), idle = through || (used && (f == "0" || f == "1"));
        size_t from = g.size();
        box(g, idle ? GraphicElement::STYLE_INACTIVE : style, x + CMP_X0, y + CMP_Y0, x + CMP_X1, y + CMP_Y1);
        lod(g, from, used ? 0 : LOD_DETAIL);
        if (through) {
            float in = f == "A" ? CMP_IN1_Y : CMP_IN2_Y, xm = mid(CMP_X0, CMP_X1);
            from = g.size();
            polyline(g, style,
                     {Pt{x + CMP_X0, y + in}, Pt{x + xm, y + in}, Pt{x + xm, y + CMP_OUT_Y},
                      Pt{x + CMP_X1, y + CMP_OUT_Y}});
            lod(g, from, LOD_DETAIL);
        } else if (used) {
            label(g, idle ? GraphicElement::STYLE_INACTIVE : style, x + CMP_X0 + 0.002f, y + CMP_OUT_Y - 0.008f,
                  x + CMP_X1 - 0.002f, y + CMP_OUT_Y + 0.008f, f);
        }
        name(CMP_X0, CMP_Y1 + 0.001f, CMP_X1, CMP_Y1 + 0.007f, "CMP", used ? LOD_PORTS : LOD_TEXT);
        port_name(CMP_X0 - 0.016f, CMP_IN2_Y + 0.0005f, CMP_X0 - 0.001f, CMP_IN2_Y + 0.0055f, "COMB2");
        port_name(CMP_X0 - 0.016f, CMP_IN1_Y + 0.0005f, CMP_X0 - 0.001f, CMP_IN1_Y + 0.0055f, "COMB1");
        port_name(CMP_X1 + 0.001f, CMP_OUT_Y + 0.0005f, OUT2_LANE - 0.001f, CMP_OUT_Y + 0.0055f, "COMPOUT");
        break;
    }
    case id_RAM.index: {
        // In the RAM column: a block 16 tiles high at any zoom (an unused one from a little zoomed
        // in), its mode at the top, and zoomed out RAM written down it
        float rx = x + 1;
        size_t from = g.size();
        box(g, style, rx + RAM_X0, y + 0.1f, rx + RAM_X1, y + 15.9f);
        lod(g, from, used ? 0 : 10);
        name(1 + RAM_X0 + 0.02f, 15.75f, 1 + RAM_X1 - 0.02f, 15.86f, used ? text(0) : "RAM", LOD_TILE);
        from = g.size();
        for (int i = 0; i < 3; i++)
            label(g, style, rx + RAM_X0 + 0.02f, y + 9.6f - 1.4f * i, rx + RAM_X1 - 0.02f, y + 10.8f - 1.4f * i,
                  std::string(1, "RAM"[i]), 0);
        lod(g, from, used ? 0 : 10, LOD_TILE);
        break;
    }
    case id_GPIO.index: {
        box(g, style, x + GPIO_X0, y + GPIO_Y0, x + GPIO_X1, y + GPIO_Y1);
        // The pin name (as in the constraints), and the port of the design on it
        const PadInfoPOD *pad = tile_pad(loc);
        if (pad != nullptr)
            label(g, style, x + GPIO_X0 + 0.01f, y + 0.775f, x + GPIO_X1 - 0.01f, y + 0.81f,
                  IdString(pad->package_pin).str(ctx), LOD_ENTITY);
        label(g, style, x + GPIO_X0 + 0.01f, y + 0.69f, x + GPIO_X1 - 0.01f, y + 0.725f, text(0), LOD_ENTITY);
        // to the IO cell: the input from the pad (Y), the output to it (A) and its enable (T)
        for (auto &pin : {std::make_pair(IO_PAD_Y, "Y"), std::make_pair(IO_OUT_Y, "A"), std::make_pair(IO_EN_Y, "T")})
            port_name(GPIO_X1 - 0.016f, pin.first - 0.007f, GPIO_X1 - 0.003f, pin.first + 0.007f, pin.second);
        break;
    }
    case id_IOSEL.index:
        box(g, style, x + IOSEL_X0, y + IOSEL_Y0, x + IOSEL_X1, y + IOSEL_Y1);
        name(IOSEL_X0 + 0.03f, 0.56f, IOSEL_X1 - 0.03f, 0.6f, "IOSEL", LOD_ENTITY);
        for (auto &pin : {std::make_pair(IO_PAD_Y, "GPIO_IN"), std::make_pair(IO_OUT_Y, "GPIO_OUT"),
                          std::make_pair(IO_EN_Y, "GPIO_EN")})
            port_name(IOSEL_X0 + 0.004f, pin.first - 0.006f, IOSEL_X0 + 0.07f, pin.first + 0.006f, pin.second);
        for (auto &pin : {std::make_pair(IO_IN1_Y, "IN1"), std::make_pair(IO_IN2_Y, "IN2")})
            port_name(IOSEL_X1 - 0.04f, pin.first - 0.006f, IOSEL_X1 - 0.004f, pin.first + 0.006f, pin.second);
        for (int k = 1; k <= 4; k++) {
            float py = 0.80f - 0.03f * k;
            port_name(IOSEL_X1 - 0.045f, py - 0.006f, IOSEL_X1 - 0.004f, py + 0.006f, "OUT" + std::to_string(k));
            float px = IOSEL_X0 + 0.03f * k;
            port_name(px - 0.013f, IOSEL_Y0 + 0.003f, px + 0.013f, IOSEL_Y0 + 0.013f, "CLK" + std::to_string(k));
        }
        port_name(IOSEL_X0 + 0.137f, IOSEL_Y0 + 0.003f, IOSEL_X0 + 0.163f, IOSEL_Y0 + 0.013f, "DDR");
        break;
    case id_CLKIN.index:
    case id_GLBOUT.index:
        box(g, style, x + 0.20f + loc.z * 0.18f, y + 0.22f, x + 0.34f + loc.z * 0.18f, y + 0.50f);
        break;
    case id_PLL.index:
        box(g, style, x + 0.20f + (loc.z - 2) * 0.18f, y + 0.60f, x + 0.34f + (loc.z - 2) * 0.18f, y + 0.90f);
        break;
    case id_USR_RSTN.index:
        box(g, style, x + 0.25f, y + 0.25f, x + 0.45f, y + 0.45f);
        break;
    }
}

bool GateMateImpl::fixed_pip(PipId pip) const
{
    const GateMatePipExtraDataPOD *extra = pip_extra_data(pip);
    return extra == nullptr || extra->type != PIP_EXTRA_MUX || IdString(extra->name).empty() || extra->bits == 0;
}

std::pair<float, float> GateMateImpl::gui_position(int tile, int index) const
{
    Pt p = placed(ctx, tile, index).pos();
    return {p.x, p.y};
}

std::array<float, 4> GateMateImpl::gui_pip_ends(PipId pip) const
{
    const auto &pd = chip_pip_info(ctx->chip_info, pip);
    WireId from(pip.tile, pd.src_wire);
    if (ends_with(tile_wire_name(ctx, pip.tile, pd.src_wire), "_n"))
        from = node_driver(ctx, ctx->getPipSrcWire(pip), pip.tile, pd.src_wire);
    Pt a = placed(ctx, from.tile, from.index).pos(), b = placed(ctx, pip.tile, pd.dst_wire).pos();
    return {a.x, a.y, b.x, b.y};
}

PlacedAnchor GateMateImpl::placed_routed(int tile, int index) const
{
    PlacedAnchor pa = placed(ctx, tile, index);
    pa.a.lane = gui_lane(tile, index, pa.a.lane);
    return pa;
}

void GateMateImpl::draw_route_from_driver(std::vector<GraphicElement> &g, GraphicElement::style_t style,
                                          WireId tile_wire, const PlacedAnchor &to, bool assigned) const
{
    WireId node = ctx->normalise_wire(tile_wire.tile, tile_wire.index);
    WireId drv = node_driver(ctx, node, tile_wire.tile, tile_wire.index);
    if (drv == WireId() || drv == tile_wire)
        return;
    TrackFn tracks = [&](int channel, float def) { return assigned ? gui_track(node, channel, def) : def; };
    connect(g, style, assigned ? placed_routed(drv.tile, drv.index) : placed(ctx, drv.tile, drv.index), to, tracks);
}

void GateMateImpl::drawTileWire(std::vector<GraphicElement> &g, GraphicElement::style_t style, WireId tile_wire)
{
    int tile = tile_wire.tile, tilewire = tile_wire.index;
    const std::string &name = tile_wire_name(ctx, tile, tilewire);
    PlacedAnchor here = placed_routed(tile, tilewire);
    Pt p = here.pos();
    float x = here.tx, y = here.ty;
    if (ends_with(name, "_n")) {
        // Where the node arrives from another tile. A used node is drawn by the pips that use it,
        // from its driver to where it is used: so here only for when the wire is selected (hidden
        // otherwise), and only the branches its net uses. An unused node (a selected wire) is
        // drawn with every branch.
        if (style != GraphicElement::STYLE_ACTIVE)
            draw_route_from_driver(g, style, tile_wire, here, true);
        else if (gui_arrival_used(tile, tilewire))
            draw_route_from_driver(g, GraphicElement::STYLE_HIDDEN, tile_wire, here, true);
        return;
    }
    // A wire fed by a fixed connection from a wire arriving from another tile is the same physical
    // wire: when it is selected, show the way it came (hidden otherwise)
    const auto &uphill = chip_wire_info(ctx->chip_info, tile_wire).pips_uphill;
    if (uphill.ssize() == 1) {
        PipId up(tile, uphill[0]);
        int src = chip_pip_info(ctx->chip_info, up).src_wire;
        if (fixed_pip(up) && ends_with(tile_wire_name(ctx, tile, src), "_n"))
            draw_route_from_driver(g, GraphicElement::STYLE_HIDDEN, WireId(tile, src), here, true);
    }
    // Mux outputs draw the mux
    size_t from = g.size();
    size_t dot = name.find('.');
    std::string kind = name.substr(0, dot), rest = name.substr(dot + 1);
    if (kind == "IM" && ends_with(rest, ".Y")) {
        mux(g, style, x + IM_X0, x + IM_X1, y + row(num(rest, 1)), 0.026f, 0.01f);
    } else if (kind == "OM" && ends_with(rest, ".Y")) {
        // inputs on the right, output on the left
        mux(g, style, x + OM_X1, x + OM_X0, y + om_centre(num(rest, 1)), 0.014f, 0.006f);
    } else if ((kind == "SB_BIG" || kind == "SB_SML") && (rest.find(".Y") != std::string::npos)) {
        int plane = num(rest, 1);
        Pt m;
        if (sb_mux_output(here, m))
            sb_mux_symbol(g, style, rest, x, y + row(plane));
        if (!ends_with(rest, "_int"))
            box(g, style, x + SB_X0, y + row(plane) - SB_H, x + SB_X1, y + row(plane) + SB_H);
    } else if (kind == "IOES" && starts_with(rest, "SB_IN")) {
        mux(g, style, x + IOES_X0, x + IOES_X1, p.y, 0.009f, 0.004f);
    } else if (kind == "CPE" &&
               cpe_mux_symbol(g,
                              // an output mux whose output goes nowhere is grey
                              (rest == "OUT1_int" || rest == "OUT2_int") && ctx->getBoundWireNet(tile_wire) &&
                                              ctx->getBoundWireNet(tile_wire)->users.empty()
                                      ? GraphicElement::STYLE_INACTIVE
                                      : style,
                              rest, x, y)) {
        lod(g, from, LOD_DETAIL);
        return;
    } else if (here.a.entity == E_OTHER) {
        // A wire without a place of its own (the pips show all others): a short mark where it is
        line(g, style, p.x - 0.005f, p.y, p.x + 0.005f, p.y);
    }
    lod(g, from, LOD_ENTITY);
}

void GateMateImpl::drawPip(std::vector<GraphicElement> &g, GraphicElement::style_t style, PipId pip)
{
    size_t from = g.size();
    draw_pip(g, style, pip, true);
    // The way into the destination is an arrow, to show which way the signal goes: the line that
    // ends there, or else the last line of the path (which may end beside the destination's own
    // place, e.g. at the row of a LUT2 input actually used). A pip drawn as a mark has none.
    Pt to = placed_routed(pip.tile, chip_pip_info(ctx->chip_info, pip).dst_wire).pos();
    auto line_at = [&](size_t i, bool at_destination) {
        auto &e = g[i];
        return e.type == GraphicElement::TYPE_LINE && std::isinf(e.max_scale) && (e.x1 != e.x2 || e.y1 != e.y2) &&
               (!at_destination || (std::abs(e.x2 - to.x) < 1e-4f && std::abs(e.y2 - to.y) < 1e-4f));
    };
    for (bool at_destination : {true, false}) {
        for (size_t i = g.size(); i-- > from;) {
            if (line_at(i, at_destination)) {
                g[i].type = GraphicElement::TYPE_ARROW;
                return;
            }
        }
    }
}

void GateMateImpl::draw_pip(std::vector<GraphicElement> &g, GraphicElement::style_t style, PipId pip,
                            bool assigned) const
{
    int tile = pip.tile;
    const auto &pip_data = chip_pip_info(ctx->chip_info, pip);
    int src_id = pip_data.src_wire, dst_id = pip_data.dst_wire;
    const std::string &src_name = tile_wire_name(ctx, tile, src_id);
    auto at = [&](int t, int index) { return assigned ? placed_routed(t, index) : placed(ctx, t, index); };
    PlacedAnchor to = at(tile, dst_id);
    if (ends_with(src_name, "_n")) {
        // The signal comes from another tile: start where it is driven
        draw_route_from_driver(g, style, WireId(tile, src_id), to, assigned);
        return;
    }
    connect(g, style, at(tile, src_id), to);
}

// The lane bands of a tile: wires whose lane lies in one share it, and after routing each used one
// gets a lane of its own (see update_gui_routing)
static int lane_band(const Anchor &a, float &lo, float &hi)
{
    static const float bands[][2] = {{0.163f, 0.199f},  {0.267f, 0.2985f}, {0.2985f, 0.3295f},
                                     {0.369f, 0.4015f}, {0.435f, 0.4535f}, {0.63f, 0.66f}};
    const int count = int(sizeof(bands) / sizeof(bands[0]));
    // The pins of a RAM: in front of it in the RAM column
    if (a.entity == E_RAM) {
        lo = 0.2f, hi = 0.5f;
        return count;
    }
    for (int i = 0; i < count; i++)
        if (a.lane >= bands[i][0] && a.lane <= bands[i][1]) {
            lo = bands[i][0], hi = bands[i][1];
            return i;
        }
    return -1;
}

void GateMateImpl::update_gui_routing()
{
    // The used pips, and the tile wires at their ends (for a pip from a wire arriving from another
    // tile: the wire that drives it there)
    std::vector<std::pair<PipId, WireId>> pips;
    for (auto &net : ctx->nets)
        for (auto &w : net.second->wires)
            if (w.second.pip != PipId())
                pips.push_back({w.second.pip, ctx->getPipSrcWire(w.second.pip)});
    // Lanes: in each band of each tile, the used wires spread out, in the order of their default lanes
    // (tile, band) -> ((default lane, -y), tile wire)
    std::map<std::pair<int, int>, std::vector<std::pair<std::pair<float, float>, int>>> bands;
    dict<std::pair<int, int>, float> lanes;
    pool<std::pair<int, int>> seen;
    auto use_wire = [&](int tile, int index) {
        if (!seen.insert({tile, index}).second)
            return;
        PlacedAnchor pa = placed(ctx, tile, index);
        float lo, hi;
        int band = lane_band(pa.a, lo, hi);
        if (band >= 0)
            bands[{tile, band}].push_back({{pa.a.lane, -pa.a.p.y}, index});
    };
    for (auto &p : pips) {
        const auto &pd = chip_pip_info(ctx->chip_info, p.first);
        use_wire(p.first.tile, pd.dst_wire);
        if (ends_with(tile_wire_name(ctx, p.first.tile, pd.src_wire), "_n")) {
            WireId drv = node_driver(ctx, p.second, p.first.tile, pd.src_wire);
            if (drv != WireId())
                use_wire(drv.tile, drv.index);
        } else {
            use_wire(p.first.tile, pd.src_wire);
        }
    }
    for (auto &b : bands) {
        auto &wires = b.second;
        std::sort(wires.begin(), wires.end());
        float lo = 0, hi = 0;
        lane_band(placed(ctx, b.first.first, wires.front().second).a, lo, hi);
        for (size_t i = 0; i < wires.size(); i++)
            lanes[{b.first.first, wires[i].second}] = lo + (hi - lo) * (i + 1) / (wires.size() + 1);
    }
    // A wire arriving from another tile is drawn where the wire it feeds is, in its lane
    pool<std::pair<int, int>> arrivals;
    for (auto &p : pips) {
        const auto &pd = chip_pip_info(ctx->chip_info, p.first);
        if (!ends_with(tile_wire_name(ctx, p.first.tile, pd.src_wire), "_n"))
            continue;
        arrivals.insert({p.first.tile, pd.src_wire});
        auto lane = lanes.find({p.first.tile, pd.dst_wire});
        if (lane != lanes.end())
            lanes[{p.first.tile, pd.src_wire}] = lane->second;
    }
    // Tracks: draw each used pip with the default tracks, find the stretch of each channel each
    // signal uses (in tiles), and give the signals overlapping in a channel different tracks
    std::map<int, std::map<WireId, std::pair<int, int>>> uses;
    for (auto &p : pips) {
        std::vector<GraphicElement> g;
        draw_pip(g, GraphicElement::STYLE_ACTIVE, p.first, false);
        for (auto &e : g) {
            if (e.type != GraphicElement::TYPE_LINE || !std::isinf(e.max_scale))
                continue;
            bool horizontal = e.y1 == e.y2 && e.x1 != e.x2, vertical = e.x1 == e.x2 && e.y1 != e.y2;
            float across = horizontal ? e.y1 : e.x1;
            if (!(horizontal || vertical) || across - std::floor(across) >= CHAN)
                continue;
            int lo = int(std::floor(horizontal ? std::min(e.x1, e.x2) : std::min(e.y1, e.y2)));
            int hi = int(std::floor(horizontal ? std::max(e.x1, e.x2) : std::max(e.y1, e.y2)));
            auto &u = uses[channel(vertical, int(std::floor(across)))];
            auto it = u.find(p.second);
            if (it == u.end())
                u[p.second] = {lo, hi};
            else
                it->second = {std::min(it->second.first, lo), std::max(it->second.second, hi)};
        }
    }
    dict<std::pair<WireId, int>, float> tracks;
    for (auto &c : uses) {
        std::vector<std::pair<std::pair<int, int>, WireId>> spans;
        for (auto &u : c.second)
            spans.push_back({u.second, u.first});
        std::sort(spans.begin(), spans.end());
        std::vector<int> track_end; // per track, the last tile it is used up to
        std::vector<int> assigned;
        for (auto &span : spans) {
            size_t t = 0;
            while (t < track_end.size() && track_end[t] >= span.first.first)
                t++;
            if (t == track_end.size())
                track_end.push_back(0);
            track_end[t] = span.first.second;
            assigned.push_back(t);
        }
        float spacing = std::min(0.0063f, 0.145f / std::max<size_t>(track_end.size() - 1, 1));
        for (size_t i = 0; i < spans.size(); i++)
            tracks[{spans[i].second, c.first}] = 0.006f + spacing * assigned[i];
    }
    std::lock_guard<std::mutex> lock(gui_labels_mutex_);
    gui_lanes_ = std::move(lanes);
    gui_tracks_ = std::move(tracks);
    gui_arrivals_ = std::move(arrivals);
}

bool GateMateImpl::gui_arrival_used(int tile, int index) const
{
    std::lock_guard<std::mutex> lock(gui_labels_mutex_);
    return gui_arrivals_.count({tile, index}) > 0;
}

float GateMateImpl::gui_lane(int tile, int index, float lane) const
{
    std::lock_guard<std::mutex> lock(gui_labels_mutex_);
    auto it = gui_lanes_.find({tile, index});
    return it == gui_lanes_.end() ? lane : it->second;
}

float GateMateImpl::gui_track(WireId node, int channel, float track) const
{
    std::lock_guard<std::mutex> lock(gui_labels_mutex_);
    auto it = gui_tracks_.find({node, channel});
    return it == gui_tracks_.end() ? track : it->second;
}

bool GateMateImpl::guiDrawsUnusedRouting() const { return false; }

float GateMateImpl::getGuiColumnX(int x) const { return gui_column(ctx, x); }

NEXTPNR_NAMESPACE_END
