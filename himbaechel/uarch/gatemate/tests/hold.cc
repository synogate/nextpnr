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

#include <map>
#include "testing.h"
#include "timing.h"
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

// The hold model (least routing delays, exact hold slack at every port, the slack accessors for hold fixing) and the
// hold check of the finished routing in all timing corners (hold-corners)
class GateMateHoldTest : public GateMateTest
{
  protected:
    NetInfo *clk = nullptr;
    CellInfo *from = nullptr, *short_to = nullptr, *long_to = nullptr;

    // A chain of n inverters
    NetInfo *chain(const std::string &name, int n, NetInfo *src)
    {
        for (int i = 0; i < n; i++)
            src = lut1(name + std::to_string(i), 0b01, src);
        return src;
    }

    // One register (from) feeding a short path (one inverter, to short_to) and a long one (that inverter and six
    // more, to long_to), of one clock at 100 MHz named sys
    void design()
    {
        clk = global_clock("clk");
        from = dff("from", input("a"), clk);
        NetInfo *x = lut1("x", 0b01, from->getPort(id_Q));
        short_to = dff("short_to", x, clk);
        long_to = dff("long_to", chain("long", 6, x), clk);
        output("s", short_to->getPort(id_Q));
        output("l", long_to->getPort(id_Q));
        sdc("create_clock -name sys -period 10 [get_nets {clk$y}]\n");
    }

    void sdc(const std::string &text)
    {
        std::istringstream in(text);
        ctx->read_sdc(in);
    }

    std::unique_ptr<TimingAnalyser> analyse()
    {
        auto tmg = std::make_unique<TimingAnalyser>(ctx);
        tmg->setup_only = false;
        tmg->with_clock_skew = true;
        tmg->setup(false, true, true);
        return tmg;
    }

    // The timings of an end point (one per launch domain)
    std::vector<TimingAnalyser::EndpointTiming> endpoints(TimingAnalyser &tmg, CellPortKey port)
    {
        std::vector<TimingAnalyser::EndpointTiming> found;
        for (auto &ep : tmg.get_endpoint_timings())
            if (ep.endpoint == port)
                found.push_back(ep);
        return found;
    }
    TimingAnalyser::EndpointTiming endpoint(TimingAnalyser &tmg, CellInfo *ff)
    {
        auto found = endpoints(tmg, CellPortKey(ff->name, id_DIN));
        if (found.size() != 1) {
            ADD_FAILURE() << found.size() << " timings of end point " << ff->name.c_str(ctx) << ".DIN";
            return TimingAnalyser::EndpointTiming{};
        }
        return found.front();
    }

    // The input ports of the shortest path into an end point (the sinks of its routing segments)
    std::vector<CellPortKey> path_inputs(TimingAnalyser &tmg, const TimingAnalyser::EndpointTiming &ep)
    {
        std::vector<CellPortKey> inputs;
        for (auto &seg : tmg.get_endpoint_path(ep, false).segments)
            if (seg.type == CriticalPath::Segment::Type::ROUTING)
                inputs.emplace_back(seg.to.first, seg.to.second);
        return inputs;
    }

    // Everything the placer and the router decided: the bel of every cell and the routing of every net
    std::string implementation()
    {
        std::map<std::string, std::string> lines;
        for (auto &cell : ctx->cells)
            lines["cell " + cell.first.str(ctx)] = cell.second->bel == BelId() ? "-" : ctx->nameOfBel(cell.second->bel);
        for (auto &net : ctx->nets) {
            std::map<std::string, std::string> wires;
            for (auto &w : net.second->wires)
                wires[ctx->nameOfWire(w.first)] = w.second.pip == PipId() ? "-" : ctx->nameOfPip(w.second.pip);
            std::string text;
            for (auto &w : wires)
                text += w.first + "<" + w.second + " ";
            lines["net " + net.first.str(ctx)] = text;
        }
        std::string all;
        for (auto &l : lines)
            all += l.first + ": " + l.second + "\n";
        return all;
    }

    std::string grade() const { return IdString(ctx->speed_grade->name).str(ctx); }
};

