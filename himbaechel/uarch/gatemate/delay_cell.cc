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

// Delay cells: one CPE configured as a pure delay, for lengthening connections with hold violations after routing
// (docs/timing_constraints.md, "Delay cells"). The data enters on IN6 and passes the input multiplexer C_I3 (at 0)
// into the first-level LUT2 L02, whose table passes its input B on (0b1100). From there, by tap:
//
//  0: L11 (passes L02, 0b1010) and L20 (passes L11, 0b1100) to COMB1OUT, out on OUT1 (the plain LUT tree,
//     timing__ARBLUT_IN6_OUT1);
//  1: the carry select of the two-bit adder (C_FUNCTION = C_ADDF2): COMB2OUT = L10 ^ (L11 ? carry in : L02), with L10
//     and L11 constant 0, so COMB2OUT = L02, out on OUT2 (timing__ADDF2Y1_IN6_OUT2, the longest arc the timing
//     database has from a CPE input to an output);
//  2: tap 1, and OUT2 (C_O2 = 3) further through the carry line select into COUTX (C_SELX = 1, C_SEL_C = 0, C_CX_I =
//     1) and back through CPOUT1 (C_HORIZ = 1, C_C_P = 1), out on OUT1 (the router's C_O1 = 2): the arc of tap 1 plus
//     the pips OUT2 -> COUTX (comb12_COUTX), COUTX -> CP_CARRY_int (CPOUT_cpout) and on to CPOUT1 (cpout_OUT1).
//
// No other input of the CPE reaches the output in any tap (checked with the CPE model of prjpeppercorn, cpe.v, for all
// values of IN1..IN8, the six CP inputs and the CP-line selects other nets may set): the unrouted CPE inputs carry
// whatever their input multiplexers select, and only IN6 is routed. A route to IN6 that arrives inverted is undone by
// L02's table, as for any LUT input (bitstream.cc).
//
// The cell (type CPE_DELAY) takes the whole CPE (bel CPE_LT_FULL); its ports are the bel pins: IN6, and the output
// of the tap (OUT1 = COMBOUT1_int, OUT2 = COMBOUT2_int, CPOUT1 = CPOUT1_int), which tells the tap. Its parameters are
// CPE configuration words, written as they are (no parameter of its own, so that the bitstream and a JSON round trip
// need nothing else).

#include "gatemate.h"
#include "pack.h"

#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {

// The bels of a CPE, by Z, that a delay cell keeps free (all but its own, CPE_LT_FULL, and the CP lines the routing
// of other nets may use, see delay_cell_fits)
const int cpe_bels[] = {CPE_LT_U_Z,    CPE_LT_L_Z,    CPE_FF_U_Z, CPE_FF_L_Z,    CPE_RAMIO_U_Z,
                        CPE_RAMIO_L_Z, CPE_COMP_Z,    CPE_CPLINES_Z, CPE_LT_FULL_Z, CPE_BRIDGE_Z};

// The CP-line selects tap 2 fixes (PipMask bits and their values)
constexpr uint32_t tap2_mask = PipMask::C_SELX | PipMask::C_SEL_C | PipMask::C_CX_I | PipMask::C_HORIZ | PipMask::C_C_P;
constexpr uint32_t tap2_data = PipMask::C_SELX | PipMask::C_CX_I | PipMask::C_HORIZ | PipMask::C_C_P;

} // namespace

int GateMateImpl::delay_cell_tap(const CellInfo *cell) const
{
    if (!is_delay_cell(cell))
        return -1;
    for (int tap = 0; tap < delay_cell_tap_count; tap++)
        if (cell->ports.count(delay_cell_output(tap)))
            return tap;
    return -1;
}

IdString GateMateImpl::delay_cell_output(int tap) const
{
    switch (tap) {
    case 0:
        return id_OUT1;
    case 1:
        return id_OUT2;
    case 2:
        return id_CPOUT1;
    default:
        return IdString();
    }
}

bool GateMateImpl::delay_cell_delay(int tap, DelayQuad &delay) const
{
    // The entries of the timing database whose sum the tap's arc is (see the head of this file); the same the router
    // takes for these pips when it routes a net that way
    static const std::vector<std::vector<const char *>> parts = {
            {"timing__ARBLUT_IN6_OUT1"},
            {"timing__ADDF2Y1_IN6_OUT2"},
            {"timing__ADDF2X_IN6_OUT2", "timing_comb12_COUTX", "timing_CPOUT_cpout", "timing_cpout_OUT1"},
    };
    delay = DelayQuad(0);
    if (tap < 0 || tap >= int(parts.size()))
        return false;
    for (const char *name : parts.at(tap)) {
        DelayQuad part;
        if (!get_delay_from_tmg_db(ctx->id(name), part))
            return false;
        delay += part;
    }
    return true;
}

