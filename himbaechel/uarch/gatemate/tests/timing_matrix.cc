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

#include <boost/algorithm/string.hpp>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include "known_bugs.h"
#include "testing.h"
#include "timing.h"
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"
#include "uarch/gatemate/extra_data.h"

USING_NEXTPNR_NAMESPACE

namespace {

// A clock event, named by the pad or global clock (as named when building the design) that the
// clock comes from; "" for the paths from and to pads, which have no clock.
struct Event
{
    std::string clock;
    ClockEdge edge = RISING_EDGE;

    bool operator==(const Event &other) const { return clock == other.clock && edge == other.edge; }
    bool operator<(const Event &other) const
    {
        return clock != other.clock ? clock < other.clock : int(edge) < int(other.edge);
    }
};

Event rising(const std::string &clock) { return Event{clock, RISING_EDGE}; }
Event falling(const std::string &clock) { return Event{clock, FALLING_EDGE}; }

std::string str(const Event &e)
{
    if (e.clock.empty())
        return "(pad)";
    return e.clock + (e.edge == RISING_EDGE ? " rising" : " falling");
}

std::string str(const std::multiset<Event> &events)
{
    std::string s;
    for (auto &e : events)
        s += (s.empty() ? "" : ", ") + str(e);
    return "{" + s + "}";
}

} // namespace

class GateMatePathTest : public GateMateTest
{
  protected:
    // Timing analysis as after placement, with the report of every endpoint and the critical
    // path of every pair of clock events. (Without logging: printing the results can fail, see
    // FMAX_OF_RELATED_CLOCKS_WITHOUT_PATHS.)
    void analyse()
    {
        TimingAnalyser tmg(ctx);
        tmg.with_clock_skew = true;
        tmg.setup(true, false, true);
        result = tmg.get_timing_result();
    }

    // The pad or global clock (by the name used when building the design) that `net` comes from,
    // through the cells the packer adds on the way: the IO cells, the CPEs in front of RAM pins
    // and LUTs with a single input. Empty if `net` does not come from a single pad.
    std::string source_of(const NetInfo *net) const
    {
        for (int depth = 0; net != nullptr && depth < 20; depth++) {
            std::string name = net->name.str(ctx);
            if (boost::ends_with(name, "$glb"))
                return name.substr(0, name.size() - 4);
            const CellInfo *drv = net->driver.cell;
            if (drv == nullptr)
                return "";
            std::string cell = drv->name.str(ctx);
            if (drv->type == id_IOSEL)
                return boost::ends_with(cell, "$iosel") ? cell.substr(0, cell.size() - 6) : cell;
            if (drv->type.in(id_CPE_IBUF, id_CPE_IOBUF, id_CPE_LVDS_IBUF, id_CPE_LVDS_IOBUF))
                return cell;
            // A CPE passing a signal to a RAM pin and one from a RAM pin to the fabric
            if (drv->type.in(id_CPE_RAMI, id_CPE_RAMO, id_CPE_RAMIO)) {
                net = drv->getPort(net->driver.port == id_RAM_O ? id_I : id_RAM_I);
                continue;
            }
            const NetInfo *next = nullptr;
            int inputs = 0;
            for (auto &port : drv->ports) {
                const NetInfo *in = port.second.net;
                if (port.second.type != PORT_IN || in == nullptr)
                    continue;
                if (in->name.in(ctx->id("$PACKER_GND"), ctx->id("$PACKER_VCC")))
                    continue;
                next = in;
                inputs++;
            }
            if (inputs != 1)
                return "";
            net = next;
        }
        return "";
    }

    Event event(const ClockEvent &e) const
    {
        if (e.clock == IdString())
            return Event{};
        return Event{source_of(ctx->nets.at(e.clock).get()), e.edge};
    }

    // Whether an endpoint belongs to the part of the design named `prefix`: the endpoint's cell has
    // that name, or (for RAMs, which the packer can merge into a block of another name) the cell
    // driving it.
    bool belongs_to(const NetInfo *net, IdString cell, const std::string &prefix) const
    {
        if (boost::starts_with(cell.str(ctx), prefix))
            return true;
        return net && net->driver.cell && boost::starts_with(net->driver.cell->name.str(ctx), prefix);
    }

    // Capture events of the arrivals launched by `launch` at the endpoints of `prefix` (one per
    // endpoint and capture event)
    std::multiset<Event> captures(const std::string &prefix, const Event &launch) const
    {
        std::multiset<Event> found;
        for (auto &net : result.detailed_net_timings)
            for (auto &sink : net.second)
                if (belongs_to(ctx->nets.at(net.first).get(), sink.cell_port.first, prefix) &&
                    event(sink.clock_pair.start) == launch)
                    found.insert(event(sink.clock_pair.end));
        return found;
    }

    // All arrivals at the endpoints of `prefix`
    std::string arrivals(const std::string &prefix) const
    {
        std::string s;
        for (auto &net : result.detailed_net_timings)
            for (auto &sink : net.second)
                if (belongs_to(ctx->nets.at(net.first).get(), sink.cell_port.first, prefix))
                    s += stringf("\n    %s.%s: %s -> %s", sink.cell_port.first.c_str(ctx),
                                 sink.cell_port.second.c_str(ctx), str(event(sink.clock_pair.start)).c_str(),
                                 str(event(sink.clock_pair.end)).c_str());
        return s.empty() ? " none" : s;
    }

    // Whether `path` goes through a cell whose name starts with `prefix`
    bool visits(const CriticalPath &path, const std::string &prefix) const
    {
        for (auto &seg : path.segments)
            if (boost::starts_with(seg.from.first.str(ctx), prefix) || boost::starts_with(seg.to.first.str(ctx), prefix))
                return true;
        return false;
    }

    // Critical path between the given clock events, nullptr if there is none
    const CriticalPath *critical_path(const Event &launch, const Event &capture) const
    {
        auto matches = [&](const CriticalPath &p) {
            return event(p.clock_pair.start) == launch && event(p.clock_pair.end) == capture;
        };
        for (auto &p : result.clock_paths)
            if (matches(p.second))
                return &p.second;
        for (auto &p : result.xclock_paths)
            if (matches(p))
                return &p;
        return nullptr;
    }

    std::string describe(const CriticalPath &p) const
    {
        static const char *types[] = {"clk-to-clk", "skew", "clk-to-q", "source", "logic", "routing", "setup"};
        std::string s;
        for (auto &seg : p.segments)
            s += stringf("\n    %-10s %s.%s -> %s.%s %d ps", types[int(seg.type)], seg.from.first.c_str(ctx),
                         seg.from.second.c_str(ctx), seg.to.first.c_str(ctx), seg.to.second.c_str(ctx),
                         int(seg.delay));
        return s;
    }

    // Clocks by name, created on first use: a global clock from pad `name`, or the fabric net of
    // pad `name`
    NetInfo *global(const std::string &name)
    {
        auto &net = clocks[name];
        if (net == nullptr)
            net = global_clock(name);
        return net;
    }
    NetInfo *fabric(const std::string &name)
    {
        auto &net = clocks[name];
        if (net == nullptr)
            net = input(name);
        return net;
    }

    // Connect `port` of `cell` to `net`, replacing what it was connected to
    void reconnect(CellInfo *cell, IdString port, NetInfo *net)
    {
        if (cell->getPort(port))
            cell->disconnectPort(port);
        connect_input(cell, port, net);
    }