TEST_F(GateMateHoldTest, hold_corners_option)
{
    using Corners = std::vector<std::pair<int, int>>;
    // all corners of the performance mode by default (speed, worst unless given)
    EXPECT_EQ(impl->hold_corners, (Corners{{1, 3}, {2, 3}, {3, 3}}));
    init_context("CCGM1A1", {"--allow-unconstrained", "--fpga_mode=economy"});
    EXPECT_EQ(impl->hold_corners, (Corners{{1, 2}, {2, 2}, {3, 2}}));
    init_context("CCGM1A1", {"--allow-unconstrained", "--hold-corners=routed"});
    EXPECT_TRUE(impl->hold_corners.empty());
    init_context("CCGM1A1", {"--allow-unconstrained", "--hold-corners=best, worst_eco,best"});
    EXPECT_EQ(impl->hold_corners, (Corners{{1, 3}, {3, 2}}));
    // timing-corners shares the syntax, without routed
    init_context("CCGM1A1", {"--allow-unconstrained", "--timing-corners=all", "--hold-corners=typ_lpr"});
    EXPECT_EQ(impl->check_corners, (Corners{{1, 3}, {2, 3}, {3, 3}}));
    EXPECT_EQ(impl->hold_corners, (Corners{{2, 1}}));
    EXPECT_LOG_ERROR(init_context("CCGM1A1", {"--allow-unconstrained", "--hold-corners=fast"}),
                     "Invalid timing corner 'fast'");
    EXPECT_LOG_ERROR(init_context("CCGM1A1", {"--allow-unconstrained", "--timing-corners=routed"}),
                     "Invalid timing corner 'routed'");
    init_context();
    EXPECT_TRUE(impl->hold_min_delays);
    EXPECT_TRUE(ctx->min_pip_delays);
    init_context("CCGM1A1", {"--allow-unconstrained", "--hold-min-delays=0"});
    EXPECT_FALSE(ctx->min_pip_delays);
}

// The finished routing has its hold checked in best, typ and worst of the performance mode; the speed grade and the
// timing result stay those of the routed corner
TEST_F(GateMateHoldTest, all_corners_checked_by_default)
{
    design();
    std::string log;
    {
        LogCapture capture;
        place_and_route();
        log = capture.str();
    }
    for (const char *corner : {"best_spd", "typ_spd", "worst_spd"})
        EXPECT_NE(log.find(std::string("Hold check in timing corner '") + corner + "': 0 violation(s)"),
                  std::string::npos)
                << corner << " in:\n"
                << log;
    EXPECT_NE(log.find("Hold check of 2 further timing corner(s)"), std::string::npos);
    EXPECT_EQ(grade(), "worst_spd");
    auto tmg = analyse();
    for (auto &fmax : tmg->get_timing_result().clock_fmax)
        EXPECT_EQ(ctx->timing_result.clock_fmax.at(fmax.first).achieved, fmax.second.achieved);
}

TEST_F(GateMateHoldTest, routed_restricts_the_hold_check)
{
    init_context("CCGM1A1", {"--allow-unconstrained", "--hold-corners=routed"});
    design();
    LogCapture capture;
    place_and_route();
    std::string log = capture.str();
    EXPECT_NE(log.find("Hold check in timing corner 'worst_spd'"), std::string::npos);
    EXPECT_EQ(log.find("Hold check in timing corner 'best_spd'"), std::string::npos);
    EXPECT_EQ(log.find("Hold check in timing corner 'typ_spd'"), std::string::npos);
    EXPECT_EQ(log.find("further timing corner"), std::string::npos);
}

// A hold violation in any corner is an error (unless timing/allowFail), raised after all corners are reported and
// naming the failing ones
TEST_F(GateMateHoldTest, violations_name_the_failing_corners)
{
    design();
    // (a setup multicycle alone moves the hold check one period later: the short path fails it everywhere)
    sdc("set_multicycle_path 2 -setup -to [get_cells short_to]\n");
    bool had_error = had_nonfatal_error;
    std::string errors, warnings;
    {
        LogCapture capture(LogLevel::ERROR_MSG);
        place_and_route();
        errors = capture.str();
    }
    had_nonfatal_error = had_error;
    EXPECT_NE(errors.find("Hold/min time violation for clock 'posedge"), std::string::npos) << errors;
    EXPECT_NE(errors.find("in timing corner 'best_spd'"), std::string::npos) << errors;
    EXPECT_NE(errors.find("Hold violations in timing corner(s) 'worst_spd', 'best_spd', 'typ_spd'"), std::string::npos)
            << errors;
    // the final message comes after the reports of all corners
    EXPECT_GT(errors.rfind("Hold violations in timing corner(s)"), errors.rfind("in timing corner 'typ_spd'"));

    init_context();
    design();
    sdc("set_multicycle_path 2 -setup -to [get_cells short_to]\n");
    ctx->settings[ctx->id("timing/allowFail")] = true;
    {
        LogCapture capture(LogLevel::WARNING_MSG);
        place_and_route();
        warnings = capture.str();
    }
    EXPECT_EQ(warnings.find("ERROR"), std::string::npos) << warnings;
    EXPECT_NE(warnings.find("Hold violations in timing corner(s) 'worst_spd', 'best_spd', 'typ_spd'"),
              std::string::npos)
            << warnings;
}

