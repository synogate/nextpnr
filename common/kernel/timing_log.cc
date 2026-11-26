/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  gatecat <gatecat@ds0.me>
 *  Copyright (C) 2018  Eddie Hung <eddieh@ece.ubc.ca>
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

#include <algorithm>
#include "log.h"
#include "nextpnr.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

static std::string clock_event_name(const Context *ctx, const ClockEvent &e, int field_width = 0)
{
    std::string value;
    if (e.clock == IdString() || e.clock == ctx->id("$async$"))
        value = std::string("<async>");
    else
        value = (e.edge == FALLING_EDGE ? std::string("negedge ") : std::string("posedge ")) + e.clock.str(ctx);
    if (int(value.length()) < field_width)
        value.insert(value.length(), field_width - int(value.length()), ' ');
    return value;
};

// hold_only: only the hold/min time violations, not the critical paths; corner: the timing corner the results are of
// (named in the hold violations, if not empty)
static void log_crit_paths(const Context *ctx, TimingResult &result, bool hold_only = false,
                           const std::string &corner = std::string())
{
    auto print_net_source = [ctx](const NetInfo *net) {
        // Check if this net is annotated with a source list
        auto sources = net->attrs.find(ctx->id("src"));
        if (sources == net->attrs.end()) {
            // No sources for this net, can't print anything
            return;
        }

        // Sources are separated by pipe characters.
        // There is no guaranteed ordering on sources, so we just print all
        auto sourcelist = sources->second.as_string();
        std::vector<std::string> source_entries;
        size_t current = 0, prev = 0;
        while ((current = sourcelist.find("|", prev)) != std::string::npos) {
            source_entries.emplace_back(sourcelist.substr(prev, current - prev));
            prev = current + 1;
        }
        // Ensure we emplace the final entry
        source_entries.emplace_back(sourcelist.substr(prev, current - prev));

        // Iterate and print our source list at the correct indentation level
        log_info("                         Defined in:\n");
        for (auto entry : source_entries) {
            log_info("                              %s\n", entry.c_str());
        }
    };

    // A helper function for reporting one critical path
    auto print_path_report = [ctx, &print_net_source](const CriticalPath &path) {
        delay_t total(0), logic_total(0), route_total(0);

        log_info("      type curr  total name\n");
        for (const auto &segment : path.segments) {

            delay_t delay = segment.delay;

            total += delay;

            if (segment.type == CriticalPath::Segment::Type::CLK_TO_Q ||
                segment.type == CriticalPath::Segment::Type::SOURCE ||
                segment.type == CriticalPath::Segment::Type::LOGIC ||
                segment.type == CriticalPath::Segment::Type::SETUP ||
                segment.type == CriticalPath::Segment::Type::HOLD ||
                segment.type == CriticalPath::Segment::Type::INPUT_DELAY ||
                segment.type == CriticalPath::Segment::Type::OUTPUT_DELAY) {
                logic_total += delay;

                log_info("%10s % 5.2f % 5.2f Source %s.%s\n", CriticalPath::Segment::type_to_str(segment.type).c_str(),
                         ctx->getDelayNS(delay), ctx->getDelayNS(total), segment.to.first.c_str(ctx),
                         segment.to.second.c_str(ctx));
            } else if (segment.type == CriticalPath::Segment::Type::ROUTING ||
                       segment.type == CriticalPath::Segment::Type::CLK_TO_CLK ||
                       segment.type == CriticalPath::Segment::Type::CLK_SKEW) {
                route_total = route_total + delay;

                const auto &driver = ctx->cells.at(segment.from.first);
                const auto &sink = ctx->cells.at(segment.to.first);

                auto driver_loc = ctx->getBelLocation(driver->bel);
                auto sink_loc = ctx->getBelLocation(sink->bel);

                log_info("%10s % 5.2f % 5.2f Net %s (%d,%d) -> (%d,%d)\n",
                         CriticalPath::Segment::type_to_str(segment.type).c_str(), ctx->getDelayNS(delay),
                         ctx->getDelayNS(total), segment.net.c_str(ctx), driver_loc.x, driver_loc.y, sink_loc.x,
                         sink_loc.y);
                log_info("                         Sink %s.%s\n", segment.to.first.c_str(ctx),
                         segment.to.second.c_str(ctx));

                // CLK_TO_CLK has no net and CLK_SKEW might have a net
                if (ctx->nets.count(segment.net) == 0) {
                    continue;
                }
                const NetInfo *net = ctx->nets.at(segment.net).get();

                if (ctx->verbose) {

                    PortRef sink_ref;
                    sink_ref.cell = sink.get();
                    sink_ref.port = segment.to.second;

                    auto driver_wire = ctx->getNetinfoSourceWire(net);
                    auto sink_wire = ctx->getNetinfoSinkWire(net, sink_ref, 0);
                    log_info("                          prediction: %f ns estimate: %f ns\n",
                             ctx->getDelayNS(ctx->predictArcDelay(net, sink_ref)),
                             ctx->getDelayNS(ctx->estimateDelay(driver_wire, sink_wire)));
                    auto cursor = sink_wire;
                    delay_t delay;
                    while (driver_wire != cursor) {
#ifdef ARCH_ECP5
                        if (net->is_global)
                            break;
#endif
                        auto it = net->wires.find(cursor);
                        assert(it != net->wires.end());
                        auto pip = it->second.pip;
                        NPNR_ASSERT(pip != PipId());
                        delay = ctx->getPipDelay(pip).maxDelay();
                        log_info("                 %1.3f %s\n", ctx->getDelayNS(delay), ctx->nameOfPip(pip));
                        cursor = ctx->getPipSrcWire(pip);
                    }
                }

                if (!ctx->disable_critical_path_source_print) {
                    print_net_source(net);
                }
            }
        }
        log_info("%.2f ns logic, %.2f ns routing\n", ctx->getDelayNS(logic_total), ctx->getDelayNS(route_total));
        if (path.setup_cycles != 1 || path.hold_shift != 0)
            log_info("multicycle path: %d clock cycle(s) for setup (%.2f ns), hold checked %.2f ns after the launch "
                     "edge\n",
                     path.setup_cycles, ctx->getDelayNS(path.max_delay),
                     ctx->getDelayNS(path.hold_shift - path.hold_edge));
    };

    // Single domain paths
    for (auto &clock : result.clock_paths) {
        if (hold_only)
            break;
        log_break();
        std::string start =
                clock.second.clock_pair.start.edge == FALLING_EDGE ? std::string("negedge") : std::string("posedge");
        std::string end =
                clock.second.clock_pair.end.edge == FALLING_EDGE ? std::string("negedge") : std::string("posedge");
        log_info("Critical path report for clock '%s' (%s -> %s):\n", clock.first.c_str(ctx), start.c_str(),
                 end.c_str());
        auto &report = clock.second;
        print_path_report(report);
    }

    // Cross-domain paths
    for (auto &report : result.xclock_paths) {
        if (hold_only)
            break;
        log_break();
        std::string start = clock_event_name(ctx, report.clock_pair.start);
        std::string end = clock_event_name(ctx, report.clock_pair.end);
        log_info("Critical path report for cross-domain path '%s' -> '%s':\n", start.c_str(), end.c_str());
        print_path_report(report);
    }

    // Multicycle paths: their worst slacks and paths (they are seldom the critical ones)
    for (auto &s : result.multicycle_slacks) {
        if (hold_only)
            break;
        log_break();
        std::string start = clock_event_name(ctx, s.clock_pair.start);
        std::string end = clock_event_name(ctx, s.clock_pair.end);
        auto endpoint = [ctx](const CriticalPath &path) {
            if (path.segments.empty())
                return std::string("-");
            auto &to = path.segments.back().to;
            return std::string(to.first.c_str(ctx)) + "." + to.second.c_str(ctx);
        };
        log_info("Multicycle paths '%s' -> '%s' (%d clock cycle(s) for setup, hold checked %.2f ns after the "
                 "launch edge): %d end point(s)\n",
                 start.c_str(), end.c_str(), s.setup_cycles,
                 ctx->getDelayNS(s.hold_shift - s.hold_edge), s.endpoints);
        if (s.setup_path.segments.empty())
            log_info("    no setup check (false path)\n");
        else
            log_info("    worst setup slack %.2f ns at %s\n", ctx->getDelayNS(s.setup_slack),
                     endpoint(s.setup_path).c_str());
        if (s.hold_path.segments.empty()) {
            log_info("    no hold check\n");
            continue;
        }
        log_info("    worst hold slack %.2f ns at %s, the shortest path:\n", ctx->getDelayNS(s.hold_slack),
                 endpoint(s.hold_path).c_str());
        print_path_report(s.hold_path);
    }

    // Min delay violated paths
    // Show maximum of 10
    auto num_min_violations = result.min_delay_violations.size();
    bool allow_fail = bool_or_default(ctx->settings, ctx->id("timing/allowFail"), false);
    std::string in_corner = corner.empty() ? std::string() : " in timing corner '" + corner + "'";
    if (num_min_violations > 0) {
        log_break();
        bool later = ctx->hold_checked_later && corner.empty();
        log_info("%zu Hold/min time violations%s (showing 10 worst paths)%s:\n", num_min_violations, in_corner.c_str(),
                 later ? ", before the repairs and the check of the finished routing" : "");
        for (size_t i = 0; i < std::min((size_t)10, num_min_violations); ++i) {
            auto &report = result.min_delay_violations.at(i);
            log_break();
            std::string start = clock_event_name(ctx, report.clock_pair.start);
            std::string end = clock_event_name(ctx, report.clock_pair.end);

            std::string message;
            if (report.clock_pair.start == report.clock_pair.end) {
                message = "Hold/min time violation for clock '" + start + "'" + in_corner + ":\n";
            } else {
                message = "Hold/min time violation for path '" + start + "' -> '" + end + "'" + in_corner + ":\n";
            }

            // (not an error yet where the arch repairs hold and checks the finished routing)
            if (ctx->hold_checked_later && corner.empty()) {
                log_info("%s", message.c_str());
            } else if (allow_fail) {
                log_warning("%s", message.c_str());
            } else {
                log_nonfatal_error("%s", message.c_str());
            }

            print_path_report(report);
        }
    }
}

