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
#include <boost/algorithm/string.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <tuple>
#include <vector>
#include "command.h"
#include "netlist_eval.h"
#include "uarch/gatemate/pack.h"
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"
#include "uarch/gatemate/extra_data.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {
// log_crit_paths() in common/kernel/timing_log.cc keeps a function-local static lambda that
// captures the Context of the first critical path report printed in the process, and uses it for
// every later report. Router2 prints such a report at the end of every run, so with one Context
// per test the second routed test would use a deleted Context. Printing the first report with a
// Context that is never deleted works around this. Temporary, see known_bugs.h.
void pin_critical_path_log_context()
{
    static Context *pinned = nullptr;
    if (pinned)
        return;
    ArchArgs args;
    args.device = "CCGM1A1";
    pinned = new Context(args);
    TimingResult empty;
    pinned->log_timing_results(empty, false, false, true, false);
}
} // namespace

void GateMateTest::SetUp()
{
    init_share_dirname();
    pin_critical_path_log_context();
    init_context();
}

void GateMateTest::TearDown()
{
    delete ctx;
    ctx = nullptr;
}

void GateMateTest::init_context(const std::string &device, const std::vector<std::string> &vopts)
{
    delete ctx;
    ctx = nullptr;
    impl = nullptr;
    this->device = device;
    this->vopts = vopts;
    chipArgs = ArchArgs();
    chipArgs.device = device;
    chipArgs.vopts = vopts;
    ctx = new Context(chipArgs);
    ctx->uarch->init(ctx);
    ctx->late_init();
    impl = (GateMateImpl *)(ctx->uarch.get());

    // Defaults normally set up by the command line handler
    ctx->rngseed(1);
    ctx->settings[ctx->id("target_freq")] = std::to_string(100e6);
    ctx->settings[ctx->id("timing_driven")] = true;
    ctx->settings[ctx->id("auto_freq")] = false;
    ctx->settings[ctx->id("placer")] = Arch::defaultPlacer;
    ctx->settings[ctx->id("router")] = Arch::defaultRouter;
    ctx->settings[ctx->id("seed")] = Property(ctx->rngstate, 64);
    ctx->settings[ctx->id("placerHeap/alpha")] = std::to_string(0.1);
    ctx->settings[ctx->id("placerHeap/beta")] = std::to_string(0.9);
    ctx->settings[ctx->id("placerHeap/criticalityExponent")] = std::to_string(2);
    ctx->settings[ctx->id("placerHeap/timingWeight")] = std::to_string(10);
}

NetInfo *GateMateTest::create_net(const std::string &name) { return ctx->createNet(ctx->id(name)); }

void GateMateTest::connect_input(CellInfo *cell, IdString port, NetInfo *net)
{
    if (!cell->ports.count(port))
        cell->addInput(port);
    cell->connectPort(port, net);
}

NetInfo *GateMateTest::connect_output(CellInfo *cell, IdString port, const std::string &net_name)
{
    if (!cell->ports.count(port))
        cell->addOutput(port);
    NetInfo *net = create_net(net_name.empty() ? stringf("%s$%s", cell->name.c_str(ctx), port.c_str(ctx)) : net_name);
    cell->connectPort(port, net);
    return net;
}

NetInfo *GateMateTest::const_net(bool value)
{
    IdString name = ctx->id(value ? "$test$vcc" : "$test$gnd");
    if (ctx->nets.count(name))
        return ctx->nets.at(name).get();
    CellInfo *drv = ctx->createCell(ctx->id(value ? "$test$vcc_drv" : "$test$gnd_drv"), ctx->id(value ? "VCC" : "GND"));
    drv->addOutput(id_Y);
    NetInfo *net = ctx->createNet(name);
    drv->connectPort(id_Y, net);
    return net;
}

NetInfo *GateMateTest::vcc() { return const_net(true); }

NetInfo *GateMateTest::gnd() { return const_net(false); }

NetInfo *GateMateTest::input(const std::string &name)
{
    CellInfo *ibuf = create_cell_ptr(id_CC_IBUF, name);
    NetInfo *net = create_net(name + "$y");
    ibuf->connectPort(id_Y, net);
    return net;
}

CellInfo *GateMateTest::output(const std::string &name, NetInfo *src)
{
    CellInfo *obuf = create_cell_ptr(id_CC_OBUF, name);
    obuf->connectPort(id_A, src);
    return obuf;
}