// Data arcs have the least routing delay of the speed grade below the greatest; clock pins one value; the greatest is
// the routing delay the routers and the setup analysis have
TEST_F(GateMateHoldTest, least_routing_delays_on_data_arcs_only)
{
    design();
    place_and_route();
    auto check = [&](bool min_delays) {
        ctx->min_pip_delays = min_delays;
        auto tmg = analyse();
        int data = 0, data_less = 0, clocks = 0;
        for (auto &net : ctx->nets) {
            NetInfo *ni = net.second.get();
            if (ni->driver.cell == nullptr || ni->wires.empty())
                continue;
            for (auto &usr : ni->users) {
                if (usr.cell->bel == BelId())
                    continue;
                DelayPair delay = tmg->get_route_delay(CellPortKey(usr));
                EXPECT_EQ(delay.maxDelay(), ctx->getNetinfoRouteDelay(ni, usr));
                EXPECT_LE(delay.minDelay(), delay.maxDelay());
                int count = 0;
                if (ctx->getPortTimingClass(usr.cell, usr.port, count) == TMG_CLOCK_INPUT) {
                    EXPECT_EQ(delay.minDelay(), delay.maxDelay()) << ni->name.c_str(ctx);
                    clocks++;
                } else if (delay.maxDelay() > 0) {
                    data++;
                    data_less += delay.minDelay() < delay.maxDelay();
                    if (!min_delays)
                        EXPECT_EQ(delay.minDelay(), delay.maxDelay());
                }
            }
        }
        EXPECT_GT(clocks, 0);
        EXPECT_GT(data, 5);
        return std::make_pair(data, data_less);
    };
    auto with = check(true);
    EXPECT_GT(with.second * 10, with.first * 9) << with.second << " of " << with.first
                                                << " routed data arcs have a lesser least delay";
    EXPECT_EQ(check(false).second, 0);
    ctx->min_pip_delays = true;
}

// The hold part of the required time goes back by the least delays: along a single path, every input port has the
// hold slack of its end point (and the setup slack of the accessor that of the end point too)
TEST_F(GateMateHoldTest, interior_slack_is_the_end_points_along_a_path)
{
    design();
    place_and_route();
    auto tmg = analyse();
    auto ep = endpoint(*tmg, long_to);
    ASSERT_NE(ep.hold_slack, std::numeric_limits<delay_t>::max());
    auto inputs = path_inputs(*tmg, ep);
    ASSERT_GE(inputs.size(), 3u);
    // Where the paths share ports (the inverter x, unless the packer copied it into the CPE of the next inverter),
    // the hold slack is the lesser of the two
    auto short_ep = endpoint(*tmg, short_to);
    auto short_inputs = path_inputs(*tmg, short_ep);
    pool<CellPortKey> shared(short_inputs.begin(), short_inputs.end());
    int own = 0;
    for (auto &port : inputs) {
        bool both = shared.count(port) > 0;
        own += !both;
        EXPECT_EQ(tmg->get_hold_slack(port), both ? std::min(ep.hold_slack, short_ep.hold_slack) : ep.hold_slack)
                << port.cell.c_str(ctx) << "." << port.port.c_str(ctx);
        EXPECT_EQ(tmg->get_absolute_setup_slack(port), both ? std::min(ep.setup_slack, short_ep.setup_slack)
                                                            : ep.setup_slack)
                << port.cell.c_str(ctx) << "." << port.port.c_str(ctx);
    }
    EXPECT_GE(own, 3);
}

