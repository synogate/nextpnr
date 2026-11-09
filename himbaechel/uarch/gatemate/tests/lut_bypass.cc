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

// The router chooses how the two inputs of a LUT2 alone in a half reach L10 / L11: each through one first-level
// LUT2 (one of IN1/TI2 and one of IN3/TI4), or both through the same one (the virtual bypass pips from D1_00_int /
// D1_02_int: that LUT2 computes the function, L10 passes it on), in either order (the virtual input swap of L10) and
// with the subtrees in place or swapped (V_L10X / V_L11X).
//
// One design per combination of two physical CPE inputs of a half (ordered: the first is the LUT2's first input) and
// per way the signals come: the sources are placed so that each signal reaches exactly one of the half's inputs
// without a plane change, so the router must take that input and the routing inside the CPE must make the rest
// work. Two kinds of sources:
//  - far: a register 16 (first input) or 20 (second input) CPEs away, at the position in its 2x2 tile whose SB_BIG
//    puts the signal on the plane of the input. The route must stay on that plane and use exactly one SB_BIG to
//    SB_BIG connection.
//  - near (the TI inputs only): a register in the CPE right below (PINY1 to TI2 / TI6) or left (CINX to TI4, PINX
//    to TI8). The route must take the CP line, without any switch box: it is faster, and leaves the switch boxes to
//    other nets. Where only one input is near, the sink sits where that CPE is at the position of the input's plane
//    in its 2x2 tile too, so that its own switch box reaches the same input (IN2 / IN4 ... behind the same
//    multiplexer). With both TI inputs near, the source left is at the position of the other input of its
//    first-level LUT2 (IN3 / IN7): a switch box route there is a shorter way in that the router must pass over.
// The routed design must compute what the netlist does.

#include <regex>
#include "testing.h"

#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

namespace {

struct BypassCase
{
    bool lower;
    // The physical CPE inputs (1..8: IN1, TI2 (IN2 or a CP line), IN3, TI4, ...) of the LUT2's first and second input
    int pin[2];
    // Whether the source of the input is near (a CP line), else far (16 / 20 CPEs)
    bool near[2];
};

std::string case_name(const BypassCase &c)
{
    std::string s = c.lower ? "lower" : "upper";
    for (int k = 0; k < 2; k++)
        s += stringf("_%s%d_%s", c.pin[k] % 2 ? "IN" : "TI", c.pin[k], c.near[k] ? "cp" : (k ? "far20" : "far16"));
    return s;
}

std::vector<BypassCase> all_cases()
{
    std::vector<BypassCase> cases;
    for (bool lower : {false, true})
        for (int a = 1; a <= 4; a++)
            for (int b = 1; b <= 4; b++) {
                if (a == b)
                    continue;
                for (bool near_a : {false, true})
                    for (bool near_b : {false, true}) {
                        // (only the TI inputs have a CP line)
                        if ((near_a && a % 2) || (near_b && b % 2))
                            continue;
                        int base = lower ? 4 : 0;
                        cases.push_back({lower, {base + a, base + b}, {near_a, near_b}});
                    }
            }
    return cases;
}

// The plane a wire name of a switch box or input multiplexer belongs to ("SB_BIG.P05.Y1", "IM.P05.D0", ...), or 0
int plane_of(const std::string &name)
{
    static const std::regex re("\\.P(\\d\\d)\\.");
    std::smatch m;
    return std::regex_search(name, m, re) ? std::stoi(m[1]) : 0;
}

// A switch box input from another switch box (D2_* to D7_* by distance 2, 4, 8, 12, 16, 20, X12 / X34 diagonal):
// returns the distance (diagonal: 1), or 0
int sb_hop(const std::string &name, bool *big = nullptr)
{
    static const std::regex re("^SB_(BIG|SML)\\.P\\d\\d\\.(D([2-7])_\\d|X12|X34)$");
    std::smatch m;
    if (!std::regex_match(name, m, re))
        return 0;
    if (big)
        *big = m[1] == "BIG";
    if (m[3].matched) {
        static const int distance[] = {2, 4, 8, 12, 16, 20};
        return distance[std::stoi(m[3]) - 2];
    }
    return 1;
}

bool is_switch_box(const std::string &name)
{
    return name.compare(0, 3, "SB_") == 0 || name.compare(0, 3, "IM.") == 0 || name.compare(0, 3, "OM.") == 0;
}

} // namespace

