/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2026  The nextpnr Authors.
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

// GUI support (gfx.cc, gui_attrs.cc): the graphics of a routed design, and the properties the
// GUI shows for a selected bel or pip.

#include <array>
#include <cmath>
#include <map>
#include <random>
#include <iostream>
#include <set>
#include "known_bugs.h"
#include "testing.h"
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

class GateMateGuiTest : public GateMateTest
{
  protected:
    std::map<std::string, std::string> props(const std::vector<std::pair<IdString, std::string>> &attrs)
    {
        std::map<std::string, std::string> m;
        for (auto &kv : attrs)
            m[kv.first.str(ctx)] = kv.second;
        return m;
    }

    std::map<std::string, std::string> bel_props(const CellInfo *cell)
    {
        EXPECT_NE(cell->bel, BelId()) << cell->name.c_str(ctx);
        return props(ctx->getBelAttrs(cell->bel));
    }

    std::string describe(const std::map<std::string, std::string> &m)
    {
        std::string s;
        for (auto &kv : m)
            s += "\n  " + kv.first + ": " + kv.second;
        return s;
    }

    CellInfo *only_cell_of_type(IdString type)
    {
        CellInfo *found = nullptr;
        for (auto &cell : ctx->cells)
            if (cell.second->type == type) {
                EXPECT_EQ(found, nullptr) << "more than one " << type.c_str(ctx);
                found = cell.second.get();
            }
        EXPECT_NE(found, nullptr) << "no " << type.c_str(ctx);
        return found;
    }

    // A design that uses much of the routing: a block RAM, random LUT2 logic between inputs,
    // registers with enables and resets, an adder chain, outputs
    void routed_mix(unsigned seed)
    {
        std::mt19937 rng(seed);
        NetInfo *clk = global_clock("clk"), *en = input("en"), *rst = input("rst");
        std::vector<NetInfo *> nets;
        for (int i = 0; i < 12; i++)
            nets.push_back(input("i" + std::to_string(i)));
        auto any = [&]() { return nets[rng() % nets.size()]; };
        // A block RAM: its address and data from the inputs, its read data into the logic
        CellInfo *ram = ctx->createCell(ctx->id("ram"), id_CC_BRAM_20K);
        ram->params[id_RAM_MODE] = Property("TDP");
        ram->params[id_A_RD_WIDTH] = Property(10, 32);
        ram->params[id_A_WR_WIDTH] = Property(10, 32);
        connect_input(ram, id_A_CLK, clk);
        connect_input(ram, id_A_EN, vcc());
        connect_input(ram, id_A_WE, en);
        for (int i = 0; i < 6; i++)
            connect_input(ram, ctx->id("A_ADDR[" + std::to_string(5 + i) + "]"), any());
        for (int i = 0; i < 4; i++)
            connect_input(ram, ctx->id("A_DI[" + std::to_string(i) + "]"), any());
        for (int i = 0; i < 4; i++)
            nets.push_back(connect_output(ram, ctx->id("A_DO[" + std::to_string(i) + "]")));
        for (int i = 0; i < 60; i++)
            nets.push_back(lut2("l" + std::to_string(i), 1 + rng() % 14, any(), any()));
        for (int i = 0; i < 12; i++)
            nets.push_back(dff("r" + std::to_string(i), any(), clk, i % 2 ? en : nullptr, i % 3 ? rst : nullptr)
                                   ->getPort(id_Q));
        NetInfo *carry = input("ci");
        for (int i = 0; i < 6; i++) {
            auto sum = addf("a" + std::to_string(i), any(), any(), carry);
            nets.push_back(sum.first);
            carry = sum.second;
        }
        for (int i = 0; i < 10; i++)
            output("o" + std::to_string(i), nets[nets.size() - 1 - i * 3]);
    }

    // The detailed lines of a decal (without the simple ones standing in for them zoomed out). An
    // unused pip is drawn hidden: shown only when selected.
    std::vector<GraphicElement> lines(DecalId decal)
    {
        std::vector<GraphicElement> result;
        for (auto &e : ctx->getDecalGraphics(decal))
            if ((e.type == GraphicElement::TYPE_LINE || e.type == GraphicElement::TYPE_ARROW) &&
                std::isinf(e.max_scale))
                result.push_back(e);
        return result;
    }

    static bool near(float a, float b) { return std::abs(a - b) < 1e-4f; }
    static bool same_point(float x1, float y1, float x2, float y2) { return near(x1, x2) && near(y1, y2); }

    // Whether two segments share a point
    static bool touch(const GraphicElement &a, const GraphicElement &b)
    {
        auto cross = [](float ax, float ay, float bx, float by, float cx, float cy) {
            return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        };
        auto on = [&](const GraphicElement &s, float px, float py) {
            return std::abs(cross(s.x1, s.y1, s.x2, s.y2, px, py)) < 1e-7f && px >= std::min(s.x1, s.x2) - 1e-4f &&
                   px <= std::max(s.x1, s.x2) + 1e-4f && py >= std::min(s.y1, s.y2) - 1e-4f &&
                   py <= std::max(s.y1, s.y2) + 1e-4f;
        };
        float d1 = cross(b.x1, b.y1, b.x2, b.y2, a.x1, a.y1), d2 = cross(b.x1, b.y1, b.x2, b.y2, a.x2, a.y2);
        float d3 = cross(a.x1, a.y1, a.x2, a.y2, b.x1, b.y1), d4 = cross(a.x1, a.y1, a.x2, a.y2, b.x2, b.y2);
        if (((d1 > 1e-7f && d2 < -1e-7f) || (d1 < -1e-7f && d2 > 1e-7f)) &&
            ((d3 > 1e-7f && d4 < -1e-7f) || (d3 < -1e-7f && d4 > 1e-7f)))
            return true;
        return on(b, a.x1, a.y1) || on(b, a.x2, a.y2) || on(a, b.x1, b.y1) || on(a, b.x2, b.y2);
    }

    // Whether a point lies on a segment
    static bool on_segment(const GraphicElement &s, float px, float py)
    {
        float len = std::hypot(s.x2 - s.x1, s.y2 - s.y1);
        if (len < 1e-6f)
            return same_point(s.x1, s.y1, px, py);
        float t = ((px - s.x1) * (s.x2 - s.x1) + (py - s.y1) * (s.y2 - s.y1)) / (len * len);
        float qx = s.x1 + t * (s.x2 - s.x1), qy = s.y1 + t * (s.y2 - s.y1);
        return t >= -1e-4f && t <= 1 + 1e-4f && same_point(qx, qy, px, py);
    }

    // Every element finite and on the chip (with a margin for the edge tiles)
    ::testing::AssertionResult on_chip(const std::vector<GraphicElement> &g)
    {
        for (auto &e : g) {
            for (float v : {e.x1, e.y1, e.x2, e.y2})
                if (!std::isfinite(v))
                    return ::testing::AssertionFailure() << "coordinate not finite";
            float w = ctx->getGuiColumnX(ctx->getGridDimX() - 1) + 1, h = ctx->getGridDimY();
            if (std::min(e.x1, e.x2) < -0.5f || std::max(e.x1, e.x2) > w + 0.5f || std::min(e.y1, e.y2) < -0.5f ||
                std::max(e.y1, e.y2) > h + 0.5f)
                return ::testing::AssertionFailure()
                       << "(" << e.x1 << ", " << e.y1 << ") - (" << e.x2 << ", " << e.y2 << ") not on the chip";
        }
        return ::testing::AssertionSuccess();
    }
};

// After routing, every used pip has graphics, on the chip, and none longer than the longest
// switch box connection (20 tiles; the global clock networks are drawn as short stubs). Lines
// between tiles are horizontal or vertical.
TEST_F(GateMateGuiTest, routed_pips_are_drawn_on_the_chip)
{
    NetInfo *clk = global_clock("clk");
    NetInfo *s = addf("add", input("a"), input("b"), input("ci")).first;
    output("q", dff("ff", lut2("x", 0b0110, s, input("c")), clk)->getPort(id_Q));
    place_and_route();
    EXPECT_FALSE(ctx->guiDrawsUnusedRouting());
    int pips = 0;
    for (auto &net : ctx->nets) {
        for (auto &w : net.second->wires) {
            EXPECT_TRUE(on_chip(ctx->getDecalGraphics(ctx->getWireDecal(w.first).decal))) << ctx->nameOfWire(w.first);
            PipId pip = w.second.pip;
            if (pip == PipId())
                continue;
            auto g = ctx->getDecalGraphics(ctx->getPipDecal(pip).decal);
            EXPECT_FALSE(g.empty()) << ctx->nameOfPip(pip);
            EXPECT_TRUE(on_chip(g)) << ctx->nameOfPip(pip);
            for (auto &e : g) {
                // (between tiles, used routing is less prominent than the tiles' contents)
                EXPECT_TRUE(e.style == GraphicElement::STYLE_ACTIVE || e.style == GraphicElement::STYLE_ROUTING)
                        << ctx->nameOfPip(pip);
                EXPECT_LE(std::abs(e.x1 - e.x2) + std::abs(e.y1 - e.y2), 22.0f) << ctx->nameOfPip(pip);
                // A line that leaves its tile runs horizontally or vertically (through the
                // routing channels), not diagonally across other tiles' entities
                bool same_tile = std::floor(e.x1) == std::floor(e.x2) && std::floor(e.y1) == std::floor(e.y2);
                EXPECT_TRUE(same_tile || e.x1 == e.x2 || e.y1 == e.y2)
                        << ctx->nameOfPip(pip) << ": (" << e.x1 << ", " << e.y1 << ") - (" << e.x2 << ", " << e.y2
                        << ")";
            }
            pips++;
        }
    }
    EXPECT_GT(pips, 20);
}

