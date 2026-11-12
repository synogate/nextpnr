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

#include <random>
#include <regex>
#include "placer_static.h"
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

class GateMateRefinementTest : public GateMateTest
{
  protected:
    // Registers between random LUTs of the inputs and of each other
    void build()
    {
        std::mt19937 rng(7);
        NetInfo *clk = global_clock("clk");
        std::vector<NetInfo *> nets;
        for (int i = 0; i < 6; i++)
            nets.push_back(input(stringf("i%d", i)));
        const int tables[] = {0b1000, 0b1110, 0b0110, 0b1001, 0b0111, 0b0001};
        for (int n = 0; n < 120; n++) {
            NetInfo *x = nets[rng() % nets.size()], *y = nets[rng() % nets.size()];
            NetInfo *f = lut2(stringf("f%d", n), tables[rng() % 6], x, y);
            nets.push_back(n % 3 ? f : dff(stringf("r%d", n), f, clk)->getPort(id_Q));
        }
        for (int i = 0; i < 8; i++)
            output(stringf("o%d", i), nets[nets.size() - 1 - i]);
    }

    // The temperature the refinement starts at and the number of its last iteration, from the log
    std::pair<double, int> refinement(const std::string &log)
    {
        std::smatch m;
        std::pair<double, int> result(-1, 0);
        if (std::regex_search(log, m, std::regex("at iteration #1: temp = ([0-9.]+)")))
            result.first = std::stod(m[1]);
        std::regex line("at iteration #([0-9]+): temp = ");
        for (auto it = std::sregex_iterator(log.begin(), log.end(), line); it != std::sregex_iterator(); ++it)
            result.second = std::max(result.second, std::stoi((*it)[1]));
        return result;
    }
};

// The refinement of the analytic placement takes only the moves that improve the placement, unless it starts
// warm (--vopt refine-temp, setting placer1/refineTemp): then it anneals, over several times the iterations
// (dmg_soc at 3e-5: 84 iterations against 20, Fmax +3.4% over 16 seeds)
TEST_F(GateMateRefinementTest, refinement_starts_at_the_temperature_given)
{
    std::pair<double, int> cold, warm;
    {
        LogCapture info(LogLevel::INFO_MSG);
        build();
        ctx->pack();
        ctx->place();
        cold = refinement(info.str());
    }
    init_context("CCGM1A1", {"--allow-unconstrained", "--refine-temp=3e-5"});
    EXPECT_NEAR(ctx->setting<float>("placer1/refineTemp", 0.0f), 3e-5, 1e-9);
    {
        LogCapture info(LogLevel::INFO_MSG);
        build();
        ctx->pack();
        ctx->place();
        warm = refinement(info.str());
    }
    EXPECT_NEAR(cold.first, 0.0, 1e-6);
    EXPECT_NEAR(warm.first, 3e-5, 1e-6);
    EXPECT_GT(cold.second, 0);
    EXPECT_GT(warm.second, cold.second + 10);
}

TEST_F(GateMateRefinementTest, annealed_design_matches_reference)
{
    init_context("CCGM1A1", {"--allow-unconstrained", "--refine-temp=3e-5"});
    EXPECT_TRUE(implementation_matches_reference([&] { build(); }, 200, 1));
}

// The electrostatic placer (--placer static) lays the modules of a design out as islands where the analytic one
// wraps them around each other (dmg_soc: 17% less wirelength, docs/timing_experiments.md). On GateMate it takes its
// delays from the placer's estimates and can pull the ends of critical arcs together (placerStatic/arcWeight).
class GateMateStaticPlacerTest : public GateMateRefinementTest
{
  protected:
    void use_static(const std::vector<std::string> &settings = {})
    {
        init_context("CCGM1A1", {"--allow-unconstrained"});
        ctx->settings[ctx->id("placer")] = std::string("static");
        for (size_t i = 0; i + 1 < settings.size(); i += 2)
            ctx->settings[ctx->id(settings[i])] = settings[i + 1];
    }

    // Places the design and returns the log
    std::string place()
    {
        LogCapture info(LogLevel::INFO_MSG);
        build();
        ctx->pack();
        ctx->place();
        return info.str();
    }