    // A RAM with one bit wide ports (SDP: port A writes, port B reads), reading address 0 and not
    // writing unless further inputs are connected; no outputs connected.
    CellInfo *bram(const std::string &name, bool is_40k, bool sdp, NetInfo *clk_a, NetInfo *clk_b)
    {
        CellInfo *ram = ctx->createCell(ctx->id(name), is_40k ? id_CC_BRAM_40K : id_CC_BRAM_20K);
        ram->params[id_RAM_MODE] = Property(sdp ? "SDP" : "TDP");
        int full = is_40k ? 80 : 40;
        ram->params[id_A_RD_WIDTH] = Property(sdp ? 0 : 1, 32);
        ram->params[id_A_WR_WIDTH] = Property(sdp ? full : 1, 32);
        ram->params[id_B_RD_WIDTH] = Property(sdp ? full : 1, 32);
        ram->params[id_B_WR_WIDTH] = Property(sdp ? 0 : 1, 32);
        connect_input(ram, id_A_CLK, clk_a);
        connect_input(ram, id_B_CLK, clk_b);
        connect_input(ram, id_A_EN, vcc());
        connect_input(ram, id_B_EN, vcc());
        connect_input(ram, id_A_WE, gnd());
        connect_input(ram, id_B_WE, gnd());
        return ram;
    }

    // A synchronous one bit wide FIFO (port A pops, port B pushes), out of reset, neither pushing
    // nor popping unless further inputs are connected
    CellInfo *fifo(const std::string &name, NetInfo *clk)
    {
        CellInfo *f = ctx->createCell(ctx->id(name), id_CC_FIFO_40K);
        f->params[id_RAM_MODE] = Property("TDP");
        f->params[id_FIFO_MODE] = Property("SYNC");
        f->params[id_A_WIDTH] = Property(1, 32);
        f->params[id_B_WIDTH] = Property(1, 32);
        f->params[id_ALMOST_FULL_OFFSET] = Property(4, 15);
        f->params[id_ALMOST_EMPTY_OFFSET] = Property(4, 15);
        connect_input(f, id_A_CLK, clk);
        connect_input(f, id_B_CLK, clk);
        connect_input(f, id_F_RST_N, vcc());
        connect_input(f, id_A_EN, gnd());
        connect_input(f, id_B_EN, gnd());
        connect_input(f, id_B_WE, vcc());
        return f;
    }

    TimingResult result;
    std::map<std::string, NetInfo *> clocks;
};

// Every kind of timing path, from each kind of start point to each kind of end point: the path
// must be analysed, launched and captured by the right clock edges, and start and end with the
// cell delays of its registers.
enum class Source
{
    PAD,
    BIDIRECTIONAL_PAD,
    FF,
    FF_FALLING,
    LATCH,
    IO_FF,
    IO_FF_FABRIC,
    IDDR,
    RAM_20K_A,
    RAM_40K_B,
    RAM_FABRIC,
    RAM_SDP,
    FIFO_STATUS,
};

enum class Sink
{
    PAD,
    TRISTATE_ENABLE,
    FF,
    FF_CONTROLS,
    FF_FALLING,
    LATCH,
    IO_FF,
    IO_FF_FABRIC,
    ODDR,
    RAM_20K_A,
    RAM_40K_B,
    RAM_FABRIC,
    RAM_SDP_WRITE,
    RAM_SDP_READ,
    FIFO_PUSH,
};

const char *name(Source s)
{
    static const char *names[] = {"pad",       "bidirectional_pad", "ff",         "ff_falling", "latch",
                                  "io_ff",     "io_ff_fabric",      "iddr",       "ram_20k_a",  "ram_40k_b",
                                  "ram_fabric", "ram_sdp",          "fifo_status"};
    return names[int(s)];
}

const char *name(Sink s)
{
    static const char *names[] = {"pad",       "tristate_enable", "ff",         "ff_controls", "ff_falling",
                                  "latch",     "io_ff",           "io_ff_fabric", "oddr",      "ram_20k_a",
                                  "ram_40k_b", "ram_fabric",      "ram_sdp_write", "ram_sdp_read", "fifo_push"};
    return names[int(s)];
}

class GateMateTimingMatrixTest : public GateMatePathTest, public ::testing::WithParamInterface<std::pair<Source, Sink>>
{
  protected:
    struct Launch
    {
        NetInfo *net;
        Event event;
    };

    // The source, named "src" (and "src_*"), clocked by global clock "cs" or fabric clock "fs";
    // returns the launched signals.
    std::vector<Launch> build_source(Source kind)
    {
        auto ff = [&](bool clk_inv) {
            CellInfo *ff = dff("src", input("src_d"), global("cs"));
            ff->params[id_CLK_INV] = Property(clk_inv, 1);
            return ff->getPort(id_Q);
        };
        auto io_ff = [&](NetInfo *clk) {
            NetInfo *d = input("src");
            ctx->cells.at(ctx->id("src"))->params[id_FF_IBF] = Property(1, 1);
            return dff("src_ff", d, clk)->getPort(id_Q);
        };
        auto ram = [&](bool is_40k, bool sdp, NetInfo *clk, bool port_b) {
            // SDP: written from fabric clock "src_wclk", read with `clk`
            CellInfo *ram = bram("src", is_40k, sdp, sdp ? input("src_wclk") : clk, clk);
            char port = (port_b && !sdp) ? 'B' : 'A';
            connect_input(ram, ctx->idf("%c_ADDR[7]", sdp ? 'B' : port), input("src_addr"));
            return connect_output(ram, ctx->idf("%c_DO[0]", port));
        };
        switch (kind) {
        case Source::PAD:
            return {{input("src"), Event{}}};
        case Source::BIDIRECTIONAL_PAD:
            return {{bidirectional("src", input("src_a"), input("src_t")), Event{}}};
        case Source::FF:
            return {{ff(false), rising("cs")}};
        case Source::FF_FALLING:
            return {{ff(true), falling("cs")}};
        case Source::LATCH:
            return {{dlt("src", input("src_d"), global("cs"))->getPort(id_Q), rising("cs")}};
        case Source::IO_FF:
            return {{io_ff(global("cs")), rising("cs")}};
        case Source::IO_FF_FABRIC:
            return {{io_ff(fabric("fs")), rising("fs")}};
        case Source::IDDR: {
            auto q = iddr("src", input("src_d"), global("cs"));
            return {{q.first, rising("cs")}, {q.second, falling("cs")}};
        }
        case Source::RAM_20K_A:
            return {{ram(false, false, global("cs"), false), rising("cs")}};
        case Source::RAM_40K_B:
            return {{ram(true, false, global("cs"), true), rising("cs")}};
        case Source::RAM_FABRIC:
            return {{ram(false, false, fabric("fs"), false), rising("fs")}};
        case Source::RAM_SDP:
            return {{ram(false, true, global("cs"), false), rising("cs")}};
        case Source::FIFO_STATUS: {
            CellInfo *f = fifo("src", global("cs"));
            reconnect(f, id_A_EN, input("src_pop"));
            reconnect(f, id_B_EN, input("src_push"));
            return {{connect_output(f, id_F_EMPTY), rising("cs")}};
        }
        }
        NPNR_ASSERT_FALSE("unknown source");
    }

    static bool registered(Source kind) { return !(kind == Source::PAD || kind == Source::BIDIRECTIONAL_PAD); }