std::vector<DelayQuad> GateMateImpl::delay_cell_taps() const
{
    std::vector<DelayQuad> taps;
    for (int tap = 0; tap < delay_cell_tap_count; tap++) {
        DelayQuad delay;
        if (!delay_cell_delay(tap, delay))
            log_error("The timing database has no delay for delay cell tap %d.\n", tap);
        taps.push_back(delay);
    }
    return taps;
}

void GateMateImpl::configure_delay_cell(CellInfo *cell, int tap) const
{
    NPNR_ASSERT(tap >= 0 && tap < delay_cell_tap_count);
    for (int t = 0; t < delay_cell_tap_count; t++)
        if (t != tap && cell->ports.count(delay_cell_output(t)))
            cell->ports.erase(delay_cell_output(t));
    if (!cell->ports.count(id_IN6))
        cell->addInput(id_IN6);
    if (!cell->ports.count(delay_cell_output(tap)))
        cell->addOutput(delay_cell_output(tap));
    cell->cell_bel_pins.clear();
    cell->cell_bel_pins[id_IN6] = {id_IN6};
    cell->cell_bel_pins[delay_cell_output(tap)] = {delay_cell_output(tap)};

    cell->params.clear();
    auto set = [&](IdString name, int value, int width) { cell->params[name] = Property(value, width); };
    // All tables written, so that nothing depends on defaults
    set(id_INIT_L00, LUT_ZERO, 4);
    set(id_INIT_L01, LUT_ZERO, 4);
    set(id_INIT_L02, LUT_D1, 4); // IN6 (through C_I3 = 0)
    set(id_INIT_L03, LUT_ZERO, 4);
    set(id_INIT_L10, LUT_ZERO, 4);
    if (tap == 0) {
        set(id_C_FUNCTION, 0, 3);
        set(id_INIT_L11, LUT_D0, 4); // L02
        set(id_INIT_L20, LUT_D1, 4); // L11
    } else {
        // COMB2OUT = L10 ^ (L11 ? carry in : L02): L10 and L11 0
        set(id_C_FUNCTION, C_ADDF2, 3);
        set(id_INIT_L11, LUT_ZERO, 4);
        set(id_INIT_L20, LUT_ZERO, 4);
    }
    if (tap == 2) {
        // OUT2 = COMB2OUT into the routing multiplexer of COUTX ({C_SEL_C, C_SELX} = 01), COUTX taking it (C_CX_I),
        // CPOUT1 = COUTX (C_HORIZ: the X pair, C_C_P: the carry)
        set(id_C_O2, 3, 2);
        set(id_C_SELX, 1, 1);
        set(id_C_SEL_C, 0, 1);
        set(id_C_CX_I, 1, 1);
        set(id_C_HORIZ, 1, 1);
        set(id_C_C_P, 1, 1);
    }
}