CellInfo *GateMateTest::tristate_output(const std::string &name, NetInfo *src, NetInfo *t)
{
    CellInfo *obuf = create_cell_ptr(id_CC_TOBUF, name);
    obuf->connectPort(id_A, src);
    obuf->connectPort(id_T, t);
    return obuf;
}

NetInfo *GateMateTest::bidirectional(const std::string &name, NetInfo *src, NetInfo *t)
{
    CellInfo *iobuf = create_cell_ptr(id_CC_IOBUF, name);
    iobuf->connectPort(id_A, src);
    iobuf->connectPort(id_T, t);
    NetInfo *y = create_net(name + "$y");
    iobuf->connectPort(id_Y, y);
    return y;
}

std::pair<NetInfo *, NetInfo *> GateMateTest::iddr(const std::string &name, NetInfo *d, NetInfo *clk, bool clk_inv)
{
    CellInfo *cell = create_cell_ptr(id_CC_IDDR, name);
    cell->params[id_CLK_INV] = Property(clk_inv, 1);
    cell->connectPort(id_D, d);
    cell->connectPort(id_CLK, clk);
    NetInfo *q0 = create_net(name + "$q0"), *q1 = create_net(name + "$q1");
    cell->connectPort(id_Q0, q0);
    cell->connectPort(id_Q1, q1);
    return {q0, q1};
}

NetInfo *GateMateTest::oddr(const std::string &name, NetInfo *d0, NetInfo *d1, NetInfo *clk, NetInfo *ddr,
                            bool clk_inv)
{
    CellInfo *cell = create_cell_ptr(id_CC_ODDR, name);
    cell->params[id_CLK_INV] = Property(clk_inv, 1);
    cell->connectPort(id_D0, d0);
    cell->connectPort(id_D1, d1);
    cell->connectPort(id_CLK, clk);
    cell->connectPort(id_DDR, ddr);
    NetInfo *q = create_net(name + "$q");
    cell->connectPort(id_Q, q);
    return q;
}

NetInfo *GateMateTest::lut1(const std::string &name, int init, NetInfo *i0)
{
    CellInfo *lut = create_cell_ptr(id_CC_LUT1, name);
    lut->params[id_INIT] = Property(init, 2);
    lut->connectPort(id_I0, i0);
    NetInfo *o = create_net(name + "$o");
    lut->connectPort(id_O, o);
    return o;
}

NetInfo *GateMateTest::lut2(const std::string &name, int init, NetInfo *i0, NetInfo *i1)
{
    CellInfo *lut = create_cell_ptr(id_CC_LUT2, name);
    lut->params[id_INIT] = Property(init, 4);
    lut->connectPort(id_I0, i0);
    lut->connectPort(id_I1, i1);
    NetInfo *o = create_net(name + "$o");
    lut->connectPort(id_O, o);
    return o;
}

NetInfo *GateMateTest::l2t4(const std::string &name, int init_l00, int init_l01, int init_l10, NetInfo *i0,
                            NetInfo *i1, NetInfo *i2, NetInfo *i3)
{
    CellInfo *lut = create_cell_ptr(id_CC_L2T4, name);
    lut->params[id_INIT_L00] = Property(init_l00, 4);
    lut->params[id_INIT_L01] = Property(init_l01, 4);
    lut->params[id_INIT_L10] = Property(init_l10, 4);
    lut->connectPort(id_I0, i0);
    lut->connectPort(id_I1, i1);
    lut->connectPort(id_I2, i2);
    lut->connectPort(id_I3, i3);
    NetInfo *o = create_net(name + "$o");
    lut->connectPort(id_O, o);
    return o;
}

NetInfo *GateMateTest::mx2(const std::string &name, NetInfo *d0, NetInfo *d1, NetInfo *s0)
{
    CellInfo *mux = create_cell_ptr(id_CC_MX2, name);
    mux->connectPort(id_D0, d0);
    mux->connectPort(id_D1, d1);
    mux->connectPort(id_S0, s0);
    NetInfo *y = create_net(name + "$y");
    mux->connectPort(id_Y, y);
    return y;
}

NetInfo *GateMateTest::mx4(const std::string &name, NetInfo *d0, NetInfo *d1, NetInfo *d2, NetInfo *d3, NetInfo *s0,
                           NetInfo *s1)
{
    CellInfo *mux = create_cell_ptr(id_CC_MX4, name);
    mux->connectPort(id_D0, d0);
    mux->connectPort(id_D1, d1);
    mux->connectPort(id_D2, d2);
    mux->connectPort(id_D3, d3);
    mux->connectPort(id_S0, s0);
    mux->connectPort(id_S1, s1);
    NetInfo *y = create_net(name + "$y");
    mux->connectPort(id_Y, y);
    return y;
}