    // The sink, named `prefix` (and `prefix`_*), clocked by global clock "ck" or fabric clock "fk",
    // with the signal `d` on all its inputs that take part (each through a LUT of its own);
    // returns the expected capture event of each endpoint.
    std::vector<Event> build_sink(Sink kind, const std::string &prefix, NetInfo *d)
    {
        int count = 0;
        auto via = [&]() { return lut1(stringf("%s_via%d", prefix.c_str(), count++), 0b01, d); };
        auto name = [&](const char *suffix) { return prefix + suffix; };
        auto ram = [&](bool is_40k, bool sdp, bool read_side, NetInfo *clk, bool port_b) {
            // SDP: the other port is clocked from fabric clock `prefix`_oclk
            NetInfo *other = sdp ? input(name("_oclk")) : clk;
            CellInfo *ram = bram(prefix, is_40k, sdp, (sdp && read_side) ? other : clk, (sdp && !read_side) ? other : clk);
            char port = sdp ? (read_side ? 'B' : 'A') : (port_b ? 'B' : 'A');
            reconnect(ram, ctx->idf("%c_ADDR[7]", port), via());
            reconnect(ram, ctx->idf("%c_EN", port), via());
            if (!(sdp && read_side)) {
                reconnect(ram, ctx->idf("%c_DI[0]", port), via());
                reconnect(ram, ctx->idf("%c_WE", port), via());
            }
            output(name("_q"), connect_output(ram, ctx->idf("%c_DO[0]", sdp ? 'A' : port)));
            return std::vector<Event>((sdp && read_side) ? 2 : 4, Event{});
        };
        auto with_clock = [](std::vector<Event> events, const Event &e) {
            for (auto &ev : events)
                ev = e;
            return events;
        };
        switch (kind) {
        case Sink::PAD:
            output(prefix, via());
            return {Event{}};
        case Sink::TRISTATE_ENABLE:
            tristate_output(prefix, input(name("_a")), via());
            return {Event{}};
        case Sink::FF:
        case Sink::FF_FALLING: {
            CellInfo *ff = dff(prefix, via(), global("ck"));
            ff->params[id_CLK_INV] = Property(kind == Sink::FF_FALLING, 1);
            output(name("_q"), ff->getPort(id_Q));
            return {kind == Sink::FF_FALLING ? falling("ck") : rising("ck")};
        }
        case Sink::FF_CONTROLS: {
            NetInfo *dd = via(), *en = via(), *sr = via();
            output(name("_q"), dff(prefix, dd, global("ck"), en, sr)->getPort(id_Q));
            return {rising("ck"), rising("ck"), rising("ck")};
        }
        case Sink::LATCH:
            output(name("_q"), dlt(prefix, via(), global("ck"))->getPort(id_Q));
            return {rising("ck")};
        case Sink::IO_FF:
        case Sink::IO_FF_FABRIC: {
            bool from_fabric = kind == Sink::IO_FF_FABRIC;
            CellInfo *ff = dff(name("_ff"), via(), from_fabric ? fabric("fk") : global("ck"));
            output(prefix, ff->getPort(id_Q))->params[id_FF_OBF] = Property(1, 1);
            return {rising(from_fabric ? "fk" : "ck")};
        }
        case Sink::ODDR: {
            NetInfo *d0 = via(), *d1 = via();
            // All DDR outputs of a bank need the same select
            output(prefix, oddr(name("_oddr"), d0, d1, global("ck"), fabric("ddr_sel")));
            return {rising("ck"), falling("ck")};
        }
        case Sink::RAM_20K_A:
            return with_clock(ram(false, false, false, global("ck"), false), rising("ck"));
        case Sink::RAM_40K_B:
            return with_clock(ram(true, false, false, global("ck"), true), rising("ck"));
        case Sink::RAM_FABRIC:
            return with_clock(ram(false, false, false, fabric("fk"), false), rising("fk"));
        case Sink::RAM_SDP_WRITE:
            return with_clock(ram(false, true, false, global("ck"), false), rising("ck"));
        case Sink::RAM_SDP_READ:
            return with_clock(ram(false, true, true, global("ck"), false), rising("ck"));
        case Sink::FIFO_PUSH: {
            CellInfo *f = fifo(prefix, global("ck"));
            reconnect(f, id_B_EN, via());
            reconnect(f, ctx->id("B_DI[0]"), via());
            connect_input(f, ctx->id("B_BM[0]"), vcc());
            output(name("_q"), connect_output(f, ctx->id("A_DO[0]")));
            return {rising("ck"), rising("ck")};
        }
        }
        NPNR_ASSERT_FALSE("unknown sink");
    }

    static bool registered(Sink kind) { return !(kind == Sink::PAD || kind == Sink::TRISTATE_ENABLE); }
};

TEST_P(GateMateTimingMatrixTest, path_is_timed)
{
    Source source = GetParam().first;
    Sink sink = GetParam().second;
    if (avoid(KnownBug::FIFO_STATUS_NOT_TIMED) && source == Source::FIFO_STATUS)
        GTEST_SKIP() << "FIFO status outputs are not timing start points";
    if (avoid(KnownBug::IOSEL_ARCS_WITH_OUTPUT_ENABLE) && source == Source::BIDIRECTIONAL_PAD)
        GTEST_SKIP() << "bidirectional pads have no input arc";
    if (avoid(KnownBug::IOSEL_ARCS_WITH_OUTPUT_ENABLE) && sink == Sink::TRISTATE_ENABLE)
        GTEST_SKIP() << "an IOSEL with an output enable has no arc to it";
    auto launches = build_source(source);
    std::vector<std::vector<Event>> expected;
    for (int i = 0; i < int(launches.size()); i++)
        expected.push_back(build_sink(sink, stringf("snk%d", i), launches[i].net));
    ASSERT_TRUE(packs());
    ctx->place();
    analyse();

    for (int i = 0; i < int(launches.size()); i++) {
        std::string prefix = stringf("snk%d", i);
        const Event &launch = launches[i].event;
        SCOPED_TRACE("launched by " + str(launch));
        std::multiset<Event> want(expected[i].begin(), expected[i].end());
        EXPECT_EQ(str(captures(prefix, launch)), str(want)) << "arrivals at the sink:" << arrivals(prefix);
        for (const Event &capture : std::set<Event>(want.begin(), want.end())) {
            const CriticalPath *path = critical_path(launch, capture);
            if (path == nullptr || path->segments.empty()) {
                ADD_FAILURE() << "no critical path from " << str(launch) << " to " << str(capture);
                continue;
            }
            // Skip the clock skew at the start
            auto first = path->segments.begin();
            while (first != path->segments.end() && (first->type == CriticalPath::Segment::Type::CLK_SKEW ||
                                                      first->type == CriticalPath::Segment::Type::CLK_TO_CLK))
                ++first;
            ASSERT_NE(first, path->segments.end());
            const auto &last = path->segments.back();
            EXPECT_TRUE(visits(*path, "src")) << "path:" << describe(*path);
            EXPECT_TRUE(visits(*path, prefix)) << "path:" << describe(*path);
            if (registered(source)) {
                EXPECT_EQ(int(first->type), int(CriticalPath::Segment::Type::CLK_TO_Q)) << "path:" << describe(*path);
                EXPECT_GT(first->delay, 0) << "clock to output of the source; path:" << describe(*path);
            }
            bool enable_setup_missing = avoid(KnownBug::REGISTER_ENABLE_SETUP_MISSING) && last.to.second == id_EN_INT;
            if (registered(sink) && !enable_setup_missing) {
                EXPECT_EQ(int(last.type), int(CriticalPath::Segment::Type::SETUP)) << "path:" << describe(*path);
                EXPECT_GT(last.delay, 0) << "setup time of the sink; path:" << describe(*path);
            }
        }
        // Every register input of the sink reached from the source has a setup time
        if (!registered(sink))
            continue;
        for (auto &net : result.detailed_net_timings) {
            for (auto &arrival : net.second) {
                const CellInfo *cell = ctx->cells.at(arrival.cell_port.first).get();
                IdString port = arrival.cell_port.second;
                if (!belongs_to(ctx->nets.at(net.first).get(), cell->name, prefix) ||
                    !(event(arrival.clock_pair.start) == launch))
                    continue;
                if (avoid(KnownBug::REGISTER_ENABLE_SETUP_MISSING) && port == id_EN_INT)
                    continue;
                int count = 0;
                ASSERT_EQ(ctx->getPortTimingClass(cell, port, count), TMG_REGISTER_INPUT)
                        << cell->name.c_str(ctx) << "." << port.c_str(ctx);
                for (int k = 0; k < count; k++)
                    EXPECT_GT(ctx->getPortClockingInfo(cell, port, k).setup.maxDelay(), 0)
                            << "setup time of " << cell->name.c_str(ctx) << "." << port.c_str(ctx);
            }
        }
    }
}

