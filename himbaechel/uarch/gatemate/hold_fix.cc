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

// Hold fixing after routing (docs/timing_constraints.md, "Hold fixing"): delay cells (delay_cell.cc) on the
// connections whose hold slack is below the margin in any hold corner, where the setup slack of the routed grade
// allows the delay. In rounds: analyse every corner exactly; per end point below the margin pick the connection
// nearest to it on its shortest path whose setup slack covers the delay (going upstream where it does not, which also
// keeps a connection shared with paths that have no setup slack to spare untouched); the fewest cells whose least
// delay covers the need in every corner, in a free CPE near the sink; insert, route (route_ripped), analyse again,
// keep what verifies and undo the rest.

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <memory>
#include <string>

#include "gatemate.h"
#include "log.h"
#include "timing.h"
#include "util.h"

#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {

constexpr delay_t no_slack = std::numeric_limits<delay_t>::max();

// A connection: a net and one of its sinks, by name (a delay cell moves the sink to its own output net)
struct Conn
{
    IdString net;
    CellPortKey sink;
    bool operator==(const Conn &other) const { return net == other.net && sink == other.sink; }
    unsigned int hash() const { return mkhash(net.hash(), sink.hash()); }
};

// An end point below the margin in one corner, for one domain pair, with the connections of its shortest path
// (nearest to the end point first)
struct Failing
{
    CellPortKey endpoint;
    int corner = 0;
    delay_t slack = 0;
    std::vector<Conn> path;
    // a multicycle constraint moved the hold check later (a setup multiplier without -hold)
    bool hold_moved = false;
    // the period of the capturing clock (0 if not constrained)
    delay_t period = 0;
};

struct CornerTiming
{
    std::string grade;
    // The input ports whose hold slack is below the margin: the hold slack and the least routing delay into the port.
    // (Every connection on the shortest path of an end point below the margin is one of them.)
    dict<CellPortKey, std::pair<delay_t, delay_t>> below;
    std::vector<Failing> failing;
    delay_t worst = no_slack;
    int endpoints_below = 0;
};

// The setup result of the routed grade that the pass keeps to
struct SetupSummary
{
    // per clock: Fmax achieved and the constraint (MHz)
    std::map<std::string, std::pair<double, double>> fmax;
    int64_t tns = 0;
    delay_t worst = no_slack;
};

struct Analysis
{
    std::vector<CornerTiming> corners;
    // The analyser of the routed grade (setup slacks): valid until the netlist or the routing changes
    std::unique_ptr<TimingAnalyser> routed;
    SetupSummary setup;
};

// The delay cells for one connection: a chain of cells from the net's source side to the sink
struct Plan
{
    Conn conn;
    CellPortKey endpoint; // the end point it was chosen for
    std::vector<BelId> bels;
    std::vector<int> taps;
    // per corner the hold slack at the sink before (no_slack where it was not below the margin), the setup slack
    std::vector<delay_t> hold_before;
    delay_t setup_before = no_slack;
    std::vector<IdString> cells; // as inserted, from the source side
};

// Why the connections of an end point's path did not take delay cells, the most telling reason first
struct Why
{
    bool chain_cap = false, no_cpe = false, not_routable = false, no_gain = false, no_budget = false;
    delay_t need = 0, budget = std::numeric_limits<delay_t>::lowest();
};

struct HoldFixer
{
    GateMateImpl &impl;
    Context *ctx;
    // The hold corners, the routed grade last (analysed last, so that it stays selected)
    std::vector<std::pair<int, int>> corners;
    int routed = 0;
    delay_t margin = 0, guard = 0;
    // The taps' least delay per corner, and their greatest in the routed grade
    std::vector<std::vector<delay_t>> tap_min;
    std::vector<delay_t> tap_max;
    // Connections that failed (setup, no gain, routing) and are not tried again
    dict<Conn, GateMateImpl::HoldFixReason> rejected;
    // Per end point the last reason it could not be repaired, by corner of the entry
    dict<CellPortKey, Why> why;
    int inserted_total = 0;

    HoldFixer(GateMateImpl &impl) : impl(impl), ctx(impl.ctx) {}

    std::string name(const CellPortKey &port) const { return port.cell.str(ctx) + "." + port.port.str(ctx); }

    // The least routing delay between two wires, as the other repairs estimate it
    delay_t estimate(WireId from, WireId to) const
    {
        if (from == WireId() || to == WireId())
            return 0;
        delay_t d = impl.has_least_delay() ? impl.least_delay(from, to) : -1;
        return d >= 0 ? d : ctx->estimateDelay(from, to);
    }

