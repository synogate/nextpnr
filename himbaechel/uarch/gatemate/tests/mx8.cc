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

// The 8:1 multiplexer of a CPE (CC_MX8): it takes the whole CPE, its selects come in on the register
// clock, enable and set/reset inputs, and its data inputs, having no LUT to undo an inversion, must be
// routed through an even number of inverting muxes.

#include <fstream>
#include <random>
#include "testing.h"

#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

class GateMateMx8Test : public GateMateTest
{
  protected:
    // Multiplexers between registers, their data and selects from a pool of registered inputs, so that
    // registers compete for the CPEs the multiplexers take
    void build(int muxes, uint32_t seed)
    {
        std::mt19937 rng(seed);
        NetInfo *clk = global_clock("clk");
        std::vector<NetInfo *> pool;
        for (int i = 0; i < 12; i++)
            pool.push_back(dff(stringf("in_ff%d", i), input(stringf("in%d", i)), clk)->getPort(id_Q));
        for (int m = 0; m < muxes; m++) {
            std::vector<NetInfo *> d, s;
            for (int i = 0; i < 8; i++)
                d.push_back(pool[rng() % pool.size()]);
            for (int i = 0; i < 3; i++)
                s.push_back(pool[rng() % pool.size()]);
            output(stringf("out%d", m),
                   dff(stringf("out_ff%d", m), mx8(stringf("mux%d", m), d, s), clk)->getPort(id_Q));
        }
    }

    // Each data input arrives as the multiplexer needs it: from the source, through the bridges on the way
    // (cells of their own after routing), inverted when either its net or the multiplexer's output is
    void expect_inputs_arrive_as_needed(const std::vector<CellInfo *> &found)
    {
        int inputs = 0;
        for (CellInfo *mux : found)
            for (int i = 1; i <= 8; i++) {
                NetInfo *net = mux->getPort(ctx->idf("IN%d", i));
                ASSERT_NE(net, nullptr);
                const CellInfo *cell = mux;
                IdString port = ctx->idf("IN%d", i);
                bool inverted = impl->inverted_nets.count(mux->getPort(id_MUXOUT)->name);
                while (true) {
                    const NetInfo *n = cell->getPort(port);
                    WireId sink = ctx->getBelPinWire(cell->bel, port);
                    ASSERT_TRUE(n->wires.count(sink)) << mux->name.str(ctx) << ".IN" << i;
                    inverted ^= impl->routed_inverted(n, sink) != bool(impl->inverted_nets.count(n->name));
                    if (!n->driver.cell || n->driver.cell->type != id_CPE_BRIDGE)
                        break;
                    cell = n->driver.cell;
                    port = impl->pass_backtrace.at(cell->name).at(n->driver.port);
                }
                EXPECT_FALSE(inverted) << mux->name.str(ctx) << ".IN" << i;
                inputs++;
            }
        EXPECT_GT(inputs, 0);
    }

    std::vector<CellInfo *> muxes()
    {
        std::vector<CellInfo *> found;
        for (auto &c : ctx->cells)
            if (c.second->type == id_CPE_MX8)
                found.push_back(c.second.get());
        return found;
    }
};

TEST_F(GateMateMx8Test, computes_the_reference)
{
    EXPECT_TRUE(implementation_matches_reference([&] { build(8, 1); }, 200));
}

TEST_F(GateMateMx8Test, constant_selects)
{
    EXPECT_TRUE(implementation_matches_reference([&] {
        std::vector<NetInfo *> d;
        for (int i = 0; i < 8; i++)
            d.push_back(input(stringf("d%d", i)));
        NetInfo *s = input("s");
        for (int c = 0; c < 8; c++) {
            // One select a signal, the others constant
            std::vector<NetInfo *> sel;
            for (int i = 0; i < 3; i++)
                sel.push_back(i == c % 3 ? s : ((c >> i) & 1) ? vcc() : gnd());
            output(stringf("o%d", c), mx8(stringf("mux%d", c), d, sel));
        }
        output("all_const", mx8("mux_const", d, {vcc(), gnd(), vcc()}));
    }));
}

TEST_F(GateMateMx8Test, takes_a_whole_cpe)
{
    build(16, 2);
    ASSERT_TRUE(packs());
    ASSERT_TRUE(ctx->place());
    auto found = muxes();
    ASSERT_EQ(found.size(), 16u);
    for (CellInfo *mux : found) {
        Loc loc = ctx->getBelLocation(mux->bel);
        EXPECT_EQ(loc.z, CPE_LT_FULL_Z) << mux->name.str(ctx);
        // (the bels of the halves and the registers are free again after repack(), but no other cell
        // was placed there)
        for (int z : {CPE_LT_U_Z, CPE_LT_L_Z, CPE_FF_U_Z, CPE_FF_L_Z})
            EXPECT_TRUE(ctx->checkBelAvail(ctx->getBelByLocation(Loc(loc.x, loc.y, z))))
                    << mux->name.str(ctx) << " z=" << z;
        EXPECT_TRUE(impl->used_cpes[mux->bel.tile]) << mux->name.str(ctx);
    }
    auto missing = missing_timing_arcs();
    EXPECT_TRUE(missing.empty()) << describe_arcs(missing);
}