static void log_fmax(Context *ctx, TimingResult &result, bool warn_on_failure)
{
    log_break();

    bool allow_fail = bool_or_default(ctx->settings, ctx->id("timing/allowFail"), false);

    // What set_false_path takes out of the analysis (end points per launch clock edge)
    auto log_false_paths = [&]() {
        if (result.false_setup_paths > 0 || result.false_hold_paths > 0)
            log_info("False paths: %d end point(s) without a setup check, %d without a hold check\n",
                     result.false_setup_paths, result.false_hold_paths);
    };

    // The input and output delays (set_input_delay, set_output_delay): the worst slacks of their paths
    auto log_io_delays = [&]() {
        for (auto &note : result.io_notes)
            log_warning("%s\n", note.c_str());
        if (result.io_slacks.empty())
            return;
        const delay_t none = std::numeric_limits<delay_t>::max();
        // One entry per command (or pair of commands, for the minimum and the maximum) of the SDC file: its ports
        // have the same delays
        struct Group
        {
            const IoDelay *io;
            int ports = 0;
            delay_t setup = std::numeric_limits<delay_t>::max(), hold = std::numeric_limits<delay_t>::max();
            IdString setup_port, hold_port;
        };
        std::vector<Group> groups;
        for (auto &s : result.io_slacks) {
            const IoDelay &io = ctx->io_delays.at(s.index);
            auto group = std::find_if(groups.begin(), groups.end(), [&](const Group &g) {
                return g.io->output == io.output && g.io->source == io.source && g.io->clock == io.clock &&
                       g.io->edge == io.edge;
            });
            if (group == groups.end())
                group = groups.insert(groups.end(), Group{&io});
            IdString port = io.port != IdString() ? io.port : io.cell;
            group->ports++;
            if (s.setup_slack < group->setup) {
                group->setup = s.setup_slack;
                group->setup_port = port;
            }
            if (s.hold_slack < group->hold) {
                group->hold = s.hold_slack;
                group->hold_port = port;
            }
        }
        auto slack = [&](const char *check, delay_t value, IdString port) {
            return value == none ? stringf("no %s check", check)
                                 : stringf("worst %s slack %.2f ns at '%s'", check, ctx->getDelayNS(value),
                                           port.c_str(ctx));
        };
        log_info("Input and output delays (the paths from the inputs and to the outputs):\n");
        // The clocks the design puts out that delays are against: where their edge at the port is
        for (auto &fc : result.forwarded_clocks) {
            const ForwardedClock &clock = ctx->forwarded_clocks.at(fc.index);
            log_info("    clock '%s' at the port '%s': %.2f to %.2f ns after the clock net '%s'%s\n",
                     clock.name.c_str(ctx), clock.port.c_str(ctx), ctx->getDelayNS(fc.min_delay),
                     ctx->getDelayNS(fc.max_delay), fc.clock_net.c_str(ctx), clock.invert ? ", inverted" : "");
        }
        for (auto &g : groups) {
            std::string source = g.io->source;
            if (source.empty())
                source = g.io->output ? "set_output_delay" : "set_input_delay";
            log_info("    %s\n", source.c_str());
            std::string message = stringf("        %d port(s): %s, %s\n", g.ports,
                                          slack("setup", g.setup, g.setup_port).c_str(),
                                          slack("hold", g.hold, g.hold_port).c_str());
            // (hold is no error yet where the arch repairs it and checks the finished routing)
            bool failed = (g.setup != none && g.setup < 0) ||
                          (g.hold != none && g.hold < 0 && !ctx->hold_checked_later);
            // (as the frequencies of the clocks below)
            if (!warn_on_failure || !failed)
                log_info("%s", message.c_str());
            else if (allow_fail)
                log_warning("%s", message.c_str());
            else
                log_nonfatal_error("%s", message.c_str());
        }
    };

    if (result.clock_paths.empty() && result.clock_paths.empty()) {
        log_info("No Fmax available; no interior timing paths found in design.\n");
        log_false_paths();
        log_io_delays();
        return;
    }

    unsigned max_width = 0;
    for (auto &clock : result.clock_paths)
        max_width = std::max<unsigned>(max_width, clock.first.str(ctx).size());

    for (auto &clock : result.clock_paths) {
        const auto &clock_name = clock.first.str(ctx);
        const int width = max_width - clock_name.size();

        float fmax = result.clock_fmax[clock.first].achieved;
        float target = result.clock_fmax[clock.first].constraint;
        bool passed = target < fmax;

        if (!warn_on_failure || passed)
            log_info("Max frequency for clock %*s'%s': %.02f MHz (%s at %.02f MHz)\n", width, "", clock_name.c_str(),
                     fmax, passed ? "PASS" : "FAIL", target);
        else if (allow_fail)
            log_warning("Max frequency for clock %*s'%s': %.02f MHz (%s at %.02f MHz)\n", width, "", clock_name.c_str(),
                        fmax, passed ? "PASS" : "FAIL", target);
        else
            log_nonfatal_error("Max frequency for clock %*s'%s': %.02f MHz (%s at %.02f MHz)\n", width, "",
                               clock_name.c_str(), fmax, passed ? "PASS" : "FAIL", target);
    }
    log_break();

    // Clock to clock delays for xpaths
    dict<ClockPair, delay_t> xclock_delays;
    for (auto &report : result.xclock_paths) {
        // Check if this path has a clock-2-clock delay
        // clock-2-clock delays are always the first segment in the path
        // But we walk the entire path anyway.
        bool has_clock_to_clock = false;
        delay_t clock_delay = 0;
        for (const auto &seg : report.segments) {
            if (seg.type == CriticalPath::Segment::Type::CLK_TO_CLK) {
                has_clock_to_clock = true;
                clock_delay += seg.delay;
            }
        }

        if (has_clock_to_clock) {
            xclock_delays[report.clock_pair] = clock_delay;
        }
    }

    unsigned max_width_xca = 0;
    unsigned max_width_xcb = 0;
    for (auto &report : result.xclock_paths) {
        max_width_xca = std::max((unsigned)clock_event_name(ctx, report.clock_pair.start).length(), max_width_xca);
        max_width_xcb = std::max((unsigned)clock_event_name(ctx, report.clock_pair.end).length(), max_width_xcb);
    }

    // Check and report xpath delays for related clocks
    if (!result.xclock_paths.empty()) {
        for (auto &report : result.xclock_paths) {
            const auto &clock_a = report.clock_pair.start.clock;
            const auto &clock_b = report.clock_pair.end.clock;

            if (!xclock_delays.count(report.clock_pair)) {
                continue;
            }

            delay_t path_delay = 0;
            for (const auto &segment : report.segments) {
                path_delay += segment.delay;
            }

            // Compensate path delay for clock-to-clock delay. If the
            // result is negative then only the latter matters. Otherwise
            // the compensated path delay is taken.
            auto clock_delay = xclock_delays.at(report.clock_pair);

            float fmax = std::numeric_limits<float>::infinity();
            if (path_delay < 0) {
                fmax = 1e3f / ctx->getDelayNS(clock_delay);
            } else if (path_delay > 0) {
                fmax = 1e3f / ctx->getDelayNS(path_delay);
            }

            // Both clocks are related so they should have the same
            // frequency. However, they may get different constraints from
            // user input. In case of only one constraint preset take it,
            // otherwise get the worst case (min.)
            float target;
            auto &clock_fmax = result.clock_fmax;
            if (clock_fmax.count(clock_a) && !clock_fmax.count(clock_b)) {
                target = clock_fmax.at(clock_a).constraint;
            } else if (!clock_fmax.count(clock_a) && clock_fmax.count(clock_b)) {
                target = clock_fmax.at(clock_b).constraint;
            } else if (clock_fmax.count(clock_a)) {
                target = std::min(clock_fmax.at(clock_a).constraint, clock_fmax.at(clock_b).constraint);
            } else {
                // Neither clock has register-to-register paths of its own: the constraint of the
                // clock nets, otherwise the default target
                auto constraint = [&](IdString clock) {
                    const NetInfo *net = ctx->nets.count(clock) ? ctx->nets.at(clock).get() : nullptr;
                    if (net && net->clkconstr)
                        return 1000 / ctx->getDelayNS(net->clkconstr->period.minDelay());
                    return ctx->setting<float>("target_freq") / 1e6f;
                };
                target = std::min(constraint(clock_a), constraint(clock_b));
            }

            bool passed = target < fmax;

            auto ev_a = clock_event_name(ctx, report.clock_pair.start, max_width_xca);
            auto ev_b = clock_event_name(ctx, report.clock_pair.end, max_width_xcb);

            if (!warn_on_failure || passed)
                log_info("Max frequency for %s -> %s: %.02f MHz (%s at %.02f MHz)\n", ev_a.c_str(), ev_b.c_str(), fmax,
                         passed ? "PASS" : "FAIL", target);
            else if (allow_fail || bool_or_default(ctx->settings, ctx->id("timing/ignoreRelClk"), false))
                log_warning("Max frequency for  %s -> %s: %.02f MHz (%s at %.02f MHz)\n", ev_a.c_str(), ev_b.c_str(),
                            fmax, passed ? "PASS" : "FAIL", target);
            else
                log_nonfatal_error("Max frequency for %s -> %s: %.02f MHz (%s at %.02f MHz)\n", ev_a.c_str(),
                                   ev_b.c_str(), fmax, passed ? "PASS" : "FAIL", target);
        }
        log_break();
    }

    // Report clock delays for xpaths
    if (!xclock_delays.empty()) {
        for (auto &pair : xclock_delays) {
            auto ev_a = clock_event_name(ctx, pair.first.start, max_width_xca);
            auto ev_b = clock_event_name(ctx, pair.first.end, max_width_xcb);

            delay_t delay = pair.second;
            if (pair.first.start.edge != pair.first.end.edge) {
                delay /= 2;
            }

            log_info("Clock to clock delay %s -> %s: %0.02f ns\n", ev_a.c_str(), ev_b.c_str(), ctx->getDelayNS(delay));
        }

        log_break();
    }

    for (auto &eclock : result.empty_paths) {
        if (eclock != IdString())
            log_info("Clock '%s' has no interior paths\n", eclock.c_str(ctx));
    }
    log_false_paths();
    log_io_delays();
    log_break();

    int start_field_width = 0, end_field_width = 0;
    for (auto &report : result.xclock_paths) {
        start_field_width = std::max((int)clock_event_name(ctx, report.clock_pair.start).length(), start_field_width);
        end_field_width = std::max((int)clock_event_name(ctx, report.clock_pair.end).length(), end_field_width);
    }

    for (auto &report : result.xclock_paths) {
        const ClockEvent &a = report.clock_pair.start;
        const ClockEvent &b = report.clock_pair.end;
        delay_t path_delay = 0;
        for (const auto &segment : report.segments) {
            path_delay += segment.delay;
        }
        auto ev_a = clock_event_name(ctx, a, start_field_width), ev_b = clock_event_name(ctx, b, end_field_width);
        log_info("Max delay %s -> %s: %0.02f ns\n", ev_a.c_str(), ev_b.c_str(), ctx->getDelayNS(path_delay));
    }
    log_break();
}

