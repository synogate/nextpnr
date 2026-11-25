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

#include "testing.h"
#include "timing.h"
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

// Multicycle paths (set_multicycle_path of the SDC file) in the timing analysis
class GateMateMulticycleTest : public GateMateTest
{
  protected:
    NetInfo *clk = nullptr;
    CellInfo *slow_from = nullptr, *slow_to = nullptr, *fast_from = nullptr, *fast_to = nullptr;

    // A chain of n inverters
    NetInfo *chain(const std::string &name, int n, NetInfo *src)
    {
        for (int i = 0; i < n; i++)
            src = lut1(name + std::to_string(i), 0b01, src);
        return src;
    }

    // Two register to register paths of one clock (100 MHz, named sys): a slow one (slow_from -> slow_to, eight
    // inverters) and a fast one (fast_from -> fast_to, two)
    void design()
    {
        clk = global_clock("clk");
        slow_from = dff("slow_from", input("a"), clk);
        slow_to = dff("slow_to", chain("slow", 8, slow_from->getPort(id_Q)), clk);
        output("x", slow_to->getPort(id_Q));
        fast_from = dff("fast_from", input("b"), clk);
        fast_to = dff("fast_to", chain("fast", 2, fast_from->getPort(id_Q)), clk);
        output("y", fast_to->getPort(id_Q));
        sdc("create_clock -name sys -period 10 [get_nets {clk$y}]\n");
    }

    void sdc(const std::string &text)
    {
        std::istringstream in(text);
        ctx->read_sdc(in);
    }

    void implement()
    {
        pack();
        ctx->place();
    }

    // The analysis with the critical paths
    std::unique_ptr<TimingAnalyser> analyse()
    {
        auto tmg = std::make_unique<TimingAnalyser>(ctx);
        tmg->setup_only = false;
        tmg->with_clock_skew = true;
        tmg->setup(false, true, true);
        return tmg;
    }

    // The clock the registers are clocked by after packing
    IdString clock_net()
    {
        NetInfo *net = slow_to->getPort(id_CLK_INT) ? slow_to->getPort(id_CLK_INT) : slow_to->getPort(id_CLK);
        return net->name;
    }

    float fmax(TimingAnalyser &tmg) { return tmg.get_timing_result().clock_fmax.at(clock_net()).achieved; }

    // The timing of the setup check at an end point (its data input)
    TimingAnalyser::EndpointTiming endpoint(TimingAnalyser &tmg, CellInfo *ff)
    {
        for (auto &ep : tmg.get_endpoint_timings())
            if (ep.endpoint == CellPortKey(ff->name, id_DIN))
                return ep;
        ADD_FAILURE() << "no end point " << ff->name.c_str(ctx) << ".DIN";
        return TimingAnalyser::EndpointTiming{};
    }

    delay_t period() { return ctx->getDelayFromNS(10); }
};

TEST_F(GateMateMulticycleTest, sdc_command)
{
    design();
    sdc("set_multicycle_path 3 -setup -from [get_cells slow_from] -to [get_pins {slow_to/D}]\n"
        "set_multicycle_path 2 -hold -end -from [get_clocks sys] # a comment\n");
    ASSERT_EQ(ctx->multicycle_paths.size(), 2u);
    auto &setup = ctx->multicycle_paths.at(0);
    EXPECT_EQ(setup.multiplier, 3);
    EXPECT_FALSE(setup.hold);
    EXPECT_FALSE(setup.start);
    ASSERT_EQ(setup.from.size(), 1u);
    EXPECT_EQ(setup.from.at(0).type, TimingPathPoint::CELL);
    EXPECT_EQ(setup.from.at(0).name, ctx->id("slow_from"));
    ASSERT_EQ(setup.to.size(), 1u);
    EXPECT_EQ(setup.to.at(0).type, TimingPathPoint::PIN);
    EXPECT_EQ(setup.to.at(0).pin, id_D);
    auto &hold = ctx->multicycle_paths.at(1);
    EXPECT_EQ(hold.multiplier, 2);
    EXPECT_TRUE(hold.hold);
    EXPECT_FALSE(hold.start);
    ASSERT_EQ(hold.from.size(), 1u);
    EXPECT_EQ(hold.from.at(0).type, TimingPathPoint::CLOCK);
    EXPECT_EQ(hold.from.at(0).name, ctx->id("sys"));
    EXPECT_TRUE(hold.to.empty());
}