TEST_F(GateMateMx8Test, data_inputs_are_routed_without_inversion)
{
    std::string out = write_temp_file("mx8.cfg", "");
    init_context("CCGM1A1", {"--allow-unconstrained", "--out=" + out});
    build(32, 3);
    LogCapture info(LogLevel::INFO_MSG);
    place_and_route();
    auto found = muxes();
    ASSERT_EQ(found.size(), 32u);
    expect_inputs_arrive_as_needed(found);
    std::ifstream cfg(out);
    std::string text((std::istreambuf_iterator<char>(cfg)), std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("C_CPE_CLK"), std::string::npos);
}

// Between LUTs, which can invert what they put out and undo what arrives inverted, nets are inverted instead
// of routing their multiplexer inputs again
TEST_F(GateMateMx8Test, inverts_nets_between_luts)
{
    std::string out = write_temp_file("mx8_luts.cfg", "");
    init_context("CCGM1A1", {"--allow-unconstrained", "--out=" + out});
    std::mt19937 rng(4);
    std::vector<NetInfo *> pool;
    for (int i = 0; i < 12; i++)
        pool.push_back(input(stringf("in%d", i)));
    for (int m = 0; m < 16; m++) {
        std::vector<NetInfo *> d, s;
        for (int i = 0; i < 8; i++)
            d.push_back(lut2(stringf("d%d_%d", m, i), 0b0110, pool[rng() % pool.size()], pool[rng() % pool.size()]));
        for (int i = 0; i < 3; i++)
            s.push_back(pool[rng() % pool.size()]);
        NetInfo *y = mx8(stringf("mux%d", m), d, s);
        output(stringf("out%d", m), lut2(stringf("o%d", m), 0b1000, y, pool[rng() % pool.size()]));
    }
    LogCapture info(LogLevel::INFO_MSG);
    place_and_route();
    auto found = muxes();
    ASSERT_EQ(found.size(), 16u);
    EXPECT_FALSE(impl->inverted_nets.empty()) << info.str();
    for (auto &name : impl->inverted_nets) {
        const NetInfo *net = ctx->nets.at(name).get();
        EXPECT_TRUE(impl->can_invert_driver(net->driver)) << name.str(ctx);
    }
    expect_inputs_arrive_as_needed(found);
}

// Registers (some with a reset) store the inverse, with the LUT before them inverted too
TEST_F(GateMateMx8Test, inverts_registers)
{
    std::string out = write_temp_file("mx8_ffs.cfg", "");
    init_context("CCGM1A1", {"--allow-unconstrained", "--out=" + out});
    std::mt19937 rng(5);
    NetInfo *clk = global_clock("clk");
    NetInfo *rst = input("rst");
    std::vector<NetInfo *> pool;
    for (int i = 0; i < 12; i++)
        pool.push_back(input(stringf("in%d", i)));
    for (int m = 0; m < 16; m++) {
        std::vector<NetInfo *> d, s;
        for (int i = 0; i < 8; i++) {
            NetInfo *x = lut2(stringf("x%d_%d", m, i), 0b0110, pool[rng() % pool.size()], pool[rng() % pool.size()]);
            d.push_back(dff(stringf("r%d_%d", m, i), x, clk, nullptr, i % 2 ? rst : nullptr)->getPort(id_Q));
        }
        for (int i = 0; i < 3; i++)
            s.push_back(pool[rng() % pool.size()]);
        NetInfo *y = mx8(stringf("mux%d", m), d, s);
        output(stringf("out%d", m), lut2(stringf("o%d", m), 0b1000, y, pool[rng() % pool.size()]));
    }
    LogCapture info(LogLevel::INFO_MSG);
    place_and_route();
    auto found = muxes();
    ASSERT_EQ(found.size(), 16u);
    int registers = 0;
    for (auto &name : impl->inverted_nets) {
        const NetInfo *net = ctx->nets.at(name).get();
        if (net->driver.cell->type == id_CPE_FF) {
            registers++;
            EXPECT_TRUE(impl->inverted_nets.count(net->driver.cell->getPort(id_DIN)->name)) << name.str(ctx);
        }
    }
    EXPECT_GT(registers, 0) << info.str();
    expect_inputs_arrive_as_needed(found);
}