// Every wire of every tile type has a position in (or, for wires leaving the tile, next to) its
// tile, where the GUI draws the tile's column: drawing the wires of one tile of each type puts
// everything near that tile. A RAM's pins are in the RAM's own column, right of its tile's.
TEST_F(GateMateGuiTest, every_tile_wire_has_a_position)
{
    const ChipInfoPOD *chip = ctx->chip_info;
    std::set<int> seen;
    int checked = 0;
    for (int tile = 0; tile < chip->tile_insts.ssize(); tile++) {
        if (!seen.insert(chip->tile_insts[tile].type).second)
            continue;
        int x, ty;
        tile_xy(chip, tile, x, ty);
        float column = ctx->getGuiColumnX(x);
        const auto &tt = chip_tile_info(chip, tile);
        for (int i = 0; i < tt.wires.ssize(); i++) {
            std::vector<GraphicElement> g;
            ctx->uarch->drawTileWire(g, GraphicElement::STYLE_ACTIVE, WireId(tile, i));
            std::string name = IdString(tt.wires[i].name).str(ctx);
            // RAM wires lie along the RAM block, in the RAM column next to their tile's, along the
            // tile's 8 rows of the RAM
            bool ram = name.compare(0, 4, "RAM.") == 0;
            float tx = column + (ram ? 1 : 0), y_min = 0.0f, y_max = ram ? 8.0f : 1.0f;
            bool arriving = name.size() > 2 && name.compare(name.size() - 2, 2, "_n") == 0;
            for (auto &e : g) {
                // (hidden: the way a wire came from another tile, shown when it is selected)
                if (e.style == GraphicElement::STYLE_HIDDEN)
                    continue;
                for (float ex : {e.x1, e.x2})
                    EXPECT_TRUE(ex >= tx - 0.01f && ex <= tx + 1.01f) << IdString(tt.type_name).str(ctx) << " " << name;
                for (float ey : {e.y1, e.y2})
                    EXPECT_TRUE(ey >= ty + y_min - 0.01f && ey <= ty + y_max + 0.01f)
                            << IdString(tt.type_name).str(ctx) << " " << name;
                // and outside the routing channels along the bottom and the left edge of the tile
                // (the wires arriving from other tiles, drawn when selected, run through them)
                if (!arriving) {
                    EXPECT_TRUE(std::min(e.x1, e.x2) >= tx + 0.16f && (ram || std::min(e.y1, e.y2) >= ty + 0.16f))
                            << IdString(tt.type_name).str(ctx) << " " << name;
                }
            }
            checked++;
        }
    }
    EXPECT_GT(checked, 3000);
}

TEST_F(GateMateGuiTest, lut_function_and_inputs)
{
    NetInfo *a = input("a"), *b = input("b");
    output("q", lut2("and", 0b1000, a, b));
    output("r", lut2("xor", 0b0110, input("c"), input("d")));
    place_and_route();
    bool found = false;
    for (auto &cell : ctx->cells) {
        if (!cell.second->type.in(id_CPE_L2T4))
            continue;
        auto p = bel_props(cell.second.get());
        EXPECT_EQ(p["Function"], "LUT tree") << describe(p);
        // One of its LUT2s computes the AND of a and b, on its inputs A and B in some order
        for (auto &kv : p) {
            if (kv.second != "A & B  (INIT 1000)")
                continue;
            // Inputs, possibly through LUT2s that pass them on: "a$y (through L02)"
            std::string lut = kv.first.substr(0, kv.first.find(' '));
            auto net = [&](const std::string &s) { return s.substr(0, s.find(" (through")); };
            std::set<std::string> inputs{net(p[lut + " A"]), net(p[lut + " B"])};
            if (inputs.count(a->name.str(ctx)) && inputs.count(b->name.str(ctx)))
                found = true;
        }
    }
    if (!found)
        for (auto &cell : ctx->cells)
            if (cell.second->type.in(id_CPE_L2T4))
                ADD_FAILURE() << cell.second->name.c_str(ctx) << describe(bel_props(cell.second.get()));
    EXPECT_TRUE(found) << "no LUT2 computing a & b";
}

// The GUI routes without finishing the routing (Arch::defer_route_finish): the cells keep their logical LUT2
// tables until finishRoute writes the physical ones. The drawing shows the logical tables, and in front of each
// first-level LUT2 input a 4:1 mux showing which physical input the virtual LUT2 input and subtree swaps of the
// routing select for it; after finishing, the physical tables, with those selects all 0. Both compute the same:
// the LUT2 tree of each half, as drawn (labels from the cell's tables, each input the net routed to it), before and
// after finishing, for every value of its nets.
TEST_F(GateMateGuiTest, lut_drawing_after_finishing_the_routing_computes_the_same)
{
    routed_mix(5);
    ctx->defer_route_finish = true;
    place_and_route();
    ASSERT_TRUE(ctx->route_finish_pending);
    auto table = [&](const CellInfo *cell, const char *name) {
        return cell->params.count(ctx->id(name)) ? int_or_default(cell->params, ctx->id(name), 0) : -1;
    };
    auto eval = [](int lut, int a, int b) { return lut < 0 ? 0 : (lut >> (a | (b << 1))) & 1; };
    // The half's result for each assignment of values to its nets (bit i: the i-th net), as the cell's tables and
    // inputs say
    auto results = [&](const CellInfo *cell, const std::vector<const NetInfo *> &nets, bool finished) {
        auto value = [&](IdString port, int assignment) {
            const NetInfo *net = cell->getPort(port);
            for (size_t i = 0; i < nets.size(); i++)
                if (net != nullptr && nets[i] == net)
                    return (assignment >> i) & 1;
            return 0;
        };
        std::vector<int> r;
        for (int v = 0; v < (1 << nets.size()); v++) {
            int l00 = table(cell, "INIT_L00"), l01 = table(cell, "INIT_L01"), l10 = table(cell, "INIT_L10");
            int out;
            // (a cell the packer gave physical inputs already is drawn as finished)
            bool logical = false;
            for (IdString port : {id_D0_00, id_D1_00, id_D0_01, id_D1_01, id_D0_10, id_D1_10})
                logical |= cell->getPort(port) != nullptr;
            if (finished || !logical) {
                // (the second input of a first-level LUT2 may be a carry line, by C_I1..C_I4)
                auto either = [&](IdString a, IdString b) { return cell->getPort(a) ? value(a, v) : value(b, v); };
                out = eval(l10, eval(l00, value(id_IN1, v), either(id_IN2, id_PINY1)),
                           eval(l01, value(id_IN3, v), either(id_IN4, cell->getPort(id_CINX) ? id_CINX : id_PINX)));
            } else if (cell->getPort(id_D0_10) || cell->getPort(id_D1_10)) {
                out = eval(l10, value(id_D0_10, v), value(id_D1_10, v));
            } else {
                out = eval(l10, eval(l00, value(id_D0_00, v), value(id_D1_00, v)),
                           eval(l01, value(id_D0_01, v), value(id_D1_01, v)));
            }
            r.push_back(out);
        }
        return r;
    };
    // (the labels are the cell's tables)
    auto check_labels = [&](const CellInfo *cell, const char *when) {
        auto labels = impl->gui_labels(cell->bel);
        bool lower = ctx->getBelType(cell->bel) == id_CPE_LT_L;
        int l10 = table(cell, "INIT_L10");
        ASSERT_GE(labels.size(), 6u);
        if (l10 >= 0)
            EXPECT_EQ(labels.at(lower ? 5 : 4), gui_lut_label(l10))
                    << cell->name.c_str(ctx) << " " << when;
        for (int k = 0; k < 2; k++) {
            int t = table(cell, k ? "INIT_L01" : "INIT_L00");
            if (t >= 0)
                EXPECT_EQ(labels.at((lower ? 2 : 0) + k), gui_lut_label(t)) << cell->name.c_str(ctx) << " " << when;
        }
    };
    struct Half
    {
        std::vector<const NetInfo *> nets;
        std::vector<int> before;
    };
    std::map<IdString, Half> halves;
    int swapped = 0;
    for (auto &c : ctx->cells) {
        const CellInfo *cell = c.second.get();
        if (cell->type != id_CPE_L2T4 || cell->bel == BelId())
            continue;
        Half h;
        for (auto &port : cell->ports)
            if (port.second.type == PORT_IN && port.second.net != nullptr &&
                std::find(h.nets.begin(), h.nets.end(), port.second.net) == h.nets.end())
                h.nets.push_back(port.second.net);
        h.before = results(cell, h.nets, false);
        check_labels(cell, "routed");
        // (whether the case the test is about came up: a subtree swap in the routing)
        for (auto &port : cell->ports)
            if (port.second.net != nullptr) {
                WireId w = ctx->getBelPinWire(cell->bel, port.first);
                auto found = port.second.net->wires.find(w);
                if (found != port.second.net->wires.end() && found->second.pip != PipId()) {
                    const auto &extra = *impl->pip_extra_data(found->second.pip);
                    swapped += extra.name != 0 && IdString(extra.name).in(id_LUT2_10X, id_LUT2_11X) && extra.value;
                }
            }
        halves[c.first] = h;
    }
    EXPECT_GT(halves.size(), 5u);
    ctx->finishRoute();
    int compared = 0;
    for (auto &h : halves) {
        const CellInfo *cell = ctx->cells.at(h.first).get();
        check_labels(cell, "finished");
        // (an input from a carry line comes on the net of a CP-line cell finishRoute made: not compared)
        bool same_nets = true;
        for (auto &port : cell->ports)
            if (port.second.type == PORT_IN && port.second.net != nullptr &&
                std::find(h.second.nets.begin(), h.second.nets.end(), port.second.net) == h.second.nets.end())
                same_nets = false;
        if (!same_nets)
            continue;
        compared++;
        std::string ports;
        for (auto &port : cell->ports)
            ports += " " + port.first.str(ctx) + "=" + (port.second.net ? port.second.net->name.str(ctx) : "-");
        for (auto &prm : cell->params)
            ports += " " + prm.first.str(ctx) + "=" + prm.second.to_string();
        EXPECT_EQ(h.second.before, results(cell, h.second.nets, true)) << cell->name.c_str(ctx) << ", finished:"
                                                                        << ports;
    }
    EXPECT_GT(compared, 5);
    // (logged for the record; the design may route without swaps)
    std::cout << compared << " LUT trees compared, " << swapped << " logical LUT inputs through a subtree swap\n";
}

