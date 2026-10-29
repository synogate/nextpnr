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
#include <boost/range/adaptor/reversed.hpp>

#include "gatemate.h"

#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

NEXTPNR_NAMESPACE_BEGIN

delay_t GateMateImpl::estimateDelay(WireId src, WireId dst) const
{
    int sx, sy, dx, dy;
    int d2d = 0;
    if (tile_extra_data(src.tile)->die != tile_extra_data(dst.tile)->die)
        d2d += 2000;
    if (table_route) {
        delay_t delay = lookahead.estimate(src, dst);
        if (delay >= 0)
            return delay + d2d;
    }
    tile_xy(ctx->chip_info, src.tile, sx, sy);
    tile_xy(ctx->chip_info, dst.tile, dx, dy);

    return 100 + 100 * (std::abs(dx - sx) + std::abs(dy - sy)) + d2d;
}

delay_t GateMateImpl::predictDelay(BelId src_bel, IdString src_pin, BelId dst_bel, IdString dst_pin) const
{
    int d2d = 0;
    if (tile_extra_data(src_bel.tile)->die != tile_extra_data(dst_bel.tile)->die)
        d2d += 2000;
    if (table_place) {
        WireId src = ctx->getBelPinWire(src_bel, src_pin), dst = ctx->getBelPinWire(dst_bel, dst_pin);
        delay_t delay = src != WireId() && dst != WireId() ? lookahead.estimate(src, dst) : -1;
        if (delay >= 0)
            return delay + d2d;
    }

    Loc src_loc = ctx->getBelLocation(src_bel), dst_loc = ctx->getBelLocation(dst_bel);
    return 100 + 100 * (std::abs(dst_loc.x - src_loc.x) + std::abs(dst_loc.y - src_loc.y)) + d2d;
}

bool GateMateImpl::get_delay_from_tmg_db(IdString id, DelayQuad &delay) const
{
    auto fnd = timing.find(id);
    if (fnd != timing.end()) {
        delay = DelayQuad(fnd->second->delay.fast_min, fnd->second->delay.fast_max, fnd->second->delay.slow_min,
                          fnd->second->delay.slow_max);
        return true;
    }
    return false;
}

void GateMateImpl::get_setuphold_from_tmg_db(IdString id_setup, IdString id_hold, DelayPair &setup,
                                             DelayPair &hold) const
{
    auto fnd = timing.find(id_setup);
    if (fnd != timing.end()) {
        setup.min_delay = fnd->second->delay.fast_min;
        setup.max_delay = fnd->second->delay.fast_max;
    }
    fnd = timing.find(id_hold);
    if (fnd != timing.end()) {
        hold.min_delay = fnd->second->delay.fast_min;
        hold.max_delay = fnd->second->delay.fast_max;
    }
}

void GateMateImpl::get_setuphold_from_tmg_db(IdString id_setuphold, DelayPair &setup, DelayPair &hold) const
{
    auto fnd = timing.find(id_setuphold);
    if (fnd != timing.end()) {
        setup.min_delay = fnd->second->delay.fast_min;
        setup.max_delay = fnd->second->delay.fast_max;
        hold.min_delay = fnd->second->delay.slow_min;
        hold.max_delay = fnd->second->delay.slow_max;
    }
}

namespace {
bool iosel_ff(const CellInfo *cell, IdString path_ff) { return bool_or_default(cell->params, path_ff); }

bool iosel_in_ff(const CellInfo *cell) { return iosel_ff(cell, id_IN1_FF) || iosel_ff(cell, id_IN2_FF); }

bool iosel_out_ff(const CellInfo *cell) { return iosel_ff(cell, id_OUT1_FF) || iosel_ff(cell, id_OUT2_FF); }

bool iosel_fabric_clock(const CellInfo *cell, bool out)
{
    return bool_or_default(cell->params, out ? id_SEL_OUT_CLOCK : id_SEL_IN_CLOCK);
}

int iosel_clock_index(const CellInfo *cell, bool out)
{
    if (iosel_fabric_clock(cell, out))
        return -1; // when FFs are clocked from the fabric (OUT4)
    return int_or_default(cell->params, out ? id_OUT_CLOCK : id_IN_CLOCK, 0);
}

IdString iosel_clock_port(const CellInfo *cell, bool out)
{
    static const IdString clocks[4] = {id_CLOCK1, id_CLOCK2, id_CLOCK3, id_CLOCK4};
    int idx = iosel_clock_index(cell, out);
    return idx < 0 ? id_OUT4 : clocks[idx & 3];
}

// OUT4 carries a clock instead of data
bool iosel_out4_is_clock(const CellInfo *cell)
{
    return (iosel_in_ff(cell) && iosel_fabric_clock(cell, false)) ||
           (iosel_out_ff(cell) && iosel_fabric_clock(cell, true));
}

ClockEdge iosel_edge(const CellInfo *cell, IdString inv_param)
{
    return bool_or_default(cell->params, inv_param) ? FALLING_EDGE : RISING_EDGE;
}
} // namespace