static void log_histogram(Context *ctx, TimingResult &result)
{
    unsigned num_bins = 20;
    unsigned bar_width = 60;

    int min_slack = std::numeric_limits<int>::max();
    int max_slack = std::numeric_limits<int>::min();

    for (const auto &i : result.slack_histogram) {
        if (i.first < min_slack)
            min_slack = i.first;
        if (i.first > max_slack)
            max_slack = i.first;
    }

    auto bin_size = std::max<unsigned>(1, ceil((max_slack - min_slack + 1) / float(num_bins)));
    std::vector<unsigned> bins(num_bins);
    unsigned max_freq = 0;
    for (const auto &i : result.slack_histogram) {
        int bin_idx = int((i.first - min_slack) / bin_size);
        if (bin_idx < 0)
            bin_idx = 0;
        else if (bin_idx >= int(num_bins))
            bin_idx = num_bins - 1;
        auto &bin = bins.at(bin_idx);
        bin += i.second;
        max_freq = std::max(max_freq, bin);
    }
    bar_width = std::min(bar_width, max_freq);

    log_break();
    log_info("Slack histogram:\n");
    log_info(" legend: times are in ps\n");
    log_info("         * represents %d endpoint(s)\n", max_freq / bar_width);
    log_info("         + represents [1,%d) endpoint(s)\n", max_freq / bar_width);
    for (unsigned i = 0; i < num_bins; ++i)
        log_info("[%6d, %6d) |%s%c\n", min_slack + bin_size * i, min_slack + bin_size * (i + 1),
                 std::string(bins[i] * bar_width / max_freq, '*').c_str(),
                 (bins[i] * bar_width) % max_freq > 0 ? '+' : ' ');
}

static void log_tns(const TimingResult &result)
{
    // total negative slack: sum over all failing endpoints (per launch/capture edge pair)
    int64_t tns_ps = 0;
    int failing = 0;
    for (const auto &i : result.slack_histogram)
        if (i.first < 0) {
            tns_ps += int64_t(i.first) * i.second;
            failing += i.second;
        }
    log_info("Total negative slack: %.3f ns over %d failing endpoint(s)\n", tns_ps / 1000.0, failing);
}

void Context::log_hold_violations(TimingResult &result, const std::string &corner)
{
    log_crit_paths(this, result, true, corner);
}

void Context::log_timing_results(TimingResult &result, bool print_histogram, bool print_fmax, bool print_path,
                                 bool warn_on_failure)
{
    if (print_path)
        log_crit_paths(this, result);

    if (print_fmax)
        log_fmax(this, result, warn_on_failure);

    if (print_fmax && !result.slack_histogram.empty())
        log_tns(result);

    if (print_histogram && !result.slack_histogram.empty())
        log_histogram(this, result);
}

NEXTPNR_NAMESPACE_END
