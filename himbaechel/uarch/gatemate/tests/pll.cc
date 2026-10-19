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

// CC_PLL: the divider settings must produce the requested frequency within the DCO range of the
// performance mode, the outputs must carry the right clock constraints, and invalid uses must be
// reported.

#include <cmath>
#include "testing.h"
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

class GateMatePllTest : public GateMateTest
{
  protected:
    // An input on a clock capable pin
    NetInfo *clock_input(const std::string &name, int index = 0)
    {
        NetInfo *net = input(name);
        ctx->cells.at(ctx->id(name))->params[id_PIN_NAME] = Property(package_pins(true).at(index));
        return net;
    }

    // A CC_PLL with the given parameters, its CLK0 output clocking a register
    CellInfo *pll(const std::string &name, NetInfo *ref, const std::vector<std::pair<IdString, Property>> &params)
    {
        CellInfo *cell = create_cell_ptr(id_CC_PLL, name);
        for (auto &p : params)
            cell->params[p.first] = p.second;
        if (ref)
            cell->connectPort(id_CLK_REF, ref);
        NetInfo *clk0 = create_net(name + "$clk0");
        cell->connectPort(id_CLK0, clk0);
        output(name + "_q", dff(name + "_ff", input(name + "_d"), clk0)->getPort(id_Q));
        return cell;
    }

    int param(CellInfo *cell, const char *name) { return int_or_default(cell->params, ctx->id(name), -1); }
};

struct PllFrequencyCase
{
    double ref, out;
    const char *mode;
    double dco_min, dco_max;
};

class GateMatePllFrequencyTest : public GateMatePllTest, public ::testing::WithParamInterface<PllFrequencyCase>
{
};

// The output frequency follows from the dividers as f_out = f_ref * N1 * N2 / (2 * K * M1 * M2),
// with the DCO at f_ref / K * PDIV1 * N1 * N2.
TEST_P(GateMatePllFrequencyTest, configured_frequency)
{
    auto c = GetParam();
    int fpga_mode = std::string(c.mode) == "LOWPOWER" ? 1 : std::string(c.mode) == "ECONOMY" ? 2 : 3;
    init_context("CCGM1A1", {"--allow-unconstrained", stringf("--fpga_mode=%d", fpga_mode)});
    CellInfo *cell = pll("pll", clock_input("clkin"),
                         {{id_REF_CLK, Property(stringf("%g", c.ref))},
                          {id_OUT_CLK, Property(stringf("%g", c.out))},
                          {id_PERF_MD, Property(c.mode)},
                          {id_LOW_JITTER, Property(0, 32)}});
    ASSERT_TRUE(packs());
    int k = param(cell, "CFG_A_K"), n1 = param(cell, "CFG_A_N1"), n2 = param(cell, "CFG_A_N2");
    int m1 = param(cell, "CFG_A_M1"), m2 = param(cell, "CFG_A_M2");
    int pdiv1 = param(cell, "CFG_A_PDIV1_SEL") ? 2 : 1;
    ASSERT_GT(k, 0);
    ASSERT_GT(m1, 0);
    ASSERT_GT(m2, 0);
    double f_dco = c.ref / k * pdiv1 * n1 * n2;
    double f_out = c.ref * n1 * n2 / (2.0 * k * m1 * m2);
    EXPECT_NEAR(f_out, c.out, c.out * 0.002) << "K=" << k << " N1=" << n1 << " N2=" << n2 << " M1=" << m1
                                               << " M2=" << m2 << " PDIV1=" << pdiv1;
    EXPECT_GT(f_dco, c.dco_min);
    EXPECT_LE(f_dco, c.dco_max);
    // The requested frequency becomes the clock constraint of the output
    NetInfo *clk0 = ctx->nets.at(ctx->id("pll$clk0")).get();
    ASSERT_NE(clk0->clkconstr, nullptr);
    EXPECT_NEAR(ctx->getDelayNS(clk0->clkconstr->period.minDelay()), 1000.0 / c.out, 0.01);
}

INSTANTIATE_TEST_SUITE_P(Frequencies, GateMatePllFrequencyTest,
                         ::testing::Values(PllFrequencyCase{10, 50, "SPEED", 1250, 2500},
                                           PllFrequencyCase{10, 100, "SPEED", 1250, 2500},
                                           PllFrequencyCase{10, 125, "SPEED", 1250, 2500},
                                           PllFrequencyCase{10, 333, "SPEED", 1250, 2500},
                                           PllFrequencyCase{12, 48, "SPEED", 1250, 2500},
                                           PllFrequencyCase{25, 100, "ECONOMY", 1000, 2000},
                                           PllFrequencyCase{25, 250, "ECONOMY", 1000, 2000},
                                           PllFrequencyCase{10, 50, "LOWPOWER", 500, 1000},
                                           PllFrequencyCase{10, 200, "LOWPOWER", 500, 1000},
                                           PllFrequencyCase{33.333, 66.666, "SPEED", 1250, 2500}),
                         [](const ::testing::TestParamInfo<PllFrequencyCase> &info) {
                             std::string s = stringf("%s_%g_to_%g", info.param.mode, info.param.ref, info.param.out);
                             for (auto &c : s)
                                 if (c == '.')
                                     c = '_';
                             return s;
                         });

// Parameters left out in the netlist (yosys does not write defaults) take the defaults of the
// primitive library (techlibs/gatemate/cells_bb.v): LOCK_REQ = 1, LOW_JITTER = 1.
TEST_F(GateMatePllTest, primitive_defaults)
{
    CellInfo *cell = pll("pll", clock_input("clkin"),
                         {{id_REF_CLK, Property("10.0")}, {id_OUT_CLK, Property("100.0")}, {id_PERF_MD, Property("SPEED")}});
    ASSERT_TRUE(packs());
    EXPECT_EQ(param(cell, "LOCK_REQ"), 1);
    // Low jitter mode does not divide the reference clock
    EXPECT_EQ(param(cell, "CFG_A_K"), 1);
}