// A -from or -to list whose objects all do not exist must not stand for every path
TEST_F(GateMateMulticycleTest, objects_matching_nothing_ignore_the_constraint)
{
    design();
    sdc("set_multicycle_path 2 -from [get_cells no_such_cell] -to [get_cells slow_to]\n"
        "set_multicycle_path 2 -from [get_cells {nothing*}]\n");
    EXPECT_TRUE(ctx->multicycle_paths.empty());
}

// The packer renames the register pins (D to DIN): a pin named in the SDC file stands for the whole register
TEST_F(GateMateMulticycleTest, setup_multicycle_doubles_fmax)
{
    design();
    // (read before packing, as in the flow)
    sdc("set_multicycle_path 2 -from [get_pins slow_from/Q] -to [get_pins slow_to/D]\n");
    ASSERT_EQ(ctx->multicycle_paths.size(), 1u);
    implement();
    auto after = analyse();
    auto slow_after = endpoint(*after, slow_to);
    auto constraints = ctx->multicycle_paths;
    ctx->multicycle_paths.clear();
    auto before = analyse();
    float fmax_before = fmax(*before);
    auto slow_before = endpoint(*before, slow_to);
    ctx->multicycle_paths = constraints;
    EXPECT_NEAR(fmax_before, 1000 / ctx->getDelayNS(slow_before.delay), fmax_before * 0.01f)
            << "fast path " << ctx->getDelayNS(endpoint(*before, fast_to).delay) << " ns";
    // The path has two periods, the same delay
    EXPECT_EQ(slow_after.delay, slow_before.delay);
    EXPECT_EQ(slow_after.setup_slack, slow_before.setup_slack + period());
    // the fast path unchanged
    EXPECT_EQ(endpoint(*after, fast_to).setup_slack, endpoint(*before, fast_to).setup_slack);
    // Fmax: the slow path at half its delay, unless the fast path is slower than that
    auto fast_delay = endpoint(*after, fast_to).delay;
    float expected = 1000 / std::max(ctx->getDelayNS(slow_before.delay) / 2, ctx->getDelayNS(fast_delay));
    EXPECT_GT(fmax(*after), fmax_before * 1.2f);
    EXPECT_NEAR(fmax(*after), expected, expected * 0.02f);
    // The report of the path says so
    auto path = after->get_endpoint_path(slow_after, true);
    EXPECT_EQ(path.setup_cycles, 2);
    EXPECT_EQ(path.max_delay, 2 * period());
}

// Without -hold, the hold check moves with the setup check to one period before it; -hold 1 brings it back
TEST_F(GateMateMulticycleTest, hold_check_follows_setup_unless_given)
{
    design();
    implement();
    auto hold_slack = [&](const std::string &constraints) {
        ctx->multicycle_paths.clear();
        sdc(constraints);
        auto tmg = analyse();
        return endpoint(*tmg, slow_to).hold_slack;
    };
    delay_t plain = hold_slack("");
    std::string setup = "set_multicycle_path 3 -from [get_cells slow_from] -to [get_cells slow_to]\n";
    EXPECT_EQ(hold_slack(setup), plain - 2 * period());
    EXPECT_EQ(hold_slack(setup + "set_multicycle_path 2 -hold -from [get_cells slow_from] -to [get_cells slow_to]\n"),
              plain);
    EXPECT_EQ(hold_slack(setup + "set_multicycle_path 1 -hold -from [get_cells slow_from] -to [get_cells slow_to]\n"),
              plain - period());
}