// The virtual LUT2 input and subtree swaps (LUT2_0x, LUT2_10X / LUT2_11X) are drawn through the 4:1 muxes in front
// of the first-level LUT2 inputs: a pip of theirs runs along a source's bus (vertical, left of the muxes) or stays
// within one mux, never from one LUT2 input's row to another's. And a routed way through a subtree swap is one line:
// the pip into the physical input P ends in the mux of the logical input it goes on to, where the swap's pip
// starts, which ends at that input.
TEST_F(GateMateGuiTest, lut_input_swaps_are_drawn_through_the_input_muxes)
{
    auto is_virtual = [&](PipId pip) {
        const auto &extra = *impl->pip_extra_data(pip);
        if (extra.type != PipExtra::PIP_EXTRA_MUX || extra.name == 0)
            return false;
        std::string name = IdString(extra.name).str(ctx);
        return name.compare(0, 6, "LUT2_0") == 0 || name == "LUT2_10X" || name == "LUT2_11X";
    };
    // (left of the muxes: the buses; a mux is about 0.012 high, the rows 0.0645 apart)
    auto stays_in_place = [&](const std::vector<GraphicElement> &g) {
        for (auto &e : g) {
            float fx = std::min(e.x1, e.x2) - std::floor(std::min(e.x1, e.x2));
            bool bus = near(e.x1, e.x2) && fx < 0.51f;
            if (!bus && std::abs(e.y1 - e.y2) > 0.0065f)
                return false;
        }
        return true;
    };
    // Unused, in the schematic's place
    int checked = 0;
    for (int t = 0; t < ctx->chip_info->tile_insts.ssize() && checked == 0; t++) {
        int x, y;
        tile_xy(ctx->chip_info, t, x, y);
        if (std::abs(x - ctx->getGridDimX() / 2) > 2 || std::abs(y - ctx->getGridDimY() / 2) > 2)
            continue;
        const auto &tt = chip_tile_info(ctx->chip_info, t);
        for (int i = 0; i < tt.pips.ssize(); i++) {
            PipId pip(t, i);
            if (!is_virtual(pip))
                continue;
            auto g = lines(ctx->getPipDecal(pip).decal);
            EXPECT_FALSE(g.empty()) << ctx->nameOfPip(pip);
            EXPECT_TRUE(stays_in_place(g)) << ctx->nameOfPip(pip);
            checked++;
        }
    }
    EXPECT_GE(checked, 32); // (16 per half: 8 input swaps, 8 subtree swaps)
    // Routed (not finished: the virtual selects are as the router set them)
    routed_mix(5);
    ctx->defer_route_finish = true;
    place_and_route();
    int swaps = 0;
    for (auto &n : ctx->nets) {
        const NetInfo *net = n.second.get();
        for (auto &w : net->wires) {
            PipId swap = w.second.pip;
            if (swap == PipId() || !is_virtual(swap))
                continue;
            auto g2 = lines(ctx->getPipDecal(swap).decal);
            EXPECT_TRUE(stays_in_place(g2)) << ctx->nameOfPip(swap);
            const auto &extra = *impl->pip_extra_data(swap);
            if (!IdString(extra.name).in(id_LUT2_10X, id_LUT2_11X))
                continue;
            // The pip into P, ending where the swap's starts
            WireId p = ctx->getPipSrcWire(swap);
            ASSERT_TRUE(net->wires.count(p)) << ctx->nameOfPip(swap);
            PipId into = net->wires.at(p).pip;
            ASSERT_NE(into, PipId()) << ctx->nameOfWire(p);
            auto g1 = lines(ctx->getPipDecal(into).decal);
            ASSERT_FALSE(g1.empty() || g2.empty()) << ctx->nameOfPip(into);
            bool joined = false;
            for (auto &e : g1)
                joined |= same_point(e.x2, e.y2, g2.front().x1, g2.front().y1);
            EXPECT_TRUE(joined) << ctx->nameOfPip(into) << " -> " << ctx->nameOfPip(swap);
            // and the swap's at the logical input
            auto d = impl->gui_position(swap.tile, chip_pip_info(ctx->chip_info, swap).dst_wire);
            EXPECT_TRUE(same_point(g2.back().x2, g2.back().y2, d.first, d.second)) << ctx->nameOfPip(swap);
            swaps += extra.value;
        }
    }
    // (the design routes some inputs through a subtree swap)
    EXPECT_GT(swaps, 0);
}

TEST_F(GateMateGuiTest, register_modes)
{
    NetInfo *clk = global_clock("clk"), *en = input("en"), *rst = input("rst");
    CellInfo *ff = dff("ff", input("d"), clk, en, rst);
    ff->params[id_CLK_INV] = Property(1, 1);
    output("q", ff->getPort(id_Q));
    place_and_route();
    auto p = bel_props(only_cell_of_type(id_CPE_FF));
    EXPECT_EQ(p["Type"], "flip-flop") << describe(p);
    EXPECT_EQ(p["Clock"].compare(0, 12, "falling edge"), 0) << describe(p);
    EXPECT_EQ(p["Enable"], "active high (" + en->name.str(ctx) + ")") << describe(p);
    EXPECT_EQ(p["Reset"], "from SR, active high (" + rst->name.str(ctx) + ")") << describe(p);
    EXPECT_EQ(p["Set"], "unused") << describe(p);
    EXPECT_EQ(p["Initial value"], "not set") << describe(p);
}

// The drawing labels a used bel with its configuration: the LUT2 functions in the LUT2 boxes, and
// the kind and mode of a register. Labels are details: only shown when zoomed in.
TEST_F(GateMateGuiTest, labels_show_the_configuration)
{
    NetInfo *clk = global_clock("clk"), *en = input("en"), *rst = input("rst");
    CellInfo *ff = dff("ff", lut2("and", 0b1000, input("a"), input("b")), clk, en, rst);
    ff->params[id_CLK_INV] = Property(1, 1);
    output("q", ff->getPort(id_Q));
    place_and_route();
    std::multiset<std::string> labels;
    for (auto &cell : ctx->cells) {
        if (cell.second->bel == BelId())
            continue;
        for (auto &e : ctx->getDecalGraphics(ctx->getBelDecal(cell.second->bel).decal)) {
            if (e.type != GraphicElement::TYPE_LABEL)
                continue;
            labels.insert(e.text);
            EXPECT_GT(e.min_scale, 0) << e.text;
            EXPECT_LT(e.x1, e.x2) << e.text;
            EXPECT_LT(e.y1, e.y2) << e.text;
        }
    }
    std::string all;
    for (auto &l : labels)
        all += " '" + l + "'";
    EXPECT_EQ(labels.count("AND"), 1u) << all;
    EXPECT_EQ(labels.count("FF"), 1u) << all;
    // falling edge, enable, reset
    EXPECT_EQ(labels.count("~ER"), 1u) << all;
}

