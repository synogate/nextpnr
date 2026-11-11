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

// What the placers take for the delays of the design while it is placed, against what the routing makes of them:
// the estimate of a connection into a cell whose inputs cannot trade places, and the delays of the clocks.

#include "testing.h"

#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

namespace {

struct SelectCase
{
    // The select of the 4:1 multiplexer (0, 1) and where its source is: in the CPE below, else in the one left
    int select;
    bool below;
};

std::string case_name(const SelectCase &c) { return stringf("s%d_from_%s", c.select, c.below ? "below" : "left"); }

} // namespace

class GateMateSelectEstimateTest : public GateMateTest, public ::testing::WithParamInterface<SelectCase>
{
  protected:
    std::string describe(const std::vector<PipId> &pips)
    {
        std::string s;
        for (auto it = pips.rbegin(); it != pips.rend(); ++it) {
            int x, y;
            tile_xy(ctx->chip_info, it->tile, x, y);
            IdStringList name = ctx->getPipName(*it);
            s += stringf("\n  (%d,%d) %s -> %s", x, y, name[2].c_str(ctx), name[1].c_str(ctx));
        }
        return s;
    }
};

// The selects of a 4:1 multiplexer are the first inputs of the two first-level LUTs of the lower half: S0 comes in on
// IN5 / IN6 or on the CP line from the CPE below (PINY1), S1 on IN7 / IN8 or on the CP line from the CPE left (PINX).
// A plain LUT tree can swap its two subtrees and reach either pair from either LUT; a multiplexer cannot. The
// estimates come from the empty chip, where that swap is always there: S1 from the CPE below was estimated at the
// 0.3 ns of the CP line, which only S0 can take, and routed through the switch boxes at three times that (likewise
// S0 from the left). On dmg_soc the worst path of a seed has three such selects, estimated 1.1 ns too fast in all.
TEST_P(GateMateSelectEstimateTest, estimate_of_a_multiplexer_select_is_what_the_routing_takes)
{
    const SelectCase &c = GetParam();
    init_context("CCGM1A1", {"--allow-unconstrained"});
    NetInfo *clk = global_clock("clk");
    NetInfo *sel = dff("src", input("in"), clk)->getPort(id_Q), *other = input("s");
    NetInfo *o = mx4("mux", input("d0"), input("d1"), input("d2"), input("d3"), c.select == 0 ? sel : other,
                     c.select == 0 ? other : sel);
    output("out", dff("dst", o, clk)->getPort(id_Q));
    ctx->pack();

    // Next to a big switch box, as in lut_bypass.cc
    const Loc sink(41, 43, 0);
    CellInfo *mux = ctx->cells.at(ctx->id("mux")).get(), *src = ctx->cells.at(ctx->id("src")).get();
    ASSERT_EQ(mux->type, id_CPE_LT_L);
    ASSERT_TRUE(place_locked(mux, ctx->getBelByLocation(Loc(sink.x, sink.y, CPE_LT_L_Z))));
    Loc at = c.below ? Loc(sink.x, sink.y - 1, 0) : Loc(sink.x - 1, sink.y, 0);
    ASSERT_TRUE(place_locked(src, ctx->getBelByLocation(Loc(at.x, at.y, CPE_FF_L_Z))));
    NetInfo *net = src->getPort(id_DOUT);
    ASSERT_NE(net, nullptr);
    ASSERT_EQ(net->users.entries(), 1u);
    auto user = [&]() -> const PortRef & { return *net->users.begin(); };
    // While the design is placed the multiplexer is two cells, the selects on the lower one
    ASSERT_EQ(user().cell, mux);
    delay_t packed = ctx->predictArcDelay(net, user());

    ctx->place();
    ASSERT_EQ(net->users.entries(), 1u);
    ASSERT_EQ(user().cell->type, id_CPE_MX4);
    delay_t placed = ctx->predictArcDelay(net, user());
    EXPECT_EQ(packed, placed) << "the estimate changed when the halves of the multiplexer became one cell";

    ctx->defer_route_finish = true;
    ctx->route();
    delay_t routed = ctx->getNetinfoRouteDelay(net, user());
    std::string route = describe(route_to(net, ctx->getNetinfoSinkWire(net, user(), 0)));
    ctx->finishRoute();

    EXPECT_NEAR(placed, routed, 100) << "estimate " << placed << " ps, routed " << routed << " ps:" << route;
    // S0 has the CP line from below, S1 the one from the left (0.4 ns from a register); the other way round it is
    // the switch boxes
    if (c.below == (c.select == 0))
        EXPECT_LT(routed, 500) << route;
    else
        EXPECT_GT(routed, 600) << route;
}