class GateMateLutBypassTest : public GateMateTest, public ::testing::WithParamInterface<BypassCase>
{
  protected:
    std::string tile_name(Loc loc)
    {
        return ctx->getBelName(ctx->getBelByLocation(Loc(loc.x, loc.y, CPE_LT_U_Z)))[0].str(ctx);
    }

    WireId tile_wire(Loc loc, const std::string &name)
    {
        return ctx->getWireByName(IdStringList::concat(ctx->id(tile_name(loc)), ctx->id(name)));
    }

    Loc tile_loc(int tile)
    {
        int x, y;
        tile_xy(ctx->chip_info, tile, x, y);
        return Loc(x, y, 0);
    }

    // The CPE outputs (location, 1: OUT1, 2: OUT2) from which a route on `plane` alone with exactly one switch box
    // connection, from SB_BIG to SB_BIG over `distance`, reaches CPE.IN<plane> of the CPE at `sink`
    std::vector<std::pair<Loc, int>> far_sources(Loc sink, int plane, int distance)
    {
        std::vector<std::pair<Loc, int>> found;
        std::set<std::pair<WireId, int>> visited;
        std::function<void(WireId, int, int)> search = [&](WireId wire, int hops, int depth) {
            if (depth > 10 || !visited.insert({wire, hops}).second)
                return;
            for (PipId pip : ctx->getPipsUphill(wire)) {
                IdStringList name = ctx->getPipName(pip);
                std::string dst = name[1].str(ctx), src = name[2].str(ctx);
                if (dst == "CPE.OUT1" || dst == "CPE.OUT2") {
                    // (the route must have come through a switch box: not the sink's own outputs)
                    if (hops == 1)
                        found.push_back({tile_loc(pip.tile), dst.back() - '0'});
                    continue;
                }
                auto off_plane = [&](const std::string &n) {
                    return plane_of(n) != plane && n.compare(0, 4, "CPE.") != 0;
                };
                if (off_plane(dst) || off_plane(src))
                    continue;
                bool big = false;
                int hop = sb_hop(src, &big);
                if (hop && (hops > 0 || !big || hop != distance))
                    continue;
                // (from the switch box straight into the input multiplexer, not through the neighbours' ones)
                if (src.compare(0, 3, "IM.") == 0 && dst.compare(0, 3, "IM.") == 0 &&
                    src.find(".Y") != std::string::npos)
                    continue;
                // the sink side needs the connection before it reaches a CPE output
                if (hops == 0 && !hop && src.size() > 4 && src.compare(src.size() - 3, 3, ".D0") == 0 &&
                    src.compare(0, 3, "SB_") == 0)
                    continue;
                search(ctx->getPipSrcWire(pip), hops + (hop ? 1 : 0), depth + 1);
            }
        };
        search(tile_wire(sink, stringf("CPE.IN%d", plane)), 0, 0);
        return found;
    }