CellInfo *GateMateTest::dff(const std::string &name, NetInfo *d, NetInfo *clk, NetInfo *en, NetInfo *sr)
{
    CellInfo *ff = create_cell_ptr(id_CC_DFF, name);
    ff->connectPort(id_D, d);
    ff->connectPort(id_CLK, clk);
    ff->connectPort(id_EN, en ? en : vcc());
    ff->connectPort(id_SR, sr ? sr : gnd());
    create_net(name + "$q");
    ff->connectPort(id_Q, ctx->nets.at(ctx->id(name + "$q")).get());
    return ff;
}

CellInfo *GateMateTest::dlt(const std::string &name, NetInfo *d, NetInfo *g, NetInfo *sr)
{
    CellInfo *latch = create_cell_ptr(id_CC_DLT, name);
    latch->connectPort(id_D, d);
    latch->connectPort(id_G, g);
    latch->connectPort(id_SR, sr ? sr : gnd());
    latch->connectPort(id_Q, create_net(name + "$q"));
    return latch;
}

std::pair<NetInfo *, NetInfo *> GateMateTest::addf(const std::string &name, NetInfo *a, NetInfo *b, NetInfo *ci)
{
    CellInfo *add = create_cell_ptr(id_CC_ADDF, name);
    add->connectPort(id_A, a);
    add->connectPort(id_B, b);
    add->connectPort(id_CI, ci);
    NetInfo *s = create_net(name + "$s");
    NetInfo *co = create_net(name + "$co");
    add->connectPort(id_S, s);
    add->connectPort(id_CO, co);
    return {s, co};
}

NetInfo *GateMateTest::global_clock(const std::string &name)
{
    CellInfo *bufg = create_cell_ptr(id_CC_BUFG, name + "_bufg");
    bufg->connectPort(id_I, input(name));
    NetInfo *clk = create_net(name + "$glb");
    bufg->connectPort(id_O, clk);
    return clk;
}

NetInfo *GateMateTest::top_input(const std::string &name)
{
    NetInfo *pad = create_net(name + "$pad");
    CellInfo *port = ctx->createCell(ctx->id(name), ctx->id("$nextpnr_ibuf"));
    port->addOutput(id_O);
    port->connectPort(id_O, pad);
    PortInfo &pi = ctx->ports[ctx->id(name)];
    pi.name = ctx->id(name);
    pi.type = PORT_IN;
    pi.net = pad;
    CellInfo *ibuf = create_cell_ptr(id_CC_IBUF, name + "$iopad");
    ibuf->connectPort(id_I, pad);
    NetInfo *y = create_net(name + "$y");
    ibuf->connectPort(id_Y, y);
    return y;
}

CellInfo *GateMateTest::top_output(const std::string &name, NetInfo *src)
{
    CellInfo *obuf = create_cell_ptr(id_CC_OBUF, name + "$iopad");
    obuf->connectPort(id_A, src);
    NetInfo *pad = create_net(name + "$pad");
    obuf->connectPort(id_O, pad);
    CellInfo *port = ctx->createCell(ctx->id(name), ctx->id("$nextpnr_obuf"));
    port->addInput(id_I);
    port->connectPort(id_I, pad);
    PortInfo &pi = ctx->ports[ctx->id(name)];
    pi.name = ctx->id(name);
    pi.type = PORT_OUT;
    pi.net = pad;
    return obuf;
}

std::string GateMateTest::write_temp_file(const std::string &filename, const std::string &content)
{
    const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string test = stringf("%s.%s", info->test_suite_name(), info->name());
    for (auto &c : test)
        if (!isalnum(static_cast<unsigned char>(c)))
            c = '_';
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "nextpnr-gatemate-tests" / test;
    std::filesystem::create_directories(dir);
    std::filesystem::path path = dir / filename;
    std::ofstream out(path);
    out << content;
    return path.string();
}

void GateMateTest::init_with_ccf(const std::string &ccf, const std::vector<std::string> &vopts,
                                 const std::string &device)
{
    std::vector<std::string> options = vopts;
    options.push_back("--ccf=" + write_temp_file("test.ccf", ccf));
    init_context(device, options);
}