bool GateMateImpl::delay_cell_fits(BelId bel, int tap) const
{
    if (bel == BelId() || tap < 0 || tap >= delay_cell_tap_count)
        return false;
    Loc loc = ctx->getBelLocation(bel);
    BelId full = ctx->getBelByLocation(Loc(loc.x, loc.y, CPE_LT_FULL_Z));
    if (full == BelId() || ctx->getBelType(full) != id_CPE_LT_FULL)
        return false;
    // Imported partitions and the CPEs a partition keeps free
    if (partition_tiles.count(full.tile) || (as_partition && partition_io_tiles.count(full.tile)))
        return false;
    for (int z : cpe_bels) {
        BelId b = ctx->getBelByLocation(Loc(loc.x, loc.y, z));
        if (b != BelId() && (ctx->getBoundBelCell(b) != nullptr || !ctx->checkBelAvail(b)))
            return false;
    }
    // The wires of the path inside the CPE, which the cell drives itself or (tap 2) has run unrouted, and the input
    // multiplexer in front of IN6 (another net can pass through it to the input multiplexers of other planes)
    std::vector<const char *> wires = {"IM.P06.Y", "CPE.IN6", "CPE.IN6_int"};
    if (tap == 0)
        wires.insert(wires.end(), {"CPE.COMBOUT1_int", "CPE.OUT1_int", "CPE.OUT1"});
    else
        wires.insert(wires.end(), {"CPE.COMBOUT2_int", "CPE.OUT2_int", "CPE.OUT2"});
    if (tap == 2)
        wires.insert(wires.end(), {"CPE.OUT2_IN_int", "CPE.CX_OUT", "CPE.CX_VAL", "CPE.COUTX", "CPE.CP_CARRY_int",
                                   "CPE.CPOUT1_int", "CPE.OUT1_int", "CPE.OUT1"});
    IdString tile_name = ctx->getBelName(full)[0];
    for (const char *name : wires) {
        WireId w = ctx->getWireByName(IdStringList::concat(tile_name, ctx->id(name)));
        if (w == WireId() || !ctx->checkWireAvail(w))
            return false;
    }
    // The routing of other nets through the CPE: no bridge (the cell marks the CPE used), and for tap 2 no select of
    // the carry lines set to another value than the cell sets it
    const auto &pips = chip_tile_info(ctx->chip_info, full.tile).pips;
    for (int i = 0; i < pips.ssize(); i++) {
        PipId pip(full.tile, i);
        const auto &extra = *pip_extra_data(pip);
        if (extra.type != PipExtra::PIP_EXTRA_MUX || (!(extra.flags & MUX_ROUTING) && !(extra.resource & tap2_mask)))
            continue;
        if (ctx->getBoundPipNet(pip) == nullptr)
            continue;
        if (extra.flags & MUX_ROUTING)
            return false;
        if (tap == 2 && (extra.value ? extra.resource : 0) != (tap2_data & extra.resource))
            return false;
    }
    return true;
}

namespace {

// Unbind the wires of `net` that lead only to `sink`, from the sink up to where the routing branches, and append them
// (wire, pip, strength) to `released`, sink first
void release_branch(Context *ctx, NetInfo *net, WireId sink,
                    std::vector<std::tuple<WireId, PipId, PlaceStrength>> &released)
{
    if (sink == WireId() || !net->wires.count(sink))
        return;
    dict<WireId, int> fanout;
    for (auto &w : net->wires)
        if (w.second.pip != PipId())
            fanout[ctx->getPipSrcWire(w.second.pip)]++;
    WireId src = ctx->getNetinfoSourceWire(net);
    WireId w = sink;
    while (w != WireId() && w != src && net->wires.count(w) && fanout[w] == 0) {
        PipMap pm = net->wires.at(w);
        released.emplace_back(w, pm.pip, pm.strength);
        WireId up = pm.pip != PipId() ? ctx->getPipSrcWire(pm.pip) : WireId();
        ctx->unbindWire(w);
        if (up != WireId())
            fanout[up]--;
        w = up;
    }
}

} // namespace

CellInfo *GateMateImpl::insert_delay_cell(NetInfo *net, const PortRef &user, BelId bel, int tap)
{
    if (!net || !user.cell || user.cell->getPort(user.port) != net || !delay_cell_fits(bel, tap))
        return nullptr;
    Loc loc = ctx->getBelLocation(bel);
    BelId full = ctx->getBelByLocation(Loc(loc.x, loc.y, CPE_LT_FULL_Z));

    // The route to that sink is released (other users sharing its sink wire keep it)
    DelayCellUndo undo;
    undo.net = net->name;
    for (WireId sink : ctx->getNetinfoSinkWires(net, user)) {
        bool shared = false;
        for (auto &other : net->users)
            if (other.cell != user.cell || other.port != user.port)
                for (WireId w : ctx->getNetinfoSinkWires(net, other))
                    shared |= w == sink;
        if (!shared)
            release_branch(ctx, net, sink, undo.released);
    }

    // (no '$' in front: the JSON frontend takes such a cell for one yosys named and renames it on loading)
    IdString name;
    do
        name = ctx->idf("hold$delay$%d", delay_cell_serial++);
    while (ctx->cells.count(name) || ctx->nets.count(ctx->idf("%s$out", name.c_str(ctx))));
    CellInfo *cell = ctx->createCell(name, delay_cell_type);
    configure_delay_cell(cell, tap);
    ctx->bindBel(full, cell, STRENGTH_FIXED);
    NetInfo *out = ctx->createNet(ctx->idf("%s$out", name.c_str(ctx)));

    CellInfo *sink_cell = user.cell;
    IdString sink_port = user.port;
    sink_cell->disconnectPort(sink_port);
    cell->connectPort(id_IN6, net);
    cell->connectPort(delay_cell_output(tap), out);
    sink_cell->connectPort(sink_port, out);
    undo.sink_cell = sink_cell->name;
    undo.sink_port = sink_port;

    // A timing exception that names the old net as an end point (-to [get_nets ...]) finds the end point by the net
    // on its pin, which is the cell's output net now: it names that net too (taken out again by remove_delay_cell).
    // Start points, cells and pins stay as they were: the cell is neither.
    for (auto &exc : ctx->timing_exceptions) {
        bool named = false;
        for (auto &point : exc.to) {
            if (point.type != TimingPathPoint::NET)
                continue;
            IdString n = point.name;
            if (!ctx->nets.count(n) && ctx->net_aliases.count(n))
                n = ctx->net_aliases.at(n);
            named |= n == net->name;
        }
        if (named)
            exc.to.push_back(TimingPathPoint{TimingPathPoint::NET, out->name, IdString()});
    }

    delay_cell_undo[name] = std::move(undo);
    any_delay_cells = true;
    // The CPE's configuration for the router (its selects, the CPE marked used); recomputed with the routing when
    // next needed
    pip_masks_ready = false;
    return cell;
}