std::vector<std::pair<Source, Sink>> all_pairs()
{
    std::vector<std::pair<Source, Sink>> pairs;
    for (int s = 0; s <= int(Source::FIFO_STATUS); s++)
        for (int k = 0; k <= int(Sink::FIFO_PUSH); k++)
            pairs.emplace_back(Source(s), Sink(k));
    return pairs;
}

INSTANTIATE_TEST_SUITE_P(Pairs, GateMateTimingMatrixTest, ::testing::ValuesIn(all_pairs()),
                         [](const ::testing::TestParamInfo<std::pair<Source, Sink>> &info) {
                             return stringf("%s_to_%s", name(info.param.first), name(info.param.second));
                         });

// The clock each RAM pin is timed against. Each input pin must be captured by the clock of the
// port it belongs to; in SDP mode the data inputs and bit masks (of both ports) belong to the
// writing port A, the read address and enable to port B. Each data output must be launched by
// the clock of the port that reads it (port B in SDP mode). The edge follows the clock inversion
// of the port. Checked for both kinds of RAM, with global and fabric clocks, for the two halves
// of a block shared by two 20K RAMs, and for cascades.
struct RamClockCase
{
    std::string name;
    bool is_40k = false, sdp = false;
    bool fabric[2] = {false, false};
    bool inverted[2] = {false, false};
    bool same_clock = false;
    // A second 20K RAM "o" in the same block, with clocks of its own; the case's RAM "r" is in
    // the lower or (with `upper`) the upper half
    bool pair = false, upper = false;
    // Two cascaded 40K RAMs
    bool cascade = false;
};

std::ostream &operator<<(std::ostream &os, const RamClockCase &c) { return os << c.name; }

class GateMateRamClockTest : public GateMatePathTest, public ::testing::WithParamInterface<RamClockCase>
{
  protected:
    // RAM `name` (or the cascade `name`_LOWER/`name`_UPPER) as given by `c`, with port clocks
    // `name`_ca and `name`_cb. Each input pin in use is driven by a pad named after it (A_ADDR[7]:
    // `name`_A_ADDR7); the data outputs in use go into registers named likewise (`name`_A_DO0) on
    // fabric clock "cap". Records the expected capture event of each such pad and the expected
    // launch event of each register.
    void build_ram(const std::string &name, const RamClockCase &c)
    {
        std::vector<CellInfo *> cells;
        if (c.cascade) {
            for (const char *which : {"LOWER", "UPPER"}) {
                CellInfo *cell = ctx->createCell(ctx->idf("%s_%s", name.c_str(), which), id_CC_BRAM_40K);
                cell->params[id_CAS] = Property(which);
                cells.push_back(cell);
            }
        } else {
            cells.push_back(ctx->createCell(ctx->id(name), c.is_40k ? id_CC_BRAM_40K : id_CC_BRAM_20K));
        }
        int data_width = c.is_40k ? 40 : 20;
        int width = c.cascade ? 1 : c.sdp ? 2 * data_width : data_width;
        Event clock[2];
        NetInfo *clock_net[2];
        for (int p = 0; p < 2; p++) {
            std::string clk = name + (p == 1 && !c.same_clock ? "_cb" : "_ca");
            clock_net[p] = c.fabric[p] ? fabric(clk) : global(clk);
            clock[p] = Event{clk, c.inverted[p] ? FALLING_EDGE : RISING_EDGE};
        }
        for (CellInfo *cell : cells) {
            cell->params[id_RAM_MODE] = Property(c.sdp ? "SDP" : "TDP");
            cell->params[id_A_RD_WIDTH] = Property(c.sdp ? 0 : width, 32);
            cell->params[id_A_WR_WIDTH] = Property(width, 32);
            cell->params[id_B_RD_WIDTH] = Property(width, 32);
            cell->params[id_B_WR_WIDTH] = Property(c.sdp ? 0 : width, 32);
            for (int p = 0; p < 2; p++) {
                cell->params[ctx->idf("%c_CLK_INV", "AB"[p])] = Property(c.inverted[p], 1);
                connect_input(cell, ctx->idf("%c_CLK", "AB"[p]), clock_net[p]);
            }
        }
        auto pin = [&](char port, const std::string &pin_name, int capture_port) {
            std::string pad = stringf("%s_%c_%s", name.c_str(), port, pin_name.c_str());
            boost::erase_all(pad, "[");
            boost::erase_all(pad, "]");
            NetInfo *net = input(pad);
            for (CellInfo *cell : cells)
                connect_input(cell, ctx->idf("%c_%s", port, pin_name.c_str()), net);
            expected_capture[pad] = clock[capture_port];
        };
        // The lowest and highest data bit in use
        std::set<int> bits = {0, std::min(width, data_width) - 1};
        for (int p = 0; p < 2; p++) {
            char port = "AB"[p];
            // Ports read and written by the port itself in TDP mode; in SDP mode port A writes
            // (with the upper data bits on port B's data inputs) and port B reads
            bool writes = !c.sdp || p == 0;
            pin(port, "ADDR[7]", p);
            pin(port, "ADDR[15]", p);
            pin(port, "EN", p);
            if (writes)
                pin(port, "WE", p);
            if (writes || c.sdp) {
                for (int bit : bits) {
                    pin(port, stringf("DI[%d]", bit), c.sdp ? 0 : p);
                    pin(port, stringf("BM[%d]", bit), c.sdp ? 0 : p);
                }
            }
            for (int bit : bits) {
                std::string reg = stringf("%s_%c_DO%d", name.c_str(), port, bit);
                NetInfo *out = connect_output(cells.back(), ctx->idf("%c_DO[%d]", port, bit));
                output(reg + "_q", dff(reg, out, fabric("cap"))->getPort(id_Q));
                expected_launch[reg] = clock[c.sdp ? 1 : p];
            }
        }
        if (c.cascade) {
            connect_input(cells[1], id_A_CI, connect_output(cells[0], id_A_CO));
            connect_input(cells[1], id_B_CI, connect_output(cells[0], id_B_CO));
        }
    }

    std::map<std::string, Event> expected_capture, expected_launch;
};

TEST_P(GateMateRamClockTest, pins_use_clock_of_their_port)
{
    const RamClockCase &c = GetParam();
    build_ram("r", c);
    if (c.pair) {
        // The other RAM in the block needs the same mode
        RamClockCase other;
        other.sdp = c.sdp;
        build_ram("o", other);
    }
    ASSERT_TRUE(packs());
    if (c.pair) {
        ASSERT_TRUE(c.upper ? place_ram_pair("o", "r") : place_ram_pair("r", "o"));
    }
    ctx->place();
    if (c.pair) {
        int rams = 0;
        for (auto &cell : ctx->cells)
            rams += cell.second->type == id_RAM;
        ASSERT_EQ(rams, 1) << "the two 20K RAMs should share one block";
    }
    analyse();

    // Capture events at the RAM pins, by the pad driving them
    std::map<std::string, std::multiset<Event>> captured;
    std::map<std::string, std::string> endpoints;
    for (auto &net : result.detailed_net_timings) {
        std::string pad = source_of(ctx->nets.at(net.first).get());
        for (auto &sink : net.second) {
            if (!ctx->cells.at(sink.cell_port.first)->type.in(id_RAM, id_RAM_HALF))
                continue;
            captured[pad].insert(event(sink.clock_pair.end));
            endpoints[pad] += " " + sink.cell_port.second.str(ctx);
        }
    }
    for (auto &exp : expected_capture) {
        auto &got = captured[exp.first];
        EXPECT_FALSE(got.empty()) << exp.first << " reaches no RAM pin that is timed";
        for (auto &e : got)
            EXPECT_EQ(str(e), str(exp.second)) << exp.first << " at RAM pin(s)" << endpoints[exp.first];
    }
    // Launch events at the registers behind the data outputs
    for (auto &exp : expected_launch) {
        std::multiset<Event> launched;
        for (auto &net : result.detailed_net_timings)
            for (auto &sink : net.second)
                if (sink.cell_port.first == ctx->id(exp.first))
                    launched.insert(event(sink.clock_pair.start));
        EXPECT_EQ(str(launched), str(std::multiset<Event>{exp.second})) << "data output into " << exp.first;
    }
}

