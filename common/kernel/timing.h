/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  gatecat <gatecat@ds0.me>
 *  Copyright (C) 2023  rowanG077 <goemansrowan@gmail.com>
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

#ifndef TIMING_H
#define TIMING_H

#include <map>
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

struct CellPortKey
{
    CellPortKey() {};
    CellPortKey(IdString cell, IdString port) : cell(cell), port(port) {};
    explicit CellPortKey(const PortRef &pr)
    {
        NPNR_ASSERT(pr.cell != nullptr);
        cell = pr.cell->name;
        port = pr.port;
    }
    IdString cell, port;
    unsigned int hash() const { return mkhash(cell.hash(), port.hash()); }
    inline bool operator==(const CellPortKey &other) const { return (cell == other.cell) && (port == other.port); }
    inline bool operator!=(const CellPortKey &other) const { return (cell != other.cell) || (port != other.port); }
    inline bool operator<(const CellPortKey &other) const
    {
        return cell == other.cell ? port < other.port : cell < other.cell;
    }
};

struct ClockDomainKey
{
    IdString clock;
    ClockEdge edge;
    // The timing exceptions (multicycle and false paths) whose -from (launch) or -to (capture) lists name the start
    // or end points of the domain: start and end points of one clock edge named by different constraints are
    // different domains, so that each pair of domains has one exception. 0: named by none
    int tag = 0;
    // The start points (launch) or end points (capture) with an input or output delay against the clock edge
    // (IO_POINT): they are at the source of the clock (its pad), not behind its network as the registers are, and
    // without a maximum or a minimum delay their paths have no setup or no hold check (IO_NO_SETUP, IO_NO_HOLD).
    // 0: the registers of the clock
    // The bits from IO_REF_SHIFT on: what the delays are against. 0: the clock at its source; else 1 + the index
    // of a clock the design puts out (Context::forwarded_clocks), and the points are at the port of that clock.
    enum
    {
        IO_POINT = 1,
        IO_NO_SETUP = 2,
        IO_NO_HOLD = 4,
        IO_REF_SHIFT = 3,
    };
    int io = 0;
    inline int io_ref() const { return io >> IO_REF_SHIFT; }
    ClockDomainKey(IdString clock_net, ClockEdge edge, int tag = 0, int io = 0)
            : clock(clock_net), edge(edge), tag(tag), io(io) {};
    inline bool is_async() const { return clock == IdString(); }

    unsigned int hash() const { return mkhash(mkhash(mkhash(clock.hash(), int(edge)), tag), io); }

    inline bool operator==(const ClockDomainKey &other) const
    {
        return (clock == other.clock) && (edge == other.edge) && (tag == other.tag) && (io == other.io);
    }
};

typedef int domain_id_t;

struct ClockDomainPairKey
{
    domain_id_t launch, capture;
    ClockDomainPairKey(domain_id_t launch, domain_id_t capture) : launch(launch), capture(capture) {};
    inline bool operator==(const ClockDomainPairKey &other) const
    {
        return (launch == other.launch) && (capture == other.capture);
    }
    unsigned int hash() const { return mkhash(launch, capture); }
};

struct TimingAnalyser
{
  public:
    TimingAnalyser(Context *ctx);

    void setup(bool update_net_timings = false, bool update_histogram = false, bool update_crit_paths = false);
    void run(bool update_route_delays = true, bool update_net_timings = false, bool update_histogram = false,
             bool update_crit_paths = false);

    // This is used when routers etc are not actually binding detailed routing (due to congestion or an abstracted
    // model), but want to re-run STA with their own calculated delays
    void set_route_delay(CellPortKey port, DelayPair value);