    // The wirelength the refinement ends with
    static int final_wirelength(const std::string &log)
    {
        int result = -1;
        std::regex line("at iteration #[0-9]+: temp = [0-9.]+, timing cost = [0-9]+, wirelen = ([0-9]+)");
        for (auto it = std::sregex_iterator(log.begin(), log.end(), line); it != std::sregex_iterator(); ++it)
            result = std::stoi((*it)[1]);
        return result;
    }
};

// Without bels the placer cannot ask predictDelay; its own default (0.1 ns and 0.1 ns per tile) is far from a chip
// where the CPE next door costs 0.5 to 1 ns and a tile more a few 10 ps (dmg_soc: Fmax +4.6% with these delays)
TEST_F(GateMateStaticPlacerTest, delays_are_those_of_the_estimates)
{
    use_static();
    build();
    ctx->pack();
    // (as Arch::place sets the placer up)
    PlacerStaticCfg cfg(ctx);
    impl->configurePlacerStatic(cfg);
    ASSERT_TRUE(bool(cfg.predict_delay));
    auto across = [&](int tiles) {
        return int(cfg.predict_delay(ctx, cfg, Loc(20, 60, 0), IdString(), Loc(20 + tiles, 60, 0), IdString()));
    };
    int next = across(1), d4 = across(4), d10 = across(10), d40 = across(40);
    EXPECT_GT(next, 400);
    EXPECT_LT(next, 1500);
    EXPECT_GT(d10, d4);
    EXPECT_GT(d40, d10);
    // (the default would give 4.1 ns)
    EXPECT_LT(d40, 3500);
    EXPECT_LT(d40 - next, 40 * 60);
}

TEST_F(GateMateStaticPlacerTest, placed_design_matches_reference)
{
    use_static();
    EXPECT_TRUE(implementation_matches_reference([&] { build(); }, 200, 1));
}

// The pull is towards the other end of the arc. On all arcs alike (no threshold, exponent 0) it is three more
// two-pin nets per arc: the wirelength stays about what it was (772 and 769); pushed apart it would triple
TEST_F(GateMateStaticPlacerTest, critical_arcs_pull_their_ends_together)
{
    use_static();
    // (on by default on GateMate, for the critical arcs)
    EXPECT_NEAR(PlacerStaticCfg(ctx).arc_weight, 3.0f, 1e-6);
    EXPECT_NEAR(PlacerStaticCfg(ctx).arc_min_crit, 0.5f, 1e-6);
    use_static({"placerStatic/arcWeight", "0"});
    int plain = final_wirelength(place());
    use_static({"placerStatic/arcWeight", "3", "placerStatic/arcMinCrit", "0", "placerStatic/arcExponent", "0"});
    int pulled = final_wirelength(place());
    ASSERT_GT(plain, 0);
    ASSERT_GT(pulled, 0);
    EXPECT_LT(pulled, plain * 3 / 2);
}

TEST_F(GateMateStaticPlacerTest, design_placed_without_the_pull_matches_reference)
{
    use_static({"placerStatic/arcWeight", "0"});
    EXPECT_TRUE(implementation_matches_reference([&] { build(); }, 200, 1));
}

// The block RAMs get their places while the solver still has the logic on a heap, and the RAMs of a module nearly on
// one spot. The strict legaliser then gave each RAM the first free place it drew around that spot, one after the
// other (dmg_soc: 35 tiles from the solver's position on average, the 13 RAMs of the caches all over three
// columns, the FIFO in front of the HyperBus pads 72 to 88 tiles from them in 6 of 16 seeds). With
// placerStatic/ipAssign they go to their places as a whole: each where the weighted wirelength of its nets is
// least, then moves and exchanges while the total falls.
class GateMateStaticRamTest : public GateMateStaticPlacerTest
{
  protected:
    // `count` 20K block RAMs read at one address, which comes from registers, with a register for a data bit each
    void build_rams(int count)
    {
        NetInfo *clk = global_clock("clk");
        std::vector<NetInfo *> address;
        for (int i = 0; i < 6; i++)
            address.push_back(dff(stringf("a%d", i), input(stringf("i%d", i)), clk)->getPort(id_Q));
        for (int r = 0; r < count; r++) {
            CellInfo *ram = ctx->createCell(ctx->idf("ram%d", r), id_CC_BRAM_20K);
            ram->params[id_RAM_MODE] = Property("TDP");
            for (auto p : {id_A_RD_WIDTH, id_A_WR_WIDTH})
                ram->params[p] = Property(5, 32);
            connect_input(ram, id_A_CLK, clk);
            connect_input(ram, id_A_EN, vcc());
            connect_input(ram, id_A_WE, gnd());
            for (int i = 0; i < 6; i++)
                connect_input(ram, ctx->idf("A_ADDR[%d]", i + 7), address[i]);
            output(stringf("o%d", r),
                   dff(stringf("q%d", r), connect_output(ram, ctx->id("A_DO[0]")), clk)->getPort(id_Q));
        }
    }