std::string GateMateTest::package_pin(const std::string &buffer)
{
    CellInfo *cell = ctx->cells.at(ctx->id(buffer)).get();
    if (cell->bel == BelId())
        return "";
    // The pad bel is the IOSEL in the same tile (the GPIO cell is at z = 0)
    Loc loc = ctx->getBelLocation(cell->bel);
    for (auto bel : ctx->getBelsByTile(loc.x, loc.y)) {
        const PadInfoPOD *pad = ctx->get_bel_package_pin(bel);
        if (pad)
            return IdString(pad->package_pin).str(ctx);
    }
    return "";
}

std::vector<std::string> GateMateTest::package_pins(bool clock_capable_only)
{
    std::vector<std::string> pins;
    for (const auto &pad : ctx->package_info->pads) {
        std::string name = IdString(pad.package_pin).str(ctx);
        if (clock_capable_only && pad.flags == 0)
            continue;
        if (std::find(pins.begin(), pins.end(), name) == pins.end())
            pins.push_back(name);
    }
    std::sort(pins.begin(), pins.end());
    return pins;
}

void GateMateTest::pack() { ctx->pack(); }

::testing::AssertionResult GateMateTest::packs()
{
    LogCapture errors(LogLevel::ERROR_MSG);
    try {
        ctx->pack();
    } catch (const log_execution_error_exception &) {
        return ::testing::AssertionFailure() << "packing failed: " << errors.str();
    } catch (const std::exception &e) {
        return ::testing::AssertionFailure() << "packing failed with exception: " << e.what();
    }
    return ::testing::AssertionSuccess();
}

std::vector<GateMateTest::MissingArc> GateMateTest::missing_timing_arcs(uint64_t seed)
{
    NetlistEvaluator ev(ctx);
    std::mt19937_64 rng(seed);
    std::vector<MissingArc> missing;
    auto constant = [&](const NetInfo *net) -> Tri {
        if (net->name == ctx->id("$PACKER_VCC"))
            return Tri::T;
        if (net->name == ctx->id("$PACKER_GND"))
            return Tri::F;
        const CellInfo *drv = net->driver.cell;
        if (drv && drv->type.in(ctx->id("GND"), ctx->id("VCC")))
            return drv->type == ctx->id("VCC") ? Tri::T : Tri::F;
        return Tri::X;
    };
    for (auto &c : ctx->cells) {
        const CellInfo *cell = c.second.get();
        if (!cell->type.in(id_CPE_L2T4, id_CPE_ADDF, id_CPE_ADDF2, id_CPE_MX4, id_CPE_CPLINES, id_CPE_RAMI,
                           id_CPE_RAMO, id_CPE_RAMIO, id_IOSEL))
            continue;
        // The input nets, with the ports they are connected to
        std::map<const NetInfo *, std::vector<IdString>> inputs;
        std::unordered_map<const NetInfo *, Tri> values;
        for (auto &port : cell->ports) {
            const NetInfo *net = port.second.net;
            if (port.second.type != PORT_IN || net == nullptr)
                continue;
            Tri value = constant(net);
            if (value != Tri::X)
                values[net] = value;
            else
                inputs[net].push_back(port.first);
        }
        for (auto &out : cell->ports) {
            if (out.second.type != PORT_OUT || out.second.net == nullptr)
                continue;
            for (auto &in : inputs) {
                bool depends = false;
                try {
                    for (int k = 0; k < 32 && !depends; k++) {
                        for (auto &other : inputs)
                            values[other.first] = (rng() & 1) ? Tri::T : Tri::F;
                        values[in.first] = Tri::F;
                        Tri v0 = ev.eval_cell(cell, out.first, values);
                        values[in.first] = Tri::T;
                        Tri v1 = ev.eval_cell(cell, out.first, values);
                        depends = v0 != Tri::X && v1 != Tri::X && v0 != v1;
                    }
                } catch (const EvalUnsupported &) {
                    break; // no model for this output
                }
                if (!depends)
                    continue;
                // One of the ports the net is connected to needs an arc to the output
                bool arc = false;
                int count = 0;
                for (IdString port : in.second) {
                    DelayQuad delay;
                    arc = arc || (ctx->getPortTimingClass(cell, port, count) == TMG_COMB_INPUT &&
                                  ctx->getPortTimingClass(cell, out.first, count) == TMG_COMB_OUTPUT &&
                                  ctx->getCellDelay(cell, port, out.first, delay));
                }
                if (!arc)
                    missing.push_back(MissingArc{cell, in.second, out.first});
            }
        }
    }
    return missing;
}