    void select(int corner) { impl.select_speed_grade(corners.at(corner).first, corners.at(corner).second); }

    SetupSummary summarise(TimingAnalyser &tmg)
    {
        SetupSummary s;
        dict<int, int> cycles;
        for (auto &ep : tmg.get_endpoint_timings()) {
            s.worst = std::min(s.worst, ep.setup_slack);
            if (ep.setup_slack < 0)
                s.tns += ep.setup_slack;
            auto &start = ep.clock_pair.start, &end = ep.clock_pair.end;
            // (a false path limits no clock)
            if (start.clock == IdString() || start.clock != end.clock || ep.delay <= 0 || ep.false_setup)
                continue;
            if (!cycles.count(ep.domain_pair))
                cycles[ep.domain_pair] = tmg.get_pair_constraints(ep.domain_pair).setup_cycles;
            // (as the critical path report has Fmax)
            double c = (start.edge == end.edge ? 1.0 : 0.5) + (cycles.at(ep.domain_pair) - 1);
            double f = 1000 * c / ctx->getDelayNS(ep.delay);
            std::string clock = start.clock.str(ctx);
            auto found = s.fmax.find(clock);
            if (found == s.fmax.end() || f < found->second.first) {
                double target = ctx->settings.count(ctx->id("target_freq"))
                                        ? ctx->setting<float>("target_freq") / 1e6
                                        : 0;
                if (ctx->nets.count(start.clock) && ctx->nets.at(start.clock)->clkconstr)
                    target = 1000 / ctx->getDelayNS(ctx->nets.at(start.clock)->clkconstr->period.minDelay());
                s.fmax[clock] = {f, target};
            }
        }
        return s;
    }

    // Every corner analysed exactly (each with its speed grade selected), the routed grade last
    Analysis analyse()
    {
        Analysis a;
        a.corners.resize(corners.size());
        for (int c = 0; c < int(corners.size()); c++) {
            select(c);
            auto tmg = std::make_unique<TimingAnalyser>(ctx);
            tmg->setup_only = false;
            tmg->with_clock_skew = true;
            tmg->setup();
            auto &ct = a.corners.at(c);
            ct.grade = IdString(ctx->speed_grade->name).str(ctx);
            pool<CellPortKey> endpoints_below;
            for (auto &ep : tmg->get_endpoint_timings()) {
                if (ep.hold_slack == no_slack)
                    continue;
                ct.worst = std::min(ct.worst, ep.hold_slack);
                if (ep.hold_slack >= margin)
                    continue;
                endpoints_below.insert(ep.endpoint);
                Failing f;
                f.endpoint = ep.endpoint;
                f.corner = c;
                f.slack = ep.hold_slack;
                f.hold_moved = tmg->get_pair_constraints(ep.domain_pair).hold_shift > 0;
                IdString clock = ep.clock_pair.end.clock;
                if (ctx->nets.count(clock) && ctx->nets.at(clock)->clkconstr)
                    f.period = ctx->nets.at(clock)->clkconstr->period.minDelay();
                // (the shortest path, with the least delays: the grade must be selected)
                auto path = tmg->get_endpoint_path(ep, false);
                for (auto seg = path.segments.rbegin(); seg != path.segments.rend(); ++seg)
                    if (seg->type == CriticalPath::Segment::Type::ROUTING)
                        f.path.push_back(Conn{seg->net, CellPortKey(seg->to.first, seg->to.second)});
                ct.failing.push_back(std::move(f));
            }
            ct.endpoints_below = int(endpoints_below.size());
            for (auto &cell : ctx->cells) {
                if (cell.second->bel == BelId())
                    continue;
                for (auto &port : cell.second->ports) {
                    if (port.second.type != PORT_IN || port.second.net == nullptr)
                        continue;
                    CellPortKey key(cell.first, port.first);
                    delay_t slack = tmg->get_hold_slack(key);
                    if (slack < margin)
                        ct.below[key] = {slack, tmg->get_route_delay(key).minDelay()};
                }
            }
            if (c == routed) {
                a.setup = summarise(*tmg);
                a.routed = std::move(tmg);
            }
        }
        return a;
    }