INSTANTIATE_TEST_SUITE_P(Selects, GateMateSelectEstimateTest,
                         ::testing::Values(SelectCase{0, true}, SelectCase{0, false}, SelectCase{1, true},
                                           SelectCase{1, false}),
                         [](const ::testing::TestParamInfo<SelectCase> &info) { return case_name(info.param); });

class GateMateClockEstimateTest : public GateMateTest
{
};

// The placers analyse the timing with the clock skew (placer/clockSkew), from the estimates of the clock delays. A
// global clock reaches the clock inputs of the block RAMs with no delay, and the registers after 0.8 to 2.4 ns. The
// search of the clock network that the estimates come from stops before it has seen all of the RAMs' clock inputs:
// the others were estimated like any net, 2.2 ns through the switch boxes.
TEST_F(GateMateClockEstimateTest, clock_inputs_of_block_rams_are_estimated_without_delay)
{
    BelId glb;
    IdString glb_pin;
    for (BelId bel : ctx->getBels())
        if (ctx->getBelType(bel) == id_GLBOUT && glb == BelId())
            for (IdString pin : ctx->getBelPins(bel))
                if (ctx->getBelPinType(bel, pin) == PORT_OUT && ctx->getBelPinWire(bel, pin) != WireId()) {
                    glb = bel;
                    glb_pin = pin;
                    break;
                }
    ASSERT_NE(glb, BelId());
    int inputs = 0;
    for (BelId bel : ctx->getBels()) {
        if (ctx->getBelBucketForBel(bel) != id_RAM_HALF)
            continue;
        for (IdString pin : {id_CLOCK1, id_CLOCK2, id_CLOCK3, id_CLOCK4}) {
            if (ctx->getBelPinWire(bel, pin) == WireId())
                continue;
            inputs++;
            EXPECT_EQ(impl->predictDelay(glb, glb_pin, bel, pin), 0)
                    << ctx->nameOfBel(bel) << " " << pin.c_str(ctx) << " from " << glb_pin.c_str(ctx);
        }
    }
    EXPECT_GT(inputs, 0);
    EXPECT_TRUE(ctx->setting<bool>("placer/clockSkew", false));
}

// The estimates of the clock delays of a placed design are the delays of the routed one, at the registers and at
// the block RAM
TEST_F(GateMateClockEstimateTest, clock_estimates_are_the_routed_delays)
{
    NetInfo *clk = global_clock("clk");
    CellInfo *ram = ctx->createCell(ctx->id("ram"), id_CC_BRAM_20K);
    ram->params[id_RAM_MODE] = Property("TDP");
    for (auto p : {id_A_RD_WIDTH, id_A_WR_WIDTH})
        ram->params[p] = Property(5, 32);
    connect_input(ram, id_A_CLK, clk);
    connect_input(ram, ctx->id("A_ADDR[7]"), dff("address", input("a"), clk)->getPort(id_Q));
    output("o", dff("data", connect_output(ram, ctx->id("A_DO[0]")), clk)->getPort(id_Q));
    ctx->pack();
    ctx->place();

    NetInfo *global = nullptr;
    for (auto &net : ctx->nets)
        if (net.second->driver.cell && net.second->driver.cell->bel != BelId() &&
            ctx->getBelType(net.second->driver.cell->bel) == id_GLBOUT)
            global = net.second.get();
    ASSERT_NE(global, nullptr);
    dict<IdString, delay_t> estimates;
    for (auto &usr : global->users)
        estimates[usr.cell->name] = ctx->predictArcDelay(global, usr);

    ctx->defer_route_finish = true;
    ctx->route();
    int rams = 0, registers = 0;
    for (auto &usr : global->users) {
        ASSERT_TRUE(estimates.count(usr.cell->name)) << usr.cell->name.c_str(ctx);
        delay_t routed = ctx->getNetinfoRouteDelay(global, usr);
        EXPECT_NEAR(estimates.at(usr.cell->name), routed, 50)
                << usr.cell->name.c_str(ctx) << "." << usr.port.c_str(ctx);
        if (usr.cell->type == id_RAM) {
            rams++;
            EXPECT_EQ(routed, 0);
        } else if (usr.cell->type == id_CPE_FF) {
            registers++;
            EXPECT_GT(routed, 500);
        }
    }
    EXPECT_EQ(rams, 1);
    EXPECT_GE(registers, 2);
    ctx->finishRoute();
}