    float get_criticality(CellPortKey port) const { return ports.at(port).worst_crit; }
    // The routing delay into an input port as the analysis takes it (see analysis_route_delay)
    DelayPair get_route_delay(CellPortKey port) const { return ports.at(port).route_delay; }
    float get_setup_slack(CellPortKey port) const { return ports.at(port).worst_setup_slack; }
    float get_domain_setup_slack(CellPortKey port) const
    {
        delay_t slack = std::numeric_limits<delay_t>::max();
        for (const auto &dp : ports.at(port).domain_pairs) {
            auto &pair = domain_pairs.at(dp.first);
            if (pair.worst_setup_slack != std::numeric_limits<delay_t>::max())
                slack = std::min(slack, pair.worst_setup_slack + pair.setup_shift);
        }
        return slack;
    }

    // The slacks of the paths through a port (any port the analysis has, typically the input port of a net's user),
    // after setup() or run(), without the path reports:
    // - the worst hold slack, over the domain pairs that have a hold check (as get_min_delay_violations: a clocked
    //   launch, the same or related clocks, register end points or outputs with a minimum output delay, no false
    //   path, no input without a minimum input delay), with the hold edge and multicycle
    //   shift of the pair; max() where there is none (and with setup_only). It is <= 0 exactly where a path
    //   violating hold passes through the port, and at an end point it is the least EndpointTiming::hold_slack of
    //   the point.
    delay_t get_hold_slack(CellPortKey port) const;
    // - the worst setup slack in absolute terms, over all domain pairs (also between the two edges of a clock,
    //   multicycle and unrelated or unclocked pairs, each against get_max_delay as get_endpoint_timings has it,
    //   without the false paths); at an end point the least EndpointTiming::setup_slack of the point; max() if
    //   only false paths pass through the port. (get_setup_slack covers the pairs of one clock edge only, against
    //   their period.)
    delay_t get_absolute_setup_slack(CellPortKey port);

    dict<std::pair<IdString, IdString>, delay_t> get_clock_delays() const { return clock_delays; }

    // The clock edges that clock cells of the design (not async)
    std::vector<ClockEvent> get_clock_events() const
    {
        std::vector<ClockEvent> events;
        pool<ClockEvent> seen;
        for (auto &dom : domains)
            if (!dom.key.is_async() && seen.insert(ClockEvent{dom.key.clock, dom.key.edge}).second)
                events.push_back(ClockEvent{dom.key.clock, dom.key.edge});
        return events;
    }

    TimingResult &get_timing_result() { return result; }

    // The timing of one endpoint for one launch/capture domain pair
    struct EndpointTiming
    {
        domain_id_t domain_pair;
        CellPortKey endpoint;
        ClockPair clock_pair;
        // Longest path delay, including setup time and clock-to-clock delay
        delay_t delay;
        // Slacks; the hold slack is max() where no hold analysis applies (unclocked endpoints, unrelated clocks),
        // and either is max() where a false path takes the check away
        delay_t setup_slack, hold_slack;
        // A false path (set_false_path) takes the setup check, or a hold check there would be, away
        bool false_setup = false, false_hold = false;
        // An input or output delay of the path has no maximum (no setup check) or no minimum (no hold check there
        // would be): the slack is max()
        bool no_setup = false, no_hold = false;
    };
    // All endpoints with their timing, for listing paths
    std::vector<EndpointTiming> get_endpoint_timings();
    // The longest (setup) or the shortest (hold) path into an endpoint
    CriticalPath get_endpoint_path(const EndpointTiming &ep, bool longest_path)
    {
        return build_critical_path_report(ep.domain_pair, ep.endpoint, longest_path);
    }