// get_clocks finds the clock by its name, which stays when the packer copies the constraint behind the clock buffer
TEST_F(GateMateMulticycleTest, clocks_by_name)
{
    design();
    implement();
    ASSERT_NE(ctx->nets.at(clock_net())->clkconstr, nullptr);
    EXPECT_EQ(ctx->nets.at(clock_net())->clkconstr->name, ctx->id("sys"));
    auto before = analyse();
    sdc("set_multicycle_path 4 -from [get_clocks sys] -to [get_clocks s?s]\n");
    auto after = analyse();
    for (CellInfo *ff : {slow_to, fast_to})
        EXPECT_EQ(endpoint(*after, ff).setup_slack, endpoint(*before, ff).setup_slack + 3 * period())
                << ff->name.c_str(ctx);
    EXPECT_NEAR(fmax(*after), 4 * fmax(*before), fmax(*before) * 0.01f);
}

// Of the constraints for a path the most specific wins (-from and -to over -from over -to), of equally specific
// ones the last
TEST_F(GateMateMulticycleTest, most_specific_constraint_wins)
{
    design();
    implement();
    auto before = analyse();
    sdc("set_multicycle_path 2 -from [get_cells slow_from] -to [get_cells slow_to]\n"
        "set_multicycle_path 5 -from [get_cells slow_from]\n"
        "set_multicycle_path 6 -to [get_cells slow_to fast_to]\n"
        "set_multicycle_path 3 -to [get_cells fast_to]\n");
    auto after = analyse();
    EXPECT_EQ(endpoint(*after, slow_to).setup_slack, endpoint(*before, slow_to).setup_slack + period());
    EXPECT_EQ(endpoint(*after, fast_to).setup_slack, endpoint(*before, fast_to).setup_slack + 2 * period());
}

// The placer and router see the criticality of the time the path has: a multicycle path is less critical than an
// ordinary path of the same clock that is slower than its share
TEST_F(GateMateMulticycleTest, criticality_of_the_time_a_path_has)
{
    design();
    implement();
    auto crit = [&](TimingAnalyser &tmg, CellInfo *ff) { return tmg.get_criticality(CellPortKey(ff->name, id_DIN)); };
    auto before = analyse();
    EXPECT_FLOAT_EQ(crit(*before, slow_to), 1.0f);
    EXPECT_LT(crit(*before, fast_to), 1.0f);
    sdc("set_multicycle_path 4 -from [get_cells slow_from] -to [get_cells slow_to]\n");
    auto after = analyse();
    // (4 periods of 10 ns: the slow path is far from critical, the fast one is now the worst of the clock)
    EXPECT_FLOAT_EQ(crit(*after, fast_to), 1.0f);
    EXPECT_LT(crit(*after, slow_to), crit(*before, slow_to));
    // its slack as the router sees it
    EXPECT_EQ(after->get_setup_slack(CellPortKey(slow_to->name, id_DIN)),
              before->get_setup_slack(CellPortKey(slow_to->name, id_DIN)) + 3 * period());
}

// Placed and routed, the paths keep their constraint (the router rewrites the CPE ports)
TEST_F(GateMateMulticycleTest, routed_design)
{
    design();
    sdc("set_multicycle_path 2 -from [get_cells slow_from] -to [get_cells slow_to]\n"
        "set_multicycle_path 1 -hold -from [get_cells slow_from] -to [get_cells slow_to]\n");
    place_and_route();
    auto tmg = analyse();
    auto path = tmg->get_endpoint_path(endpoint(*tmg, slow_to), true);
    EXPECT_EQ(path.setup_cycles, 2);
    EXPECT_EQ(path.hold_shift, 0);
    EXPECT_EQ(path.max_delay, 2 * period());
    for (auto &violation : tmg->get_timing_result().min_delay_violations)
        EXPECT_NE(violation.segments.back().to.first, slow_to->name) << "hold violation at the multicycle path";
    // The report of the multicycle paths: their worst slacks and paths
    auto &slacks = tmg->get_timing_result().multicycle_slacks;
    ASSERT_EQ(slacks.size(), 1u);
    EXPECT_EQ(slacks.at(0).setup_cycles, 2);
    EXPECT_EQ(slacks.at(0).endpoints, 1);
    EXPECT_EQ(slacks.at(0).setup_slack, endpoint(*tmg, slow_to).setup_slack);
    EXPECT_EQ(slacks.at(0).hold_slack, endpoint(*tmg, slow_to).hold_slack);
    ASSERT_FALSE(slacks.at(0).hold_path.segments.empty());
    EXPECT_EQ(slacks.at(0).hold_path.segments.back().to.first, slow_to->name);
    EXPECT_EQ(slacks.at(0).setup_path.segments.back().to.first, slow_to->name);
}