TEST_F(GateMatePllTest, doubled_outputs)
{
    CellInfo *cell = pll("pll", clock_input("clkin"),
                         {{id_REF_CLK, Property("10.0")},
                          {id_OUT_CLK, Property("100.0")},
                          {id_PERF_MD, Property("SPEED")},
                          {id_CLK180_DOUB, Property(1, 1)}});
    NetInfo *clk180 = create_net("pll$clk180");
    cell->connectPort(id_CLK180, clk180);
    output("q180", dff("ff180", input("d180"), clk180)->getPort(id_Q));
    ASSERT_TRUE(packs());
    ASSERT_NE(clk180->clkconstr, nullptr);
    EXPECT_NEAR(ctx->getDelayNS(clk180->clkconstr->period.minDelay()), 5.0, 0.01);
}

// Errors

TEST_F(GateMatePllTest, no_reference_clock)
{
    pll("pll", nullptr, {{id_REF_CLK, Property("10.0")}, {id_OUT_CLK, Property("100.0")}});
    EXPECT_LOG_ERROR(pack(), "At least one reference clock (CLK_REF or USR_CLK_REF) must be set for cell 'pll'.");
}

TEST_F(GateMatePllTest, two_reference_clocks)
{
    CellInfo *cell = pll("pll", clock_input("clkin"), {{id_REF_CLK, Property("10.0")}, {id_OUT_CLK, Property("100.0")}});
    cell->connectPort(id_USR_CLK_REF, input("usr"));
    EXPECT_LOG_ERROR(pack(), "CLK_REF and USR_CLK_REF are not allowed to be set in same time for cell 'pll'.");
}

TEST_F(GateMatePllTest, reference_from_logic)
{
    pll("pll", lut1("inv", 0b01, input("clkin")), {{id_REF_CLK, Property("10.0")}, {id_OUT_CLK, Property("100.0")}});
    EXPECT_LOG_ERROR(pack(), "CLK_REF must be driven with GPIO pin for cell 'pll'.");
}

TEST_F(GateMatePllTest, reference_from_ordinary_pin)
{
    NetInfo *clkin = input("clkin");
    // A pin that cannot be used as a clock input
    auto clock_pins = package_pins(true);
    for (auto &p : package_pins())
        if (std::find(clock_pins.begin(), clock_pins.end(), p) == clock_pins.end()) {
            ctx->cells.at(ctx->id("clkin"))->params[id_PIN_NAME] = Property(p);
            break;
        }
    pll("pll", clkin, {{id_REF_CLK, Property("10.0")}, {id_OUT_CLK, Property("100.0")}});
    EXPECT_LOG_ERROR(pack(), "CLK_REF must be driven with CLK dedicated pin for cell 'pll'.");
}

TEST_F(GateMatePllTest, unknown_performance_mode)
{
    pll("pll", clock_input("clkin"),
        {{id_REF_CLK, Property("10.0")}, {id_OUT_CLK, Property("100.0")}, {id_PERF_MD, Property("TURBO")}});
    EXPECT_LOG_ERROR(pack(), "Unknown PERF_MD parameter value 'TURBO' for cell pll.");
}

TEST_F(GateMatePllTest, invalid_reference_frequency)
{
    pll("pll", clock_input("clkin"), {{id_REF_CLK, Property("0")}, {id_OUT_CLK, Property("100.0")}});
    EXPECT_LOG_ERROR(pack(), "REF_CLK parameter is <= 0 for 'pll'.");
}

TEST_F(GateMatePllTest, output_frequency_out_of_range)
{
    pll("pll", clock_input("clkin"),
        {{id_REF_CLK, Property("10.0")}, {id_OUT_CLK, Property("500.0")}, {id_PERF_MD, Property("SPEED")}});
    EXPECT_LOG_ERROR(pack(), "OUT_CLK parameter is out of range (0,416.75] for 'pll'.");
}

TEST_F(GateMatePllTest, reference_output_used)
{
    CellInfo *cell = pll("pll", clock_input("clkin"), {{id_REF_CLK, Property("10.0")}, {id_OUT_CLK, Property("100.0")}});
    output("ref_out", connect_output(cell, id_CLK_REF_OUT));
    EXPECT_LOG_ERROR(pack(), "Output CLK_REF_OUT cannot be used if PLL 'pll' is used.");
}

TEST_F(GateMatePllTest, too_many_plls)
{
    NetInfo *clkin = clock_input("clkin");
    for (int i = 0; i < 5; i++)
        pll(stringf("pll%d", i), clkin, {{id_REF_CLK, Property("10.0")}, {id_OUT_CLK, Property("100.0")}});
    EXPECT_LOG_ERROR(pack(), "Used more than available PLLs.");
}

TEST_F(GateMatePllTest, output_to_bufg_and_logic)
{
    CellInfo *cell = pll("pll", clock_input("clkin"), {{id_REF_CLK, Property("10.0")}, {id_OUT_CLK, Property("100.0")}});
    NetInfo *clk0 = cell->getPort(id_CLK0);
    CellInfo *bufg = create_cell_ptr(id_CC_BUFG, "bufg");
    bufg->connectPort(id_I, clk0);
    NetInfo *glb = create_net("bufg$o");
    bufg->connectPort(id_O, glb);
    output("q_glb", dff("ff_glb", input("d_glb"), glb)->getPort(id_Q));
    EXPECT_LOG_ERROR(pack(), "A PLL output (here CLK0 of pll) connected to a BUFG could only connect to it");
}