    // How the required times of the paths of a launch/capture domain pair come about: from the clock constraints,
    // and from the timing exceptions (ctx->timing_exceptions) whose -from and -to lists match the pair
    struct PairConstraints
    {
        ClockPair clock_pair;
        // The time from the launch to the capture edge that the clocks give (one period, or the time between the
        // two edges of a clock), without multicycle paths
        delay_t clock_required;
        // The multicycle path: its clock cycles for setup, the time it adds to the required time, and how much
        // later than its default edge the hold check is. The default hold check is at the capture edge hold_edge
        // before the launch edge (0, or between the two edges of one clock its high or low time).
        int setup_cycles;
        delay_t setup_shift, hold_shift, hold_edge;
        // A false path: the setup or the hold check of the paths is not made (a false path goes before every
        // multicycle path; the cycles and shifts above are those of the multicycle paths all the same, as the hold
        // check of a path that is false for setup only follows them)
        bool false_setup, false_hold;
        // The exceptions that apply to the setup and the hold check (-1: none): the false path, else the
        // multicycle path
        int setup_exception, hold_exception;
        // All exceptions matching the pair: also those a more specific or later one, or a false path, overrides
        std::vector<int> exceptions;
        // The paths start at inputs with an input delay (io_launch), or end at outputs with an output delay
        // (io_capture), against the clock; such a delay without a maximum or a minimum leaves the setup or the
        // hold check out
        bool io_launch, io_capture, no_setup, no_hold;
        // What these delays are against: 0 the clock at its source, else 1 + the index of the clock the design
        // puts out (Context::forwarded_clocks)
        int io_launch_ref = 0, io_capture_ref = 0;
    };
    PairConstraints get_pair_constraints(domain_id_t domain_pair);

    // The clock delays that the analysis counts in the arrival time at the start point and in the required time at
    // the end point of a path of an end point: the routing of the clock nets to the clock pins (with
    // with_clock_skew; 0 for unclocked points). On a path from an input or to an output with a delay against the
    // clock, that end is at the source of the clock (0), and the register at the other end has the delay of the
    // clock network from there to its clock net in addition (get_clock_latency: the value that is against the
    // check, which is the longest path's setup or the shortest path's hold check).
    struct PathClockDelays
    {
        delay_t launch = 0, capture = 0;
    };
    PathClockDelays get_clock_delays(const EndpointTiming &ep, const CriticalPath &path, bool longest_path = true);

    // The input delays on a start point or the output delays on an end point (indices into ctx->io_delays)
    std::vector<int> get_io_delays(CellPortKey point, bool output) const;
    // The delay from the source of a clock to the driver of a clock net: back through the clock buffers and the
    // routing between them to the pad, or to where another clock comes in (the reference of a PLL); its least and
    // its greatest value. Of the clock nets that inputs or outputs with a delay are timed against; 0 for others.
    // The paths from an input and to an output have no part of the clock network in common with the clock of the
    // device outside, so each check takes the value against it: a late capture clock for the hold of an input and
    // a late launch clock for the setup of an output (the greatest), an early one for the setup of an input and
    // the hold of an output (the least).
    // With ref (1 + the index of a clock the design puts out), the points are at the port of that clock instead:
    // its edge there is after the edge at the driver of the clock net, by the way from the net to the port, so
    // the value is that delay negated (its least: the greatest delay to the port).
    DelayPair get_clock_latency(IdString clock_net, int ref = 0) const
    {
        auto found = clock_latency.find(std::make_pair(clock_net, ref));
        return found == clock_latency.end() ? DelayPair(0) : found->second;
    }

    // Enable analysis of clock skew between FFs.
    bool with_clock_skew = false;

    bool setup_only = false;
    // Analyse despite combinational loops (as with the timing/ignoreLoops setting)
    bool ignore_loops = false;
    bool have_loops = false;
    bool updated_domains = false;

  private:
    void init_ports();
    void get_cell_delays();
    void get_route_delays();
    // The routing delay of an arc as the analysis takes it: the least and the greatest delay into data pins (unless
    // setup_only), one value (the greatest) into clock pins
    DelayPair analysis_route_delay(const NetInfo *net, const PortRef &usr) const;
    void topo_sort();
    void setup_port_domains();
    void identify_related_domains();