    // Places the RAM design; returns the log, with the places the RAMs got from the placer's step for them
    std::string place_rams(int count)
    {
        LogCapture info(LogLevel::INFO_MSG);
        build_rams(count);
        ctx->pack();
        ctx->verbose = true;
        ctx->place();
        ctx->verbose = false;
        return info.str();
    }

    // The blocks the RAMs went to in that step (before the refinement, which moves them in a design as small as
    // this), and the half perimeter of the box around them
    static std::pair<int, int> ram_blocks(const std::string &log)
    {
        pool<std::pair<int, int>> blocks;
        int x0 = 1000, x1 = -1, y0 = 1000, y1 = -1;
        std::regex line("solver [(][0-9.]+, [0-9.]+[)] -> X([0-9]+)Y([0-9]+)/(RAM_HALF_L|RAM)");
        for (auto it = std::sregex_iterator(log.begin(), log.end(), line); it != std::sregex_iterator(); ++it) {
            int x = std::stoi((*it)[1]), y = std::stoi((*it)[2]) - ((*it)[3] == "RAM_HALF_L" ? 8 : 0);
            x0 = std::min(x0, x);
            x1 = std::max(x1, x);
            y0 = std::min(y0, y);
            y1 = std::max(y1, y);
            blocks.emplace(x, y);
        }
        return std::make_pair(int(blocks.size()), blocks.empty() ? -1 : (x1 - x0) + (y1 - y0));
    }
};

TEST_F(GateMateStaticRamTest, block_rams_of_a_module_take_neighbouring_places)
{
    use_static();
    // (on by default on GateMate)
    EXPECT_TRUE(PlacerStaticCfg(ctx).ip_assign);
    use_static({"placerStatic/ipAssign", "0"});
    std::string log = place_rams(6);
    EXPECT_EQ(log.find("IP macros as a whole"), std::string::npos);
    auto drawn = ram_blocks(log);
    use_static({"placerStatic/ipAssign", "1"});
    log = place_rams(6);
    std::smatch m;
    ASSERT_TRUE(std::regex_search(log, m,
                                  std::regex("Placed ([0-9]+) of ([0-9]+) IP macros as a whole: weighted wirelength of "
                                             "their nets ([0-9]+) at the solver's positions, ([0-9]+) at the first "
                                             "places, ([0-9]+) after")));
    EXPECT_EQ(std::stoi(m[1]), 6);
    EXPECT_EQ(std::stoi(m[2]), 6);
    EXPECT_LE(std::stoi(m[5]), std::stoi(m[4]));
    auto assigned = ram_blocks(log);
    RecordProperty("drawn", stringf("%d blocks, box %d", drawn.first, drawn.second));
    RecordProperty("assigned", stringf("%d blocks, box %d", assigned.first, assigned.second));
    // Six halves in three blocks next to each other
    EXPECT_EQ(assigned.first, 3);
    EXPECT_LE(assigned.second, 48);
    EXPECT_GT(drawn.first, 3);
    EXPECT_GT(drawn.second, assigned.second);
}

TEST_F(GateMateStaticRamTest, design_with_assigned_block_rams_matches_reference)
{
    use_static();
    EXPECT_TRUE(implementation_matches_reference([&] { build_rams(6); }, 100, 1));
}