// Zoomed out, the drawing leaves out details: a used bel and every connection between tiles is
// drawn at any zoom, the inside of a tile and unused CPEs only once zoomed in.
TEST_F(GateMateGuiTest, level_of_detail)
{
    NetInfo *clk = global_clock("clk");
    NetInfo *s = addf("add", input("a"), input("b"), input("ci")).first;
    output("q", dff("ff", lut2("x", 0b0110, s, input("c")), clk)->getPort(id_Q));
    place_and_route();
    // in pixels per tile: the whole chip, a few tiles, one CPE
    const float scales[] = {4, 40, 300, 3000};
    auto drawn_at = [](const std::vector<GraphicElement> &g, float scale) {
        for (auto &e : g)
            if (e.min_scale <= scale && scale < e.max_scale)
                return true;
        return false;
    };
    auto leaves_tile = [](const std::vector<GraphicElement> &g) {
        for (auto &e : g)
            if (std::floor(e.x1) != std::floor(e.x2) || std::floor(e.y1) != std::floor(e.y2))
                return true;
        return false;
    };
    for (auto &cell : ctx->cells) {
        if (cell.second->bel == BelId())
            continue;
        auto g = ctx->getDecalGraphics(ctx->getBelDecal(cell.second->bel).decal);
        for (auto &e : g)
            EXPECT_LT(e.min_scale, e.max_scale) << ctx->nameOfBel(cell.second->bel);
        if (!g.empty()) {
            for (float scale : scales)
                EXPECT_TRUE(drawn_at(g, scale)) << ctx->nameOfBel(cell.second->bel) << " at " << scale;
        }
    }
    int between_tiles = 0;
    for (auto &net : ctx->nets) {
        for (auto &w : net.second->wires) {
            if (w.second.pip == PipId())
                continue;
            auto g = ctx->getDecalGraphics(ctx->getPipDecal(w.second.pip).decal);
            EXPECT_TRUE(drawn_at(g, 3000)) << ctx->nameOfPip(w.second.pip);
            if (!leaves_tile(g))
                continue;
            between_tiles++;
            for (float scale : scales)
                EXPECT_TRUE(drawn_at(g, scale)) << ctx->nameOfPip(w.second.pip) << " at " << scale;
        }
    }
    EXPECT_GT(between_tiles, 5);
    // An unused CPE: nothing when the whole chip is shown, its LUT2s once zoomed in
    for (BelId bel : ctx->getBels()) {
        if (ctx->getBelType(bel) != id_CPE_LT_U)
            continue;
        Loc loc = ctx->getBelLocation(bel);
        bool tile_used = false;
        for (BelId other : ctx->getBelsByTile(loc.x, loc.y))
            tile_used |= !ctx->checkBelAvail(other);
        if (tile_used)
            continue;
        for (BelId other : ctx->getBelsByTile(loc.x, loc.y))
            EXPECT_FALSE(drawn_at(ctx->getDecalGraphics(ctx->getBelDecal(other).decal), 4)) << ctx->nameOfBel(other);
        EXPECT_TRUE(drawn_at(ctx->getDecalGraphics(ctx->getBelDecal(bel).decal), 3000));
        break;
    }
}

// Zoomed in, a tile shows the schematic of its CPE and routing muxes: every connection inside them
// in dark grey, with the used ones drawn over it. So every line of a used pip inside a CPE lies on
// a line of the schematic, or of the CPE's LUT2 trees (which draw the ways between the LUT2s).
TEST_F(GateMateGuiTest, used_cpe_paths_lie_on_the_schematic)
{
    NetInfo *clk = global_clock("clk");
    NetInfo *s = addf("add", input("a"), input("b"), input("ci")).first;
    output("q", dff("ff", lut2("x", 0b0110, s, input("c")), clk)->getPort(id_Q));
    place_and_route();
    auto split = [](const std::string &name) {
        std::vector<std::string> parts;
        size_t start = 0;
        for (size_t end; (end = name.find('/', start)) != std::string::npos; start = end + 1)
            parts.push_back(name.substr(start, end - start));
        parts.push_back(name.substr(start));
        return parts;
    };
    int checked = 0;
    for (auto &net : ctx->nets) {
        for (auto &w : net.second->wires) {
            PipId pip = w.second.pip;
            if (pip == PipId())
                continue;
            // tile/destination/source, possibly followed by a variant (INV)
            auto parts = split(ctx->nameOfPip(pip));
            ASSERT_GE(parts.size(), 3u) << ctx->nameOfPip(pip);
            auto in_cpe = [](const std::string &n) {
                return n.compare(0, 4, "CPE.") == 0 && n.compare(n.size() - 2, 2, "_n") != 0;
            };
            if (!in_cpe(parts[1]) || !in_cpe(parts[2]))
                continue;
            // The schematic: the inactive lines of the tile's detail decal
            std::vector<GraphicElement> schematic;
            for (auto &d : ctx->getDetailDecals())
                if (d.first.decal.tile == pip.tile)
                    for (auto &e : ctx->getDecalGraphics(d.first.decal))
                        if (e.type == GraphicElement::TYPE_LINE && e.style == GraphicElement::STYLE_INACTIVE)
                            schematic.push_back(e);
            for (BelId bel : ctx->getBelsByTile(ctx->getPipLocation(pip).x, ctx->getPipLocation(pip).y))
                for (auto &e : ctx->getDecalGraphics(ctx->getBelDecal(bel).decal))
                    if (e.type == GraphicElement::TYPE_LINE)
                        schematic.push_back(e);
            auto on_schematic = [&](const GraphicElement &e) {
                const float eps = 1e-4f;
                auto near = [&](float a, float b) { return std::abs(a - b) < eps; };
                for (auto &l : schematic) {
                    // A diagonal (across a mux): the same line
                    if (!near(e.x1, e.x2) && !near(e.y1, e.y2)) {
                        if ((near(l.x1, e.x1) && near(l.y1, e.y1) && near(l.x2, e.x2) && near(l.y2, e.y2)) ||
                            (near(l.x1, e.x2) && near(l.y1, e.y2) && near(l.x2, e.x1) && near(l.y2, e.y1)))
                            return true;
                        continue;
                    }
                    bool vertical = std::abs(e.x1 - e.x2) < eps;
                    if (vertical && std::abs(l.x1 - l.x2) < eps && std::abs(l.x1 - e.x1) < eps &&
                        std::min(l.y1, l.y2) <= std::min(e.y1, e.y2) + eps &&
                        std::max(l.y1, l.y2) >= std::max(e.y1, e.y2) - eps)
                        return true;
                    if (!vertical && std::abs(l.y1 - l.y2) < eps && std::abs(l.y1 - e.y1) < eps &&
                        std::min(l.x1, l.x2) <= std::min(e.x1, e.x2) + eps &&
                        std::max(l.x1, l.x2) >= std::max(e.x1, e.x2) - eps)
                        return true;
                }
                return false;
            };
            for (auto &e : ctx->getDecalGraphics(ctx->getPipDecal(pip).decal))
                EXPECT_TRUE(on_schematic(e)) << ctx->nameOfPip(pip) << ": (" << e.x1 << ", " << e.y1 << ") - (" << e.x2
                                             << ", " << e.y2 << ")";
            checked++;
        }
    }
    EXPECT_GT(checked, 5);
}

// The schematics of the tiles are details (only drawn for the tiles in view, when zoomed in), and
// clicking on one selects the unused pip drawn there: the pips found near a point are drawn there.
TEST_F(GateMateGuiTest, tile_schematics_are_selectable_details)
{
    auto decals = ctx->getDetailDecals();
    EXPECT_GT(decals.size(), size_t(ctx->getGridDimX() * ctx->getGridDimY() / 2));
    int checked = 0;
    for (auto &d : decals) {
        int tile = d.first.decal.tile;
        float x = ctx->getGuiColumnX(d.second.x0);
        int y = d.second.y0;
        // a CPE tile in the middle of the chip
        if (std::abs(d.second.x0 - ctx->getGridDimX() / 2) > 3 || std::abs(y - ctx->getGridDimY() / 2) > 3)
            continue;
        auto g = ctx->getDecalGraphics(d.first.decal);
        for (auto &e : g) {
            EXPECT_GE(e.min_scale, 150.0f);
            EXPECT_TRUE(e.x1 >= x && e.x2 <= x + 1 && e.y1 >= y - 0.01f && e.y2 <= y + 1.01f);
        }
        // The input of input mux 1: its pips (from that input to the mux output) are found there,
        // and drawn through it
        WireId in;
        for (int i = 0; i < chip_tile_info(ctx->chip_info, tile).wires.ssize(); i++)
            if (IdString(chip_tile_info(ctx->chip_info, tile).wires[i].name).str(ctx) == "IM.P01.D2")
                in = WireId(tile, i);
        if (in == WireId())
            continue;
        bool found = false;
        for (auto &e : g) {
            if (e.type != GraphicElement::TYPE_LINE || e.style != GraphicElement::STYLE_INACTIVE)
                continue;
            // the line across the mux from that input: try picking at its start
            for (PipId pip : ctx->getDetailPips(d.first.decal, e.x1 + 0.001f * (e.x2 - e.x1), e.y1))
                if (ctx->getPipSrcWire(pip) == ctx->normalise_wire(in.tile, in.index))
                    found = true;
        }
        EXPECT_TRUE(found) << ctx->nameOfWire(in);
        checked++;
    }
    EXPECT_GT(checked, 0);
}