bool GateMateImpl::getCellDelay(const CellInfo *cell, IdString fromPort, IdString toPort, DelayQuad &delay) const
{
    delay = DelayQuad{0};
    static dict<IdString, IdString> map_upper = {
            {id_D0_00, id_IN1}, {id_D1_00, id_IN2}, {id_D0_01, id_IN3},    {id_D1_01, id_IN4},    {id_D0_10, id_IN1},
            {id_D1_10, id_IN3}, {id_OUT, id_OUT2},  {id_RAM_O, id_RAM_O2}, {id_RAM_I, id_RAM_I2}, {id_CPOUT, id_CPOUT2},
    };
    static dict<IdString, IdString> map_lower = {
            {id_D0_00, id_IN5},    {id_D1_00, id_IN6}, {id_D0_01, id_IN7}, {id_D1_01, id_IN8},    {id_D0_10, id_IN5},
            {id_D1_10, id_IN7},    {id_D0_02, id_IN5}, {id_D1_02, id_IN6}, {id_D0_03, id_IN7},    {id_D1_03, id_IN8},
            {id_D0_11, id_IN5},    {id_D1_11, id_IN7}, {id_OUT, id_OUT1},  {id_RAM_O, id_RAM_O1}, {id_RAM_I, id_RAM_I1},
            {id_CPOUT, id_CPOUT1}, {id_IN1, id_IN5},   {id_IN2, id_IN6},   {id_IN3, id_IN7},      {id_IN4, id_IN8},
    };

    static dict<IdString, IdString> map_both = {
            {id_D0_00, id_IN1}, {id_D1_00, id_IN2}, {id_D0_01, id_IN3}, {id_D1_01, id_IN4},
            {id_D0_10, id_IN1}, {id_D1_10, id_IN3}, {id_D0_02, id_IN5}, {id_D1_02, id_IN6},
            {id_D0_03, id_IN7}, {id_D1_03, id_IN8}, {id_D0_11, id_IN5}, {id_D1_11, id_IN7},
    };

    int z = (cell->bel != BelId()) ? (ctx->getBelLocation(cell->bel).z % 2) : 0;
    if (cell->type.in(id_CPE_L2T4, id_CPE_LT_L, id_CPE_LT_U)) {
        // Internal connection from upper L2T4 into the lower half
        if (fromPort == id_COMBIN && toPort == id_OUT)
            return true;
        IdString fp = fromPort, tp = toPort;
        if (z == 0) {
            if (map_upper.count(fp))
                fp = map_upper[fp];
            if (map_upper.count(tp))
                tp = map_upper[tp];
        } else {
            if (map_lower.count(fp))
                fp = map_lower[fp];
            if (map_lower.count(tp))
                tp = map_lower[tp];
        }
        if (timing_fixes) {
            // Bug 31: the database has no arcs from the CPE's internal inputs. From COMBIN (the upper
            // half's result) the upper half's own arc covers the path: its inputs reach the lower
            // output (ARBLUT_IN1_OUT1) about as fast as its own (ARBLUT_IN1_OUT2). From CINY1, a
            // carry in, the adder's carry-in-to-sum delay.
            if (fp == id_COMBIN)
                return true;
            if (fp == id_CINY1)
                return get_delay_from_tmg_db(ctx->idf("timing__ADDF2Y1_CINY1_%s", tp.c_str(ctx)), delay);
        }
        return get_delay_from_tmg_db(ctx->idf("timing__ARBLUT_%s_%s", fp.c_str(ctx), tp.c_str(ctx)), delay);
    } else if (cell->type.in(id_CPE_ADDF, id_CPE_ADDF2)) {
        IdString fp = fromPort, tp = toPort;
        if (map_both.count(fp))
            fp = map_both[fp];
        if (map_both.count(tp))
            tp = map_both[tp];
        // The carry arc: ..._COUTY1, in older databases ..._COUTY
        if (fp == id_CINY1 && tp == id_COUTY1)
            return get_delay_from_tmg_db(ctx->id("timing__ADDF2Y1_CINY1_COUTY1"), delay) ||
                   get_delay_from_tmg_db(ctx->id("timing__ADDF2Y1_CINY1_COUTY"), delay);
        if (timing_fixes) {
            // Bug 37: the first bit's sum does not depend on the second bit's operands
            if (cell->type == id_CPE_ADDF2 && tp == id_OUT1 && fp.in(id_IN1, id_IN2, id_IN3, id_IN4))
                return false;
            // Bug 33: a carry from the middle of a chain leaving through CPOUT, estimated as the carry
            // out plus the CPOUT driver
            if (tp.in(id_CPOUT1, id_CPOUT2)) {
                if (!get_delay_from_tmg_db(fp == id_CINY1 ? ctx->id("timing__ADDF2Y1_CINY1_COUTY")
                                                          : ctx->idf("timing__ADDF2Y1_%s_COUTY1", fp.c_str(ctx)),
                                           delay))
                    return false;
                DelayQuad cpout;
                if (get_delay_from_tmg_db(ctx->id("timing_CPOUT_cpout"), cpout))
                    delay += cpout;
                return true;
            }
        }
        return get_delay_from_tmg_db(ctx->idf("timing__ADDF2Y1_%s_%s", fp.c_str(ctx), tp.c_str(ctx)), delay);
    } else if (cell->type.in(id_CPE_MX4)) {
        IdString fp = fromPort, tp = toPort;
        if (map_both.count(fp))
            fp = map_both[fp];
        if (map_both.count(tp))
            tp = map_both[tp];
        return get_delay_from_tmg_db(ctx->idf("timing__MX4A_%s_%s", fp.c_str(ctx), tp.c_str(ctx)), delay);
    } else if (cell->type.in(id_CPE_MULT)) {
        if (toPort == id_CPOUT1)
            return get_delay_from_tmg_db(ctx->id("timing_cpout_OUT1"), delay);
        if (toPort == id_CPOUT2)
            return get_delay_from_tmg_db(ctx->id("timing_cpout_OUT2"), delay);
        return get_delay_from_tmg_db(ctx->idf("timing__MULT_%s_%s", fromPort.c_str(ctx), toPort.c_str(ctx)), delay);
    } else if (cell->type.in(id_CPE_FF, id_CPE_LATCH, id_CPE_FF_L, id_CPE_FF_U)) {
        return false;
    } else if (cell->type.in(id_CPE_CPLINES)) {
        if (timing_fixes) {
            // Bug 28: from a LUT output (comb12_*), from the comparator (comb12_compout_*), carries
            // passing through (_ROUTING_*); zero where the database has nothing
            IdString name = fromPort.in(id_OUT1, id_OUT2) ? ctx->idf("timing_comb12_%s", toPort.c_str(ctx))
                            : fromPort == id_COMPOUT
                                    ? ctx->idf("timing_comb12_compout_%s", toPort.c_str(ctx))
                                    : ctx->idf("timing__ROUTING_%s_%s", fromPort.c_str(ctx), toPort.c_str(ctx));
            if (!get_delay_from_tmg_db(name, delay))
                delay = DelayQuad{0};
        }
        return true;
    } else if (cell->type.in(id_CPE_BRIDGE)) {
        return true;
    } else if (cell->type.in(id_CPE_COMP)) {
        return get_delay_from_tmg_db(fromPort == id_COMB1 ? id_timing_comb1_compout : id_timing_comb2_compout, delay);
    } else if (cell->type.in(id_CPE_RAMI, id_CPE_RAMO, id_CPE_RAMIO)) {
        if (fromPort == id_I && toPort == id_RAM_O)
            return get_delay_from_tmg_db(z ? id_timing_comb12_RAM_O1 : id_timing_comb12_RAM_O2, delay);
        if (fromPort == id_RAM_I && toPort == id_OUT) {
            // About 0.1 ns, not zero
            get_delay_from_tmg_db(ctx->id(z ? "timing__ARBLUT_RAM_I1_OUT1" : "timing__ARBLUT_RAM_I2_OUT2"), delay);
            return true;
        }
        return false;
    } else if (cell->type.in(id_CPE_IBUF, id_CPE_OBUF, id_CPE_TOBUF, id_CPE_IOBUF)) {
        if (fromPort == id_A && toPort.in(id_O, id_IO))
            return get_delay_from_tmg_db(id_timing_del_OBF, delay);
        if (fromPort == id_T && toPort.in(id_O, id_IO))
            return get_delay_from_tmg_db(id_timing_del_TOBF_ctrl, delay);
        if (fromPort.in(id_I, id_IO) && toPort == id_Y)
            return get_delay_from_tmg_db(id_timing_del_IBF, delay);
        return false;
    } else if (cell->type.in(id_CPE_LVDS_IBUF, id_CPE_LVDS_OBUF, id_CPE_LVDS_TOBUF, id_CPE_LVDS_IOBUF)) {
        if (fromPort == id_A && toPort.in(id_O_P, id_O_N, id_IO_P, id_IO_N))
            return get_delay_from_tmg_db(id_timing_del_LVDS_OBF, delay);
        if (fromPort == id_T && toPort.in(id_O_P, id_O_N, id_IO_P, id_IO_N))
            return get_delay_from_tmg_db(id_timing_del_LVDS_TOBF_ctrl, delay);
        if (fromPort.in(id_I_P, id_I_N, id_IO_P, id_IO_N) && toPort == id_Y)
            return get_delay_from_tmg_db(id_timing_del_LVDS_IBF, delay);
        return false;
    } else if (cell->type.in(id_IOSEL)) {
        bool output = bool_or_default(cell->params, id_OUT_SIGNAL);
        bool enable = bool_or_default(cell->params, id_OE_ENABLE);
        IdString o_s = bool_or_default(cell->params, id_OUT23_14_SEL)
                               ? (bool_or_default(cell->params, id_OUT2_3) ? id_OUT3 : id_OUT2)
                               : (bool_or_default(cell->params, id_OUT1_4) ? id_OUT4 : id_OUT1);
        int oe = int_or_default(cell->params, id_OE_SIGNAL);
        IdString oe_s = (oe & 2) ? ((oe & 1) ? id_OUT4 : id_OUT3) : ((oe & 1) ? id_OUT2 : id_OUT1);
        if (!fromPort.in(id_OUT1, id_OUT2, id_OUT3, id_OUT4, id_GPIO_IN))
            return false;
        if (fromPort == id_OUT4 && iosel_out4_is_clock(cell))
            return false;
        // Registered paths are modelled via getPortClockingInfo
        if ((toPort == id_IN1 && iosel_ff(cell, id_IN1_FF)) || (toPort == id_IN2 && iosel_ff(cell, id_IN2_FF)))
            return false;
        if (toPort == id_GPIO_OUT && iosel_out_ff(cell))
            return false;
        if (output && fromPort != o_s)
            return false;
        if (enable && fromPort != oe_s)
            return false;
        return get_delay_from_tmg_db(ctx->idf("timing_io_sel_%s_%s", fromPort.c_str(ctx), toPort.c_str(ctx)), delay);
    } else if (cell->type.in(id_CLKIN)) {
        return get_delay_from_tmg_db(ctx->idf("timing_clkin_%s_%s", fromPort.c_str(ctx), toPort.c_str(ctx)), delay);
    } else if (cell->type.in(id_GLBOUT)) {
        return get_delay_from_tmg_db(ctx->idf("timing_glbout_%s_%s", fromPort.c_str(ctx), toPort.c_str(ctx)), delay);
    } else if (cell->type.in(id_RAM, id_RAM_HALF)) {
        return false;
    } else if (cell->type.in(id_PLL)) {
        if (fromPort.in(id_CLK_REF, id_USR_CLK_REF) && toPort.in(id_CLK0, id_CLK90, id_CLK180, id_CLK270))
            return get_delay_from_tmg_db(ctx->id("timing_pll_clk_ref_i_clk_core0_o"), delay);
        return false;
    }
    return false;
}