    // The timing exceptions of the design, resolved for this analysis
    void resolve_timing_exceptions();
    // The input and output delays of the design on its start and end points (io_starts, io_ends)
    void resolve_io_delays();
    // clock_latency, with the routing delays of the run
    void update_clock_latencies();
    DelayPair clock_source_latency(const NetInfo *net);
    // The way of a clock back from a point: the nets on it, each with the delay from its driver to the point
    typedef std::vector<std::pair<IdString, DelayPair>> ClockWay;
    // Back from a net (already 'delay' from the point): through the cell that drives it by the input the clock
    // comes in by, and over the net into that input, to the source of the clock. Returns the delay from there.
    // clock: the clock looked for; none takes the first clock met. through_ddr: also through a register that puts
    // out both edges of its clock (which so puts out the clock itself).
    DelayPair clock_way(const NetInfo *net, const ClockConstraint *clock, DelayPair delay, ClockWay *way,
                        bool through_ddr);
    // The routing delay into a pin of a clock's way: its least and its greatest value
    DelayPair clock_route_delay(const CellInfo *cell, IdString pin);
    // The start points (input) or end points (output) at a top level port, or of the cell of its pad buffer
    std::vector<CellPortKey> pad_points(IdString port, IdString cell, bool output);
    // The delay of the pad buffer of such a point: of the cell arcs between it and the pins of the pad side (which
    // the arch gives no timing class). pad_pin: the pin of the pad side (that on pad_net, if several have an arc)
    DelayPair pad_buffer_delay(CellPortKey point, bool output, const NetInfo *pad_net, IdString &pad_pin);
    // A clock the design puts out (index into ctx->forwarded_clocks): the time of its edge at the port after the
    // edge at the driver of a clock net. False if the clock at the port does not come from that net (or from the
    // clock network before it).
    bool forwarded_clock_delay(int index, IdString clock_net, DelayPair &delay);
    // The domain tag of a start point (launch) or an end point (capture) clocked by clock_port
    int point_tag(CellPortKey port, IdString clock_port, bool launch);
    // The cycles and the shifts of the setup and hold checks of a domain pair from its multicycle paths, the checks
    // its false paths take away, and the exceptions matching it
    void apply_timing_exceptions(domain_id_t pair);
    // Whether two domains are the same clock edge (they may differ in their tags)
    bool same_event(domain_id_t a, domain_id_t b) const
    {
        return domains.at(a).key.clock == domains.at(b).key.clock && domains.at(a).key.edge == domains.at(b).key.edge;
    }

    void reset_times();

    void walk_forward();
    void walk_backward();

    void compute_slack();
    void compute_criticality();

    // Walk the endpoint back to a startpoint and get back the input ports walked
    // and the startpoint.
    std::vector<PortRef> walk_crit_path(domain_id_t domain_pair, CellPortKey endpoint, bool longest_path);

    void build_detailed_net_timing_report();
    // longest_path indicate whether to follow the longest or shortest path from endpoint to startpoint
    // longest paths are interesting for setup violations and shortest paths are interesting for hold violations
    CriticalPath build_critical_path_report(domain_id_t domain_pair, CellPortKey endpoint, bool longest_path);
    void build_crit_path_reports();
    // The worst slacks of the multicycle paths (TimingResult::multicycle_slacks)
    void build_multicycle_slack_report();
    // The worst slacks of the input and output delays (TimingResult::io_slacks)
    void build_io_slack_report();
    void build_slack_histogram_report();

    std::vector<CriticalPath> get_min_delay_violations();

    dict<domain_id_t, delay_t> max_delay_by_domain_pairs();

    // The delay a path between the clocks of a domain pair may have
    delay_t get_max_delay(domain_id_t domain_pair);
    // get_max_delay per domain pair, filled on demand by get_absolute_setup_slack (cleared by run())
    std::vector<delay_t> max_delay_cache;
    // Whether the paths of a domain pair have a hold check (see get_hold_slack)
    bool has_hold_check(domain_id_t domain_pair) const;
    // Whether the clocks of a domain pair time its paths against each other: the same or related clocks
    bool timed_pair(domain_id_t domain_pair) const;
    // From an input to an output with delays against one clock net, the clock network is on neither side: what the
    // times of the two points differ by in its latency (each has the value against its own check) is added to
    // the setup and to the hold slack of the pair again. 0 for other pairs.
    delay_t io_pair_spread(domain_id_t domain_pair) const;