    // Binds `cell` (with the rest of its cluster) to `bel`, locked; false if that does not fit
    bool place_locked(CellInfo *cell, BelId bel)
    {
        if (bel == BelId())
            return false;
        if (cell->cluster == ClusterId()) {
            if (!ctx->checkBelAvail(bel) || !ctx->isValidBelForCellType(cell->type, bel))
                return false;
            ctx->bindBel(bel, cell, STRENGTH_LOCKED);
            return true;
        }
        Loc at = ctx->getBelLocation(bel);
        for (BelId root_bel : ctx->getBelsByTile(at.x, at.y)) {
            std::vector<std::pair<CellInfo *, BelId>> placement;
            if (!ctx->getClusterPlacement(cell->cluster, root_bel, placement))
                continue;
            bool fits = false, free = true;
            for (auto &p : placement) {
                fits |= p.first == cell && p.second == bel;
                free &= p.second != BelId() && ctx->checkBelAvail(p.second);
            }
            if (!fits || !free)
                continue;
            for (auto &p : placement)
                ctx->bindBel(p.second, p.first, STRENGTH_LOCKED);
            return true;
        }
        return false;
    }

    // The pips of the route of `net` to `wire`, from the sink back to the source
    std::vector<PipId> route_to(const NetInfo *net, WireId wire)
    {
        std::vector<PipId> pips;
        while (net->wires.count(wire) && net->wires.at(wire).pip != PipId() && pips.size() < 100) {
            PipId pip = net->wires.at(wire).pip;
            pips.push_back(pip);
            wire = ctx->getPipSrcWire(pip);
        }
        return pips;
    }

    std::string describe(const std::vector<PipId> &pips)
    {
        std::string s;
        for (auto it = pips.rbegin(); it != pips.rend(); ++it) {
            Loc l = tile_loc(it->tile);
            IdStringList name = ctx->getPipName(*it);
            s += stringf("\n  (%d,%d) %s -> %s", l.x, l.y, name[2].c_str(ctx), name[1].c_str(ctx));
        }
        return s;
    }
};