// Every pip of every tile type, drawn by itself, is a simple path from where its source is drawn (or
// for a source arriving from another tile, its driver) to where its destination is: one unbroken
// line that does not turn back, cross or touch itself, horizontal and vertical except for the last
// step across a mux.
TEST_F(GateMateGuiTest, pip_drawings_are_simple_paths)
{
    // An instance of each tile type near the middle of the chip
    std::map<int, std::pair<int, int>> example;
    for (int t = 0; t < ctx->chip_info->tile_insts.ssize(); t++) {
        int x, y;
        tile_xy(ctx->chip_info, t, x, y);
        int type = ctx->chip_info->tile_insts[t].type;
        int distance = std::abs(x - ctx->getGridDimX() / 2) + std::abs(y - ctx->getGridDimY() / 2);
        if (!example.count(type) || distance < example[type].second)
            example[type] = {t, distance};
    }
    int checked = 0, failures = 0;
    for (auto &ex : example) {
        int tile = ex.second.first;
        const auto &tt = chip_tile_info(ctx->chip_info, tile);
        std::vector<GraphicElement> boxes;
        int x, y;
        tile_xy(ctx->chip_info, tile, x, y);
        for (BelId bel : ctx->getBelsByTile(x, y))
            for (auto &e : ctx->getDecalGraphics(ctx->getBelDecal(bel).decal))
                if (e.type == GraphicElement::TYPE_BOX)
                    boxes.push_back(e);
        for (int i = 0; i < tt.pips.ssize() && failures < 20; i++) {
            PipId pip(tile, i);
            auto g = lines(ctx->getPipDecal(pip).decal);
            if (g.empty())
                continue;
            auto ends = impl->gui_pip_ends(pip);
            // the global clock networks: a stub where they arrive
            if (std::abs(ends[0] - ends[2]) + std::abs(ends[1] - ends[3]) > 22)
                continue;
            // A signal passing through a LUT2 starts at its box, and one into a LUT2 input ends at
            // the input's own row (LUT2 inputs are interchangeable: postRoute adapts the function)
            auto on_box = [&](float x, float y) {
                for (auto &b : boxes)
                    if ((x >= b.x1 - 1e-4f && x <= b.x2 + 1e-4f && (near(y, b.y1) || near(y, b.y2))) ||
                        (y >= b.y1 - 1e-4f && y <= b.y2 + 1e-4f && (near(x, b.x1) || near(x, b.x2))))
                        return true;
                return false;
            };
            // (and one passing through a first-level LUT2 into the second level runs from its output to
            // where that goes in, in a tile whose LUT2s are not drawn as well)
            auto is_lut_input = [&](int w) {
                std::string n = IdString(tt.wires[w].name).str(ctx);
                return n.size() == 13 && n.compare(0, 5, "CPE.D") == 0 && n.compare(9, 4, "_int") == 0;
            };
            const auto &pd = chip_pip_info(ctx->chip_info, pip);
            bool through_lut = is_lut_input(pd.src_wire) && is_lut_input(pd.dst_wire);
            std::string problem;
            if (!same_point(g.front().x1, g.front().y1, ends[0], ends[1]) && !on_box(g.front().x1, g.front().y1) &&
                !through_lut)
                problem = "does not start at its source";
            else if (!same_point(g.back().x2, g.back().y2, ends[2], ends[3]) && !on_box(g.back().x2, g.back().y2) &&
                     !through_lut)
                problem = "does not end at its destination";
            for (size_t k = 0; k < g.size() && problem.empty(); k++) {
                const auto &e = g[k];
                if (k + 1 < g.size() && !same_point(e.x2, e.y2, g[k + 1].x1, g[k + 1].y1))
                    problem = "is broken after line " + std::to_string(k);
                else if (!near(e.x1, e.x2) && !near(e.y1, e.y2) && k + 1 != g.size())
                    problem = "is diagonal before its last step (line " + std::to_string(k) + ")";
                else if (k + 1 < g.size()) {
                    const auto &f = g[k + 1];
                    float dot = (e.x2 - e.x1) * (f.x2 - f.x1) + (e.y2 - e.y1) * (f.y2 - f.y1);
                    float crs = (e.x2 - e.x1) * (f.y2 - f.y1) - (e.y2 - e.y1) * (f.x2 - f.x1);
                    if (dot < 0 && std::abs(crs) < 1e-9f)
                        problem = "turns back after line " + std::to_string(k);
                }
                for (size_t j = k + 2; j < g.size() && problem.empty(); j++)
                    if (touch(e, g[j]))
                        problem = "crosses itself (lines " + std::to_string(k) + " and " + std::to_string(j) + ")";
            }
            if (!problem.empty()) {
                ADD_FAILURE() << ctx->nameOfPip(pip) << " " << problem;
                failures++;
            }
            checked++;
        }
    }
    EXPECT_GT(checked, 5000);
}

// Routing by hand, the GUI shows the pips leaving the head wire, and a click takes the one drawn
// nearest. So in a CPE every pip has a stretch of its own, away from the other pips leaving the same
// wire (those into the routing included), long enough to click: every way through the CPE, the carry
// and propagate lines included, can be clicked pip by pip. (The two inputs of a LUT2 are one choice:
// postRoute swaps them to where the signals are, so their pips may be drawn alike; so are the two
// first-level LUT2s a physical input P*_0n_int can go into through the virtual subtree swap, where
// postRoute moves the tables; a wire with one pip leaving it needs only to show it.)
TEST_F(GateMateGuiTest, cpe_pips_can_be_told_apart_by_a_click)
{
    int tile = -1;
    for (int t = 0; t < ctx->chip_info->tile_insts.ssize() && tile < 0; t++) {
        int x, y;
        tile_xy(ctx->chip_info, t, x, y);
        if (std::abs(x - ctx->getGridDimX() / 2) <= 2 && std::abs(y - ctx->getGridDimY() / 2) <= 2)
            for (auto &w : chip_tile_info(ctx->chip_info, t).wires)
                if (IdString(w.name).str(ctx) == "CPE.CX_VAL")
                    tile = t;
    }
    ASSERT_GE(tile, 0);
    // (in tile units: at the zoom that shows the CPE's port names, 1000 pixels per tile)
    const float SEPARATION = 0.002f, MIN_LENGTH = 0.008f;
    auto distance = [](float px, float py, const GraphicElement &s) {
        float dx = s.x2 - s.x1, dy = s.y2 - s.y1, len2 = dx * dx + dy * dy;
        float t = len2 < 1e-12f ? 0 : std::max(0.0f, std::min(1.0f, ((px - s.x1) * dx + (py - s.y1) * dy) / len2));
        return std::hypot(px - (s.x1 + t * dx), py - (s.y1 + t * dy));
    };
    const auto &tt = chip_tile_info(ctx->chip_info, tile);
    // The LUT2 whose input a wire is (the logical inputs CPE.D0_00_int and CPE.D1_00_int, or the physical
    // first-level ones before the subtree swap, CPE.P0_00_int and CPE.P1_00_int: "00"), or ""
    auto lut_input_name = [](const std::string &n) {
        return n.size() == 13 && (n.compare(0, 5, "CPE.D") == 0 || n.compare(0, 5, "CPE.P") == 0) &&
               n.compare(9, 4, "_int") == 0;
    };
    auto lut_of = [&](WireId w) {
        std::string n = ctx->nameOfWire(w);
        n = n.substr(n.find('/') + 1);
        return lut_input_name(n) ? n.substr(7, 2) : std::string();
    };
    int checked = 0;
    std::vector<std::string> failures;
    for (int i = 0; i < tt.wires.ssize(); i++) {
        std::string name = IdString(tt.wires[i].name).str(ctx);
        if (name.compare(0, 4, "CPE.") != 0 || name.compare(name.size() - 2, 2, "_n") == 0)
            continue;
        WireId wire = ctx->normalise_wire(tile, i);
        std::vector<std::pair<PipId, std::vector<GraphicElement>>> pips;
        for (PipId pip : ctx->getPipsDownhill(wire))
            pips.push_back({pip, lines(ctx->getPipDecal(pip).decal)});
        for (auto &p : pips) {
            WireId dst = ctx->getPipDstWire(p.first);
            std::string dst_name = ctx->nameOfWire(dst);
            if (p.first.tile != tile || dst_name.find("/CPE.") == std::string::npos)
                continue;
            // The length of the pip's drawing that is further than SEPARATION from the others' (the pips of
            // the same choice left out: into the same LUT2, or from a physical first-level input through the
            // subtree swap into either LUT2)
            bool from_physical = name.compare(0, 5, "CPE.P") == 0 && lut_input_name(name);
            auto choice = [&](WireId d) { return from_physical && !lut_of(d).empty() ? std::string("swap") : lut_of(d); };
            std::string lut = choice(dst);
            float own = 0, length = 0;
            int others = 0;
            for (auto &e : p.second) {
                float len = std::hypot(e.x2 - e.x1, e.y2 - e.y1);
                length += len;
                int steps = std::max(2, int(len / 0.0005f));
                for (int k = 0; k < steps; k++) {
                    float t = (k + 0.5f) / steps, px = e.x1 + t * (e.x2 - e.x1), py = e.y1 + t * (e.y2 - e.y1);
                    bool apart = true;
                    others = 0;
                    for (auto &q : pips) {
                        if (q.first == p.first || (!lut.empty() && choice(ctx->getPipDstWire(q.first)) == lut))
                            continue;
                        others++;
                        for (auto &f : q.second)
                            apart = apart && distance(px, py, f) >= SEPARATION;
                    }
                    if (apart)
                        own += len / steps;
                }
            }
            if (p.second.empty() || (others > 0 && own < MIN_LENGTH) || length < 0.003f)
                failures.push_back(std::string(ctx->nameOfPip(p.first)) + ": " + std::to_string(own) + " of its own (" +
                                   std::to_string(pips.size()) + " pips from " + name + ")");
            checked++;
        }
    }
    for (size_t k = 0; k < failures.size() && k < 60; k++)
        ADD_FAILURE() << failures[k];
    EXPECT_EQ(failures.size(), 0u);
    EXPECT_GT(checked, 100);
}