std::vector<RamClockCase> ram_clock_cases()
{
    std::vector<RamClockCase> cases;
    auto add = [&](const std::string &name, bool is_40k, bool sdp) -> RamClockCase & {
        RamClockCase c;
        c.name = name;
        c.is_40k = is_40k;
        c.sdp = sdp;
        cases.push_back(c);
        return cases.back();
    };
    for (bool is_40k : {false, true}) {
        std::string size = is_40k ? "40k" : "20k";
        for (bool sdp : {false, true}) {
            std::string mode = size + (sdp ? "_sdp" : "_tdp");
            add(mode + "_global", is_40k, sdp);
            add(mode + "_same_clock", is_40k, sdp).same_clock = true;
            add(mode + "_fabric_a", is_40k, sdp).fabric[0] = true;
            add(mode + "_fabric_b", is_40k, sdp).fabric[1] = true;
            auto &both = add(mode + "_fabric_both", is_40k, sdp);
            both.fabric[0] = both.fabric[1] = true;
            add(mode + "_inverted_a", is_40k, sdp).inverted[0] = true;
            add(mode + "_inverted_b", is_40k, sdp).inverted[1] = true;
            auto &fabric_inv = add(mode + "_fabric_inverted", is_40k, sdp);
            fabric_inv.fabric[0] = fabric_inv.fabric[1] = fabric_inv.inverted[1] = true;
        }
    }
    for (bool upper : {false, true}) {
        std::string half = upper ? "pair_upper" : "pair_lower";
        for (bool sdp : {false, true}) {
            std::string mode = half + (sdp ? "_sdp" : "_tdp");
            auto &g = add(mode + "_global", false, sdp);
            g.pair = true;
            g.upper = upper;
            auto &f = add(mode + "_fabric_a", false, sdp);
            f.pair = true;
            f.upper = upper;
            f.fabric[0] = true;
            auto &i = add(mode + "_inverted_b", false, sdp);
            i.pair = true;
            i.upper = upper;
            i.inverted[1] = true;
        }
    }
    for (bool inverted : {false, true}) {
        auto &c = add(inverted ? "cascade_inverted_a" : "cascade_global", true, false);
        c.cascade = true;
        c.inverted[0] = inverted;
        auto &f = add(inverted ? "cascade_fabric_inverted_b" : "cascade_fabric", true, false);
        f.cascade = true;
        f.fabric[0] = f.fabric[1] = true;
        f.inverted[1] = inverted;
    }
    return cases;
}

INSTANTIATE_TEST_SUITE_P(Cases, GateMateRamClockTest, ::testing::ValuesIn(ram_clock_cases()),
                         [](const ::testing::TestParamInfo<RamClockCase> &info) { return info.param.name; });

// Cell delays compared with the timing database (prjpeppercorn's conversion of the vendor's
// delay files), and cell arcs compared with the simulator's cell models.
class GateMateTimingArcTest : public GateMatePathTest
{
  protected:
    // An entry of the timing database of the current speed grade, nullptr if there is none
    const GateMateTimingExtraDataPOD *timing_entry(const std::string &name)
    {
        const auto *sp = reinterpret_cast<const GateMateSpeedGradeExtraDataPOD *>(ctx->speed_grade->extra_data.get());
        IdString id = ctx->id("timing_" + name);
        for (const auto &t : sp->timings)
            if (IdString(t.name) == id)
                return &t;
        return nullptr;
    }

    bool has_timing(const std::string &name) { return timing_entry(name) != nullptr; }

    // The worst case of an entry of the timing database
    delay_t timing_db(const std::string &name)
    {
        const auto *t = timing_entry(name);
        if (t == nullptr) {
            ADD_FAILURE() << "timing database has no entry " << name;
            return 0;
        }
        return DelayQuad(t->delay.fast_min, t->delay.fast_max, t->delay.slow_min, t->delay.slow_max).maxDelay();
    }

    // The setup time of a setup/hold entry of the timing database (setup in the fast values, hold
    // in the slow ones)
    delay_t setup_db(const std::string &name)
    {
        const auto *t = timing_entry(name);
        if (t == nullptr) {
            ADD_FAILURE() << "timing database has no entry " << name;
            return 0;
        }
        return t->delay.fast_max;
    }

    void implement()
    {
        ASSERT_TRUE(packs());
        ctx->place();
    }

    std::vector<CellInfo *> cells_of_type(IdString type)
    {
        std::vector<CellInfo *> found;
        for (auto &cell : ctx->cells)
            if (cell.second->type == type)
                found.push_back(cell.second.get());
        return found;
    }

    bool is_lower(const CellInfo *cell) { return ctx->getBelLocation(cell->bel).z % 2 == 1; }

    // A registered 16 bit adder: a, b and the sum in registers on global clock "clk"
    void registered_adder(int width = 16)
    {
        NetInfo *clk = global("clk");
        NetInfo *carry = gnd();
        std::vector<NetInfo *> sum;
        for (int i = 0; i < width; i++) {
            NetInfo *a = dff(stringf("a%d", i), input(stringf("ia%d", i)), clk)->getPort(id_Q);
            NetInfo *b = dff(stringf("b%d", i), input(stringf("ib%d", i)), clk)->getPort(id_Q);
            auto sc = addf(stringf("add%d", i), a, b, carry);
            carry = sc.second;
            sum.push_back(sc.first);
        }
        for (int i = 0; i < width; i++)
            output(stringf("s%d", i), dff(stringf("s%d_ff", i), sum[i], clk)->getPort(id_Q));
    }
};

// The carry chain: each adder CPE needs an arc from its carry input to its carry output (in the
// timing database: _ADDF2Y1_CINY1_COUTY1, in older ones _ADDF2Y1_CINY1_COUTY).
TEST_F(GateMateTimingArcTest, carry_in_to_carry_out)
{
    registered_adder();
    implement();
    int checked = 0;
    for (IdString type : {id_CPE_ADDF, id_CPE_ADDF2}) {
        for (CellInfo *cell : cells_of_type(type)) {
            if (!cell->getPort(id_CINY1) || !cell->getPort(id_COUTY1))
                continue;
            DelayQuad delay;
            EXPECT_TRUE(ctx->getCellDelay(cell, id_CINY1, id_COUTY1, delay)) << cell->name.c_str(ctx);
            EXPECT_EQ(delay.maxDelay(), timing_db(has_timing("_ADDF2Y1_CINY1_COUTY1") ? "_ADDF2Y1_CINY1_COUTY1"
                                                                                      : "_ADDF2Y1_CINY1_COUTY"))
                    << cell->name.c_str(ctx);
            checked++;
        }
    }
    EXPECT_GE(checked, 6);
}

// End to end: the critical path of a registered adder starts at the lowest bits and runs
// through the whole carry chain.
TEST_F(GateMateTimingArcTest, critical_path_runs_through_carry_chain)
{
    registered_adder();
    implement();
    analyse();
    const CriticalPath *path = critical_path(rising("clk"), rising("clk"));
    ASSERT_NE(path, nullptr);
    std::set<IdString> adders;
    for (auto &seg : path->segments)
        for (IdString cell : {seg.from.first, seg.to.first})
            if (ctx->cells.at(cell)->type.in(id_CPE_ADDF, id_CPE_ADDF2))
                adders.insert(cell);
    // 16 bits in 8 CPEs of two bits each
    EXPECT_GE(int(adders.size()), 8) << "path:" << describe(*path);
}