TEST_P(GateMateLutBypassTest, router_takes_the_forced_inputs)
{
    const BypassCase &c = GetParam();
    init_context("CCGM1A1", {"--allow-unconstrained"});
    // Next to SB_BIGs (a far signal needs no further switch box). The CPE left of (41, 42) is at the position of
    // planes 4 / 8 in its 2x2 tile, the one below (41, 43) at that of planes 2 / 6 (and the one left of (41, 43) at
    // that of planes 3 / 7).
    bool near_below = false;
    for (int k = 0; k < 2; k++)
        near_below |= c.near[k] && c.pin[k] % 4 == 2;
    const Loc sink(41, near_below ? 43 : 42, 0);
    const int half_lt = c.lower ? CPE_LT_L_Z : CPE_LT_U_Z;
    const IdString ports[2] = {id_D0_10, id_D1_10};
    // The CP line into each TI input, and where its source sits
    auto cp_line = [](int pin) -> std::string { return pin == 4 ? "CPE.CINX" : pin == 8 ? "CPE.PINX" : "CPE.PINY1"; };
    auto cp_source = [&](int pin) { return pin % 4 == 0 ? Loc(sink.x - 1, sink.y, 0) : Loc(sink.x, sink.y - 1, 0); };

    bool routed = false;
    auto build = [&] {
        NetInfo *clk = global_clock("clk");
        NetInfo *a = dff("src0", input("in0"), clk)->getPort(id_Q);
        NetInfo *b = dff("src1", input("in1"), clk)->getPort(id_Q);
        // (not symmetric: inputs that trade places show)
        output("out", dff("dst", lut2("sink", 0b0100, a, b), clk)->getPort(id_Q));
    };
    auto after_pack = [&] {
        // The sink LUT2 in the half, its inputs from the registers placed for the forced CPE inputs
        CellInfo *src[2] = {ctx->cells.at(ctx->id("src0")).get(), ctx->cells.at(ctx->id("src1")).get()};
        CellInfo *lut = nullptr;
        for (auto &usr : src[0]->getPort(id_DOUT)->users)
            if (usr.cell->type == id_CPE_L2T4 && usr.port.in(id_D0_10, id_D1_10))
                lut = usr.cell;
        ASSERT_NE(lut, nullptr);
        ASSERT_EQ(lut->getPort(id_D0_10), src[0]->getPort(id_DOUT));
        ASSERT_EQ(lut->getPort(id_D1_10), src[1]->getPort(id_DOUT));
        ASSERT_TRUE(place_locked(lut, ctx->getBelByLocation(Loc(sink.x, sink.y, half_lt))));
        for (int k = 0; k < 2; k++) {
            SCOPED_TRACE(stringf("input %d", k));
            bool placed = false;
            if (c.near[k]) {
                Loc at = cp_source(c.pin[k]);
                for (int z : {CPE_FF_L_Z, CPE_FF_U_Z})
                    if (!placed)
                        placed = place_locked(src[k], ctx->getBelByLocation(Loc(at.x, at.y, z)));
            } else {
                for (auto &s : far_sources(sink, c.pin[k], k ? 20 : 16)) {
                    // OUT1 comes from the lower half, OUT2 from the upper
                    BelId bel =
                            ctx->getBelByLocation(Loc(s.first.x, s.first.y, s.second == 1 ? CPE_FF_L_Z : CPE_FF_U_Z));
                    if ((placed = place_locked(src[k], bel)))
                        break;
                }
            }
            ASSERT_TRUE(placed) << "no place for the source";
        }
    };
    auto after_place = [&] {
        ctx->defer_route_finish = true;
        ctx->route();
        // The routes before finishRoute turns CP lines into cells
        CellInfo *lut = ctx->getBoundBelCell(ctx->getBelByLocation(Loc(sink.x, sink.y, half_lt)));
        ASSERT_NE(lut, nullptr);
        for (int k = 0; k < 2; k++) {
            SCOPED_TRACE(stringf("input %d (pin %d)", k, c.pin[k]));
            const NetInfo *net = lut->getPort(ports[k]);
            ASSERT_NE(net, nullptr);
            auto pips = route_to(net, ctx->getBelPinWire(lut->bel, ports[k]));
            std::string route = describe(pips);
            int hops = 0, big_hops = 0;
            bool to_pin = false;
            for (PipId pip : pips) {
                IdStringList name = ctx->getPipName(pip);
                std::string dst = name[1].str(ctx), src = name[2].str(ctx);
                bool sink_tile = tile_loc(pip.tile) == Loc(sink.x, sink.y, 0);
                if (c.near[k]) {
                    EXPECT_FALSE(is_switch_box(src) || is_switch_box(dst))
                            << "a switch box on the CP line route" << route;
                    to_pin |= sink_tile && src == cp_line(c.pin[k]);
                } else {
                    for (const std::string &n : {src, dst})
                        EXPECT_TRUE(plane_of(n) == 0 || plane_of(n) == c.pin[k]) << "plane change at " << n << route;
                    bool big = false;
                    int hop = sb_hop(src, &big);
                    hops += hop != 0;
                    big_hops += hop != 0 && big;
                    to_pin |= sink_tile && dst == stringf("CPE.IN%d", c.pin[k]);
                }
            }
            if (c.near[k]) {
                EXPECT_TRUE(to_pin) << "not through " << cp_line(c.pin[k]) << route;
            } else {
                EXPECT_TRUE(to_pin) << "not through CPE.IN" << c.pin[k] << route;
                EXPECT_EQ(hops, 1) << "switch box to switch box connections" << route;
                EXPECT_EQ(big_hops, 1) << "SB_BIG to SB_BIG connections" << route;
            }
        }
        ctx->finishRoute();
        routed = true;
    };
    EXPECT_TRUE(implementation_matches_reference(build, 50, 1, after_pack, after_place));
    EXPECT_TRUE(routed);
}

INSTANTIATE_TEST_SUITE_P(Combinations, GateMateLutBypassTest, ::testing::ValuesIn(all_cases()),
                         [](const ::testing::TestParamInfo<BypassCase> &info) { return case_name(info.param); });