// After placement, repack_ram replaces the RAM halves by a new RAM cell per block ($ram$merged$id..., ...$full),
// with the ports renamed: a constraint naming the RAM (or one of its pins) must name the new cell's ports then.
// It did not, so the router and the final analysis timed the RAM's paths with one period (dmg_soc register file).
TEST_F(GateMateMulticycleTest, ram_keeps_its_constraint_after_the_repack)
{
    for (std::string from : {"[get_cells ram]", "[get_pins {ram/A_DO[0]}]"}) {
        SCOPED_TRACE(from);
        init_context("CCGM1A1", {"--allow-unconstrained"});
        clk = global_clock("clk");
        CellInfo *ram = ctx->createCell(ctx->id("ram"), id_CC_BRAM_20K);
        ram->params[id_RAM_MODE] = Property("TDP");
        for (auto p : {id_A_RD_WIDTH, id_A_WR_WIDTH})
            ram->params[p] = Property(5, 32);
        connect_input(ram, id_A_CLK, clk);
        connect_input(ram, ctx->id("A_ADDR[7]"), input("a"));
        slow_to = dff("after_ram", chain("inv", 4, connect_output(ram, ctx->id("A_DO[0]"))), clk);
        output("x", slow_to->getPort(id_Q));
        sdc("create_clock -name sys -period 10 [get_nets {clk$y}]\n"
            "set_multicycle_path 2 -from " +
            from + "\n");
        ASSERT_EQ(ctx->multicycle_paths.size(), 1u);
        implement();
        EXPECT_FALSE(ctx->cells.count(ctx->id("ram"))) << "the RAM was not replaced by the repack";
        auto tmg = analyse();
        auto path = tmg->get_endpoint_path(endpoint(*tmg, slow_to), true);
        EXPECT_EQ(path.setup_cycles, 2);
        EXPECT_EQ(path.max_delay, 2 * period());
    }
}

// Between the two edges of one clock (falling edge registers feeding rising edge ones, as the register file of
// dmg_soc) the default hold check is at the rising edge half a period before the launch edge. These paths had no
// hold check at all, so neither had a multicycle path between them: with setup 2, its data must take half a
// period (the next data is launched half a period before the capture edge).
TEST_F(GateMateMulticycleTest, hold_check_between_the_two_edges_of_a_clock)
{
    design();
    fast_from->params[id_CLK_INV] = Property(1, 1);
    implement();
    auto check = [&](const std::string &constraints) {
        ctx->multicycle_paths.clear();
        sdc(constraints);
        auto tmg = analyse();
        bool violation = false;
        for (auto &path : tmg->get_timing_result().min_delay_violations)
            violation |= path.segments.back().to.first == fast_to->name;
        return std::make_pair(endpoint(*tmg, fast_to).hold_slack, violation);
    };
    auto plain = check("");
    ASSERT_NE(plain.first, std::numeric_limits<delay_t>::max()) << "no hold check";
    // (the data may change from half a period before the launch edge on)
    EXPECT_GT(plain.first, period() / 2);
    EXPECT_FALSE(plain.second);
    std::string setup = "set_multicycle_path 2 -from [get_cells fast_from] -to [get_cells fast_to]\n";
    auto two = check(setup);
    EXPECT_EQ(two.first, plain.first - period());
    // two inverters take less than half a period
    EXPECT_LT(two.first, 0);
    EXPECT_TRUE(two.second);
    auto back = check(setup + "set_multicycle_path 1 -hold -from [get_cells fast_from] -to [get_cells fast_to]\n");
    EXPECT_EQ(back.first, plain.first);
    EXPECT_FALSE(back.second);
}