// The CPE that brings a carry input into the chain, and pass-through connections, go through
// the CP lines (CPE_CPLINES). Pass-through arcs have their own entries (_ROUTING_<in>_<out>); for
// the carry input, comb12_COUTY1 is the delay from the LUT output to COUTY1, of which the LUT's
// arc already includes the output stage (comb1_OUT1).
TEST_F(GateMateTimingArcTest, carry_lines_delays)
{
    NetInfo *carry = input("ci");
    std::vector<NetInfo *> sum;
    for (int i = 0; i < 4; i++) {
        auto sc = addf(stringf("add%d", i), input(stringf("ia%d", i)), input(stringf("ib%d", i)), carry);
        carry = sc.second;
        output(stringf("s%d", i), sc.first);
    }
    implement();
    int checked = 0;
    for (CellInfo *cell : cells_of_type(id_CPE_CPLINES)) {
        for (auto &in : cell->ports) {
            for (auto &out : cell->ports) {
                if (in.second.type != PORT_IN || out.second.type != PORT_OUT || !in.second.net || !out.second.net)
                    continue;
                DelayQuad delay;
                if (!ctx->getCellDelay(cell, in.first, out.first, delay))
                    continue;
                std::string from = in.first.str(ctx), to = out.first.str(ctx);
                SCOPED_TRACE(cell->name.str(ctx) + " " + from + " -> " + to);
                if (from == "OUT1" || from == "OUT2") {
                    EXPECT_GE(delay.maxDelay(), timing_db("comb12_" + to) - timing_db("comb1_OUT1"));
                    checked++;
                } else if (has_timing("_ROUTING_" + from + "_" + to)) {
                    EXPECT_EQ(delay.maxDelay(), timing_db("_ROUTING_" + from + "_" + to));
                    checked++;
                }
            }
        }
    }
    EXPECT_GE(checked, 1);
}

// A RAM data output enters the fabric through a CPE (CPE_RAMI, or CPE_RAMIO when the CPE also
// drives a RAM input; RAM_I1/RAM_I2 to OUT1/OUT2).
TEST_F(GateMateTimingArcTest, ram_output_feed_through)
{
    NetInfo *clk = global("clk");
    CellInfo *ram = bram("ram", false, false, clk, clk);
    connect_input(ram, ctx->id("A_ADDR[7]"), input("a"));
    for (int i = 0; i < 2; i++)
        output(stringf("q%d", i), dff(stringf("q%d_ff", i), connect_output(ram, ctx->idf("A_DO[%d]", i)), clk)->getPort(id_Q));
    implement();
    int checked = 0;
    for (IdString type : {id_CPE_RAMI, id_CPE_RAMO, id_CPE_RAMIO}) {
        for (CellInfo *cell : cells_of_type(type)) {
            if (!cell->getPort(id_RAM_I) || !cell->getPort(id_OUT))
                continue;
            DelayQuad delay;
            ASSERT_TRUE(ctx->getCellDelay(cell, id_RAM_I, id_OUT, delay)) << cell->name.c_str(ctx);
            EXPECT_EQ(delay.maxDelay(), timing_db(is_lower(cell) ? "_ARBLUT_RAM_I1_OUT1" : "_ARBLUT_RAM_I2_OUT2"))
                    << cell->name.c_str(ctx);
            checked++;
        }
    }
    EXPECT_EQ(checked, 2);
}

class GateMateRamTimingTest : public GateMateTimingArcTest, public ::testing::WithParamInterface<bool>
{
  protected:
    // A 20K TDP RAM with its data outputs in use, with or without output registers; returns the
    // packed RAM
    CellInfo *ram_with_outputs(bool do_reg)
    {
        NetInfo *clk = global("clk");
        CellInfo *ram = bram("ram", false, false, clk, clk);
        ram->params[id_A_DO_REG] = Property(do_reg, 1);
        ram->params[id_B_DO_REG] = Property(do_reg, 1);
        for (char port : {'A', 'B'}) {
            connect_input(ram, ctx->idf("%c_ADDR[7]", port), input(stringf("a%c", port)));
            NetInfo *q = connect_output(ram, ctx->idf("%c_DO[0]", port));
            output(stringf("q%c", port), dff(stringf("q%c_ff", port), q, clk)->getPort(id_Q));
        }
        pack();
        ctx->place();
        auto rams = cells_of_type(id_RAM);
        return rams.size() == 1 ? rams.front() : nullptr;
    }
};

// Clock to output of the data outputs: RAM_NOECC_IOPATH_2 (port A) and _3 (port B) of the timing
// database; with output registers RAM_REG_IOPATH_2 and _3.
TEST_P(GateMateRamTimingTest, clock_to_out)
{
    bool do_reg = GetParam();
    CellInfo *ram = ram_with_outputs(do_reg);
    ASSERT_NE(ram, nullptr);
    std::string kind = do_reg ? "RAM_REG_IOPATH_" : "RAM_NOECC_IOPATH_";
    EXPECT_EQ(impl->getPortClockingInfo(ram, ctx->id("DOA[0]"), 0).clockToQ.maxDelay(), timing_db(kind + "2"));
    EXPECT_EQ(impl->getPortClockingInfo(ram, ctx->id("DOB[0]"), 0).clockToQ.maxDelay(), timing_db(kind + "3"));
}

INSTANTIATE_TEST_SUITE_P(OutputRegisters, GateMateRamTimingTest, ::testing::Values(false, true),
                         [](const ::testing::TestParamInfo<bool> &info) {
                             return info.param ? "registered" : "unregistered";
                         });

// Setup times of the RAM inputs. The setup/hold entries of the timing database, as grouped by the
// vendor's table of RAM pins: 1 addresses, 3/4 data inputs A/B, 5/6 enables, 7/8 write enables,
// 9/10 bit masks.
TEST_F(GateMateTimingArcTest, ram_setup_times)
{
    NetInfo *clk = global("clk");
    CellInfo *ram = bram("ram", false, false, clk, clk);
    for (char port : {'A', 'B'}) {
        for (const char *pin : {"ADDR[7]", "DI[0]", "BM[0]", "EN", "WE"})
            reconnect(ram, ctx->idf("%c_%s", port, pin), input(stringf("%c_%s", port, pin)));
        output(stringf("q%c", port), connect_output(ram, ctx->idf("%c_DO[0]", port)));
    }
    implement();
    auto rams = cells_of_type(id_RAM);
    ASSERT_EQ(rams.size(), 1u);
    // The group of each pin, by name (in either half of the block)
    const std::pair<const char *, int> groups[] = {{"ADDRA", 1}, {"ADDRB", 1}, {"DIA", 3},   {"DIB", 4},
                                                   {"ENA", 5},   {"ENB", 6},   {"GLWEA", 7}, {"GLWEB", 8},
                                                   {"WEA", 9},   {"WEB", 10}};
    std::set<int> seen;
    for (auto &port : rams[0]->ports) {
        if (port.second.type != PORT_IN || !port.second.net || boost::starts_with(port.first.str(ctx), "CLK") ||
            boost::starts_with(port.first.str(ctx), "CLOCK"))
            continue;
        std::string name = port.first.str(ctx);
        int group = 0;
        for (auto &g : groups)
            if (boost::starts_with(name, g.first) && name.size() > strlen(g.first) &&
                (std::isdigit(name[strlen(g.first)]) || name[strlen(g.first)] == '['))
                group = g.second;
        ASSERT_NE(group, 0) << "unexpected RAM input " << name;
        seen.insert(group);
        EXPECT_EQ(impl->getPortClockingInfo(rams[0], port.first, 0).setup.maxDelay(),
                  setup_db(stringf("RAM_NOECC_SETUPHOLD_%d", group)))
                << name;
    }
    EXPECT_EQ(seen.size(), 9u) << "pin groups in use";
}