// In routed designs, the drawings of a net's pips join up: a pip starts on the drawing of the pip
// that drives its source wire (if the net has one), and the pips continuing from its destination
// wire start on its drawing; except that a signal through a LUT2 or into one of its inputs is drawn
// from or to its box. And the lines of different nets never overlap: each signal has its own lanes
// and tracks.
TEST_F(GateMateGuiTest, routed_drawings_join_up_and_keep_apart)
{
    for (unsigned seed : {1u, 2u, 3u}) {
        init_context();
        routed_mix(seed);
        place_and_route();
        int failures = 0;
        // horizontal and vertical lines by net, for the overlap check: (vertical, coordinate)
        std::map<std::pair<bool, long>, std::vector<std::tuple<const NetInfo *, GraphicElement, PipId>>> straight;
        for (auto &n : ctx->nets) {
            const NetInfo *net = n.second.get();
            std::vector<GraphicElement> boxes;
            auto add_bel = [&](const CellInfo *cell) {
                if (cell != nullptr && cell->bel != BelId())
                    for (auto &e : ctx->getDecalGraphics(ctx->getBelDecal(cell->bel).decal))
                        if (e.type == GraphicElement::TYPE_BOX)
                            boxes.push_back(e);
            };
            add_bel(net->driver.cell);
            for (auto &usr : net->users)
                add_bel(usr.cell);
            std::vector<std::pair<PipId, std::vector<GraphicElement>>> drawn;
            for (auto &w : net->wires) {
                PipId pip = w.second.pip;
                if (pip == PipId())
                    continue;
                auto ends = impl->gui_pip_ends(pip);
                if (std::abs(ends[0] - ends[2]) + std::abs(ends[1] - ends[3]) > 22)
                    continue; // global network stub
                auto g = lines(ctx->getPipDecal(pip).decal);
                if (!g.empty())
                    drawn.push_back({pip, g});
            }
            auto on_box = [&](float x, float y) {
                for (auto &b : boxes)
                    if ((x >= b.x1 - 1e-4f && x <= b.x2 + 1e-4f && (near(y, b.y1) || near(y, b.y2))) ||
                        (y >= b.y1 - 1e-4f && y <= b.y2 + 1e-4f && (near(x, b.x1) || near(x, b.x2))))
                        return true;
                return false;
            };
            auto on_drawing = [&](size_t k, float x, float y) {
                for (auto &e : drawn[k].second)
                    if (on_segment(e, x, y))
                        return true;
                return false;
            };
            for (size_t k = 0; k < drawn.size() && failures < 10; k++) {
                const auto &g = drawn[k].second;
                WireId src = ctx->getPipSrcWire(drawn[k].first), dst = ctx->getPipDstWire(drawn[k].first);
                for (size_t j = 0; j < drawn.size(); j++) {
                    if (j == k)
                        continue;
                    // j drives k's source: k starts on j's drawing
                    if (ctx->getPipDstWire(drawn[j].first) == src && !on_drawing(j, g.front().x1, g.front().y1) &&
                        !on_box(g.front().x1, g.front().y1)) {
                        ADD_FAILURE() << "seed " << seed << ": net " << net->name.str(ctx) << ", pip "
                                      << ctx->nameOfPip(drawn[k].first) << " does not start on the drawing of "
                                      << ctx->nameOfPip(drawn[j].first);
                        failures++;
                    }
                    // j continues from k's destination: j starts on k's drawing
                    const auto &h = drawn[j].second;
                    if (ctx->getPipSrcWire(drawn[j].first) == dst && !on_drawing(k, h.front().x1, h.front().y1) &&
                        !on_box(h.front().x1, h.front().y1)) {
                        ADD_FAILURE() << "seed " << seed << ": net " << net->name.str(ctx) << ", pip "
                                      << ctx->nameOfPip(drawn[j].first) << " does not start on the drawing of "
                                      << ctx->nameOfPip(drawn[k].first);
                        failures++;
                    }
                }
                for (auto &s : g)
                    if (near(s.x1, s.x2) != near(s.y1, s.y2)) {
                        bool vertical = near(s.x1, s.x2);
                        straight[{vertical, std::lround((vertical ? s.x1 : s.y1) * 1e4)}].push_back(
                                {net, s, drawn[k].first});
                    }
            }
        }
        for (auto &line : straight) {
            auto &segs = line.second;
            bool vertical = line.first.first;
            for (size_t i = 0; i < segs.size() && failures < 20; i++)
                for (size_t j = i + 1; j < segs.size() && failures < 20; j++) {
                    // (a bridge's net starts on the wire of the net it copies, a CP-line cell's net where the net
                    // it passes on ends: the same signal, also where the routing the configuration makes, drawn
                    // with the pip into that wire, goes on beside it)
                    PipId pi = std::get<2>(segs[i]), pj = std::get<2>(segs[j]);
                    if (std::get<0>(segs[i]) == std::get<0>(segs[j]) ||
                        ctx->getPipSrcWire(pi) == ctx->getPipSrcWire(pj) ||
                        ctx->getPipDstWire(pi) == ctx->getPipSrcWire(pj) ||
                        ctx->getPipDstWire(pj) == ctx->getPipSrcWire(pi))
                        continue;
                    const auto &a = std::get<1>(segs[i]), &b = std::get<1>(segs[j]);
                    float a0 = vertical ? std::min(a.y1, a.y2) : std::min(a.x1, a.x2);
                    float a1 = vertical ? std::max(a.y1, a.y2) : std::max(a.x1, a.x2);
                    float b0 = vertical ? std::min(b.y1, b.y2) : std::min(b.x1, b.x2);
                    float b1 = vertical ? std::max(b.y1, b.y2) : std::max(b.x1, b.x2);
                    if (std::min(a1, b1) - std::max(a0, b0) > 1e-3f) {
                        ADD_FAILURE() << "seed " << seed << ": nets " << std::get<0>(segs[i])->name.str(ctx) << " ("
                                      << ctx->nameOfPip(std::get<2>(segs[i])) << ") and "
                                      << std::get<0>(segs[j])->name.str(ctx) << " ("
                                      << ctx->nameOfPip(std::get<2>(segs[j])) << ") overlap at (" << a.x1 << ", "
                                      << a.y1 << ") - (" << a.x2 << ", " << a.y2 << ")";
                        failures++;
                    }
                }
        }
    }
}