std::string GateMateTest::describe_arcs(const std::vector<MissingArc> &arcs)
{
    std::map<std::string, std::string> kinds; // kind -> example cell
    for (auto &arc : arcs) {
        std::string ports;
        for (IdString port : arc.from)
            ports += (ports.empty() ? "" : "/") + port.str(ctx);
        kinds.emplace(stringf("%s %s -> %s", arc.cell->type.c_str(ctx), ports.c_str(), arc.to.c_str(ctx)),
                      arc.cell->name.str(ctx));
    }
    std::string s;
    for (auto &k : kinds)
        s += "\n    " + k.first + " (e.g. " + k.second + ")";
    return s;
}

int GateMateTest::cpe_half_bound(std::map<std::string, int> *by_type)
{
    auto lut_like = [&](const CellInfo *cell) { return cell->type.in(id_CC_LUT1, id_CC_LUT2, id_CC_L2T4, id_CC_MX2); };
    // The only user of the output of a cell, nullptr if there is none or several
    auto only_user = [&](const CellInfo *cell) -> const PortRef * {
        for (auto &p : cell->ports)
            if (p.second.type == PORT_OUT && p.second.net)
                return p.second.net->users.entries() == 1 ? &*p.second.net->users.begin() : nullptr;
        return nullptr;
    };
    // The adder continuing a carry chain
    auto next_bit = [&](const CellInfo *cy) -> const CellInfo * {
        const NetInfo *co = cy->getPort(id_CO);
        if (co)
            for (auto &usr : co->users)
                if (usr.cell->type == id_CC_ADDF && usr.port == id_CI)
                    return usr.cell;
        return nullptr;
    };
    // Clocked from a global clock (from a clock buffer)
    auto global = [&](const CellInfo *cell) {
        const NetInfo *clk = cell->getPort(id_CLK);
        return clk && clk->driver.cell && clk->driver.cell->type == id_CC_BUFG;
    };
    // A register the packer moves into the IOSEL of a pad (as asked for with FF_IBF / FF_OBF)
    auto io_register = [&](const CellInfo *ff) {
        const NetInfo *d = ff->getPort(id_D), *q = ff->getPort(id_Q);
        if (d && d->driver.cell && d->driver.cell->type == id_CC_IBUF && d->users.entries() == 1 &&
            bool_or_default(d->driver.cell->params, id_FF_IBF))
            return true;
        if (q && q->users.entries() == 1) {
            const PortRef &usr = *q->users.begin();
            return usr.cell->type == id_CC_OBUF && bool_or_default(usr.cell->params, id_FF_OBF);
        }
        return false;
    };
    std::set<const NetInfo *> ddr_selects;
    auto halves = [&](const CellInfo *cell) -> int {
        IdString t = cell->type;
        if (lut_like(cell)) {
            // A LUT that only copies its input is removed; one that only drives a register shares
            // its half (counted with the register), one that only drives an output pad or a RAM
            // pin goes into that pin's CPE (counted with the pin)
            if (t == id_CC_LUT1 && int_or_default(cell->params, id_INIT, 0) == 0b10)
                return 0;
            const PortRef *usr = only_user(cell);
            if (!usr)
                return 1;
            IdString ut = usr->cell->type;
            bool absorbed = (ut.in(id_CC_DFF, id_CC_DLT) && usr->port == id_D) ||
                            (ut.in(id_CC_OBUF, id_CC_TOBUF, id_CC_IOBUF) && usr->port.in(id_A, id_T)) ||
                            ut.in(id_CC_BRAM_20K, id_CC_BRAM_40K, id_CC_FIFO_40K);
            return absorbed ? 0 : 1;
        }
        if (t.in(id_CC_DFF, id_CC_IDDR)) {
            // An IO register needs no CPE, except to bring in a clock from the fabric
            bool in_iosel = t == id_CC_IDDR || io_register(cell);
            return in_iosel && global(cell) ? 0 : 1;
        }
        if (t.in(id_CC_DLT, id_CC_OBUF, id_CC_LVDS_OBUF, id_CC_BUFG))
            return 1;
        if (t.in(id_CC_L2T5, id_CC_MX4, id_CC_TOBUF, id_CC_IOBUF, id_CC_LVDS_TOBUF, id_CC_LVDS_IOBUF))
            return 2;
        if (t == id_CC_ODDR) {
            // Both data inputs and a fabric clock; the select (shared by the DDR outputs of a
            // bank) is counted once
            ddr_selects.insert(cell->getPort(id_DDR));
            return 2 + (global(cell) ? 0 : 1);
        }
        if (t == id_CC_ADDF) {
            // Two bits per CPE: each bit takes one half, except that a bit whose carry out is also
            // used elsewhere cannot share its CPE with the next bit
            const NetInfo *co = cell->getPort(id_CO);
            int n = next_bit(cell) && co->users.entries() > 1 ? 2 : 1;
            // The first bit of a chain: the CPE bringing the carry in, the one taking the carry
            // out if it is used (both again every 64 bits, where the chain is split), and one half
            // for an odd length
            const NetInfo *ci = cell->getPort(id_CI);
            if (!ci || !ci->driver.cell || ci->driver.cell->type != id_CC_ADDF) {
                int length = 1;
                const CellInfo *last = cell;
                for (const CellInfo *cy = next_bit(cell); cy; cy = next_bit(cy)) {
                    length++;
                    last = cy;
                }
                const NetInfo *last_co = last->getPort(id_CO);
                int splits = (length + 63) / 64;
                n += 2 * splits + 2 * (splits - 1) + (last_co && last_co->users.entries() > 0 ? 2 : 0) + 1;
            }
            return n;
        }
        if (t.in(id_CC_BRAM_20K, id_CC_BRAM_40K, id_CC_FIFO_40K)) {
            // One half per pin, the data input and output of a bit sharing one
            std::set<std::string> pins;
            for (auto &p : cell->ports) {
                if (!p.second.net)
                    continue;
                std::string name = p.first.str(ctx);
                for (const char *data : {"_DI[", "_DO["})
                    if (name.find(data) != std::string::npos)
                        name.replace(name.find(data), 4, "_D[");
                pins.insert(name);
            }
            return int(pins.size());
        }
        if (!t.in(id_CC_IBUF, id_CC_LVDS_IBUF, ctx->id("VCC"), ctx->id("GND"), ctx->id("$nextpnr_ibuf"),
                  ctx->id("$nextpnr_obuf"), ctx->id("$nextpnr_iobuf")))
            ADD_FAILURE() << "no CPE bound for cell type " << t.str(ctx);
        return 0;
    };
    int bound = 2; // constant drivers
    for (auto &c : ctx->cells) {
        int n = halves(c.second.get());
        bound += n;
        if (by_type)
            (*by_type)[c.second->type.str(ctx)] += n;
    }
    if (by_type) {
        (*by_type)["constants"] += 2;
        (*by_type)["DDR selects"] += int(ddr_selects.size());
    }
    return bound + int(ddr_selects.size());
}

