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

// Input and output delays (set_input_delay, set_output_delay of the SDC file) in the timing analysis
class GateMateIoDelayTest : public GateMateTest
{
  protected:
    NetInfo *clk = nullptr;
    CellInfo *in_ff = nullptr, *out_ff = nullptr;
    const delay_t none = std::numeric_limits<delay_t>::max();

    NetInfo *chain(const std::string &name, int n, NetInfo *src)
    {
        for (int i = 0; i < n; i++)
            src = lut1(name + std::to_string(i), 0b01, src);
        return src;
    }

    // Top level ports as the frontend makes them: the clock clk (100 MHz, named sys, constrained at its port) through
    // a global buffer, the input d through an inverter into the register in_ff, four inverters from there to the
    // register out_ff, which drives the output q; and the input p through an inverter to the output r
    void design()
    {
        CellInfo *bufg = create_cell_ptr(id_CC_BUFG, "clk_bufg");
        bufg->connectPort(id_I, top_input("clk"));
        clk = create_net("clk$glb");
        bufg->connectPort(id_O, clk);
        in_ff = dff("in_ff", lut1("in_inv", 0b01, top_input("d")), clk);
        out_ff = dff("out_ff", chain("mid", 4, in_ff->getPort(id_Q)), clk);
        top_output("q", out_ff->getPort(id_Q));
        top_output("r", lut1("comb", 0b01, top_input("p")));
        sdc("create_clock -name sys -period 10 [get_ports clk]\n");
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

    std::unique_ptr<TimingAnalyser> analyse()
    {
        auto tmg = std::make_unique<TimingAnalyser>(ctx);
        tmg->setup_only = false;
        tmg->with_clock_skew = true;
        tmg->setup(false, true, true);
        return tmg;
    }

    // The clock net of the registers after packing
    IdString clock_net()
    {
        NetInfo *net = in_ff->getPort(id_CLK_INT) ? in_ff->getPort(id_CLK_INT) : in_ff->getPort(id_CLK);
        return net->name;
    }

    delay_t ns(double value) { return ctx->getDelayFromNS(float(value)); }

    // The timings of the end points of a cell (a register's data input, an output buffer's input), per launch domain
    std::vector<TimingAnalyser::EndpointTiming> endpoints(TimingAnalyser &tmg, const std::string &cell)
    {
        std::vector<TimingAnalyser::EndpointTiming> found;
        for (auto &ep : tmg.get_endpoint_timings())
            if (ep.endpoint.cell == ctx->id(cell) && (ep.endpoint.port == id_DIN || ep.endpoint.port == id_A))
                found.push_back(ep);
        return found;
    }
    TimingAnalyser::EndpointTiming endpoint(TimingAnalyser &tmg, const std::string &cell)
    {
        auto found = endpoints(tmg, cell);
        if (found.size() != 1) {
            ADD_FAILURE() << found.size() << " timings of the end point of " << cell;
            return TimingAnalyser::EndpointTiming{};
        }
        return found.front();
    }

    delay_t sum(const CriticalPath &path)
    {
        delay_t total = 0;
        for (auto &seg : path.segments)
            total += seg.delay;
        return total;
    }
    delay_t segment(const CriticalPath &path, CriticalPath::Segment::Type type)
    {
        delay_t total = 0;
        for (auto &seg : path.segments)
            if (seg.type == type)
                total += seg.delay;
        return total;
    }
    std::string describe(const CriticalPath &path)
    {
        std::string text;
        for (auto &seg : path.segments)
            text += stringf("\n  %-10s %7.3f %s.%s -> %s.%s", CriticalPath::Segment::type_to_str(seg.type).c_str(),
                            ctx->getDelayNS(seg.delay), seg.from.first.c_str(ctx), seg.from.second.c_str(ctx),
                            seg.to.first.c_str(ctx), seg.to.second.c_str(ctx));
        return text;
    }
};

using SegmentType = CriticalPath::Segment::Type;

TEST_F(GateMateIoDelayTest, sdc_commands)
{
    design();
    sdc("set_input_delay -clock sys -max 2 [get_ports d]\n"
        "set_input_delay -clock [get_clocks sys] -min 0.5 [get_ports d] # the same port and clock: one delay\n"
        "set_output_delay -clock sys 3 [get_ports q]\n"
        "set_output_delay -clock sys -clock_fall -min -1.5 -add_delay q\n");
    ASSERT_EQ(ctx->io_delays.size(), 3u);
    auto &in = ctx->io_delays.at(0);
    EXPECT_FALSE(in.output);
    EXPECT_EQ(in.port, ctx->id("d"));
    EXPECT_EQ(in.clock, ctx->id("sys"));
    EXPECT_EQ(in.edge, RISING_EDGE);
    EXPECT_TRUE(in.has_min);
    EXPECT_TRUE(in.has_max);
    EXPECT_FLOAT_EQ(in.min_ns, 0.5f);
    EXPECT_FLOAT_EQ(in.max_ns, 2.0f);
    EXPECT_EQ(in.line, 2);
    EXPECT_NE(in.source.find("-max 2"), std::string::npos) << in.source;
    EXPECT_NE(in.source.find("-min 0.5"), std::string::npos) << in.source;
    EXPECT_EQ(in.source.find("#"), std::string::npos) << in.source;
    // Neither -min nor -max: both
    auto &out = ctx->io_delays.at(1);
    EXPECT_TRUE(out.output);
    EXPECT_TRUE(out.has_min && out.has_max);
    EXPECT_FLOAT_EQ(out.min_ns, 3.0f);
    EXPECT_FLOAT_EQ(out.max_ns, 3.0f);
    // -add_delay: a further delay of the port against another clock edge; a delay may be negative
    auto &fall = ctx->io_delays.at(2);
    EXPECT_EQ(fall.port, ctx->id("q"));
    EXPECT_EQ(fall.edge, FALLING_EDGE);
    EXPECT_TRUE(fall.has_min);
    EXPECT_FALSE(fall.has_max);
    EXPECT_FLOAT_EQ(fall.min_ns, -1.5f);

    // Without -add_delay, a delay against another clock edge replaces those of the port
    sdc("set_output_delay -clock sys -clock_fall 1 [get_ports q]\n");
    ASSERT_EQ(ctx->io_delays.size(), 2u);
    EXPECT_EQ(ctx->io_delays.at(1).edge, FALLING_EDGE);
    EXPECT_TRUE(ctx->io_delays.at(1).has_max);
    EXPECT_FLOAT_EQ(ctx->io_delays.at(1).min_ns, 1.0f);

    // all_inputs and all_outputs; the pad buffer of a port by its cell
    ctx->io_delays.clear();
    sdc("set_input_delay -clock sys 1 [all_inputs]\n"
        "set_output_delay -clock sys 1 [all_outputs]\n"
        "set_input_delay -clock sys 1 [get_cells {p$iopad}]\n");
    int inputs = 0, outputs = 0, cells = 0;
    for (auto &io : ctx->io_delays) {
        inputs += !io.output && io.port != IdString();
        outputs += io.output;
        cells += io.cell == ctx->id("p$iopad");
    }
    EXPECT_EQ(inputs, 3); // clk, d, p
    EXPECT_EQ(outputs, 2);
    EXPECT_EQ(cells, 1);
}

TEST_F(GateMateIoDelayTest, sdc_errors)
{
    design();
    EXPECT_LOG_ERROR(sdc("set_input_delay 2 [get_ports d]\n"), "needs -clock");
    EXPECT_LOG_ERROR(sdc("set_input_delay -clock sys [get_ports d]\n"), "needs a delay");
    EXPECT_LOG_ERROR(sdc("set_output_delay -clock sys 2\n"), "needs the ports");
    EXPECT_LOG_ERROR(sdc("set_output_delay -clock sys -rise 2 [get_ports q]\n"), "unsupported argument '-rise'");
    // (a word at the end of a line ends there)
    EXPECT_LOG_ERROR(sdc("set_output_delay 2 [get_ports q] -clock\nset_false_path\n"), "missing clock after -clock");
    EXPECT_LOG_ERROR(sdc("set_output_delay -clock sys 2 [get_nets {clk$glb}]\n"), "expects ports");
    EXPECT_TRUE(ctx->io_delays.empty());
    // The wrong direction, a clock or ports that do not exist: ignored with a warning
    LogCapture capture(LogLevel::WARNING_MSG);
    sdc("set_input_delay -clock sys 2 [get_ports q]\n"
        "set_output_delay -clock [get_clocks nosuch] 2 [get_ports q]\n"
        "set_output_delay -clock sys 2 [get_ports nosuch]\n");
    std::string log = capture.str();
    EXPECT_NE(log.find("'q' is an output port"), std::string::npos) << log;
    EXPECT_NE(log.find("-clock matched no clock"), std::string::npos) << log;
    EXPECT_NE(log.find("the ports matched nothing"), std::string::npos) << log;
    EXPECT_TRUE(ctx->io_delays.empty());
}

// An input delay makes the paths from the pad paths of the clock: launched at its edge at the clock pad, with the
// delay outside of the device and the pad buffer, against registers behind the clock network
TEST_F(GateMateIoDelayTest, input_delay)
{
    design();
    implement();
    auto plain = analyse();
    auto before = endpoint(*plain, "in_ff");
    EXPECT_EQ(before.clock_pair.start.clock, IdString());
    EXPECT_EQ(before.hold_slack, none);
    auto registers = endpoint(*plain, "out_ff");

    sdc("set_input_delay -clock sys 2 [get_ports d]\n");
    auto tmg = analyse();
    EXPECT_TRUE(tmg->get_timing_result().io_notes.empty());
    auto ep = endpoint(*tmg, "in_ff");
    EXPECT_EQ(ep.clock_pair.start.clock, clock_net());
    EXPECT_EQ(ep.clock_pair.start.edge, RISING_EDGE);
    EXPECT_EQ(ep.clock_pair.end.clock, clock_net());
    auto pc = tmg->get_pair_constraints(ep.domain_pair);
    EXPECT_TRUE(pc.io_launch);
    EXPECT_FALSE(pc.io_capture);
    EXPECT_FALSE(pc.no_setup);
    EXPECT_FALSE(pc.no_hold);
    EXPECT_EQ(tmg->get_io_delays(CellPortKey(ctx->id("d$iopad"), id_Y), false), std::vector<int>{0});

    auto longest = tmg->get_endpoint_path(ep, true), shortest = tmg->get_endpoint_path(ep, false);
    SCOPED_TRACE(describe(longest));
    EXPECT_EQ(segment(longest, SegmentType::INPUT_DELAY), ns(2));
    EXPECT_EQ(segment(shortest, SegmentType::INPUT_DELAY), ns(2));
    // (the pad buffer)
    EXPECT_GT(segment(longest, SegmentType::SOURCE), 0);
    EXPECT_GE(segment(longest, SegmentType::SOURCE), segment(shortest, SegmentType::SOURCE));
    EXPECT_EQ(sum(longest), ep.delay);
    EXPECT_EQ(longest.max_delay, ns(10));
    EXPECT_EQ(ep.setup_slack, ns(10) - ep.delay);
    EXPECT_EQ(ep.hold_slack, sum(shortest));
    EXPECT_FALSE(ep.no_setup || ep.no_hold || ep.false_setup || ep.false_hold);

    // The clock: at the pad for the input, behind the clock network (its latency and the routing of the clock
    // net) for the register
    DelayPair latency = tmg->get_clock_latency(clock_net());
    EXPECT_GT(latency.minDelay(), 0);
    EXPECT_LE(latency.minDelay(), latency.maxDelay());
    auto clocks = tmg->get_clock_delays(ep, longest);
    EXPECT_EQ(clocks.launch, 0);
    EXPECT_GE(clocks.capture, latency.minDelay());
    EXPECT_EQ(segment(longest, SegmentType::CLK_SKEW), -clocks.capture);
    // (no part of the clock network is common with the clock outside: the capturing register's clock at its
    // earliest for setup, at its latest for hold)
    auto hold_clocks = tmg->get_clock_delays(ep, shortest, false);
    EXPECT_EQ(hold_clocks.launch, 0);
    EXPECT_EQ(segment(shortest, SegmentType::CLK_SKEW), -hold_clocks.capture);
    EXPECT_EQ(hold_clocks.capture - clocks.capture, latency.maxDelay() - latency.minDelay());
    // the path without the clocks, the delay and the pad buffer is the one from the unclocked pad (which has the
    // routing of the clock net at the register in it)
    EXPECT_EQ(ep.delay - segment(longest, SegmentType::CLK_SKEW) - segment(longest, SegmentType::INPUT_DELAY) -
                      segment(longest, SegmentType::SOURCE),
              before.delay + (clocks.capture - latency.minDelay()));

    // The paths between the registers are as they were
    auto registers_after = endpoint(*tmg, "out_ff");
    EXPECT_EQ(registers_after.setup_slack, registers.setup_slack);
    EXPECT_EQ(registers_after.hold_slack, registers.hold_slack);

    // The slacks of the delay
    auto &slacks = tmg->get_timing_result().io_slacks;
    ASSERT_EQ(slacks.size(), 1u);
    EXPECT_EQ(slacks.at(0).index, 0);
    EXPECT_EQ(slacks.at(0).points, 1);
    EXPECT_EQ(slacks.at(0).setup_slack, ep.setup_slack);
    EXPECT_EQ(slacks.at(0).hold_slack, ep.hold_slack);

    // One ns more outside: one less for setup, one more for hold
    sdc("set_input_delay -clock sys 3 [get_ports d]\n");
    ASSERT_EQ(ctx->io_delays.size(), 1u);
    auto later = endpoint(*analyse(), "in_ff");
    EXPECT_EQ(later.setup_slack, ep.setup_slack - ns(1));
    EXPECT_EQ(later.hold_slack, ep.hold_slack + ns(1));
}

// An output delay ends the paths to the pad at the clock edge at the clock pad: the data must be there the maximum
// before it, and stay until the minimum before it
TEST_F(GateMateIoDelayTest, output_delay)
{
    design();
    implement();
    auto plain = analyse();
    EXPECT_EQ(endpoint(*plain, "q$iopad").clock_pair.end.clock, IdString());
    auto registers = endpoint(*plain, "out_ff");

    sdc("set_output_delay -clock sys 3 [get_ports q]\n");
    auto tmg = analyse();
    auto ep = endpoint(*tmg, "q$iopad");
    EXPECT_EQ(ep.clock_pair.start.clock, clock_net());
    EXPECT_EQ(ep.clock_pair.end.clock, clock_net());
    auto pc = tmg->get_pair_constraints(ep.domain_pair);
    EXPECT_FALSE(pc.io_launch);
    EXPECT_TRUE(pc.io_capture);
    EXPECT_EQ(tmg->get_io_delays(ep.endpoint, true), std::vector<int>{0});

    auto longest = tmg->get_endpoint_path(ep, true), shortest = tmg->get_endpoint_path(ep, false);
    SCOPED_TRACE(describe(longest));
    ASSERT_GE(longest.segments.size(), 3u);
    auto &last = longest.segments.back(), &buffer = longest.segments.at(longest.segments.size() - 2);
    EXPECT_EQ(int(last.type), int(SegmentType::OUTPUT_DELAY));
    EXPECT_EQ(last.delay, ns(3));
    EXPECT_EQ(segment(shortest, SegmentType::OUTPUT_DELAY), ns(3));
    // (the pad buffer: from the end point to the pad pin)
    EXPECT_EQ(int(buffer.type), int(SegmentType::LOGIC));
    EXPECT_EQ(buffer.from, std::make_pair(ctx->id("q$iopad"), IdString(id_A)));
    EXPECT_EQ(buffer.to, std::make_pair(ctx->id("q$iopad"), IdString(id_O)));
    EXPECT_GT(buffer.delay, 0);
    EXPECT_EQ(sum(longest), ep.delay);
    EXPECT_EQ(ep.setup_slack, ns(10) - ep.delay);
    EXPECT_EQ(ep.hold_slack, sum(shortest));

    DelayPair latency = tmg->get_clock_latency(clock_net());
    auto clocks = tmg->get_clock_delays(ep, longest);
    EXPECT_EQ(clocks.capture, 0);
    EXPECT_GE(clocks.launch, latency.maxDelay());
    EXPECT_GT(latency.minDelay(), 0);
    EXPECT_EQ(segment(longest, SegmentType::CLK_SKEW), clocks.launch);
    // (the launching register's clock at its latest for setup, at its earliest for hold)
    auto hold_clocks = tmg->get_clock_delays(ep, shortest, false);
    EXPECT_EQ(hold_clocks.capture, 0);
    EXPECT_EQ(segment(shortest, SegmentType::CLK_SKEW), hold_clocks.launch);
    EXPECT_EQ(clocks.launch - hold_clocks.launch, latency.maxDelay() - latency.minDelay());

    auto registers_after = endpoint(*tmg, "out_ff");
    EXPECT_EQ(registers_after.setup_slack, registers.setup_slack);
    EXPECT_EQ(registers_after.hold_slack, registers.hold_slack);

    // One ns more outside (for setup and hold): one less for setup, one more for hold
    sdc("set_output_delay -clock sys 4 [get_ports q]\n");
    auto more = endpoint(*analyse(), "q$iopad");
    EXPECT_EQ(more.setup_slack, ep.setup_slack - ns(1));
    EXPECT_EQ(more.hold_slack, ep.hold_slack + ns(1));
    // A hold time of the device outside (less the trace): a negative minimum, which the data must outlast
    sdc("set_output_delay -clock sys -min -1.5 [get_ports q]\n");
    auto held = endpoint(*analyse(), "q$iopad");
    EXPECT_EQ(held.setup_slack, more.setup_slack);
    EXPECT_EQ(held.hold_slack, more.hold_slack - ns(5.5));
}

// -max alone gives no hold check, -min alone no setup check
TEST_F(GateMateIoDelayTest, maximum_or_minimum_alone)
{
    design();
    sdc("set_input_delay -clock sys -max 2 [get_ports d]\n"
        "set_output_delay -clock sys -min -40 [get_ports q]\n");
    implement();
    auto tmg = analyse();
    auto in = endpoint(*tmg, "in_ff");
    EXPECT_NE(in.setup_slack, none);
    EXPECT_EQ(in.hold_slack, none);
    EXPECT_TRUE(in.no_hold);
    EXPECT_FALSE(in.no_setup);
    EXPECT_FALSE(in.false_hold);
    EXPECT_TRUE(tmg->get_pair_constraints(in.domain_pair).no_hold);
    auto out = endpoint(*tmg, "q$iopad");
    EXPECT_EQ(out.setup_slack, none);
    EXPECT_TRUE(out.no_setup);
    EXPECT_FALSE(out.false_setup);
    ASSERT_NE(out.hold_slack, none);
    EXPECT_LT(out.hold_slack, 0) << "precondition: the data does not stay 40 ns";
    EXPECT_FALSE(out.no_hold);
    EXPECT_EQ(tmg->get_hold_slack(out.endpoint), out.hold_slack);
    EXPECT_EQ(tmg->get_absolute_setup_slack(out.endpoint), none);
    EXPECT_FLOAT_EQ(tmg->get_criticality(out.endpoint), 0.0f);

    auto &result = tmg->get_timing_result();
    EXPECT_EQ(result.false_setup_paths, 0);
    EXPECT_EQ(result.false_hold_paths, 0);
    ASSERT_EQ(result.io_slacks.size(), 2u);
    EXPECT_EQ(result.io_slacks.at(0).setup_slack, in.setup_slack);
    EXPECT_EQ(result.io_slacks.at(0).hold_slack, none);
    EXPECT_EQ(result.io_slacks.at(1).setup_slack, none);
    EXPECT_EQ(result.io_slacks.at(1).hold_slack, out.hold_slack);
    // The hold violation at the output is one; the input has none to have
    int at_output = 0, at_input = 0;
    for (auto &violation : result.min_delay_violations) {
        at_output += violation.segments.back().to.first == ctx->id("q$iopad");
        at_input += violation.segments.back().to.first == in_ff->name;
    }
    EXPECT_EQ(at_output, 1);
    EXPECT_EQ(at_input, 0);
}

// From an input to an output, both with a delay: the latency of the clock network is on neither side
TEST_F(GateMateIoDelayTest, pad_to_pad)
{
    design();
    sdc("set_input_delay -clock sys 2 [get_ports p]\n"
        "set_output_delay -clock sys 3 [get_ports r]\n");
    implement();
    auto tmg = analyse();
    auto ep = endpoint(*tmg, "r$iopad");
    EXPECT_EQ(ep.clock_pair.start.clock, clock_net());
    EXPECT_EQ(ep.clock_pair.end.clock, clock_net());
    auto pc = tmg->get_pair_constraints(ep.domain_pair);
    EXPECT_TRUE(pc.io_launch && pc.io_capture);
    auto longest = tmg->get_endpoint_path(ep, true), shortest = tmg->get_endpoint_path(ep, false);
    SCOPED_TRACE(describe(longest));
    EXPECT_EQ(segment(longest, SegmentType::CLK_SKEW), 0);
    EXPECT_EQ(segment(longest, SegmentType::INPUT_DELAY), ns(2));
    EXPECT_EQ(segment(longest, SegmentType::OUTPUT_DELAY), ns(3));
    EXPECT_EQ(sum(longest), ep.delay);
    EXPECT_GT(ep.delay, ns(5));
    EXPECT_EQ(ep.setup_slack, ns(10) - ep.delay);
    EXPECT_EQ(ep.hold_slack, sum(shortest));
    auto clocks = tmg->get_clock_delays(ep, longest);
    EXPECT_EQ(clocks.launch, 0);
    EXPECT_EQ(clocks.capture, 0);
}

// -clock_fall: against the falling edge of the clock
TEST_F(GateMateIoDelayTest, falling_clock_edge)
{
    design();
    sdc("set_input_delay -clock sys -clock_fall 2 [get_ports d]\n"
        "set_output_delay -clock sys -clock_fall 3 [get_ports q]\n");
    implement();
    auto tmg = analyse();
    auto in = endpoint(*tmg, "in_ff");
    EXPECT_EQ(in.clock_pair.start.edge, FALLING_EDGE);
    EXPECT_EQ(in.clock_pair.end.edge, RISING_EDGE);
    EXPECT_EQ(tmg->get_endpoint_path(in, true).max_delay, ns(5));
    EXPECT_EQ(in.setup_slack, ns(5) - in.delay);
    auto out = endpoint(*tmg, "q$iopad");
    EXPECT_EQ(out.clock_pair.start.edge, RISING_EDGE);
    EXPECT_EQ(out.clock_pair.end.edge, FALLING_EDGE);
    EXPECT_EQ(out.setup_slack, ns(5) - out.delay);
}

// Timing exceptions name the paths by their ports (the net of a port is on the pad side of its buffer)
TEST_F(GateMateIoDelayTest, exceptions_by_port)
{
    design();
    sdc("set_input_delay -clock sys 2 [get_ports d]\n"
        "set_output_delay -clock sys 3 [get_ports q]\n");
    implement();
    auto plain = analyse();
    auto in = endpoint(*plain, "in_ff"), out = endpoint(*plain, "q$iopad");

    sdc("set_multicycle_path 2 -from [get_ports d]\n"
        "set_multicycle_path 1 -hold -from [get_ports d]\n"
        "set_multicycle_path 3 -to [get_ports q]\n");
    auto multi = analyse();
    EXPECT_EQ(endpoint(*multi, "in_ff").setup_slack, in.setup_slack + ns(10));
    EXPECT_EQ(endpoint(*multi, "in_ff").hold_slack, in.hold_slack);
    EXPECT_EQ(endpoint(*multi, "q$iopad").setup_slack, out.setup_slack + ns(20));
    EXPECT_EQ(endpoint(*multi, "out_ff").setup_slack, endpoint(*plain, "out_ff").setup_slack);

    ctx->timing_exceptions.clear();
    sdc("set_false_path -from [get_ports d]\n"
        "set_false_path -hold -to [get_ports q]\n");
    auto falsed = analyse();
    auto in_false = endpoint(*falsed, "in_ff"), out_false = endpoint(*falsed, "q$iopad");
    EXPECT_TRUE(in_false.false_setup && in_false.false_hold);
    EXPECT_EQ(in_false.setup_slack, none);
    EXPECT_FALSE(out_false.false_setup);
    EXPECT_TRUE(out_false.false_hold);
    EXPECT_EQ(out_false.setup_slack, out.setup_slack);
    EXPECT_EQ(out_false.hold_slack, none);
    // (the slacks of the delays: without the false paths)
    auto &slacks = falsed->get_timing_result().io_slacks;
    ASSERT_EQ(slacks.size(), 2u);
    EXPECT_EQ(slacks.at(0).setup_slack, none);
    EXPECT_EQ(slacks.at(0).hold_slack, none);
    EXPECT_EQ(slacks.at(1).setup_slack, out.setup_slack);
    EXPECT_EQ(slacks.at(1).hold_slack, none);
}

// The paths of the delays count for the clock: its Fmax, its critical path, the criticalities
TEST_F(GateMateIoDelayTest, paths_of_the_clock)
{
    design();
    implement();
    auto plain = analyse();
    float fmax = plain->get_timing_result().clock_fmax.at(clock_net()).achieved;

    // The data of an input long before the clock edge: no path that limits the clock
    sdc("set_input_delay -clock sys -30 [get_ports d]\n");
    auto early = analyse();
    EXPECT_LT(endpoint(*early, "in_ff").delay, 0);
    EXPECT_EQ(early->get_timing_result().clock_fmax.at(clock_net()).achieved, fmax);

    // An output that needs the data 30 ns before the edge
    sdc("set_output_delay -clock sys 30 [get_ports q]\n");
    auto tmg = analyse();
    auto out = endpoint(*tmg, "q$iopad");
    EXPECT_LT(out.setup_slack, 0);
    auto &result = tmg->get_timing_result();
    EXPECT_NEAR(result.clock_fmax.at(clock_net()).achieved, 1000 / ctx->getDelayNS(out.delay), 0.01);
    EXPECT_LT(result.clock_fmax.at(clock_net()).achieved, 1000.0f / 30);
    auto &critical = result.clock_paths.at(clock_net());
    EXPECT_EQ(int(critical.segments.back().type), int(SegmentType::OUTPUT_DELAY));
    EXPECT_FLOAT_EQ(tmg->get_criticality(out.endpoint), 1.0f);
    EXPECT_LT(tmg->get_criticality(CellPortKey(out_ff->name, id_DIN)), 1.0f);

    // The report
    std::string log;
    {
        LogCapture capture;
        ctx->log_timing_results(tmg->get_timing_result(), false, true, true, false);
        log = capture.str();
    }
    EXPECT_NE(log.find("Input and output delays"), std::string::npos) << log;
    EXPECT_NE(log.find("set_input_delay -clock sys -30 [get_ports d]"), std::string::npos) << log;
    EXPECT_NE(log.find("1 port(s): worst setup slack "), std::string::npos) << log;
    EXPECT_NE(log.find(stringf("1 port(s): worst setup slack %.2f ns at 'q', worst hold slack ",
                               ctx->getDelayNS(out.setup_slack))),
              std::string::npos)
            << log;
    EXPECT_NE(log.find("out-delay"), std::string::npos) << log;
}

// A delay that can not be applied is reported, and its port stays unclocked
TEST_F(GateMateIoDelayTest, delays_not_applied)
{
    design();
    sdc("create_clock -name other -period 20 [get_ports p]\n"
        "set_input_delay -clock nosuch 2 [get_ports d]\n"
        "set_output_delay -clock other 2 [get_ports q]\n");
    implement();
    auto tmg = analyse();
    EXPECT_EQ(endpoint(*tmg, "in_ff").clock_pair.start.clock, IdString());
    EXPECT_EQ(endpoint(*tmg, "q$iopad").clock_pair.end.clock, IdString());
    auto &result = tmg->get_timing_result();
    EXPECT_TRUE(result.io_slacks.empty());
    ASSERT_EQ(result.io_notes.size(), 2u);
    EXPECT_NE(result.io_notes.at(0).find("set_input_delay for 'd' (line 2): no clock 'nosuch'"), std::string::npos)
            << result.io_notes.at(0);
    EXPECT_NE(result.io_notes.at(1).find("the clock 'other' clocks no register"), std::string::npos)
            << result.io_notes.at(1);
    LogCapture capture(LogLevel::WARNING_MSG);
    ctx->log_timing_results(result, false, true, false, false);
    EXPECT_NE(capture.str().find("no clock 'nosuch'"), std::string::npos) << capture.str();
}

// The clock at a clock pin: its latency through the clock input and the global buffer
TEST_F(GateMateIoDelayTest, clock_pin)
{
    init_with_ccf(stringf("NET \"clk\" Loc = \"%s\";\n", package_pins(true).front().c_str()),
                  {"--allow-unconstrained"});
    design();
    sdc("set_input_delay -clock sys 2 [get_ports d]\n"
        "set_output_delay -clock sys 3 [get_ports q]\n");
    place_and_route();
    auto tmg = analyse();
    DelayPair latency = tmg->get_clock_latency(clock_net());
    RecordProperty("latency_ns", std::to_string(ctx->getDelayNS(latency.maxDelay())));
    // (the pad buffer, the clock input, the global buffer: a few ns; the least of the routed design below the
    // greatest)
    EXPECT_GT(latency.minDelay(), ns(1));
    EXPECT_LT(latency.maxDelay(), ns(8));
    EXPECT_LT(latency.minDelay(), latency.maxDelay());
    auto in = endpoint(*tmg, "in_ff"), out = endpoint(*tmg, "q$iopad");
    auto in_path = tmg->get_endpoint_path(in, true), out_path = tmg->get_endpoint_path(out, true);
    EXPECT_EQ(sum(in_path), in.delay) << describe(in_path);
    EXPECT_EQ(sum(out_path), out.delay) << describe(out_path);
    EXPECT_EQ(in.hold_slack, sum(tmg->get_endpoint_path(in, false)));
    EXPECT_EQ(out.hold_slack, sum(tmg->get_endpoint_path(out, false)));
    EXPECT_LE(segment(in_path, SegmentType::CLK_SKEW), -latency.minDelay());
    EXPECT_GE(segment(out_path, SegmentType::CLK_SKEW), latency.maxDelay());
    EXPECT_LE(segment(tmg->get_endpoint_path(in, false), SegmentType::CLK_SKEW), -latency.maxDelay());
}

// The hold fixer pads the paths from an input whose data changes soon after the clock edge: the clock network makes
// the registers see that edge later
TEST_F(GateMateIoDelayTest, hold_fixed_at_an_input)
{
    design();
    sdc("set_input_delay -clock sys -max 2 [get_ports d]\n"
        "set_input_delay -clock sys -min 0.5 [get_ports d]\n");
    // The router's report lists the violations it leaves, which are repaired after it: no error, the finished
    // routing is what is checked
    bool had_error = had_nonfatal_error;
    had_nonfatal_error = false;
    std::string log;
    {
        LogCapture capture;
        place_and_route();
        log = capture.str();
    }
    EXPECT_FALSE(had_nonfatal_error) << log;
    had_nonfatal_error = had_error;
    EXPECT_NE(log.find("Hold/min time violations (showing 10 worst paths), before the repairs"), std::string::npos)
            << log;
    EXPECT_EQ(log.find("ERROR"), std::string::npos) << log;
    auto *impl = static_cast<GateMateImpl *>(ctx->uarch.get());
    int delay_cells = 0;
    for (auto &cell : ctx->cells)
        delay_cells += impl->is_delay_cell(cell.second.get());
    EXPECT_GT(delay_cells, 0) << log;
    for (const char *corner : {"best_spd", "typ_spd", "worst_spd"})
        EXPECT_NE(log.find(std::string("Hold check in timing corner '") + corner + "': 0 violation(s)"),
                  std::string::npos)
                << corner << " in:\n"
                << log;
    auto tmg = analyse();
    auto in = endpoint(*tmg, "in_ff");
    EXPECT_GE(in.hold_slack, ns(0.1));
    EXPECT_GE(in.setup_slack, 0);
    EXPECT_TRUE(tmg->get_timing_result().min_delay_violations.empty());
}

// ... and the paths to an output whose data must stay after the clock edge (the hold time of the device it goes to)
TEST_F(GateMateIoDelayTest, hold_fixed_at_an_output)
{
    design();
    // (the routed corner meets about 13 ns, the best corner does not)
    sdc("set_output_delay -clock sys -min -12 [get_ports q]\n");
    bool had_error = had_nonfatal_error;
    std::string log;
    {
        LogCapture capture;
        place_and_route();
        log = capture.str();
    }
    had_nonfatal_error = had_error;
    auto *impl = static_cast<GateMateImpl *>(ctx->uarch.get());
    int delay_cells = 0;
    for (auto &cell : ctx->cells)
        delay_cells += impl->is_delay_cell(cell.second.get());
    EXPECT_GT(delay_cells, 0) << log;
    EXPECT_NE(log.find("-> 0 end point(s) below the margin"), std::string::npos) << log;
    for (const char *corner : {"best_spd", "typ_spd", "worst_spd"})
        EXPECT_NE(log.find(std::string("Hold check in timing corner '") + corner + "': 0 violation(s)"),
                  std::string::npos)
                << corner << " in:\n"
                << log;
    EXPECT_EQ(log.find("Hold fix left"), std::string::npos) << log;
    auto tmg = analyse();
    auto out = endpoint(*tmg, "q$iopad");
    EXPECT_GE(out.hold_slack, ns(0.1));
    EXPECT_EQ(out.setup_slack, none);
    // (the delay cells are on the way to the pad)
    bool through_cell = false;
    for (auto &seg : tmg->get_endpoint_path(out, false).segments)
        through_cell |= ctx->cells.count(seg.to.first) && impl->is_delay_cell(ctx->cells.at(seg.to.first).get());
    EXPECT_TRUE(through_cell) << describe(tmg->get_endpoint_path(out, false));
}

// A clock the design puts out at a port (create_generated_clock on an output port), for a device that is clocked by
// it: the delays against it are times against its edge at that port
class GateMateForwardedClockTest : public GateMateIoDelayTest
{
  protected:
    // The design with the clock of the registers at the output cko too
    void design_with_clock_output()
    {
        design();
        top_output("cko", clk);
        sdc("create_generated_clock -name fwd -source [get_ports clk] [get_ports cko]\n");
    }
};

TEST_F(GateMateForwardedClockTest, sdc_commands)
{
    design_with_clock_output();
    ASSERT_EQ(ctx->forwarded_clocks.size(), 1u);
    EXPECT_EQ(ctx->forwarded_clocks.at(0).name, ctx->id("fwd"));
    EXPECT_EQ(ctx->forwarded_clocks.at(0).port, ctx->id("cko"));
    EXPECT_FALSE(ctx->forwarded_clocks.at(0).invert);
    EXPECT_EQ(ctx->forwarded_clocks.at(0).source,
              "create_generated_clock -name fwd -source [get_ports clk] [get_ports cko]");
    EXPECT_EQ(ctx->forwarded_clocks.at(0).line, 1);
    // The clock is one of get_clocks
    sdc("set_output_delay -clock fwd -max 2 [get_ports q]\n"
        "set_output_delay -clock [get_clocks fwd] -min -0.5 [get_ports q]\n"
        "set_false_path -hold -to [get_clocks fwd]\n");
    ASSERT_EQ(ctx->io_delays.size(), 1u);
    EXPECT_EQ(ctx->io_delays.at(0).clock, ctx->id("fwd"));
    EXPECT_TRUE(ctx->io_delays.at(0).has_min && ctx->io_delays.at(0).has_max);
    ASSERT_EQ(ctx->timing_exceptions.size(), 1u);
    EXPECT_EQ(ctx->timing_exceptions.at(0).to.size(), 1u);
    // Again for the port: the later one replaces it (named by the port without -name)
    sdc("create_generated_clock -invert -divide_by 1 cko\n");
    ASSERT_EQ(ctx->forwarded_clocks.size(), 1u);
    EXPECT_EQ(ctx->forwarded_clocks.at(0).name, ctx->id("cko"));
    EXPECT_TRUE(ctx->forwarded_clocks.at(0).invert);

    EXPECT_LOG_ERROR(sdc("create_generated_clock -name half -divide_by 2 [get_ports cko]\n"),
                     "-divide_by 2 is not supported");
    EXPECT_LOG_ERROR(sdc("create_generated_clock -name in [get_ports d]\n"), "expects one output port");
    EXPECT_LOG_ERROR(sdc("create_generated_clock -name none\n"), "expects one output port");
    EXPECT_LOG_ERROR(sdc("create_generated_clock -name two [get_ports {q r}]\n"), "expects one output port");
    EXPECT_LOG_ERROR(sdc("create_generated_clock -name pin [get_pins in_ff/Q]\n"), "expects one output port");
    EXPECT_LOG_ERROR(sdc("create_generated_clock -name sys [get_ports cko]\n"), "there is a clock 'sys' already");
    EXPECT_LOG_ERROR(sdc("create_generated_clock -edges {1 2 3} [get_ports cko]\n"), "unsupported argument '-edges'");
    EXPECT_LOG_ERROR(sdc("create_generated_clock nosuch\n"), "no port 'nosuch'");
    EXPECT_EQ(ctx->forwarded_clocks.size(), 1u);
}

// An output against the clock at the port: the edge there is after that at the clock net of the registers, by the
// way of the clock to the port, instead of before it by the clock network from the clock pad
TEST_F(GateMateForwardedClockTest, output_delay)
{
    design_with_clock_output();
    sdc("set_output_delay -clock sys 3 [get_ports q]\n");
    implement();
    auto at_source = analyse();
    auto before = endpoint(*at_source, "q$iopad");
    DelayPair latency = at_source->get_clock_latency(clock_net());
    EXPECT_TRUE(at_source->get_timing_result().forwarded_clocks.empty());
    EXPECT_EQ(at_source->get_clock_latency(clock_net(), 1).maxDelay(), 0);

    sdc("set_output_delay -clock fwd 3 [get_ports q]\n");
    ASSERT_EQ(ctx->io_delays.size(), 1u);
    auto tmg = analyse();
    auto &result = tmg->get_timing_result();
    EXPECT_TRUE(result.io_notes.empty()) << (result.io_notes.empty() ? "" : result.io_notes.front());
    auto ep = endpoint(*tmg, "q$iopad");
    EXPECT_EQ(ep.clock_pair.start.clock, clock_net());
    EXPECT_EQ(ep.clock_pair.end.clock, clock_net());
    EXPECT_EQ(ep.clock_pair.end.edge, RISING_EDGE);
    auto pc = tmg->get_pair_constraints(ep.domain_pair);
    EXPECT_TRUE(pc.io_capture);
    EXPECT_EQ(pc.io_capture_ref, 1);
    EXPECT_EQ(pc.io_launch_ref, 0);

    // The delay of the clock from the clock net to the port: its pad buffer at least
    ASSERT_EQ(result.forwarded_clocks.size(), 1u);
    auto &fc = result.forwarded_clocks.front();
    EXPECT_EQ(fc.index, 0);
    EXPECT_EQ(fc.clock_net, clock_net());
    EXPECT_GT(fc.min_delay, ns(0.5));
    EXPECT_GE(fc.max_delay, fc.min_delay);
    DelayPair edge = tmg->get_clock_latency(clock_net(), 1);
    EXPECT_EQ(edge.minDelay(), -fc.max_delay);
    EXPECT_EQ(edge.maxDelay(), -fc.min_delay);
    // (that of the clock network from the pad is as before)
    EXPECT_EQ(tmg->get_clock_latency(clock_net()).maxDelay(), 0);

    // Setup against the earliest edge at the port, hold against the latest
    EXPECT_EQ(ep.setup_slack, before.setup_slack + latency.maxDelay() + fc.min_delay);
    EXPECT_EQ(ep.hold_slack, before.hold_slack - latency.minDelay() - fc.max_delay);

    auto longest = tmg->get_endpoint_path(ep, true), shortest = tmg->get_endpoint_path(ep, false);
    SCOPED_TRACE(describe(longest));
    EXPECT_EQ(segment(longest, SegmentType::OUTPUT_DELAY), ns(3));
    EXPECT_EQ(sum(longest), ep.delay);
    EXPECT_EQ(ep.setup_slack, ns(10) - ep.delay);
    EXPECT_EQ(ep.hold_slack, sum(shortest));
    auto clocks = tmg->get_clock_delays(ep, longest), hold_clocks = tmg->get_clock_delays(ep, shortest, false);
    EXPECT_EQ(clocks.capture, 0);
    EXPECT_EQ(segment(longest, SegmentType::CLK_SKEW), clocks.launch);
    EXPECT_EQ(segment(shortest, SegmentType::CLK_SKEW), hold_clocks.launch);
    EXPECT_EQ(clocks.launch - hold_clocks.launch, fc.max_delay - fc.min_delay);

    // The paths between the registers are as before
    EXPECT_EQ(endpoint(*tmg, "out_ff").setup_slack, endpoint(*at_source, "out_ff").setup_slack);
    EXPECT_EQ(endpoint(*tmg, "out_ff").hold_slack, endpoint(*at_source, "out_ff").hold_slack);

    // The exceptions name these paths by the clock at the port
    sdc("set_false_path -hold -to [get_clocks fwd]\n");
    auto falsed = analyse();
    EXPECT_TRUE(endpoint(*falsed, "q$iopad").false_hold);
    EXPECT_FALSE(endpoint(*falsed, "q$iopad").false_setup);
    EXPECT_FALSE(endpoint(*falsed, "out_ff").false_hold);
}

// An input from a device that is clocked by the clock at the port: its data comes after that edge
TEST_F(GateMateForwardedClockTest, input_delay)
{
    design_with_clock_output();
    sdc("set_input_delay -clock sys 2 [get_ports d]\n");
    implement();
    auto at_source = analyse();
    auto before = endpoint(*at_source, "in_ff");
    DelayPair latency = at_source->get_clock_latency(clock_net());

    sdc("set_input_delay -clock fwd 2 [get_ports d]\n");
    ASSERT_EQ(ctx->io_delays.size(), 1u);
    auto tmg = analyse();
    auto &result = tmg->get_timing_result();
    EXPECT_TRUE(result.io_notes.empty());
    ASSERT_EQ(result.forwarded_clocks.size(), 1u);
    auto &fc = result.forwarded_clocks.front();
    auto ep = endpoint(*tmg, "in_ff");
    auto pc = tmg->get_pair_constraints(ep.domain_pair);
    EXPECT_TRUE(pc.io_launch);
    EXPECT_EQ(pc.io_launch_ref, 1);
    // Setup against the latest edge at the port, hold against the earliest
    EXPECT_EQ(ep.setup_slack, before.setup_slack - latency.minDelay() - fc.max_delay);
    EXPECT_EQ(ep.hold_slack, before.hold_slack + latency.maxDelay() + fc.min_delay);
    auto longest = tmg->get_endpoint_path(ep, true), shortest = tmg->get_endpoint_path(ep, false);
    SCOPED_TRACE(describe(longest));
    EXPECT_EQ(segment(longest, SegmentType::INPUT_DELAY), ns(2));
    EXPECT_EQ(sum(longest), ep.delay);
    EXPECT_EQ(ep.setup_slack, ns(10) - ep.delay);
    EXPECT_EQ(ep.hold_slack, sum(shortest));
    auto clocks = tmg->get_clock_delays(ep, longest);
    EXPECT_EQ(clocks.launch, 0);
    EXPECT_EQ(segment(longest, SegmentType::CLK_SKEW), -clocks.capture);
}

// -invert: the rising edge at the port is the falling edge of the clock of the registers
TEST_F(GateMateForwardedClockTest, inverted)
{
    design();
    top_output("cko", clk);
    sdc("create_generated_clock -name fwd -invert [get_ports cko]\n"
        "set_output_delay -clock fwd 3 [get_ports q]\n"
        "set_input_delay -clock fwd -clock_fall 2 [get_ports d]\n");
    implement();
    auto tmg = analyse();
    EXPECT_TRUE(tmg->get_timing_result().io_notes.empty());
    auto out = endpoint(*tmg, "q$iopad");
    EXPECT_EQ(out.clock_pair.start.edge, RISING_EDGE);
    EXPECT_EQ(out.clock_pair.end.edge, FALLING_EDGE);
    EXPECT_EQ(out.setup_slack, ns(5) - out.delay);
    auto in = endpoint(*tmg, "in_ff");
    EXPECT_EQ(in.clock_pair.start.edge, RISING_EDGE);
    EXPECT_EQ(in.setup_slack, ns(10) - in.delay);
}

// The clock put out by a register that puts out both of its edges (CC_ODDR with the data 1 and 0)
TEST_F(GateMateForwardedClockTest, through_a_ddr_register)
{
    design();
    top_output("cko", oddr("cko_ddr", vcc(), gnd(), clk, clk));
    sdc("create_generated_clock -name fwd -source [get_ports clk] [get_ports cko]\n"
        "set_output_delay -clock fwd 3 [get_ports q]\n");
    implement();
    auto tmg = analyse();
    auto &result = tmg->get_timing_result();
    EXPECT_TRUE(result.io_notes.empty()) << (result.io_notes.empty() ? "" : result.io_notes.front());
    ASSERT_EQ(result.forwarded_clocks.size(), 1u);
    EXPECT_EQ(result.forwarded_clocks.front().clock_net, clock_net());
    EXPECT_GT(result.forwarded_clocks.front().min_delay, ns(0.5));
    auto ep = endpoint(*tmg, "q$iopad");
    EXPECT_EQ(tmg->get_pair_constraints(ep.domain_pair).io_capture_ref, 1);
    EXPECT_EQ(ep.setup_slack, ns(10) - ep.delay);
}

// From an input to an output: with both against the clock at the port, its way is on neither side; with the input
// against the clock at its source, both ways are between the two edges
TEST_F(GateMateForwardedClockTest, pad_to_pad)
{
    design_with_clock_output();
    sdc("set_input_delay -clock sys 2 [get_ports p]\n"
        "set_output_delay -clock sys 3 [get_ports r]\n");
    implement();
    auto at_source = analyse();
    auto before = endpoint(*at_source, "r$iopad");
    DelayPair latency = at_source->get_clock_latency(clock_net());

    sdc("set_input_delay -clock fwd 2 [get_ports p]\n"
        "set_output_delay -clock fwd 3 [get_ports r]\n");
    auto both = analyse();
    auto ep = endpoint(*both, "r$iopad");
    EXPECT_EQ(ep.setup_slack, before.setup_slack);
    EXPECT_EQ(ep.hold_slack, before.hold_slack);
    auto longest = both->get_endpoint_path(ep, true);
    EXPECT_EQ(segment(longest, SegmentType::CLK_SKEW), 0) << describe(longest);
    EXPECT_EQ(sum(longest), ep.delay);

    sdc("set_input_delay -clock sys 2 [get_ports p]\n");
    auto mixed = analyse();
    ASSERT_EQ(mixed->get_timing_result().forwarded_clocks.size(), 1u);
    auto fc = mixed->get_timing_result().forwarded_clocks.front();
    ep = endpoint(*mixed, "r$iopad");
    // (the launch edge at the clock pad at its latest, the capture edge at the port at its earliest; for hold
    // the other way round)
    EXPECT_EQ(ep.setup_slack, before.setup_slack + latency.minDelay() + fc.min_delay);
    EXPECT_EQ(ep.hold_slack, before.hold_slack - latency.maxDelay() - fc.max_delay);
    longest = mixed->get_endpoint_path(ep, true);
    auto shortest = mixed->get_endpoint_path(ep, false);
    SCOPED_TRACE(describe(longest));
    EXPECT_EQ(sum(longest), ep.delay);
    EXPECT_EQ(ep.setup_slack, ns(10) - ep.delay);
    EXPECT_EQ(ep.hold_slack, sum(shortest));
    auto clocks = mixed->get_clock_delays(ep, longest);
    EXPECT_EQ(segment(longest, SegmentType::CLK_SKEW), clocks.launch - clocks.capture);
    EXPECT_EQ(clocks.launch, -latency.minDelay());
    EXPECT_EQ(clocks.capture, fc.min_delay);
}

// A port that puts out no clock of the registers: the delays against it are not applied
TEST_F(GateMateForwardedClockTest, no_clock_at_the_port)
{
    design();
    // (a divided clock: a register that toggles)
    CellInfo *divider = dff("divider", gnd(), clk);
    NetInfo *half = divider->getPort(id_Q);
    divider->disconnectPort(id_D);
    divider->connectPort(id_D, lut1("toggle", 0b01, half));
    top_output("cko", half);
    sdc("create_generated_clock -name data [get_ports r]\n"
        "create_generated_clock -name divided [get_ports cko]\n"
        "set_output_delay -clock data 3 [get_ports q]\n"
        "set_input_delay -clock divided 2 [get_ports d]\n");
    implement();
    auto tmg = analyse();
    auto &result = tmg->get_timing_result();
    EXPECT_TRUE(result.io_slacks.empty());
    EXPECT_TRUE(result.forwarded_clocks.empty());
    ASSERT_EQ(result.io_notes.size(), 2u);
    EXPECT_NE(result.io_notes.at(0).find("the clock 'data' at the port 'r' comes from no clock of registers"),
              std::string::npos)
            << result.io_notes.at(0);
    EXPECT_NE(result.io_notes.at(1).find("the clock 'divided' at the port 'cko' comes from no clock of registers"),
              std::string::npos)
            << result.io_notes.at(1);
    EXPECT_EQ(endpoint(*tmg, "q$iopad").clock_pair.end.clock, IdString());
}

// The whole flow: the output must hold its data after the edge of the clock at the port, which is late by the way
// of the clock to the port; the hold fixer pads the path, and the report names the clock
TEST_F(GateMateForwardedClockTest, hold_fixed_against_the_clock_at_the_port)
{
    design_with_clock_output();
    // (hold is short by about 1.2 ns in the best corner. The padding for it is about twice as long in the worst
    // corner, which the setup check there has to have left: with -min -3 it has not, and the path stays as it is)
    sdc("set_output_delay -clock fwd -max 2 [get_ports q]\n"
        "set_output_delay -clock fwd -min -1.5 [get_ports q]\n");
    bool had_error = had_nonfatal_error;
    had_nonfatal_error = false;
    std::string log;
    {
        LogCapture capture;
        place_and_route();
        log = capture.str();
    }
    EXPECT_FALSE(had_nonfatal_error) << log;
    had_nonfatal_error = had_error;
    EXPECT_EQ(log.find("ERROR"), std::string::npos) << log;
    EXPECT_NE(log.find("clock 'fwd' at the port 'cko': "), std::string::npos) << log;
    auto *impl = static_cast<GateMateImpl *>(ctx->uarch.get());
    int delay_cells = 0;
    for (auto &cell : ctx->cells)
        delay_cells += impl->is_delay_cell(cell.second.get());
    EXPECT_GT(delay_cells, 0) << log;
    for (const char *corner : {"best_spd", "typ_spd", "worst_spd"})
        EXPECT_NE(log.find(std::string("Hold check in timing corner '") + corner + "': 0 violation(s)"),
                  std::string::npos)
                << corner << " in:\n"
                << log;
    auto tmg = analyse();
    auto out = endpoint(*tmg, "q$iopad");
    EXPECT_GE(out.hold_slack, ns(0.1));
    EXPECT_GE(out.setup_slack, 0);
    EXPECT_TRUE(tmg->get_timing_result().min_delay_violations.empty());
}