// A register draws the lanes of its clock, enable and set/reset: each starts where its signal is
// (the output of the clock or enable mux, the set/reset pin) and ends on the register, without
// overshooting either, for each combination of used inputs.
TEST_F(GateMateGuiTest, used_pips_end_in_an_arrow)
{
    // Which way a signal goes: the drawing of each used pip ends in one arrow, at the end of the
    // path away from the source (a path into a LUT2 ends at the row of the input it uses, not
    // always at the destination's own place). A used pip into a CPE wire also draws the ways on from
    // there through the CPE's configuration (drawn last, see configured_from): not part of its path.
    routed_mix(1);
    place_and_route();
    int checked = 0, failures = 0;
    for (auto &net : ctx->nets) {
        for (auto &w : net.second->wires) {
            PipId pip = w.second.pip;
            if (pip == PipId() || failures >= 20)
                continue;
            auto g = lines(ctx->getPipDecal(pip).decal);
            std::vector<GraphicElement> onward;
            impl->configured_from(onward, WireId(pip.tile, chip_pip_info(ctx->chip_info, pip).dst_wire));
            size_t onward_lines = 0;
            for (auto &e : onward)
                onward_lines += (e.type == GraphicElement::TYPE_LINE || e.type == GraphicElement::TYPE_ARROW) &&
                                std::isinf(e.max_scale);
            ASSERT_LE(onward_lines, g.size()) << ctx->nameOfPip(pip);
            g.resize(g.size() - onward_lines);
            if (g.empty())
                continue;
            auto ends = impl->gui_pip_ends(pip);
            std::vector<GraphicElement> arrows;
            bool mark = true;
            for (auto &e : g) {
                if (e.type == GraphicElement::TYPE_ARROW)
                    arrows.push_back(e);
                mark = mark && same_point(e.x1, e.y1, e.x2, e.y2);
            }
            std::string problem;
            if (mark) {
                // a pip drawn as a mark (where both its wires are, e.g. a switch box output driver)
                if (!arrows.empty())
                    problem = "an arrow on a mark";
            } else if (arrows.size() != 1) {
                problem = std::to_string(arrows.size()) + " arrows";
            } else {
                auto &a = arrows[0];
                bool tail_joins = g.size() == 1;
                for (auto &e : g) {
                    if (same_point(e.x1, e.y1, a.x1, a.y1) && same_point(e.x2, e.y2, a.x2, a.y2))
                        continue;
                    if (same_point(e.x1, e.y1, a.x2, a.y2) || same_point(e.x2, e.y2, a.x2, a.y2))
                        problem = "the path continues from the arrow's tip";
                    if (on_segment(e, a.x1, a.y1)) // (also where a stub crosses a global's mark)
                        tail_joins = true;
                }
                if (!tail_joins)
                    problem = "the arrow is apart from the rest of the path";
                if (same_point(a.x2, a.y2, ends[0], ends[1]))
                    problem = "the arrow points at the source";
            }
            EXPECT_EQ(problem, "") << ctx->nameOfPip(pip);
            failures += !problem.empty();
            checked++;
        }
    }
    EXPECT_GT(checked, 200);
}

TEST_F(GateMateGuiTest, register_lanes_hook_into_their_inputs)
{
    NetInfo *clk = global_clock("clk"), *en = input("en"), *rst = input("rst");
    for (int i = 0; i < 4; i++)
        output("q" + std::to_string(i), dff("ff" + std::to_string(i), input("d" + std::to_string(i)), clk,
                                            (i & 1) ? en : nullptr, (i & 2) ? rst : nullptr)
                                                ->getPort(id_Q));
    place_and_route();
    int checked = 0;
    for (auto &cell : ctx->cells) {
        BelId bel = cell.second->bel;
        if (bel == BelId() || !ctx->getBelType(bel).in(id_CPE_FF_U, id_CPE_FF_L))
            continue;
        auto g = lines(ctx->getBelDecal(bel).decal);
        GraphicElement ff;
        for (auto &e : ctx->getDecalGraphics(ctx->getBelDecal(bel).decal))
            if (e.type == GraphicElement::TYPE_BOX)
                ff = e;
        // where the lanes may start: the clock and enable muxes' outputs and the set/reset pin
        std::vector<std::pair<float, float>> sources;
        const auto &tt = chip_tile_info(ctx->chip_info, bel.tile);
        for (int i = 0; i < tt.wires.ssize(); i++) {
            std::string name = IdString(tt.wires[i].name).str(ctx);
            if (name == "CPE.CLK_int" || name == "CPE.EN_int" || name == "CPE.SR")
                sources.push_back(impl->gui_position(bel.tile, i));
        }
        auto on_ff = [&](float x, float y) {
            return (x >= ff.x1 - 1e-4f && x <= ff.x2 + 1e-4f && (near(y, ff.y1) || near(y, ff.y2))) ||
                   (y >= ff.y1 - 1e-4f && y <= ff.y2 + 1e-4f && (near(x, ff.x1) || near(x, ff.x2)));
        };
        for (size_t i = 0; i < g.size(); i++) {
            for (auto end : {std::make_pair(g[i].x1, g[i].y1), std::make_pair(g[i].x2, g[i].y2)}) {
                bool hooked = on_ff(end.first, end.second);
                for (auto &src : sources)
                    hooked |= same_point(src.first, src.second, end.first, end.second);
                for (size_t j = 0; j < g.size() && !hooked; j++)
                    hooked |= j != i && (same_point(g[j].x1, g[j].y1, end.first, end.second) ||
                                         same_point(g[j].x2, g[j].y2, end.first, end.second));
                EXPECT_TRUE(hooked) << ctx->nameOfBel(bel) << ": a line ends at (" << end.first << ", " << end.second
                                    << ")";
            }
        }
        checked++;
    }
    EXPECT_GE(checked, 4);
}

// The RAM I/O port, through which a CPE drives an IO cell's output or a block RAM's input, is
// drawn connected: from the output mux's output up into it and out at its RAM_O pin, where the
// routing to the IO cell continues
TEST_F(GateMateGuiTest, ram_io_port_is_drawn_connected)
{
    output("q", lut2("x", 0b0110, input("a"), input("b")));
    place_and_route();
    int checked = 0;
    for (auto &cell : ctx->cells) {
        BelId bel = cell.second->bel;
        if (bel == BelId() || !ctx->getBelType(bel).in(id_CPE_RAMIO_U, id_CPE_RAMIO_L))
            continue;
        bool upper = ctx->getBelType(bel) == id_CPE_RAMIO_U;
        std::pair<float, float> mux_out, ram_o;
        const auto &tt = chip_tile_info(ctx->chip_info, bel.tile);
        for (int i = 0; i < tt.wires.ssize(); i++) {
            std::string name = IdString(tt.wires[i].name).str(ctx);
            if (name == (upper ? "CPE.OUT2_int" : "CPE.OUT1_int"))
                mux_out = impl->gui_position(bel.tile, i);
            if (name == (upper ? "CPE.RAM_O2" : "CPE.RAM_O1"))
                ram_o = impl->gui_position(bel.tile, i);
        }
        std::vector<GraphicElement> path;
        for (auto &e : lines(ctx->getBelDecal(bel).decal))
            if (e.style == GraphicElement::STYLE_ACTIVE)
                path.push_back(e);
        ASSERT_FALSE(path.empty()) << ctx->nameOfBel(bel);
        EXPECT_TRUE(same_point(path.front().x1, path.front().y1, mux_out.first, mux_out.second)) << ctx->nameOfBel(bel);
        EXPECT_TRUE(same_point(path.back().x2, path.back().y2, ram_o.first, ram_o.second)) << ctx->nameOfBel(bel);
        for (size_t k = 0; k + 1 < path.size(); k++)
            EXPECT_TRUE(same_point(path[k].x2, path[k].y2, path[k + 1].x1, path[k + 1].y1)) << ctx->nameOfBel(bel);
        checked++;
    }
    EXPECT_GT(checked, 0);
}

TEST_F(GateMateGuiTest, output_pad_settings)
{
    CellInfo *obuf = output("q", input("d"));
    obuf->params[id_DRIVE] = Property(6, 32);
    obuf->params[id_SLEW] = Property("SLOW");
    place_and_route();
    CellInfo *iosel = nullptr;
    for (auto &cell : ctx->cells)
        if (cell.second->type == id_IOSEL && cell.second->getPort(id_OUT1) != nullptr)
            iosel = cell.second.get();
    ASSERT_NE(iosel, nullptr);
    auto p = bel_props(iosel);
    EXPECT_EQ(p["Package pin"], package_pin("q")) << describe(p);
    EXPECT_EQ(p["Drive strength"], "6 mA") << describe(p);
    EXPECT_EQ(p["Slew rate"], "slow") << describe(p);
    EXPECT_EQ(p["Output"], "enabled") << describe(p);
    // The pad itself: its package pin
    for (auto &cell : ctx->cells)
        if (cell.second->type == id_CPE_OBUF) {
            auto pad = bel_props(cell.second.get());
            EXPECT_EQ(pad["Package pin"], package_pin("q")) << describe(pad);
        }
}