    // The net and sink of a connection that can take delay cells, with its source and sink wire
    bool connection(const Conn &conn, NetInfo *&net, PortRef &user, WireId &src, WireId &sink) const
    {
        if (!ctx->nets.count(conn.net) || !ctx->cells.count(conn.sink.cell))
            return false;
        net = ctx->nets.at(conn.net).get();
        CellInfo *cell = ctx->cells.at(conn.sink.cell).get();
        if (!cell->ports.count(conn.sink.port) || cell->getPort(conn.sink.port) != net)
            return false;
        if (cell->bel == BelId() || net->driver.cell == nullptr || net->driver.cell->bel == BelId())
            return false;
        // Nets routed before router2 (clocks, multipliers) are not routed again by route_ripped; the data inputs of
        // an 8:1 multiplexer need a route of the right polarity, which route_ripped knows only while
        // fix_mx8_inputs runs
        if (impl.prerouted_nets.count(net->name) || impl.global_signals.count(net) || cell->type == id_CPE_MX8)
            return false;
        int clocks = 0;
        if (ctx->getPortTimingClass(cell, conn.sink.port, clocks) == TMG_CLOCK_INPUT)
            return false;
        user = net->users.at(cell->ports.at(conn.sink.port).user_idx);
        src = ctx->getNetinfoSourceWire(net);
        sink = ctx->getNetinfoSinkWire(net, user, 0);
        if (src == WireId() || sink == WireId())
            return false;
        for (WireId w : ctx->getNetinfoSinkWires(net, user))
            if (!net->wires.count(w))
                return false;
        // A connection inside one CPE (a LUT into its register): a delay cell would have to leave the CPE and come
        // back, which the register's input may not allow; the connection before it does as well
        bool inside = src.tile == sink.tile;
        for (WireId w = sink; inside && w != src;) {
            auto found = net->wires.find(w);
            if (found == net->wires.end() || found->second.pip == PipId())
                break;
            w = ctx->getPipSrcWire(found->second.pip);
            inside = w.tile == sink.tile;
        }
        return !inside;
    }

    // The delay cells already in a chain through the connection (before it on the net's side, after it on the sink's)
    int chain_length(const NetInfo *net, const CellInfo *sink_cell) const
    {
        int n = 0;
        for (const CellInfo *d = net->driver.cell; impl.is_delay_cell(d) && n < 1000; n++) {
            const NetInfo *in = d->getPort(id_IN6);
            d = in ? in->driver.cell : nullptr;
        }
        for (const CellInfo *s = sink_cell; impl.is_delay_cell(s) && n < 1000; n++) {
            const NetInfo *out = s->getPort(impl.delay_cell_output(impl.delay_cell_tap(s)));
            const CellInfo *next = nullptr;
            if (out && out->users.entries() == 1)
                for (auto &u : out->users)
                    next = u.cell;
            s = next;
        }
        return n;
    }