int GateMateTest::cpe_halves_used()
{
    std::set<std::tuple<int, int, int>> halves;
    for (auto &c : ctx->cells) {
        const CellInfo *cell = c.second.get();
        if (cell->bel == BelId())
            continue;
        IdString bt = ctx->getBelType(cell->bel);
        Loc loc = ctx->getBelLocation(cell->bel);
        if (bt.in(id_CPE_LT_U, id_CPE_FF_U, id_CPE_RAMIO_U))
            halves.insert({loc.x, loc.y, 0});
        else if (bt.in(id_CPE_LT_L, id_CPE_FF_L, id_CPE_RAMIO_L))
            halves.insert({loc.x, loc.y, 1});
        else if (loc.z == CPE_LT_FULL_Z && boost::starts_with(bt.str(ctx), "CPE")) {
            halves.insert({loc.x, loc.y, 0});
            halves.insert({loc.x, loc.y, 1});
        }
    }
    return int(halves.size());
}

::testing::AssertionResult GateMateTest::place_ram_pair(const std::string &lower, const std::string &upper)
{
    CellInfo *rams[2] = {ctx->cells.at(ctx->id(lower)).get(), ctx->cells.at(ctx->id(upper)).get()};
    for (CellInfo *ram : rams)
        if (ram->type != id_RAM_HALF)
            return ::testing::AssertionFailure() << ram->name.str(ctx) << " is no RAM_HALF but " << ram->type.str(ctx);
    // The placement validity check needs the cell information set up before placement
    impl->prePlace();
    for (BelId bel : ctx->getBels()) {
        if (ctx->getBelType(bel) != id_RAM)
            continue;
        Loc loc = ctx->getBelLocation(bel);
        BelId bels[2] = {bel, ctx->getBelByLocation(Loc(loc.x, loc.y + 8, RAM_HALF_L_Z))};
        if (bels[1] == BelId())
            continue;
        std::vector<std::pair<CellInfo *, BelId>> placement[2];
        bool free = true;
        for (int i = 0; i < 2 && free; i++) {
            free = ctx->getClusterPlacement(rams[i]->cluster, bels[i], placement[i]);
            for (auto &p : placement[i])
                free = free && ctx->checkBelAvail(p.second);
        }
        if (!free)
            continue;
        for (int i = 0; i < 2; i++)
            for (auto &p : placement[i])
                ctx->bindBel(p.second, p.first, STRENGTH_USER);
        // As the placer would: both halves need the same configuration
        if (ctx->isBelLocationValid(bels[0]) && ctx->isBelLocationValid(bels[1]))
            return ::testing::AssertionSuccess();
        for (int i = 0; i < 2; i++)
            for (auto &p : placement[i])
                ctx->unbindBel(p.second);
    }
    return ::testing::AssertionFailure() << "no free RAM block where " << lower << " and " << upper << " can share";
}