TEST_F(GateMateGuiTest, ram_ports)
{
    NetInfo *clk = global_clock("clk");
    CellInfo *ram = ctx->createCell(ctx->id("ram"), id_CC_BRAM_20K);
    ram->params[id_RAM_MODE] = Property("TDP");
    ram->params[id_A_RD_WIDTH] = Property(10, 32);
    ram->params[id_A_WR_WIDTH] = Property(10, 32);
    ram->params[id_A_DO_REG] = Property(1, 1);
    connect_input(ram, id_A_CLK, clk);
    connect_input(ram, id_A_EN, vcc());
    connect_input(ram, ctx->id("A_ADDR[5]"), input("a"));
    output("q", connect_output(ram, ctx->id("A_DO[0]")));
    place_and_route();
    CellInfo *placed = nullptr;
    for (auto &cell : ctx->cells)
        if (cell.second->type.in(id_RAM, id_RAM_HALF))
            placed = cell.second.get();
    ASSERT_NE(placed, nullptr);
    auto p = bel_props(placed);
    EXPECT_EQ(p["Mode"].compare(0, 4, "TDP,"), 0) << describe(p);
    EXPECT_EQ(p["Port A0"], "read 10 bit, write 10 bit, output register") << describe(p);
}

// A routing mux the router uses: which input it selects
TEST_F(GateMateGuiTest, input_mux_selection)
{
    output("q", lut2("x", 0b0110, input("a"), input("b")));
    place_and_route();
    int checked = 0;
    for (auto &net : ctx->nets) {
        for (auto &w : net.second->wires) {
            PipId pip = w.second.pip;
            if (pip == PipId())
                continue;
            auto p = props(ctx->getPipAttrs(pip));
            if (p["Mux"].compare(0, 4, "IM.P") != 0)
                continue;
            EXPECT_EQ(p["Selects input"].substr(p["Selects input"].find(" of ")), " of 8") << describe(p);
            EXPECT_EQ(p["Config bits"].size(), 3u) << describe(p);
            EXPECT_EQ(p["Used by"], net.first.str(ctx)) << describe(p);
            checked++;
        }
    }
    EXPECT_GT(checked, 0);
}

// A LUT2 alone in its half sits on L10 / L11: the routing takes its inputs through a first-level LUT2 (with the
// virtual subtree swap, through the other one), which the cell has no table for. Before finishing, that LUT2 is
// labelled with what it passes on (A, or B through the bypass), wherever the routing takes the inputs through.
TEST_F(GateMateGuiTest, a_first_level_lut2_passing_an_input_on_is_labelled)
{
    std::mt19937 rng(5);
    NetInfo *clk = global_clock("clk");
    std::vector<NetInfo *> signals;
    for (int i = 0; i < 12; i++)
        signals.push_back(dff(stringf("in_ff%d", i), input(stringf("in%d", i)), clk)->getPort(id_Q));
    for (int n = 0; n < 60; n++) {
        auto pick = [&]() { return signals.at(rng() % signals.size()); };
        NetInfo *y = n % 3 == 2 ? lut2(stringf("l%d", n), rng() & 0xf, pick(), pick())
                                : l2t4(stringf("t%d", n), rng() & 0xf, rng() & 0xf, rng() & 0xf, pick(), pick(),
                                       pick(), pick());
        signals.push_back(dff(stringf("ff%d", n), y, clk)->getPort(id_Q));
    }
    for (int i = 0; i < 12; i++)
        output(stringf("out%d", i), signals.at(signals.size() - 1 - i));
    ctx->defer_route_finish = true;
    place_and_route();
    ASSERT_TRUE(ctx->route_finish_pending);
    int passes = 0, swapped = 0;
    for (auto &c : ctx->cells) {
        const CellInfo *cell = c.second.get();
        if (cell->type != id_CPE_L2T4 || cell->bel == BelId() ||
            !(cell->getPort(id_D0_10) || cell->getPort(id_D1_10)))
            continue;
        bool lower = ctx->getBelType(cell->bel) == id_CPE_LT_L;
        auto labels = impl->gui_labels(cell->bel);
        ASSERT_GE(labels.size(), 4u);
        // The first-level LUT2s the routing passes through: a bound pip from a first-level input on to L10 / L11
        for (IdString port : {id_D0_10, id_D1_10}) {
            const NetInfo *net = cell->getPort(port);
            if (net == nullptr)
                continue;
            WireId w = ctx->getBelPinWire(cell->bel, port);
            ASSERT_TRUE(net->wires.count(w)) << cell->name.c_str(ctx);
            PipId pip = net->wires.at(w).pip;
            ASSERT_NE(pip, PipId());
            std::string src = ctx->getWireName(ctx->getPipSrcWire(pip))[1].str(ctx); // CPE.D{d}_0k_int
            ASSERT_EQ(src.compare(0, 5, "CPE.D"), 0) << src;
            int k = src[8] - '0';
            EXPECT_EQ(k / 2, lower ? 1 : 0) << src;
            // (k: the logical first-level LUT2; behind it the virtual swap may take another physical one)
            WireId sw = ctx->getPipSrcWire(pip);
            if (net->wires.count(sw) && net->wires.at(sw).pip != PipId()) {
                std::string p = ctx->getWireName(ctx->getPipSrcWire(net->wires.at(sw).pip))[1].str(ctx);
                swapped += p.size() > 8 && p.compare(0, 5, "CPE.P") == 0 && p[8] != src[8];
            }
            std::string label = labels.at(k);
            EXPECT_TRUE(label == "A" || label == "B" || cell->params.count(k % 2 ? id_INIT_L01 : id_INIT_L00))
                    << cell->name.c_str(ctx) << " through " << src << ": '" << label << "'";
            passes++;
        }
    }
    EXPECT_GT(passes, 10);
    EXPECT_GT(swapped, 0);
}

// Every wire of a CPE has a place in the schematic: none is left at the stand-in position a wire the drawing does
// not know gets (spread() in gfx.cc, from a hash of its name), from where its pips would run through the CPE to
// nothing (as CP_PROP_int / CP_CARRY_int did, the carry and propagate lines back into CPOUT1 / CPOUT2).
TEST_F(GateMateGuiTest, every_cpe_pip_runs_between_places_of_the_schematic)
{
    // (as spread() in gfx.cc)
    auto spread = [](const std::string &name) {
        size_t h = std::hash<std::string>{}(name);
        return std::make_pair(0.22f + 0.72f * float(h % 997) / 997, 0.24f + 0.70f * float((h / 997) % 991) / 991);
    };
    int checked = 0;
    for (BelId bel : ctx->getBels()) {
        if (ctx->getBelType(bel) != id_CPE_LT_U)
            continue;
        int tile = bel.tile, x, y;
        tile_xy(ctx->chip_info, tile, x, y);
        float column = ctx->getGuiColumnX(x);
        const auto &tt = chip_tile_info(ctx->chip_info, tile);
        for (int i = 0; i < tt.pips.ssize(); i++) {
            PipId pip(tile, i);
            auto ends = impl->gui_pip_ends(pip);
            for (int e = 0; e < 2; e++) {
                std::string name = IdString(tt.wires[e ? tt.pips[i].dst_wire : tt.pips[i].src_wire].name).str(ctx);
                if (name.compare(0, 4, "CPE.") != 0 || name.compare(name.size() - 2, 2, "_n") == 0)
                    continue;
                auto s = spread(name);
                EXPECT_FALSE(std::abs(ends[2 * e] - (column + s.first)) < 1e-4f &&
                             std::abs(ends[2 * e + 1] - (y + s.second)) < 1e-4f)
                        << ctx->nameOfPip(pip) << ": " << name << " has no place";
                checked++;
            }
        }
        break;
    }
    EXPECT_GT(checked, 200);
    // The carry and propagate lines back into the CPE outputs: simple paths, from the lines' ends to the rails and
    // on into CPOUT1 / CPOUT2
    for (BelId bel : ctx->getBels()) {
        if (ctx->getBelType(bel) != id_CPE_LT_U)
            continue;
        std::string tile = ctx->getBelName(bel)[0].str(ctx);
        for (const char *name : {"CPE.CPOUT2_int/CPE.CP_PROP_int", "CPE.CPOUT1_int/CPE.CP_CARRY_int",
                                 "CPE.CP_PROP_int/CPE.POUTY1", "CPE.CP_CARRY_int/CPE.COUTX"}) {
            PipId pip = ctx->getPipByName(IdStringList::parse(ctx, tile + "/" + name));
            ASSERT_NE(pip, PipId()) << name;
            auto g = ctx->getDecalGraphics(ctx->getPipDecal(pip).decal);
            ASSERT_FALSE(g.empty()) << name;
            for (auto &el : g)
                if (el.type == GraphicElement::TYPE_LINE || el.type == GraphicElement::TYPE_ARROW)
                    EXPECT_TRUE(el.x1 == el.x2 || el.y1 == el.y2) << name << ": a slanted line";
            // (one of its lines ends at the destination)
            auto ends = impl->gui_pip_ends(pip);
            bool at_end = false;
            for (auto &el : g)
                at_end |= std::abs(el.x2 - ends[2]) < 1e-4f && std::abs(el.y2 - ends[3]) < 1e-4f;
            EXPECT_TRUE(at_end) << name;
        }
        break;
    }
}