    // The free CPE for a delay cell of `tap` within the radius around `at`, entered from `from` and leading on to
    // `to`, with the least estimated routing through it (`routing`); the nearer on a tie (the scan order decides
    // further ties)
    BelId place_cell(Loc at, WireId from, WireId to, int tap, const pool<int> &planned, delay_t &routing) const
    {
        BelId best;
        int best_dist = 0;
        const int r = impl.hold_fix_radius;
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++) {
                int x = at.x + dx, y = at.y + dy;
                if (x < 0 || y < 0 || x >= ctx->getGridDimX() || y >= ctx->getGridDimY())
                    continue;
                BelId bel = ctx->getBelByLocation(Loc(x, y, CPE_LT_FULL_Z));
                if (bel == BelId() || planned.count(bel.tile) || !impl.delay_cell_fits(bel, tap))
                    continue;
                delay_t d = estimate(from, ctx->getBelPinWire(bel, id_IN6)) +
                            estimate(ctx->getBelPinWire(bel, impl.delay_cell_output(tap)), to);
                int dist = std::max(std::abs(dx), std::abs(dy));
                if (best == BelId() || d < routing || (d == routing && dist < best_dist)) {
                    best = bel;
                    routing = d;
                    best_dist = dist;
                }
            }
        return best;
    }

    // Places a chain of taps for the connection src -> sink near the sink; the estimated routing of the chain (from
    // the source to the first cell, between the cells, from the last to the sink). False if a cell finds no CPE.
    bool place_chain(Loc at, WireId src, WireId sink, const std::vector<int> &taps, std::vector<BelId> &bels,
                     delay_t &routing) const
    {
        pool<int> planned;
        bels.clear();
        routing = 0;
        WireId from = src;
        for (size_t i = 0; i < taps.size(); i++) {
            delay_t through = 0;
            BelId bel = place_cell(at, from, sink, taps[i], planned, through);
            if (bel == BelId())
                return false;
            bels.push_back(bel);
            planned.insert(bel.tile);
            routing += estimate(from, ctx->getBelPinWire(bel, id_IN6));
            from = ctx->getBelPinWire(bel, impl.delay_cell_output(taps[i]));
        }
        routing += estimate(from, sink);
        return true;
    }

    // The chains by size: k cells are k - 1 of tap 2 and one of tap 0, 1 or 2 (in front)
    static std::vector<int> chain_taps(int index)
    {
        int k = index / 3 + 1;
        std::vector<int> taps(k, 2);
        taps.front() = index % 3;
        return taps;
    }

    // The hold slack of a port in a corner (no_slack where it is not below the margin) and its least routing delay
    static std::pair<delay_t, delay_t> below(const CornerTiming &ct, const CellPortKey &port)
    {
        auto found = ct.below.find(port);
        return found == ct.below.end() ? std::make_pair(no_slack, delay_t(0)) : found->second;
    }

    enum class Try
    {
        PLANNED,
        NOT_PADDABLE,
        REJECTED,
        NO_BUDGET,
        NO_CPE,
        CHAIN_CAP
    };

    // Plans delay cells on one connection of an end point's path, for the end point's need in every corner (that of
    // the end point, not of the connection: other paths through the connection may need more, and they have
    // connections of their own nearer to their end points): the smallest chain whose estimated hold gain covers it,
    // within the setup budget of the connection
    Try plan(Analysis &a, const Conn &conn, const CellPortKey &endpoint, Plan &p, Why &why)
    {
        NetInfo *net = nullptr;
        PortRef user;
        WireId src, sink;
        if (!connection(conn, net, user, src, sink))
            return Try::NOT_PADDABLE;
        auto rej = rejected.find(conn);
        if (rej != rejected.end()) {
            if (rej->second == GateMateImpl::HoldFixReason::NOT_ROUTABLE)
                why.not_routable = true;
            else if (rej->second == GateMateImpl::HoldFixReason::NO_GAIN)
                why.no_gain = true;
            else
                why.no_budget = true;
            return Try::REJECTED;
        }
        const int n = int(corners.size());
        delay_t old_max = a.routed->get_route_delay(conn.sink).maxDelay();
        std::vector<delay_t> need(n, 0), old_min(n, old_max);
        bool any = false;
        for (int c = 0; c < n; c++) {
            delay_t slack = below(a.corners.at(c), endpoint).first;
            if (slack == no_slack)
                continue;
            need[c] = margin - slack;
            any |= need[c] > 0;
            // (the connection's least routing delay where its hold slack is below the margin too; else taken as
            // its greatest, which makes the gain the taps' delay)
            auto at_conn = below(a.corners.at(c), conn.sink);
            if (at_conn.first != no_slack)
                old_min[c] = at_conn.second;
        }
        if (!any)
            return Try::NOT_PADDABLE;
        delay_t need_max = *std::max_element(need.begin(), need.end());
        why.need = std::max(why.need, need_max);
        delay_t setup = a.routed->get_absolute_setup_slack(conn.sink);
        delay_t budget = setup == no_slack ? no_slack : setup - guard;
        why.budget = std::max(why.budget, budget);
        if (budget <= 0) {
            why.no_budget = true;
            return Try::NO_BUDGET;
        }
        int cap = impl.hold_fix_max_cells - chain_length(net, ctx->cells.at(conn.sink.cell).get());
        // The hold gain of a chain in corner c: the taps' least delay and the change of the routing, the new routing
        // estimated in the routed grade and scaled to the corner's least delays as the old connection's are
        auto gain = [&](int c, const std::vector<int> &taps, delay_t routing) {
            delay_t g = 0;
            for (int t : taps)
                g += tap_min.at(c).at(t);
            double scale = old_max > 0 ? double(old_min[c]) / old_max : 1.0;
            return g + delay_t(scale * routing) - old_min[c];
        };
        auto covers = [&](const std::vector<int> &taps, delay_t routing) {
            for (int c = 0; c < n; c++)
                if (need[c] > 0 && gain(c, taps, routing) < need[c])
                    return false;
            return true;
        };
        Loc at = ctx->getBelLocation(ctx->cells.at(conn.sink.cell)->bel);
        // Each chain placed, the smallest first: the detour through the cells' CPEs is often a good part of the gain
        // (and of the setup cost)
        for (int index = 0; index < 3 * cap; index++) {
            auto taps = chain_taps(index);
            std::vector<BelId> bels;
            delay_t routing = 0;
            if (!place_chain(at, src, sink, taps, bels, routing)) {
                // a longer chain needs more CPEs: none will fit either
                why.no_cpe = true;
                return Try::NO_CPE;
            }
            if (!covers(taps, routing))
                continue;
            delay_t cost = routing - old_max;
            for (int t : taps)
                cost += tap_max.at(t);
            if (ctx->verbose)
                log_info("      %s -> %s: need %.3f ns, %d cell(s) (first tap %d), routing %.3f ns (was %.3f), setup "
                         "cost %.3f ns, budget %.3f ns\n",
                         conn.net.c_str(ctx), name(conn.sink).c_str(), ctx->getDelayNS(need_max), int(taps.size()),
                         taps.front(), ctx->getDelayNS(routing), ctx->getDelayNS(old_max), ctx->getDelayNS(cost),
                         ctx->getDelayNS(budget));
            if (cost > budget) {
                why.no_budget = true;
                return Try::NO_BUDGET;
            }
            p.conn = conn;
            p.bels = bels;
            p.taps = taps;
            p.hold_before.assign(n, no_slack);
            for (int c = 0; c < n; c++)
                p.hold_before[c] = below(a.corners.at(c), conn.sink).first;
            p.setup_before = setup;
            return Try::PLANNED;
        }
        why.chain_cap = true;
        return Try::CHAIN_CAP;
    }

    // Inserts the cells of a plan (the chain from the source side); false (nothing changed) if one does not fit
    bool apply(Plan &p)
    {
        NetInfo *net = nullptr;
        PortRef user;
        WireId src, sink;
        p.cells.clear();
        if (!connection(p.conn, net, user, src, sink))
            return false;
        CellInfo *sink_cell = ctx->cells.at(p.conn.sink.cell).get();
        for (size_t i = 0; i < p.taps.size(); i++) {
            CellInfo *cell = impl.insert_delay_cell(net, user, p.bels[i], p.taps[i]);
            if (!cell) {
                undo(p);
                return false;
            }
            p.cells.push_back(cell->name);
            net = cell->getPort(impl.delay_cell_output(p.taps[i]));
            user = net->users.at(sink_cell->ports.at(p.conn.sink.port).user_idx);
        }
        inserted_total += int(p.cells.size());
        return true;
    }

    // Takes the cells of a plan out again (the last first); whether the old route to the sink came back
    bool undo(Plan &p)
    {
        bool restored = true;
        for (auto it = p.cells.rbegin(); it != p.cells.rend(); ++it)
            if (ctx->cells.count(*it))
                // (only the first cell's removal brings the route of the original net back)
                restored = impl.remove_delay_cell(ctx->cells.at(*it).get());
        p.cells.clear();
        return restored;
    }

    bool route()
    {
        ctx->assignArchInfo();
        bool routed_ok = impl.route_ripped({});
        return routed_ok && !impl.hold_fix_fail_routing;
    }

    // Whether the setup result of the routed grade is kept: TNS no worse, a failing clock's Fmax no lower, a clock
    // that met its constraint still meeting it
    bool setup_kept(const SetupSummary &before, const SetupSummary &after, std::string &what) const
    {
        if (after.tns < before.tns) {
            what = stringf("TNS %.3f -> %.3f ns", ctx->getDelayNS(before.tns), ctx->getDelayNS(after.tns));
            return false;
        }
        for (auto &[clock, fmax] : before.fmax) {
            auto found = after.fmax.find(clock);
            double now = found == after.fmax.end() ? std::numeric_limits<double>::infinity() : found->second.first;
            double floor = std::min(fmax.first, fmax.second);
            if (now < floor * (1 - 1e-6)) {
                what = stringf("Fmax of '%s' %.2f -> %.2f MHz", clock.c_str(), fmax.first, now);
                return false;
            }
        }
        return true;
    }

    void run();
};