TimingPortClass GateMateImpl::getPortTimingClass(const CellInfo *cell, IdString port, int &clockInfoCount) const
{
    auto disconnected = [cell](IdString p) { return !cell->ports.count(p) || cell->ports.at(p).net == nullptr; };
    clockInfoCount = 0;
    if (cell->type.in(id_CPE_L2T4, id_CPE_LT_L, id_CPE_LT_U)) {
        if (port.in(id_IN1, id_IN2, id_IN3, id_IN4, id_COMBIN, id_CINY1, id_CINY2, id_CINX, id_PINX, id_PINY1, id_PINY2,
                    id_D0_00, id_D1_00, id_D0_01, id_D1_01, id_D0_10, id_D1_10, id_D0_02, id_D1_02, id_D0_03, id_D1_03,
                    id_D0_11, id_D1_11))
            return TMG_COMB_INPUT;
        if (port == id_OUT && disconnected(id_IN1) && disconnected(id_IN2) && disconnected(id_IN3) &&
            disconnected(id_IN4) && disconnected(id_D0_00) && disconnected(id_D1_00) && disconnected(id_D0_01) &&
            disconnected(id_D1_01) && disconnected(id_D0_10) && disconnected(id_D1_10) && disconnected(id_D0_02) &&
            disconnected(id_D1_02) && disconnected(id_D0_03) && disconnected(id_D1_03) && disconnected(id_D0_11) &&
            disconnected(id_D1_11) && disconnected(id_COMBIN) && disconnected(id_CINX) && disconnected(id_PINX) &&
            disconnected(id_CINY1) && disconnected(id_PINY1) && disconnected(id_CINY2) && disconnected(id_PINY2))
            return TMG_IGNORE; // LUT with no inputs is a constant
        if (port.in(id_OUT))
            return TMG_COMB_OUTPUT;
        return TMG_IGNORE;
    } else if (cell->type.in(id_CPE_ADDF, id_CPE_ADDF2)) {
        if (port.in(id_IN1, id_IN2, id_IN3, id_IN4, id_IN5, id_IN6, id_IN7, id_IN8, id_CINX, id_CINY1, id_D0_00,
                    id_D1_00, id_D0_01, id_D1_01, id_D0_10, id_D1_10, id_D0_02, id_D1_02, id_D0_03, id_D1_03, id_D0_11,
                    id_D1_11))
            return TMG_COMB_INPUT;
        if (port.in(id_OUT1, id_OUT2, id_COUTY1) || (timing_fixes && port.in(id_CPOUT1, id_CPOUT2))) // bug 33
            return TMG_COMB_OUTPUT;
        return TMG_IGNORE;
    } else if (cell->type.in(id_CPE_MX4)) {
        if (port.in(id_IN1, id_IN2, id_IN3, id_IN4, id_IN5, id_IN6, id_IN7, id_IN8, id_D0_00, id_D1_00, id_D0_01,
                    id_D1_01, id_D0_10, id_D1_10, id_D0_02, id_D1_02, id_D0_03, id_D1_03, id_D0_11, id_D1_11))
            return TMG_COMB_INPUT;
        if (port.in(id_OUT1))
            return TMG_COMB_OUTPUT;
        return TMG_IGNORE;
    } else if (cell->type.in(id_CPE_MULT)) {
        if (port.in(id_IN1, id_IN5, id_IN8, id_CINX, id_PINX, id_CINY1, id_CINY2, id_PINY1, id_PINY2))
            return TMG_COMB_INPUT;
        return TMG_COMB_OUTPUT;
    } else if (cell->type.in(id_CPE_CPLINES)) {
        if (port.in(id_OUT1, id_OUT2, id_COMPOUT, id_CINX, id_PINX, id_CINY1, id_PINY1, id_CINY2, id_PINY2))
            return TMG_COMB_INPUT;
        return TMG_COMB_OUTPUT;
    } else if (cell->type.in(id_CPE_BRIDGE)) {
        if (port.in(id_MUXOUT))
            return TMG_COMB_OUTPUT;
        return TMG_COMB_INPUT;
    } else if (cell->type.in(id_CPE_FF, id_CPE_FF_L, id_CPE_FF_U, id_CPE_LATCH)) {
        if (port.in(id_CLK_INT))
            return TMG_CLOCK_INPUT;
        clockInfoCount = 1;
        if (port == id_DOUT)
            return TMG_REGISTER_OUTPUT;
        // DIN, EN and SR
        return TMG_REGISTER_INPUT;
    } else if (cell->type.in(id_CPE_RAMI, id_CPE_RAMO, id_CPE_RAMIO, id_CPE_RAMIO_U, id_CPE_RAMIO_L)) {
        if (port.in(id_I, id_RAM_I))
            return TMG_COMB_INPUT;
        // Bug 29: OUT, the fabric side output, is one too
        if (port.in(id_O, id_RAM_O) || (timing_fixes && port == id_OUT))
            return TMG_COMB_OUTPUT;
        return TMG_IGNORE;
    } else if (cell->type.in(id_CPE_COMP)) {
        if (port.in(id_COMB1, id_COMB2))
            return TMG_COMB_INPUT;
        return TMG_COMB_OUTPUT;
    } else if (cell->type.in(id_CPE_IBUF, id_CPE_OBUF, id_CPE_TOBUF, id_CPE_IOBUF, id_CPE_LVDS_IBUF,
                              id_CPE_LVDS_OBUF, id_CPE_LVDS_TOBUF, id_CPE_LVDS_IOBUF)) {
        // Paths end at the buffer's data and enable inputs from the fabric (the pad side has no sink
        // to end at) and begin at its output into the fabric
        if (port.in(id_A, id_T))
            return TMG_ENDPOINT;
        if (port.in(id_Y))
            return TMG_STARTPOINT;
        return TMG_IGNORE;
    } else if (cell->type.in(id_IOSEL)) {
        bool in1_ff = iosel_ff(cell, id_IN1_FF);
        bool in2_ff = iosel_ff(cell, id_IN2_FF);
        bool out1_ff = iosel_ff(cell, id_OUT1_FF);
        bool out2_ff = iosel_ff(cell, id_OUT2_FF);
        if (port.in(id_CLOCK1, id_CLOCK2, id_CLOCK3, id_CLOCK4))
            return TMG_CLOCK_INPUT;
        if (port == id_OUT4 && iosel_out4_is_clock(cell))
            return TMG_CLOCK_INPUT;
        if (port == id_IN1 && in1_ff) {
            clockInfoCount = 1;
            return TMG_REGISTER_OUTPUT;
        }
        if (port == id_IN2 && in2_ff) {
            clockInfoCount = 1;
            return TMG_REGISTER_OUTPUT;
        }
        if (port == id_GPIO_OUT && (out1_ff || out2_ff)) {
            clockInfoCount = int(out1_ff) + int(out2_ff); // two edges for ODDR
            return TMG_REGISTER_OUTPUT;
        }
        if (port == id_GPIO_IN && (in1_ff || in2_ff)) {
            clockInfoCount = int(in1_ff) + int(in2_ff); // two edges for IDDR
            return TMG_REGISTER_INPUT;
        }
        if ((port == id_OUT1 && out1_ff) || (port == id_OUT2 && out2_ff)) {
            clockInfoCount = 1;
            return TMG_REGISTER_INPUT;
        }
        if (port.in(id_IN1, id_IN2, id_GPIO_EN, id_GPIO_OUT))
            return TMG_COMB_OUTPUT;
        if (port.in(id_OUT1, id_OUT2, id_OUT3, id_OUT4, id_GPIO_IN))
            return TMG_COMB_INPUT;
        return TMG_IGNORE;
    } else if (cell->type.in(id_PLL)) {
        if (port.in(id_CLK_REF, id_USR_CLK_REF))
            return TMG_CLOCK_INPUT;
        if (port.in(id_CLK0, id_CLK90, id_CLK180, id_CLK270))
            return TMG_GEN_CLOCK;
        return TMG_IGNORE;
    } else if (cell->type.in(id_CLKIN)) {
        if (port.in(id_CLK0, id_CLK1, id_CLK2, id_CLK3, id_SER_CLK))
            return TMG_CLOCK_INPUT;
        if (port.in(id_CLK_REF0, id_CLK_REF1, id_CLK_REF2, id_CLK_REF3))
            return TMG_GEN_CLOCK;
        return TMG_IGNORE;
    } else if (cell->type.in(id_GLBOUT)) {
        if (port.in(id_CLK0_0, id_CLK90_0, id_CLK180_0, id_CLK270_0, id_CLK_REF_OUT0, id_CLK0_1, id_CLK90_1,
                    id_CLK180_1, id_CLK270_1, id_CLK_REF_OUT1, id_CLK0_2, id_CLK90_2, id_CLK180_2, id_CLK270_2,
                    id_CLK_REF_OUT2, id_CLK0_3, id_CLK90_3, id_CLK180_3, id_CLK270_3, id_CLK_REF_OUT3, id_USR_GLB0,
                    id_USR_GLB1, id_USR_GLB2, id_USR_GLB3))
            return TMG_CLOCK_INPUT;
        if (port.in(id_GLB0, id_GLB1, id_GLB2, id_GLB3))
            return TMG_GEN_CLOCK;
        return TMG_IGNORE;
    } else if (cell->type.in(id_SERDES)) {
        return TMG_IGNORE;
    } else if (cell->type.in(id_USR_RSTN)) {
        return TMG_IGNORE;
    } else if (cell->type.in(id_CFG_CTRL)) {
        if (port.in(id_CLK))
            return TMG_CLOCK_INPUT;
        return TMG_IGNORE;
    } else if (cell->type.in(id_RAM, id_RAM_HALF)) {
        std::string name = port.str(ctx);
        if (boost::starts_with(name, "CLKA[") || boost::starts_with(name, "CLKB[") || boost::starts_with(name, "CLOCK"))
            return TMG_CLOCK_INPUT;
        if (boost::ends_with(name, "_CI") || boost::ends_with(name, "_CO"))
            return TMG_IGNORE;
        if (name[0] == 'F') // Ignore forward and FIFO pins
            return TMG_IGNORE;
        for (auto c : boost::adaptors::reverse(name)) {
            if (std::isdigit(c) || c == 'X' || c == '[' || c == ']')
                continue;
            if (c == 'A' || c == 'B')
                clockInfoCount = 1;
            else
                NPNR_ASSERT_FALSE_STR("bad ram port");
            return (cell->ports.at(port).type == PORT_OUT) ? TMG_REGISTER_OUTPUT : TMG_REGISTER_INPUT;
        }
        NPNR_ASSERT_FALSE_STR("no timing type for RAM port '" + port.str(ctx) + "'");
    } else {
        log_error("Cell type '%s' is unsupported (instantiated as '%s').\n", cell->type.c_str(ctx),
                  cell->name.c_str(ctx));
    }
}