void GateMateTest::place_and_route()
{
    ctx->pack();
    ctx->place();
    ctx->route();
}

::testing::AssertionResult GateMateTest::implementation_matches_reference(const std::function<void()> &build,
                                                                          int steps, uint64_t seed,
                                                                          const std::function<void()> &after_pack)
{
    std::string dev = device;
    std::vector<std::string> opts = vopts;
    init_context(dev, opts);
    build();
    std::unique_ptr<Context> reference(ctx);
    ctx = nullptr;
    init_context(dev, opts);
    build();
    {
        LogCapture errors(LogLevel::ERROR_MSG);
        try {
            ctx->pack();
            if (after_pack)
                after_pack();
            ctx->place();
        } catch (const log_execution_error_exception &) {
            return ::testing::AssertionFailure() << "implementation failed: " << errors.str();
        } catch (const std::exception &e) {
            return ::testing::AssertionFailure() << "implementation failed with exception: " << e.what();
        }
    }
    return outputs_match(reference.get(), ctx, steps, seed);
}

LogCapture::LogCapture(LogLevel level) { log_streams.push_back(std::make_pair(&stream, level)); }

LogCapture::~LogCapture()
{
    for (auto it = log_streams.begin(); it != log_streams.end(); ++it) {
        if (it->first == &stream) {
            log_streams.erase(it);
            break;
        }
    }
}