void HoldFixer::run()
{
    auto start = std::chrono::steady_clock::now();
    auto &report = impl.hold_fix_report;
    report = GateMateImpl::HoldFixReport();
    margin = ctx->getDelayFromNS(impl.hold_fix_margin);
    guard = ctx->getDelayFromNS(impl.hold_fix_setup_guard);

    // The routed grade is selected again at the end, whatever happens
    const int routed_timing = impl.timing_mode, routed_fpga = impl.fpga_mode;
    struct Reselect
    {
        GateMateImpl &impl;
        int t, f;
        ~Reselect()
        {
            if (impl.timing_mode != t || impl.fpga_mode != f)
                impl.select_speed_grade(t, f);
        }
    } reselect{impl, routed_timing, routed_fpga};
    for (auto corner : impl.hold_corners)
        if (corner != std::make_pair(routed_timing, routed_fpga))
            corners.push_back(corner);
    corners.emplace_back(routed_timing, routed_fpga);
    routed = int(corners.size()) - 1;
    const int n = int(corners.size());

    Analysis current = analyse();
    report.ran = true;
    for (auto &ct : current.corners) {
        report.corners.push_back(ct.grade);
        report.worst_before.push_back(ct.worst);
        report.below_before.push_back(ct.endpoints_below);
    }
    report.cells_by_tap.assign(GateMateImpl::delay_cell_tap_count, 0);
    auto seconds = [&]() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    };
    bool anything = false;
    for (auto &ct : current.corners)
        anything |= !ct.failing.empty();
    if (!anything) {
        report.worst_after = report.worst_before;
        report.below_after = report.below_before;
        log_info("Hold fix: nothing below the margin of %.2f ns in %d corner(s) (%.2f s).\n", impl.hold_fix_margin, n,
                 seconds());
        return;
    }

    // The taps' delays in each corner
    tap_min.assign(n, {});
    for (int c = 0; c < n; c++) {
        select(c);
        for (auto &d : impl.delay_cell_taps())
            tap_min[c].push_back(d.minDelay());
    }
    // (the routed grade selected last, also for least_delay)
    for (auto &d : impl.delay_cell_taps())
        tap_max.push_back(d.maxDelay());

    log_info("Hold fix: margin %.2f ns in corner(s)", impl.hold_fix_margin);
    for (int c = 0; c < n; c++)
        log("%s '%s'", c == 0 ? "" : ",", current.corners[c].grade.c_str());
    log(", setup kept in '%s'.\n", current.corners[routed].grade.c_str());

    const SetupSummary baseline = current.setup;
    std::vector<Plan> kept;
    const int max_rounds = 12;
    bool valid = true;
    for (int round = 1; round <= max_rounds; round++) {
        if (!valid) {
            current = analyse();
            valid = true;
        }
        // The entries below the margin, the worst first (by name on a tie)
        std::vector<const Failing *> todo;
        for (auto &ct : current.corners)
            for (auto &f : ct.failing)
                todo.push_back(&f);
        if (todo.empty())
            break;
        std::stable_sort(todo.begin(), todo.end(), [&](const Failing *a, const Failing *b) {
            if (a->slack != b->slack)
                return a->slack < b->slack;
            if (a->endpoint != b->endpoint)
                return name(a->endpoint) < name(b->endpoint);
            return a->corner < b->corner;
        });

        // Plan and insert: one connection per path, each connection once, each end point once per round
        RoutingSnapshot snapshot = snapshot_routing(ctx);
        std::vector<Plan> plans;
        pool<Conn> blocked;
        pool<CellPortKey> handled;
        int rejected_before = int(rejected.size());
        for (const Failing *f : todo) {
            if (handled.count(f->endpoint))
                continue;
            bool covered = false;
            for (auto &conn : f->path)
                covered |= blocked.count(conn) > 0;
            if (covered) {
                // (padded this round already, or to be taken up again after the re-analysis)
                handled.insert(f->endpoint);
                continue;
            }
            Why w;
            Plan p;
            p.endpoint = f->endpoint;
            bool done = false;
            for (auto &conn : f->path) {
                Try t = plan(current, conn, f->endpoint, p, w);
                if (t == Try::PLANNED) {
                    done = true;
                    break;
                }
                // a need beyond the cap is no less upstream
                if (t == Try::CHAIN_CAP)
                    break;
            }
            handled.insert(f->endpoint);
            if (!done) {
                why[f->endpoint] = w;
                continue;
            }
            if (!apply(p)) {
                // (a CPE planned for another plan's chain; tried again next round)
                continue;
            }
            why.erase(f->endpoint);
            for (auto &conn : f->path)
                blocked.insert(conn);
            plans.push_back(std::move(p));
        }
        report.rounds = round;
        if (plans.empty()) {
            if (int(rejected.size()) == rejected_before)
                break;
            continue;
        }

        // Route the round together; where that fails, all undone and each plan alone
        if (!route()) {
            for (auto it = plans.rbegin(); it != plans.rend(); ++it)
                undo(*it);
            restore_changed_routing(ctx, snapshot);
            std::vector<Plan> alone;
            for (auto &p : plans) {
                RoutingSnapshot before = snapshot_routing(ctx);
                if (apply(p) && route()) {
                    alone.push_back(std::move(p));
                    continue;
                }
                undo(p);
                restore_changed_routing(ctx, before);
                rejected[p.conn] = GateMateImpl::HoldFixReason::NOT_ROUTABLE;
                auto &w = why[p.endpoint];
                w.not_routable = true;
                report.removed++;
            }
            plans = std::move(alone);
            ctx->assignArchInfo();
        }
        if (plans.empty()) {
            valid = true; // (all restored as it was)
            ctx->routeRepairProgress();
            continue;
        }

        // Verify exactly: per plan the setup slack at the sink and the hold gain; then the setup result as a whole
        Analysis after = analyse();
        std::vector<size_t> offenders;
        for (size_t i = 0; i < plans.size(); i++) {
            auto &p = plans[i];
            // (the guard less what the estimate of the routing may be off by)
            delay_t setup = after.routed->get_absolute_setup_slack(p.conn.sink);
            bool no_budget = setup < guard / 2, no_gain = false;
            // The hold slack at the sink must rise where it was below the margin, and stay above it elsewhere (new
            // routing faster than the old one could undo the cells' delay)
            for (int c = 0; c < n && !no_gain; c++) {
                delay_t now = below(after.corners[c], p.conn.sink).first;
                no_gain = now != no_slack && (p.hold_before[c] == no_slack || now <= p.hold_before[c]);
            }
            if (no_budget || no_gain) {
                offenders.push_back(i);
                rejected[p.conn] =
                        no_budget ? GateMateImpl::HoldFixReason::NO_BUDGET : GateMateImpl::HoldFixReason::NO_GAIN;
                (no_budget ? why[p.endpoint].no_budget : why[p.endpoint].no_gain) = true;
            }
        }
        bool restore_round = false;
        if (!offenders.empty()) {
            bool all_restored = true;
            for (auto it = offenders.rbegin(); it != offenders.rend(); ++it)
                all_restored &= undo(plans[*it]);
            report.removed += int(offenders.size());
            for (auto it = offenders.rbegin(); it != offenders.rend(); ++it)
                plans.erase(plans.begin() + *it);
            // (a route a later cell took: routed again on free wires)
            ctx->assignArchInfo();
            if (!all_restored && !route())
                restore_round = true;
            else
                after = analyse();
        }
        std::string what;
        if (!restore_round && !setup_kept(baseline, after.setup, what)) {
            log_info("    round %d: the setup result got worse (%s), undone\n", round, what.c_str());
            for (auto &p : plans) {
                rejected[p.conn] = GateMateImpl::HoldFixReason::NO_BUDGET;
                why[p.endpoint].no_budget = true;
            }
            restore_round = true;
        }
        if (restore_round) {
            for (auto it = plans.rbegin(); it != plans.rend(); ++it)
                undo(*it);
            report.removed += int(plans.size());
            restore_changed_routing(ctx, snapshot);
            ctx->assignArchInfo();
            plans.clear();
            valid = false;
        } else {
            current = std::move(after);
        }
        int cells = 0, conns = int(plans.size());
        for (auto &p : plans) {
            cells += int(p.cells.size());
            kept.push_back(std::move(p));
        }
        if (valid) {
            int below_now = 0;
            for (auto &ct : current.corners)
                below_now = std::max(below_now, ct.endpoints_below);
            log_info("    round %d: %d connection(s) padded with %d delay cell(s), %d end point(s) below the margin "
                     "in the worst corner\n",
                     round, conns, cells, below_now);
        }
        ctx->routeRepairProgress();
    }
    if (!valid)
        current = analyse();

    // The result
    for (auto &p : kept)
        for (auto &cell : p.cells)
            if (ctx->cells.count(cell))
                report.cells_by_tap.at(impl.delay_cell_tap(ctx->cells.at(cell).get()))++;
    int cells = 0;
    for (int t : report.cells_by_tap)
        cells += t;
    for (int c = 0; c < n; c++) {
        report.worst_after.push_back(current.corners[c].worst);
        report.below_after.push_back(current.corners[c].endpoints_below);
        auto ns = [&](delay_t d) { return d == no_slack ? std::string("-") : stringf("%.2f", ctx->getDelayNS(d)); };
        log_info("    corner '%s': worst hold slack %s -> %s ns, %d -> %d end point(s) below the margin\n",
                 current.corners[c].grade.c_str(), ns(report.worst_before[c]).c_str(),
                 ns(report.worst_after[c]).c_str(), report.below_before[c], report.below_after[c]);
    }
    log_info("    %d delay cell(s) kept (tap 0: %d, tap 1: %d, tap 2: %d) in %d CPE(s) on %d connection(s), %d "
             "connection(s) padded and undone again, %d round(s), %.2f s\n",
             cells, report.cells_by_tap[0], report.cells_by_tap[1], report.cells_by_tap[2], cells, int(kept.size()),
             report.removed, report.rounds, seconds());

    // The end points left below the margin, each once at its worst corner
    std::map<std::string, GateMateImpl::HoldFixLeftover> left;
    for (int c = 0; c < n; c++)
        for (auto &f : current.corners[c].failing) {
            std::string key = name(f.endpoint);
            auto found = left.find(key);
            if (found != left.end() && found->second.slack <= f.slack)
                continue;
            GateMateImpl::HoldFixLeftover l;
            l.cell = f.endpoint.cell;
            l.port = f.endpoint.port;
            l.corner = current.corners[c].grade;
            l.slack = f.slack;
            l.need = margin - f.slack;
            l.missing_hold_multicycle = f.hold_moved || (f.period > 0 && l.need > f.period / 2);
            auto w = why.find(f.endpoint);
            if (w != why.end()) {
                l.reason = w->second.chain_cap      ? GateMateImpl::HoldFixReason::CHAIN_CAP
                           : w->second.no_cpe       ? GateMateImpl::HoldFixReason::NO_CPE
                           : w->second.not_routable ? GateMateImpl::HoldFixReason::NOT_ROUTABLE
                           : w->second.no_gain      ? GateMateImpl::HoldFixReason::NO_GAIN
                           : w->second.no_budget    ? GateMateImpl::HoldFixReason::NO_BUDGET
                                                    : GateMateImpl::HoldFixReason::NO_CONNECTION;
                l.budget = w->second.budget;
            }
            left[key] = l;
        }
    for (auto &[key, l] : left) {
        std::string reason;
        switch (l.reason) {
        case GateMateImpl::HoldFixReason::NO_BUDGET:
            reason = l.budget == std::numeric_limits<delay_t>::lowest()
                             ? "no setup budget"
                             : stringf("no setup budget: need %.2f ns, budget %.2f ns", ctx->getDelayNS(l.need),
                                       ctx->getDelayNS(std::max<delay_t>(l.budget, 0)));
            break;
        case GateMateImpl::HoldFixReason::NO_CPE:
            reason = stringf("no free CPE within %d tiles", impl.hold_fix_radius);
            break;
        case GateMateImpl::HoldFixReason::NOT_ROUTABLE:
            reason = "the delay cells could not be routed";
            break;
        case GateMateImpl::HoldFixReason::NO_GAIN:
            reason = "the delay cells did not raise its hold slack";
            break;
        case GateMateImpl::HoldFixReason::CHAIN_CAP:
            reason = stringf("needs %.2f ns, more than %d delay cells give", ctx->getDelayNS(l.need),
                             impl.hold_fix_max_cells);
            break;
        case GateMateImpl::HoldFixReason::NO_CONNECTION:
            reason = "no connection of its path can take a delay cell";
            break;
        default:
            reason = "not resolved within the rounds";
            break;
        }
        if (l.missing_hold_multicycle)
            reason += " (of the order of a clock period: a 'set_multicycle_path -hold' is probably missing)";
        std::string text = stringf("hold slack %.2f ns at %s in corner '%s': %s", ctx->getDelayNS(l.slack),
                                   key.c_str(), l.corner.c_str(), reason.c_str());
        if (l.slack < 0)
            log_warning("Hold fix left %s.\n", text.c_str());
        else
            log_info("    left below the margin: %s\n", text.c_str());
        report.leftovers.push_back(l);
    }

    // Whatever happened: the routing complete and consistent
    if (inserted_total > 0)
        check_repaired_routing(ctx, "Fixing hold");
}

} // namespace

void GateMateImpl::fix_hold()
{
    if (!hold_fix)
        return;
    if (routed_before) {
        log_info("Hold fix skipped: the design was routed before.\n");
        return;
    }
    HoldFixer fixer(*this);
    fixer.run();
}

NEXTPNR_NAMESPACE_END