bool GateMateImpl::remove_delay_cell(CellInfo *cell)
{
    if (!is_delay_cell(cell))
        return false;
    IdString name = cell->name;
    NetInfo *in = cell->getPort(id_IN6);
    int tap = delay_cell_tap(cell);
    NetInfo *out = tap >= 0 ? cell->getPort(delay_cell_output(tap)) : nullptr;

    // The input's branch to IN6 and all routing of the output net go
    std::vector<std::tuple<WireId, PipId, PlaceStrength>> dropped;
    if (in)
        for (WireId sink : ctx->getNetinfoSinkWires(in, PortRef{cell, id_IN6}))
            release_branch(ctx, in, sink, dropped);
    if (out) {
        std::vector<WireId> wires;
        for (auto &w : out->wires)
            wires.push_back(w.first);
        for (WireId w : wires)
            ctx->unbindWire(w);
    }

    // The sinks back on the input net
    std::vector<PortRef> sinks;
    if (out)
        for (auto &user : out->users)
            sinks.push_back(user);
    for (auto &sink : sinks)
        sink.cell->disconnectPort(sink.port);
    if (in)
        cell->disconnectPort(id_IN6);
    if (out)
        cell->disconnectPort(delay_cell_output(tap));
    for (auto &sink : sinks)
        if (in)
            sink.cell->connectPort(sink.port, in);

    // The timing exceptions naming the output net (added by insert_delay_cell)
    if (out)
        for (auto &exc : ctx->timing_exceptions)
            exc.to.erase(std::remove_if(exc.to.begin(), exc.to.end(),
                                        [&](const TimingPathPoint &p) {
                                            return p.type == TimingPathPoint::NET && p.name == out->name;
                                        }),
                         exc.to.end());

    if (cell->bel != BelId())
        ctx->unbindBel(cell->bel);
    if (out) {
        IdString out_name = out->name;
        ctx->nets.erase(out_name);
    }
    ctx->cells.erase(name);
    pip_masks_ready = false;

    // The route to the sink as it was before the insertion, where it is still free (this run inserted the cell)
    bool restored = false;
    auto found = delay_cell_undo.find(name);
    if (found != delay_cell_undo.end()) {
        auto &undo = found->second;
        NetInfo *net = ctx->nets.count(undo.net) ? ctx->nets.at(undo.net).get() : nullptr;
        bool free = net != nullptr && net == in && !undo.released.empty();
        for (auto &[wire, pip, strength] : undo.released) {
            NetInfo *held = ctx->getBoundWireNet(wire);
            if (held != nullptr && held != net)
                free = false;
            if (free && pip != PipId() && !ctx->getBoundWireNet(wire) &&
                (!ctx->checkPipAvailForNet(pip, net) || resource_taken(pip)))
                free = false;
        }
        if (free) {
            // (top down: the sink first in the list)
            for (auto it = undo.released.rbegin(); it != undo.released.rend(); ++it) {
                auto &[wire, pip, strength] = *it;
                if (ctx->getBoundWireNet(wire) == net)
                    continue;
                if (pip == PipId())
                    ctx->bindWire(wire, net, strength);
                else
                    ctx->bindPip(pip, net, strength);
            }
            restored = true;
        }
        delay_cell_undo.erase(found);
    }
    return restored;
}

NEXTPNR_NAMESPACE_END