IdString clock(uint8_t val, IdString clk1, IdString clk2, IdString clk3, IdString clk4)
{
    switch (val) {
    case 0b00000000:
        return clk1;
    case 0b00000100:
        return clk2;
    case 0b00001000:
        return clk3;
    case 0b00001100:
        return clk4;
    case 0b00100011:
        return id_CLOCK1;
    case 0b00110011:
        return id_CLOCK2;
    case 0b00000011:
        return id_CLOCK3;
    case 0b00010011:
        return id_CLOCK4;
    default:
        return clk1;
    }
}

int GateMateImpl::ram_clock_index(const CellInfo *cell, const RamPinInfo &pin) const
{
    int mode = int_or_default(cell->params, id_RAM_cfg_sram_mode, 0);
    bool split = mode & 0b01;
    bool sdp = mode & 0b10;

    bool domain_b = pin.port_b;
    if (sdp) {
        switch (pin.kind) {
        case RamPinKind::DATA_IN:
        case RamPinKind::BITMASK:
            domain_b = false;
            break;
        case RamPinKind::DATA_OUT:
        case RamPinKind::ECC_STATUS:
            domain_b = true;
            break;
        default:
            break;
        }
    }
    // Native 40K blocks only use the a0/b0 control sets
    return (domain_b ? 2 : 0) + (split ? pin.half : 0);
}