    // get the N worst endpoints for a given domain pair
    std::vector<CellPortKey> get_worst_eps(domain_id_t domain_pair, int count);

    // Set arrival/required times if more/less than the current value
    void set_arrival_time(CellPortKey target, domain_id_t domain, DelayPair arrival, int path_length,
                          CellPortKey prev = CellPortKey());
    void set_required_time(CellPortKey target, domain_id_t domain, DelayPair required, int path_length,
                           CellPortKey prev = CellPortKey());

    // To avoid storing the domain tag structure (which could get large when considering more complex constrained tag
    // cases), assign each domain an ID and use that instead
    // An arrival or required time entry. Stores both the min/max delays; and the traversal to reach them for critical
    // path reporting
    struct ArrivReqTime
    {
        DelayPair value;
        CellPortKey bwd_min, bwd_max;
        int path_length;
    };
    // Data per port-domain tuple
    struct PortDomainPairData
    {
        delay_t setup_slack = std::numeric_limits<delay_t>::max(), hold_slack = std::numeric_limits<delay_t>::max();
        int max_path_length = 0;
        float criticality = 0;
    };

    // A cell timing arc, used to cache cell timings and reduce the number of potentially-expensive Arch API calls
    struct CellArc
    {

        enum ArcType
        {
            COMBINATIONAL,
            SETUP,
            HOLD,
            CLK_TO_Q,
            STARTPOINT,
            ENDPOINT,
        } type;

        IdString other_port;
        DelayQuad value;
        // Clock polarity, not used for combinational arcs
        ClockEdge edge;

        CellArc(ArcType type, IdString other_port, DelayQuad value)
                : type(type), other_port(other_port), value(value), edge(RISING_EDGE) {};
        CellArc(ArcType type, IdString other_port, DelayQuad value, ClockEdge edge)
                : type(type), other_port(other_port), value(value), edge(edge) {};
    };

    // Timing data for every cell port
    struct PerPort
    {
        CellPortKey cell_port;
        PortType type;
        // per domain timings
        dict<domain_id_t, ArrivReqTime> arrival;
        dict<domain_id_t, ArrivReqTime> required;
        dict<domain_id_t, PortDomainPairData> domain_pairs;
        // cell timing arcs to (outputs)/from (inputs)  from this port
        std::vector<CellArc> cell_arcs;
        // routing delay into this port (input ports only)
        DelayPair route_delay{0};
        // worst criticality and slack across domain pairs
        float worst_crit = 0;
        delay_t worst_setup_slack = std::numeric_limits<delay_t>::max(),
                worst_hold_slack = std::numeric_limits<delay_t>::max();
    };

    struct PerDomain
    {
        PerDomain(ClockDomainKey key) : key(key) {};
        ClockDomainKey key;
        // these are pairs (signal port; clock port)
        std::vector<std::pair<CellPortKey, IdString>> startpoints, endpoints;
    };

    struct PerDomainPair
    {
        PerDomainPair(ClockDomainPairKey key) : key(key) {};
        ClockDomainPairKey key;
        DelayPair period{0};
        delay_t worst_setup_slack, worst_hold_slack;
        // Multicycle path: the clock cycles for setup, the time it adds to the required time, and how much later
        // than the launch edge the hold check is
        int setup_cycles = 1;
        delay_t setup_shift = 0, hold_shift = 0;
        // False path: the setup or the hold check of the pair is not made. Its slacks do not count in the worst
        // slacks, the criticality, Fmax and the reports.
        bool false_setup = false, false_hold = false;
        // Likewise for a pair from inputs or to outputs whose delays have no maximum (setup) or minimum (hold)
        bool no_setup = false, no_hold = false;
        bool setup_checked() const { return !false_setup && !no_setup; }
        bool hold_checked() const { return !false_hold && !no_hold; }
        // The exceptions applying to the setup and the hold check, and all matching the pair (indices into
        // ctx->timing_exceptions)
        int setup_exception = -1, hold_exception = -1;
        std::vector<int> exceptions;
        // The default hold check is at the capture edge this long before the launch edge: 0, or between the two
        // edges of one clock its high (falling launch) or low time (rising launch). The hold check of the pair is
        // hold_shift - hold_edge after the launch edge.
        delay_t hold_edge = 0;
        // The pairs of the same launch and capture clock edges (which differ in their domain tags only) form a
        // group, whose criticalities are scaled together
        int group = -1;
    };