// The enable input of a CPE register (EN_INT after packing) has the same setup time as its
// data input.
TEST_F(GateMateTimingArcTest, register_enable_setup_time)
{
    CellInfo *ff = dff("ff", input("d"), global("clk"), input("en"));
    output("q", ff->getPort(id_Q));
    implement();
    ASSERT_TRUE(ff->getPort(id_EN_INT));
    int count = 0;
    EXPECT_EQ(impl->getPortTimingClass(ff, id_EN_INT, count), TMG_REGISTER_INPUT);
    EXPECT_EQ(impl->getPortClockingInfo(ff, id_EN_INT, 0).setup.maxDelay(),
              impl->getPortClockingInfo(ff, id_DIN, 0).setup.maxDelay());
}

// The status outputs of a FIFO are register outputs of its clocks: paths from them into the
// fabric must be analysed.
TEST_F(GateMateTimingArcTest, fifo_status_outputs_are_timed)
{
    CellInfo *f = fifo("fifo", global("clk"));
    reconnect(f, id_A_EN, input("pop"));
    reconnect(f, id_B_EN, input("push"));
    std::vector<IdString> flags = {id_F_EMPTY, id_F_FULL, id_F_ALMOST_EMPTY, id_F_ALMOST_FULL};
    for (IdString flag : flags) {
        NetInfo *q = connect_output(f, flag);
        output("q_" + flag.str(ctx), dff("ff_" + flag.str(ctx), lut1("inv_" + flag.str(ctx), 0b01, q), global("clk"))->getPort(id_Q));
    }
    implement();
    analyse();
    for (IdString flag : flags) {
        std::string reg = "ff_" + flag.str(ctx);
        std::multiset<Event> launched;
        for (auto &net : result.detailed_net_timings)
            for (auto &sink : net.second)
                if (sink.cell_port.first == ctx->id(reg))
                    launched.insert(event(sink.clock_pair.start));
        EXPECT_EQ(str(launched), str(std::multiset<Event>{rising("clk")})) << flag.c_str(ctx);
    }
}

// Clock to output of IO input registers on each global clock: io_sel_CLOCK<n>_IN1 of the timing
// database, n the index (from 0) of the IOSEL clock input the clock comes in on.
TEST_F(GateMateTimingArcTest, io_register_clock_to_out_per_global_clock)
{
    for (int k = 0; k < 4; k++) {
        NetInfo *d = input(stringf("d%d", k));
        ctx->cells.at(ctx->idf("d%d", k))->params[id_FF_IBF] = Property(1, 1);
        CellInfo *ff = dff(stringf("ff%d", k), d, global(stringf("clk%d", k)));
        output(stringf("q%d", k), lut1(stringf("inv%d", k), 0b01, ff->getPort(id_Q)));
    }
    implement();
    int checked = 0;
    for (CellInfo *iosel : cells_of_type(id_IOSEL)) {
        if (!bool_or_default(iosel->params, id_IN1_FF))
            continue;
        int index = -1;
        for (int n = 1; n <= 4; n++)
            if (iosel->getPort(ctx->idf("CLOCK%d", n)))
                index = n - 1;
        ASSERT_GE(index, 0) << iosel->name.c_str(ctx) << " has no global clock";
        auto info = impl->getPortClockingInfo(iosel, id_IN1, 0);
        EXPECT_EQ(info.clock_port.str(ctx), stringf("CLOCK%d", index + 1)) << iosel->name.c_str(ctx);
        EXPECT_EQ(info.clockToQ.maxDelay(), timing_db(stringf("io_sel_CLOCK%d_IN1", index)))
                << iosel->name.c_str(ctx);
        checked++;
    }
    EXPECT_EQ(checked, 4);
}

// Required times (and so slack and criticality) reach the RAM data outputs, through the CPE that
// takes each output into the fabric.
TEST_F(GateMateTimingArcTest, ram_output_slack_is_known)
{
    NetInfo *clk = global("clk");
    CellInfo *ram = bram("ram", false, false, clk, clk);
    connect_input(ram, ctx->id("A_ADDR[7]"), input("a"));
    CellInfo *ff = dff("ff", lut1("inv", 0b01, connect_output(ram, ctx->id("A_DO[0]"))), clk);
    output("q", ff->getPort(id_Q));
    implement();
    TimingAnalyser tmg(ctx);
    tmg.with_clock_skew = true;
    tmg.setup(false, false, false);
    const float unknown = float(std::numeric_limits<delay_t>::max());
    EXPECT_LT(tmg.get_setup_slack(CellPortKey(ff->name, id_DIN)), unknown) << "register behind the RAM";
    int checked = 0;
    for (IdString type : {id_CPE_RAMI, id_CPE_RAMO, id_CPE_RAMIO})
        for (CellInfo *cell : cells_of_type(type))
            if (cell->getPort(id_RAM_I) && cell->getPort(id_OUT)) {
                EXPECT_LT(tmg.get_setup_slack(CellPortKey(cell->name, id_RAM_I)), unknown) << cell->name.c_str(ctx);
                checked++;
            }
    EXPECT_EQ(checked, 1);
}

// The input of a bidirectional pad drives the fabric through its IOSEL like that of an input pad.
TEST_F(GateMateTimingArcTest, bidirectional_pad_input_is_timed)
{
    NetInfo *pad = bidirectional("b", input("a"), input("t"));
    CellInfo *ff = dff("ff", lut1("inv", 0b01, pad), global("clk"));
    output("q", ff->getPort(id_Q));
    implement();
    analyse();
    std::multiset<Event> launched;
    for (auto &net : result.detailed_net_timings)
        for (auto &sink : net.second)
            if (sink.cell_port.first == ff->name)
                launched.insert(event(sink.clock_pair.start));
    EXPECT_EQ(str(launched), str(std::multiset<Event>{Event{}})) << "arrivals at the register behind the pad";
}

// Paths through the CPE input COMBIN, which the packer uses where a LUT drives a register and
// further logic (the lower half passes the LUT output on) and for the fifth input of a 5-input LUT
// tree, and through the CPE that takes the carry out of an adder chain (CINY1): the registers
// behind them must be reached from the registers in front.
class GateMateLaunchTest : public GateMateTimingArcTest
{
  protected:
    // Launch events of the arrivals at the inputs of register `reg`
    std::string launched_at(const char *reg)
    {
        std::multiset<Event> launched;
        for (auto &net : result.detailed_net_timings)
            for (auto &sink : net.second)
                if (sink.cell_port.first == ctx->id(reg))
                    launched.insert(event(sink.clock_pair.start));
        return str(launched);
    }
};

TEST_F(GateMateLaunchTest, lut_driving_register_and_logic)
{
    NetInfo *clk = global("clk");
    CellInfo *src = dff("src", input("d"), clk);
    NetInfo *shared = lut2("shared", 0b0110, src->getPort(id_Q), input("e"));
    output("q1", dff("ff1", shared, clk)->getPort(id_Q));
    output("q2", dff("ff2", lut2("after", 0b1000, shared, input("f")), clk)->getPort(id_Q));
    implement();
    analyse();
    EXPECT_EQ(launched_at("ff1"), "{(pad), clk rising}");
    EXPECT_EQ(launched_at("ff2"), "{(pad), clk rising}") << "path src -> shared -> after -> ff2";
}

TEST_F(GateMateLaunchTest, fifth_input_of_lut_tree)
{
    NetInfo *clk = global("clk");
    CellInfo *src = dff("src", input("d"), clk);
    CellInfo *l = create_cell_ptr(id_CC_L2T5, "l5");
    l->params[id_INIT_L02] = Property(0b0110, 4);
    l->params[id_INIT_L03] = Property(0b1000, 4);
    l->params[id_INIT_L11] = Property(0b0110, 4);
    l->params[id_INIT_L20] = Property(0b1001, 4);
    for (int i = 0; i < 4; i++)
        l->connectPort(ctx->idf("I%d", i), input(stringf("h%d", i)));
    l->connectPort(id_I4, src->getPort(id_Q));
    NetInfo *o = create_net("l5$o");
    l->connectPort(id_O, o);
    output("q", dff("ff", o, clk)->getPort(id_Q));
    implement();
    analyse();
    EXPECT_NE(launched_at("ff").find("clk rising"), std::string::npos) << "path src -> I4 -> ff: " << launched_at("ff");
}