TimingClockingInfo GateMateImpl::getPortClockingInfo(const CellInfo *cell, IdString port, int index) const
{
    TimingClockingInfo info;
    info.setup = DelayPair(0);
    info.hold = DelayPair(0);
    info.clockToQ = DelayQuad(0);
    if (cell->type.in(id_CPE_FF, id_CPE_FF_L, id_CPE_FF_U, id_CPE_LATCH)) {
        bool inverted = int_or_default(cell->params, id_C_CPE_CLK, 0) == 0b01;
        info.edge = inverted ? FALLING_EDGE : RISING_EDGE;
        info.clock_port = id_CLK_INT;
        // Bug 26: the packer renames the enable to EN_INT
        if (port.in(id_DIN, id_EN, id_SR) || (timing_fixes && port == id_EN_INT))
            get_setuphold_from_tmg_db(id_timing_del_Setup_D_L, id_timing_del_Hold_D_L, info.setup, info.hold);
        if (port.in(id_DOUT)) {
            // (registers sit at CPE_FF_U_Z/CPE_FF_L_Z)
            bool is_upper = (cell->bel != BelId()) && (ctx->getBelLocation(cell->bel).z == CPE_FF_U_Z);
            get_delay_from_tmg_db(id_timing__SEQ_CLK_FF1_Q, info.clockToQ);
            DelayQuad delay = DelayQuad{0};
            get_delay_from_tmg_db(is_upper ? id_timing_Q2_OUT2 : id_timing_Q1_OUT1, delay);
            info.clockToQ += delay;
            get_delay_from_tmg_db(id_timing_del_CPE_CP_Q, delay);
            info.clockToQ += delay;
        }
    } else if (cell->type.in(id_IOSEL)) {
        bool out_side = port.in(id_OUT1, id_OUT2, id_GPIO_OUT);
        info.clock_port = iosel_clock_port(cell, out_side);
        int clk_idx = iosel_clock_index(cell, out_side);
        std::vector<std::pair<IdString, IdString>> ffs;
        if (out_side) {
            if (iosel_ff(cell, id_OUT1_FF))
                ffs.emplace_back(id_OUT1_FF, id_INV_OUT1_CLOCK);
            if (iosel_ff(cell, id_OUT2_FF))
                ffs.emplace_back(id_OUT2_FF, id_INV_OUT2_CLOCK);
        } else {
            if (iosel_ff(cell, id_IN1_FF))
                ffs.emplace_back(id_IN1_FF, id_INV_IN1_CLOCK);
            if (iosel_ff(cell, id_IN2_FF))
                ffs.emplace_back(id_IN2_FF, id_INV_IN2_CLOCK);
        }
        IdString inv;
        if (port == id_IN1)
            inv = id_INV_IN1_CLOCK;
        else if (port == id_IN2)
            inv = id_INV_IN2_CLOCK;
        else if (port == id_OUT1)
            inv = id_INV_OUT1_CLOCK;
        else if (port == id_OUT2)
            inv = id_INV_OUT2_CLOCK;
        else if (index < int(ffs.size()))
            inv = ffs.at(index).second;
        info.edge = inv == IdString() ? RISING_EDGE : iosel_edge(cell, inv);

        if (port.in(id_IN1, id_IN2, id_GPIO_OUT)) {
            IdString to = (port == id_GPIO_OUT) ? id_GPIO_OUT : port;
            if (clk_idx < 0 ||
                !get_delay_from_tmg_db(ctx->idf("timing_io_sel_CLOCK%d_%s", clk_idx, to.c_str(ctx)), info.clockToQ))
                get_delay_from_tmg_db(out_side ? id_timing_del_IO_SEL_Q_out : id_timing_del_IO_SEL_Q_in, info.clockToQ);
        } else {
            // use the CPE FF values as an approximation
            get_setuphold_from_tmg_db(id_timing_del_Setup_D_L, id_timing_del_Hold_D_L, info.setup, info.hold);
        }
    } else if (cell->type.in(id_RAM, id_RAM_HALF)) {
        std::string name = port.str(ctx);
        int is_ecc_a = int_or_default(cell->params, id_A_ECC_EN, 0);
        int is_ecc_b = int_or_default(cell->params, id_B_ECC_EN, 0);
        if (boost::starts_with(name, "CLOCK")) {
            if (is_ecc_a || is_ecc_b)
                get_delay_from_tmg_db(id_timing_RAM_ECC_IOPATH_1, info.clockToQ);
            else
                get_delay_from_tmg_db(id_timing_RAM_NOECC_IOPATH_1, info.clockToQ);
        }
        // With output registers, the registered clock-to-out
        // (after placement, the settings of the half the RAM sits in: a0/b0 or a1/b1)
        bool reg_a = int_or_default(cell->params, id_RAM_cfg_a0_set_outputreg, 0) ||
                     int_or_default(cell->params, id_RAM_cfg_a1_set_outputreg, 0);
        bool reg_b = int_or_default(cell->params, id_RAM_cfg_b0_set_outputreg, 0) ||
                     int_or_default(cell->params, id_RAM_cfg_b1_set_outputreg, 0);
        if (boost::starts_with(name, "DOA") && reg_a)
            get_delay_from_tmg_db(ctx->id("timing_RAM_REG_IOPATH_2"), info.clockToQ);
        else if (boost::starts_with(name, "DOA")) {
            if (is_ecc_a)
                get_delay_from_tmg_db(id_timing_RAM_ECC_IOPATH_2, info.clockToQ);
            else
                get_delay_from_tmg_db(id_timing_RAM_NOECC_IOPATH_2, info.clockToQ);
        }
        if (boost::starts_with(name, "DOB") && reg_b)
            get_delay_from_tmg_db(ctx->id("timing_RAM_REG_IOPATH_3"), info.clockToQ);
        else if (boost::starts_with(name, "DOB")) {
            if (is_ecc_b)
                get_delay_from_tmg_db(id_timing_RAM_ECC_IOPATH_3, info.clockToQ);
            else
                get_delay_from_tmg_db(id_timing_RAM_NOECC_IOPATH_3, info.clockToQ);
        }
        if (boost::starts_with(name, "ECC")) {
            if (is_ecc_a || is_ecc_b)
                get_delay_from_tmg_db(id_timing_RAM_ECC_IOPATH_4, info.clockToQ);
            else
                get_delay_from_tmg_db(id_timing_RAM_NOECC_IOPATH_4, info.clockToQ);
        }
        if (boost::starts_with(name, "ADDRA")) {
            if (is_ecc_a)
                get_setuphold_from_tmg_db(id_timing_RAM_ECC_SETUPHOLD_1, info.setup, info.hold);
            else
                get_setuphold_from_tmg_db(id_timing_RAM_NOECC_SETUPHOLD_1, info.setup, info.hold);
        }
        if (boost::starts_with(name, "ADDRB")) {
            if (is_ecc_b)
                get_setuphold_from_tmg_db(id_timing_RAM_ECC_SETUPHOLD_1, info.setup, info.hold);
            else
                get_setuphold_from_tmg_db(id_timing_RAM_NOECC_SETUPHOLD_1, info.setup, info.hold);
        }
        if (boost::starts_with(name, "CLOCK1")) { // TODO CLOCK1 || CLOCK2 || CLOCK3 || CLOCK4?
            if (is_ecc_a || is_ecc_b)
                get_setuphold_from_tmg_db(id_timing_RAM_ECC_SETUPHOLD_2, info.setup, info.hold);
            else
                get_setuphold_from_tmg_db(id_timing_RAM_NOECC_SETUPHOLD_2, info.setup, info.hold);
        }
        if (boost::starts_with(name, "DIA")) {
            if (is_ecc_a)
                get_setuphold_from_tmg_db(id_timing_RAM_ECC_SETUPHOLD_3, info.setup, info.hold);
            else
                get_setuphold_from_tmg_db(id_timing_RAM_NOECC_SETUPHOLD_3, info.setup, info.hold);
        }
        if (boost::starts_with(name, "DIB")) {
            if (is_ecc_b)
                get_setuphold_from_tmg_db(id_timing_RAM_ECC_SETUPHOLD_4, info.setup, info.hold);
            else
                get_setuphold_from_tmg_db(id_timing_RAM_NOECC_SETUPHOLD_4, info.setup, info.hold);
        }
        if (boost::starts_with(name, "ENA")) {
            if (is_ecc_a)
                get_setuphold_from_tmg_db(id_timing_RAM_ECC_SETUPHOLD_5, info.setup, info.hold);
            else
                get_setuphold_from_tmg_db(id_timing_RAM_NOECC_SETUPHOLD_5, info.setup, info.hold);
        }
        if (boost::starts_with(name, "ENB")) {
            if (is_ecc_b)
                get_setuphold_from_tmg_db(id_timing_RAM_ECC_SETUPHOLD_6, info.setup, info.hold);
            else
                get_setuphold_from_tmg_db(id_timing_RAM_NOECC_SETUPHOLD_6, info.setup, info.hold);
        }
        if (boost::starts_with(name, "GLWEA")) {
            if (is_ecc_a)
                get_setuphold_from_tmg_db(id_timing_RAM_ECC_SETUPHOLD_7, info.setup, info.hold);
            else
                get_setuphold_from_tmg_db(id_timing_RAM_NOECC_SETUPHOLD_7, info.setup, info.hold);
        }
        if (boost::starts_with(name, "GLWEB")) {
            if (is_ecc_b)
                get_setuphold_from_tmg_db(id_timing_RAM_ECC_SETUPHOLD_8, info.setup, info.hold);
            else
                get_setuphold_from_tmg_db(id_timing_RAM_NOECC_SETUPHOLD_8, info.setup, info.hold);
        }
        if (boost::starts_with(name, "WEA")) {
            if (is_ecc_a)
                get_setuphold_from_tmg_db(id_timing_RAM_ECC_SETUPHOLD_9, info.setup, info.hold);
            else
                get_setuphold_from_tmg_db(id_timing_RAM_NOECC_SETUPHOLD_9, info.setup, info.hold);
        }
        if (boost::starts_with(name, "WEB")) {
            if (is_ecc_b)
                get_setuphold_from_tmg_db(id_timing_RAM_ECC_SETUPHOLD_10, info.setup, info.hold);
            else
                get_setuphold_from_tmg_db(id_timing_RAM_NOECC_SETUPHOLD_10, info.setup, info.hold);
        }
        uint8_t a0_clk_val = int_or_default(cell->params, id_RAM_cfg_forward_a0_clk, 0);
        uint8_t a1_clk_val = int_or_default(cell->params, id_RAM_cfg_forward_a1_clk, 0);
        uint8_t b0_clk_val = int_or_default(cell->params, id_RAM_cfg_forward_b0_clk, 0);
        uint8_t b1_clk_val = int_or_default(cell->params, id_RAM_cfg_forward_b1_clk, 0);
        IdString a0_clk =
                clock(a0_clk_val, ctx->id("CLKA[0]"), ctx->id("CLKA[1]"), ctx->id("CLKB[0]"), ctx->id("CLKB[1]"));
        IdString a1_clk =
                clock(a1_clk_val, ctx->id("CLKA[2]"), ctx->id("CLKA[3]"), ctx->id("CLKB[2]"), ctx->id("CLKB[3]"));
        IdString b0_clk =
                clock(b0_clk_val, ctx->id("CLKB[0]"), ctx->id("CLKB[1]"), ctx->id("CLKA[0]"), ctx->id("CLKA[1]"));
        IdString b1_clk =
                clock(b1_clk_val, ctx->id("CLKB[2]"), ctx->id("CLKB[3]"), ctx->id("CLKA[2]"), ctx->id("CLKA[3]"));
        if (ram_signal_clk.count(port)) {
            IdString edge_param;
            switch (ram_clock_index(cell, ram_signal_clk.at(port))) {
            case 0:
                edge_param = id_RAM_cfg_inversion_a0;
                info.clock_port = a0_clk;
                break;
            case 1:
                edge_param = id_RAM_cfg_inversion_a1;
                info.clock_port = a1_clk;
                break;
            case 2:
                edge_param = id_RAM_cfg_inversion_b0;
                info.clock_port = b0_clk;
                break;
            case 3:
                edge_param = id_RAM_cfg_inversion_b1;
                info.clock_port = b1_clk;
                break;
            default:
                NPNR_ASSERT_FALSE_STR("bad ram clock index");
            }
            info.edge = (int_or_default(cell->params, edge_param, 0) & 0b100) ? FALLING_EDGE : RISING_EDGE;
        } else {
            log_error("Unknown clock signal for %s.\n", name.c_str());
        }
    }

    return info;
}

NEXTPNR_NAMESPACE_END