CellInfo *GateMateTest::create_cell_ptr(IdString type, std::string name)
{
    CellInfo *cell = ctx->createCell(ctx->id(name), type);

    auto add_port = [&](const IdString id, PortType dir) {
        cell->ports[id].name = id;
        cell->ports[id].type = dir;
    };
    switch (type.index) {
    case id_CC_IBUF.index:
        add_port(id_I, PORT_IN);
        add_port(id_Y, PORT_OUT);
        break;
    case id_CC_OBUF.index:
        add_port(id_A, PORT_IN);
        add_port(id_O, PORT_OUT);
        break;
    case id_CC_TOBUF.index:
        add_port(id_A, PORT_IN);
        add_port(id_T, PORT_IN);
        add_port(id_O, PORT_OUT);
        break;
    case id_CC_IOBUF.index:
        add_port(id_A, PORT_IN);
        add_port(id_T, PORT_IN);
        add_port(id_Y, PORT_OUT);
        add_port(id_IO, PORT_INOUT);
        break;
    case id_CC_LVDS_IBUF.index:
        add_port(id_I_P, PORT_IN);
        add_port(id_I_N, PORT_IN);
        add_port(id_Y, PORT_OUT);
        break;
    case id_CC_LVDS_OBUF.index:
        add_port(id_A, PORT_IN);
        add_port(id_O_P, PORT_OUT);
        add_port(id_O_N, PORT_OUT);
        break;
    case id_CC_LVDS_TOBUF.index:
        add_port(id_A, PORT_IN);
        add_port(id_T, PORT_IN);
        add_port(id_O_P, PORT_OUT);
        add_port(id_O_N, PORT_OUT);
        break;
    case id_CC_LVDS_IOBUF.index:
        add_port(id_A, PORT_IN);
        add_port(id_T, PORT_IN);
        add_port(id_Y, PORT_OUT);
        add_port(id_IO_P, PORT_INOUT);
        add_port(id_IO_N, PORT_INOUT);
        break;
    case id_CC_IDDR.index:
        add_port(id_D, PORT_IN);
        add_port(id_CLK, PORT_IN);
        add_port(id_Q0, PORT_OUT);
        add_port(id_Q1, PORT_OUT);
        break;
    case id_CC_ODDR.index:
        add_port(id_D0, PORT_IN);
        add_port(id_D1, PORT_IN);
        add_port(id_CLK, PORT_IN);
        add_port(id_DDR, PORT_IN);
        add_port(id_Q, PORT_OUT);
        break;
    case id_CC_DFF.index:
        add_port(id_D, PORT_IN);
        add_port(id_CLK, PORT_IN);
        add_port(id_EN, PORT_IN);
        add_port(id_SR, PORT_IN);
        add_port(id_Q, PORT_OUT);
        break;
    case id_CC_DLT.index:
        add_port(id_D, PORT_IN);
        add_port(id_G, PORT_IN);
        add_port(id_SR, PORT_IN);
        add_port(id_Q, PORT_OUT);
        break;
    case id_CC_L2T4.index:
        add_port(id_I0, PORT_IN);
        add_port(id_I1, PORT_IN);
        add_port(id_I2, PORT_IN);
        add_port(id_I3, PORT_IN);
        add_port(id_O, PORT_OUT);
        break;
    case id_CC_L2T5.index:
        add_port(id_I0, PORT_IN);
        add_port(id_I1, PORT_IN);
        add_port(id_I2, PORT_IN);
        add_port(id_I3, PORT_IN);
        add_port(id_I4, PORT_IN);
        add_port(id_O, PORT_OUT);
        break;
    case id_CC_LUT1.index:
        add_port(id_I0, PORT_IN);
        add_port(id_O, PORT_OUT);
        break;
    case id_CC_LUT2.index:
        add_port(id_I0, PORT_IN);
        add_port(id_I1, PORT_IN);
        add_port(id_O, PORT_OUT);
        break;
    case id_CC_MX2.index:
        add_port(id_D0, PORT_IN);
        add_port(id_D1, PORT_IN);
        add_port(id_S0, PORT_IN);
        add_port(id_Y, PORT_OUT);
        break;
    case id_CC_MX4.index:
        add_port(id_D0, PORT_IN);
        add_port(id_D1, PORT_IN);
        add_port(id_D2, PORT_IN);
        add_port(id_D3, PORT_IN);
        add_port(id_S0, PORT_IN);
        add_port(id_S1, PORT_IN);
        add_port(id_Y, PORT_OUT);
        break;
    case id_CC_ADDF.index:
        add_port(id_A, PORT_IN);
        add_port(id_B, PORT_IN);
        add_port(id_CI, PORT_IN);
        add_port(id_CO, PORT_OUT);
        add_port(id_S, PORT_OUT);
        break;
    case id_CC_BUFG.index:
        add_port(id_I, PORT_IN);
        add_port(id_O, PORT_OUT);
        break;
    case id_CC_USR_RSTN.index:
        add_port(id_USR_RSTN, PORT_OUT);
        break;
    case id_CC_PLL_ADV.index:
        add_port(id_USR_SEL_A_B, PORT_IN);
        [[fallthrough]];
    case id_CC_PLL.index:
        add_port(id_CLK_REF, PORT_IN);
        add_port(id_USR_CLK_REF, PORT_IN);
        add_port(id_CLK_FEEDBACK, PORT_IN);
        add_port(id_USR_LOCKED_STDY_RST, PORT_IN);
        add_port(id_USR_PLL_LOCKED_STDY, PORT_OUT);
        add_port(id_USR_PLL_LOCKED, PORT_OUT);
        add_port(id_CLK0, PORT_OUT);
        add_port(id_CLK90, PORT_OUT);
        add_port(id_CLK180, PORT_OUT);
        add_port(id_CLK270, PORT_OUT);
        add_port(id_CLK_REF_OUT, PORT_OUT);
        break;
    case id_CC_CFG_CTRL.index:
        for (int i = 0; i < 8; i++)
            add_port(ctx->idf("DATA[%d]", i), PORT_IN);
        add_port(id_CLK, PORT_IN);
        add_port(id_EN, PORT_IN);
        add_port(id_RECFG, PORT_IN);
        add_port(id_VALID, PORT_IN);
        break;

    default:
        log_error("Trying to create unknown cell type %s.\n", type.c_str(ctx));
        break;
    }
    return cell;
}

void GateMateTest::direct_connect(CellInfo *o_cell, IdString o_port, CellInfo *i_cell, IdString i_port)
{
    NetInfo *net = ctx->createNet(ctx->idf("%s_%s", o_cell->name.c_str(ctx), o_port.c_str(ctx)));
    o_cell->connectPort(o_port, net);
    i_cell->connectPort(i_port, net);
}

NEXTPNR_NAMESPACE_END