// A comparison a < b of two registers: yosys builds it from an adder chain whose carry out is the
// result.
TEST_F(GateMateLaunchTest, comparator_result)
{
    NetInfo *clk = global("clk");
    NetInfo *carry = vcc();
    for (int i = 0; i < 8; i++) {
        NetInfo *a = dff(stringf("a%d", i), input(stringf("ia%d", i)), clk)->getPort(id_Q);
        NetInfo *b = dff(stringf("b%d", i), input(stringf("ib%d", i)), clk)->getPort(id_Q);
        carry = addf(stringf("cmp%d", i), a, lut1(stringf("nb%d", i), 0b01, b), carry).second;
    }
    output("q", dff("lt", lut1("inv", 0b01, carry), clk)->getPort(id_Q));
    implement();
    analyse();
    EXPECT_NE(launched_at("lt").find("clk rising"), std::string::npos) << "comparison result: " << launched_at("lt");
}

// Generic: every combinational dependency inside a cell (from the simulator's cell models) has a
// timing arc, for designs with each kind of logic.
class GateMateArcCoverageTest : public GateMateTimingArcTest, public ::testing::WithParamInterface<std::string>
{
};

TEST_P(GateMateArcCoverageTest, every_dependency_has_an_arc)
{
    const std::string &design = GetParam();
    std::vector<NetInfo *> in;
    for (int i = 0; i < 8; i++)
        in.push_back(input(stringf("i%d", i)));
    NetInfo *clk = global("clk");
    if (design == "luts") {
        output("o0", lut1("l1", 0b01, in[0]));
        output("o1", lut2("l2", 0b0110, in[1], in[2]));
        output("o2", l2t4("l4", 0b0110, 0b1000, 0b1110, in[0], in[3], in[4], in[5]));
        output("o3", mx2("m2", in[1], in[6], in[7]));
        output("o4", mx4("m4", in[0], in[1], in[2], in[3], in[4], in[5]));
        CellInfo *ff = dff("ff", lut2("l2b", 0b1001, in[6], in[7]), clk, in[0], in[1]);
        output("o5", ff->getPort(id_Q));
        // A LUT driving a register and further logic, and a 5-input LUT tree
        NetInfo *shared = lut2("shared", 0b0110, in[2], in[3]);
        output("o6", dff("ff_shared", shared, clk)->getPort(id_Q));
        output("o7", lut2("after", 0b1000, shared, in[4]));
        CellInfo *l5 = create_cell_ptr(id_CC_L2T5, "l5");
        l5->params[id_INIT_L02] = Property(0b0110, 4);
        l5->params[id_INIT_L03] = Property(0b1000, 4);
        l5->params[id_INIT_L11] = Property(0b0110, 4);
        l5->params[id_INIT_L20] = Property(0b1001, 4);
        for (int i = 0; i < 5; i++)
            l5->connectPort(ctx->idf("I%d", i), in[i + 1]);
        l5->connectPort(id_O, create_net("l5$o"));
        output("o8", ctx->nets.at(ctx->id("l5$o")).get());
    } else if (design == "adder") {
        NetInfo *carry = in[7];
        for (int i = 0; i < 6; i++) {
            auto sc = addf(stringf("add%d", i), in[i], lut1(stringf("inv%d", i), 0b01, in[(i + 3) % 7]), carry);
            carry = sc.second;
            output(stringf("s%d", i), sc.first);
        }
        output("co", carry);
    } else if (design == "ram") {
        CellInfo *ram = bram("ram", false, false, clk, clk);
        reconnect(ram, id_A_EN, lut2("en", 0b1000, in[0], in[1]));
        reconnect(ram, id_A_WE, mx2("we", in[2], in[3], in[4]));
        connect_input(ram, ctx->id("A_ADDR[7]"), lut1("addr", 0b01, in[5]));
        connect_input(ram, ctx->id("A_DI[0]"), in[6]);
        output("q", lut1("q_inv", 0b01, connect_output(ram, ctx->id("A_DO[0]"))));
    } else if (design == "adder_carry_used") {
        // A carry from the middle of the chain also used as an ordinary signal
        NetInfo *carry = gnd();
        for (int i = 0; i < 4; i++) {
            auto sc = addf(stringf("add%d", i), in[i], in[i + 4], carry);
            carry = sc.second;
            output(stringf("s%d", i), sc.first);
            if (i == 1)
                output("mid", lut1("mid_inv", 0b01, carry));
        }
        output("co", carry);
    } else if (design == "multiplier") {
        CellInfo *mult = ctx->createCell(ctx->id("mult"), id_CC_MULT);
        mult->params[id_A_WIDTH] = Property(4, 32);
        mult->params[id_B_WIDTH] = Property(4, 32);
        mult->params[id_P_WIDTH] = Property(8, 32);
        for (int i = 0; i < 4; i++) {
            connect_input(mult, ctx->idf("A[%d]", i), in[i]);
            connect_input(mult, ctx->idf("B[%d]", i), in[4 + i]);
        }
        for (int i = 0; i < 8; i++)
            output(stringf("p%d", i), connect_output(mult, ctx->idf("P[%d]", i)));
    } else if (design == "io") {
        tristate_output("t", lut2("t_a", 0b0110, in[0], in[1]), in[2]);
        NetInfo *bidi = bidirectional("b", in[3], lut1("b_t", 0b01, in[4]));
        output("bo", lut1("b_inv", 0b01, bidi));
        ctx->cells.at(ctx->id("i5"))->params[id_FF_IBF] = Property(1, 1);
        output("r", dff("r_ff", in[5], clk)->getPort(id_Q))->params[id_FF_OBF] = Property(1, 1);
    }
    implement();
    auto missing = missing_timing_arcs();
    EXPECT_TRUE(missing.empty()) << "dependencies without a timing arc:" << describe_arcs(missing);
}

INSTANTIATE_TEST_SUITE_P(Designs, GateMateArcCoverageTest, ::testing::Values("luts", "adder", "adder_carry_used", "multiplier", "ram", "io"),
                         [](const ::testing::TestParamInfo<std::string> &info) { return info.param; });

// Before routing, the delay of a global clock net is estimated like that of any net, from the
// distance between its driver and each sink, while the clock tree reaches all sinks with similar
// delays. The estimates (used for the clock skew in the timing report after placement) should be
// close to the delays of the routed clock tree.
TEST_F(GateMateTimingArcTest, global_clock_estimate_matches_routed_tree)
{
    NetInfo *clk = global("clk");
    for (int i = 0; i < 40; i++)
        output(stringf("q%d", i), dff(stringf("ff%d", i), input(stringf("d%d", i)), clk)->getPort(id_Q));
    implement();
    NetInfo *glb = ctx->nets.at(ctx->id("clk$glb")).get();
    std::map<std::string, delay_t> estimate;
    for (auto &usr : glb->users)
        estimate[usr.cell->name.str(ctx) + "." + usr.port.str(ctx)] = ctx->getNetinfoRouteDelay(glb, usr);
    impl->preRoute(); // routes the clock nets
    ASSERT_FALSE(glb->wires.empty());
    for (auto &usr : glb->users) {
        std::string sink = usr.cell->name.str(ctx) + "." + usr.port.str(ctx);
        EXPECT_NEAR(estimate.at(sink), ctx->getNetinfoRouteDelay(glb, usr), 1000) << sink;
    }
}
