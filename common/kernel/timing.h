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
    // The timing exceptions (multicycle paths) whose -from (launch) or -to (capture) lists name the start or end
    // points of the domain: start and end points of one clock edge named by different constraints are different
    // domains, so that each pair of domains has one exception. 0: named by none
    int tag = 0;
    ClockDomainKey(IdString clock_net, ClockEdge edge, int tag = 0) : clock(clock_net), edge(edge), tag(tag) {};
    inline bool is_async() const { return clock == IdString(); }

    unsigned int hash() const { return mkhash(mkhash(clock.hash(), int(edge)), tag); }

    inline bool operator==(const ClockDomainKey &other) const
    {
        return (clock == other.clock) && (edge == other.edge) && (tag == other.tag);
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
        // Slacks; the hold slack is max() where no hold analysis applies (unclocked endpoints, unrelated clocks)
        delay_t setup_slack, hold_slack;
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
        // The exceptions that apply to the setup and the hold check (-1: none)
        int setup_exception, hold_exception;
        // All exceptions matching the pair: also those a more specific or later one overrides, and false paths
        std::vector<int> exceptions;
    };
    PairConstraints get_pair_constraints(domain_id_t domain_pair);

    // The clock delays (the routing of the clock nets to the clock pins) that the analysis counts in the arrival
    // time at the start point and in the required time at the end point of a path of an end point (with
    // with_clock_skew; 0 for unclocked points)
    struct PathClockDelays
    {
        delay_t launch = 0, capture = 0;
    };
    PathClockDelays get_clock_delays(const EndpointTiming &ep, const CriticalPath &path);

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
    void topo_sort();
    void setup_port_domains();
    void identify_related_domains();

    // The timing exceptions of the design, resolved for this analysis
    void resolve_timing_exceptions();
    // The domain tag of a start point (launch) or an end point (capture) clocked by clock_port
    int point_tag(CellPortKey port, IdString clock_port, bool launch);
    // The cycles and the shifts of the setup and hold checks of a domain pair from its multicycle paths, and the
    // exceptions matching it
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
    void build_slack_histogram_report();

    std::vector<CriticalPath> get_min_delay_violations();

    dict<domain_id_t, delay_t> max_delay_by_domain_pairs();

    // The delay a path between the clocks of a domain pair may have
    delay_t get_max_delay(domain_id_t domain_pair);

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

    CellInfo *cell_info(const CellPortKey &key);
    PortInfo &port_info(const CellPortKey &key);

    domain_id_t domain_id(IdString cell, IdString clock_port, ClockEdge edge, int tag = 0);
    domain_id_t domain_id(const NetInfo *net, ClockEdge edge, int tag = 0);
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