    // A timing exception resolved for this analysis: the objects of its -from and -to lists
    struct ResolvedException
    {
        const TimingException *constraint;
        struct Points
        {
            bool any = true; // an empty list: every point
            pool<IdString> cells, nets, clocks;
            dict<IdString, std::vector<IdString>> pins;
        } from, to;
    };
    std::vector<ResolvedException> exceptions;
    // The sets of tags: the constraints whose -from (2 * index) or -to (2 * index + 1) lists name a point
    std::vector<std::vector<int>> tag_sets{{}};
    std::map<std::vector<int>, int> tag_ids{{{}, 0}};

    // An input delay on a start point or an output delay on an end point, resolved for this analysis
    struct IoPoint
    {
        int index;      // in ctx->io_delays
        IdString clock; // the clock net whose registers the paths are timed against
        // What the delay is against: 0 the clock at its source, else 1 + the index of the clock the design puts
        // out; and the edge of the clock net (the other one for an inverted clock at the port)
        int ref = 0;
        ClockEdge edge = RISING_EDGE;
        domain_id_t domain = -1;
        // The delay outside of the device, and that of the pad buffer between the pad and the point (over the
        // cell arc from or to pad_pin; none: no such arc)
        DelayPair external{0}, buffer{0};
        IdString pad_pin;
    };
    dict<CellPortKey, std::vector<IoPoint>> io_starts, io_ends;
    // The point of a start or end point in a domain (of several, the one of the longest or the shortest delay)
    const IoPoint *io_point(bool output, CellPortKey port, domain_id_t domain, bool longest) const;
    // Per clock net and reference of the points: the delay from the source of the clock to the driver of the net,
    // or the delay from there to the port of a clock the design puts out, negated (see get_clock_latency)
    dict<std::pair<IdString, int>, DelayPair> clock_latency;
    // The delays that are not applied, and why
    std::vector<std::string> io_notes;

    CellInfo *cell_info(const CellPortKey &key);
    PortInfo &port_info(const CellPortKey &key);

    domain_id_t domain_id(IdString cell, IdString clock_port, ClockEdge edge, int tag = 0);
    domain_id_t domain_id(const NetInfo *net, ClockEdge edge, int tag = 0, int io = 0);
    // The domain of async start or end points with a domain tag
    domain_id_t async_domain_id(int tag);
    domain_id_t domain_pair_id(domain_id_t launch, domain_id_t capture);

    void copy_domains(const CellPortKey &from, const CellPortKey &to, bool backwards);

    [[maybe_unused]] static const std::string arcType_to_str(CellArc::ArcType typ);

    dict<CellPortKey, PerPort> ports;
    dict<ClockDomainKey, domain_id_t> domain_to_id;
    dict<ClockDomainPairKey, domain_id_t> pair_to_id;
    std::vector<PerDomain> domains;
    std::vector<PerDomainPair> domain_pairs;
    dict<std::pair<IdString, IdString>, delay_t> clock_delays;

    std::vector<CellPortKey> topological_order;

    domain_id_t async_clock_id;

    Context *ctx;

    TimingResult result;
};

// Perform timing analysis and optionaly print out slack histogram, fmax and critical paths
void timing_analysis(Context *ctx, bool slack_histogram = true, bool print_fmax = true, bool print_path = false,
                     bool warn_on_failure = false, bool update_results = false);

NEXTPNR_NAMESPACE_END

#endif