// The accessors: hold slack <= 0 exactly on the ports a violating path passes, at end points the slacks of
// get_endpoint_timings; the setup slack over all domain pairs (also a multicycle one)
TEST_F(GateMateHoldTest, slack_accessors)
{
    design();
    // the short path violates hold (its check moves one period later), the long one does not
    sdc("set_multicycle_path 2 -setup -to [get_cells short_to]\n");
    ctx->settings[ctx->id("timing/allowFail")] = true;
    place_and_route();
    auto tmg = analyse();
    auto short_ep = endpoint(*tmg, short_to), long_ep = endpoint(*tmg, long_to);
    EXPECT_LE(short_ep.hold_slack, 0);
    EXPECT_GT(long_ep.hold_slack, 0);
    auto short_inputs = path_inputs(*tmg, short_ep), long_inputs = path_inputs(*tmg, long_ep);
    pool<CellPortKey> on_short(short_inputs.begin(), short_inputs.end());
    for (auto &port : short_inputs)
        EXPECT_LE(tmg->get_hold_slack(port), short_ep.hold_slack)
                << port.cell.c_str(ctx) << "." << port.port.c_str(ctx);
    for (auto &port : long_inputs)
        if (!on_short.count(port))
            EXPECT_EQ(tmg->get_hold_slack(port), long_ep.hold_slack)
                    << port.cell.c_str(ctx) << "." << port.port.c_str(ctx);
    // the multicycle path has one more period for setup, which the absolute setup slack has
    EXPECT_GT(short_ep.setup_slack, ctx->getDelayFromNS(10));
    EXPECT_EQ(tmg->get_absolute_setup_slack(CellPortKey(short_to->name, id_DIN)), short_ep.setup_slack);
    // every port: <= 0 exactly where a violating path passes (only the short path violates), and at every end
    // point the least slacks of its timings
    int ports = 0;
    for (auto &cell : ctx->cells)
        for (auto &port : cell.second->ports) {
            if (port.second.type != PORT_IN || port.second.net == nullptr || cell.second->bel == BelId())
                continue;
            CellPortKey key(cell.first, port.first);
            delay_t hold = tmg->get_hold_slack(key);
            EXPECT_EQ(hold <= 0, on_short.count(key) > 0) << cell.first.c_str(ctx) << "." << port.first.c_str(ctx);
            auto timings = endpoints(*tmg, key);
            if (timings.empty())
                continue;
            delay_t ep_hold = std::numeric_limits<delay_t>::max(), ep_setup = std::numeric_limits<delay_t>::max();
            for (auto &t : timings) {
                ep_hold = std::min(ep_hold, t.hold_slack);
                ep_setup = std::min(ep_setup, t.setup_slack);
            }
            EXPECT_EQ(hold, ep_hold) << cell.first.c_str(ctx) << "." << port.first.c_str(ctx);
            EXPECT_EQ(tmg->get_absolute_setup_slack(key), ep_setup)
                    << cell.first.c_str(ctx) << "." << port.first.c_str(ctx);
            ports++;
        }
    EXPECT_GE(ports, 3);
    // no hold analysis, no hold slack
    TimingAnalyser setup_only(ctx);
    setup_only.setup_only = true;
    setup_only.setup();
    EXPECT_EQ(setup_only.get_hold_slack(short_inputs.back()), std::numeric_limits<delay_t>::max());
}

// The least routing delays change the hold analysis only: the placement, the routing and every setup number are the
// same with hold-min-delays=0, the hold slacks no greater with it on
TEST_F(GateMateHoldTest, setup_unchanged_by_least_delays)
{
    auto run = [&](bool min_delays) {
        std::vector<std::string> vopts = {"--allow-unconstrained"};
        if (!min_delays)
            vopts.push_back("--hold-min-delays=0");
        init_context("CCGM1A1", vopts);
        design();
        place_and_route();
        return implementation();
    };
    // (the timings by name: the context goes with the next run)
    auto timings = [&]() {
        auto tmg = analyse();
        std::map<std::string, std::vector<double>> by_port;
        for (auto &ep : tmg->get_endpoint_timings())
            by_port[ep.endpoint.cell.str(ctx) + "." + ep.endpoint.port.str(ctx) + " " +
                    ep.clock_pair.start.clock.str(ctx)] = {double(ep.delay), double(ep.setup_slack),
                                                          double(ep.hold_slack)};
        for (auto &cell : ctx->cells)
            for (auto &port : cell.second->ports)
                if (port.second.net != nullptr) {
                    CellPortKey key(cell.first, port.first);
                    by_port[cell.first.str(ctx) + "." + port.first.str(ctx)] = {
                            double(tmg->get_criticality(key)), double(tmg->get_setup_slack(key)),
                            double(tmg->get_absolute_setup_slack(key))};
                }
        return by_port;
    };
    std::string without = run(false);
    auto fmax_without = ctx->timing_result.clock_fmax;
    auto timings_without = timings();
    std::string with = run(true);
    EXPECT_EQ(with, without);
    for (auto &fmax : fmax_without)
        EXPECT_EQ(ctx->timing_result.clock_fmax.at(fmax.first).achieved, fmax.second.achieved);
    auto timings_with = timings();
    ASSERT_EQ(timings_with.size(), timings_without.size());
    int less = 0;
    for (auto &entry : timings_with) {
        ASSERT_TRUE(timings_without.count(entry.first)) << entry.first;
        auto &other = timings_without.at(entry.first);
        // delay, setup slack (end points); criticality, setup slack, absolute setup slack (ports)
        EXPECT_EQ(entry.second.at(0), other.at(0)) << entry.first;
        EXPECT_EQ(entry.second.at(1), other.at(1)) << entry.first;
        if (entry.first.find(' ') == std::string::npos) {
            EXPECT_EQ(entry.second.at(2), other.at(2)) << entry.first;
        } else {
            // hold slack: no greater with the least delays
            EXPECT_LE(entry.second.at(2), other.at(2)) << entry.first;
            less += entry.second.at(2) < other.at(2);
        }
    }
    EXPECT_GT(less, 0);
}
