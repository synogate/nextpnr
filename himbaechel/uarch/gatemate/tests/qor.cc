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

// Quality of the mapping: how many CPEs and other resources the packer and placer use for a
// design, compared with what the architecture allows.

#include <boost/algorithm/string.hpp>
#include <set>
#include "testing.h"
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"
#include "uarch/gatemate/extra_data.h"

USING_NEXTPNR_NAMESPACE

class GateMateQorTest : public GateMateTest
{
  protected:
    void SetUp() override
    {
        GateMateTest::SetUp();
        // "clk" on a clock capable pin, given in the CCF (pins given by PIN_NAME parameters
        // stay available for unconstrained IOs, see PIN_NAME_PAD_REUSED)
        std::string pin = package_pins(true).at(0);
        init_with_ccf(stringf("Pin_in \"clk\" Loc = \"%s\";\n", pin.c_str()), {"--allow-unconstrained"});
    }

    // The global clock from pad "clk", which reaches the clock network without a CPE
    NetInfo *clock_pin()
    {
        CellInfo *bufg = create_cell_ptr(id_CC_BUFG, "clk_bufg");
        bufg->connectPort(id_I, top_input("clk"));
        NetInfo *clk = create_net("clk$glb");
        bufg->connectPort(id_O, clk);
        return clk;
    }

    // RAM inputs that a CPE drives with a constant (a LUT without inputs in front of the CPE
    // that passes its value on to the RAM)
    std::vector<std::string> constant_ram_pins()
    {
        std::vector<std::string> pins;
        for (auto &c : ctx->cells) {
            const CellInfo *ram = c.second.get();
            if (!ram->type.in(id_RAM, id_RAM_HALF))
                continue;
            for (auto &port : ram->ports) {
                const NetInfo *net = port.second.net;
                if (port.second.type != PORT_IN || !net || !net->driver.cell ||
                    !net->driver.cell->type.in(id_CPE_RAMO, id_CPE_RAMIO))
                    continue;
                const NetInfo *in = net->driver.cell->getPort(id_I);
                const CellInfo *lut = in ? in->driver.cell : nullptr;
                if (!lut || lut->type != id_CPE_L2T4)
                    continue;
                bool has_inputs = false;
                for (auto &lp : lut->ports)
                    has_inputs |= lp.second.type == PORT_IN && lp.second.net != nullptr;
                if (!has_inputs)
                    pins.push_back(port.first.str(ctx));
            }
        }
        return pins;
    }

    std::vector<CellInfo *> cells_named(const std::string &prefix)
    {
        std::vector<CellInfo *> found;
        for (auto &c : ctx->cells)
            if (boost::starts_with(c.first.str(ctx), prefix))
                found.push_back(c.second.get());
        return found;
    }

    // The CPE tiles the given placed cells use
    std::set<std::pair<int, int>> tiles(const std::vector<CellInfo *> &cells)
    {
        std::set<std::pair<int, int>> used;
        for (CellInfo *cell : cells)
            if (cell->bel != BelId())
                used.insert({ctx->getBelLocation(cell->bel).x, ctx->getBelLocation(cell->bel).y});
        return used;
    }

    // The lower left corner of a box of w x h CPE tiles (bel coordinates) in the core
    Loc cpe_box(int w, int h)
    {
        std::set<std::pair<int, int>> cpe;
        for (BelId bel : ctx->getBels())
            if (ctx->getBelType(bel) == id_CPE_LT_U)
                cpe.insert({ctx->getBelLocation(bel).x, ctx->getBelLocation(bel).y});
        for (int y = 20; y < 100; y++)
            for (int x = 20; x < 100; x++) {
                bool ok = true;
                for (int dy = 0; dy < h && ok; dy++)
                    for (int dx = 0; dx < w && ok; dx++)
                        ok = cpe.count({x + dx, y + dy});
                if (ok)
                    return Loc(x, y, 0);
            }
        ADD_FAILURE() << "no box of " << w << "x" << h << " CPEs";
        return Loc(0, 0, 0);
    }

    // After packing: constrains the cells whose names start with `prefix` to a box of w x h CPE
    // tiles, places the design and checks that they are inside
    ::testing::AssertionResult places_in_box(const std::string &prefix, int w, int h)
    {
        Loc origin = cpe_box(w, h);
        IdString region = ctx->id("box");
        ctx->createRectangularRegion(region, origin.x, origin.y, origin.x + w - 1, origin.y + h - 1);
        auto cells = cells_named(prefix);
        for (CellInfo *cell : cells)
            ctx->constrainCellToRegion(cell->name, region);
        LogCapture errors(LogLevel::ERROR_MSG);
        try {
            ctx->place();
        } catch (const std::exception &e) {
            return ::testing::AssertionFailure() << "placement failed: " << errors.str();
        }
        // (Again: the packer replaces some cells after placement)
        for (CellInfo *cell : cells_named(prefix)) {
            if (cell->bel == BelId())
                return ::testing::AssertionFailure() << cell->name.str(ctx) << " not placed";
            Loc loc = ctx->getBelLocation(cell->bel);
            if (loc.x < origin.x || loc.x >= origin.x + w || loc.y < origin.y || loc.y >= origin.y + h)
                return ::testing::AssertionFailure() << cell->name.str(ctx) << " placed outside the box";
        }
        return ::testing::AssertionSuccess();
    }
};

// RAM inputs that the RAM ignores in its configuration (cells_sim.v) do not need a CPE driving
// them, even when the netlist ties them to a constant, as yosys does for the address bits a
// port does not use:
//  - TDP: the address bits below the lowest one used at the port's width (A_ADDR[5:lsb] and
//    [15:7] are used, lsb from 1 for 1 bit to 6 for 40 bits), and bit 6 on a 20K RAM;
//  - SDP: the address bits [6:0];
//  - the data inputs and bit masks of a port that never writes, and all inputs of a disabled port.
// In these designs every input the RAM uses comes from a pad, so no constant CPE is needed at all.
struct RamTieOffCase
{
    std::string name;
    bool is_40k = false, sdp = false;
    int width = 1;
    // Port B: only reads (as yosys maps a simple dual port memory), is configured to read and
    // write but its write enable is tied to 0, or is disabled
    enum PortB
    {
        READS,
        NEVER_WRITES,
        DISABLED,
    } port_b = READS;
};

std::ostream &operator<<(std::ostream &os, const RamTieOffCase &c) { return os << c.name; }

class GateMateRamTieOffTest : public GateMateQorTest, public ::testing::WithParamInterface<RamTieOffCase>
{
};

TEST_P(GateMateRamTieOffTest, ignored_inputs_take_no_cpe)
{
    const RamTieOffCase &c = GetParam();
    int data = c.is_40k ? 40 : 20;
    int full = 2 * data;
    NetInfo *clk = clock_pin();
    CellInfo *ram = ctx->createCell(ctx->id("ram"), c.is_40k ? id_CC_BRAM_40K : id_CC_BRAM_20K);
    ram->params[id_RAM_MODE] = Property(c.sdp ? "SDP" : "TDP");
    bool b_writes_configured = !c.sdp && c.port_b != RamTieOffCase::READS;
    ram->params[id_A_WR_WIDTH] = Property(c.sdp ? full : c.width, 32);
    ram->params[id_A_RD_WIDTH] = Property(c.sdp || c.port_b == RamTieOffCase::READS ? 0 : c.width, 32);
    ram->params[id_B_RD_WIDTH] = Property(c.sdp ? full : c.width, 32);
    ram->params[id_B_WR_WIDTH] = Property(b_writes_configured ? c.width : 0, 32);
    connect_input(ram, id_A_CLK, clk);
    connect_input(ram, id_B_CLK, clk);
    NetInfo *we = input("we");
    connect_input(ram, id_A_EN, vcc());
    connect_input(ram, id_A_WE, we);
    connect_input(ram, id_B_EN, c.port_b == RamTieOffCase::DISABLED ? gnd() : vcc());
    connect_input(ram, id_B_WE, gnd());
    int lsb = c.sdp            ? 7
              : c.width <= 1  ? 1
              : c.width <= 2  ? 2
              : c.width <= 5  ? 3
              : c.width <= 10 ? 4
              : c.width <= 20 ? 5
                              : 6;
    std::vector<NetInfo *> addr(16), d;
    for (int i = 0; i < 16; i++)
        if (i >= lsb && !(i == 6 && !c.is_40k))
            addr[i] = input(stringf("addr%d", i));
    for (int i = 0; i < 8; i++)
        d.push_back(input(stringf("d%d", i)));
    for (char port : {'A', 'B'}) {
        bool off = port == 'B' && c.port_b == RamTieOffCase::DISABLED;
        for (int i = 0; i < 16; i++)
            connect_input(ram, ctx->idf("%c_ADDR[%d]", port, i), addr[i] && !off ? addr[i] : gnd());
        // Data and bit masks: written by port A (in SDP mode the upper bits on port B's inputs);
        // tied to 0 on a port B configured to write
        bool writes = port == 'A' || c.sdp;
        if (writes || b_writes_configured) {
            for (int i = 0; i < std::min(c.sdp ? data : c.width, data); i++) {
                connect_input(ram, ctx->idf("%c_DI[%d]", port, i), writes ? d[i % 8] : gnd());
                connect_input(ram, ctx->idf("%c_BM[%d]", port, i), writes ? we : gnd());
            }
        }
    }
    // Read data: port B (in SDP mode the lower bits on port A's outputs, from a disabled port B
    // port A)
    for (int bit : std::set<int>{0, std::min(c.width, data) - 1}) {
        if (c.port_b != RamTieOffCase::DISABLED)
            output(stringf("qb%d", bit), connect_output(ram, ctx->idf("B_DO[%d]", bit)));
        if (c.sdp || c.port_b == RamTieOffCase::DISABLED)
            output(stringf("qa%d", bit), connect_output(ram, ctx->idf("A_DO[%d]", bit)));
    }
    ASSERT_TRUE(packs());
    auto pins = constant_ram_pins();
    EXPECT_TRUE(pins.empty()) << pins.size()
                              << " constant RAM inputs with a CPE: " << boost::algorithm::join(pins, " ");
}

std::vector<RamTieOffCase> ram_tie_off_cases()
{
    std::vector<RamTieOffCase> cases;
    auto add = [&](const std::string &name, bool is_40k, bool sdp, int width,
                   RamTieOffCase::PortB port_b = RamTieOffCase::READS) {
        RamTieOffCase c;
        c.name = name;
        c.is_40k = is_40k;
        c.sdp = sdp;
        c.width = width;
        c.port_b = port_b;
        cases.push_back(c);
    };
    add("tdp_20k_w1", false, false, 1);
    add("tdp_20k_w20", false, false, 20);
    add("tdp_40k_w1", true, false, 1);
    add("tdp_40k_w40", true, false, 40);
    add("sdp_20k", false, true, 40);
    add("sdp_40k", true, true, 80);
    add("port_never_writes", false, false, 10, RamTieOffCase::NEVER_WRITES);
    add("port_disabled", false, false, 5, RamTieOffCase::DISABLED);
    return cases;
}

INSTANTIATE_TEST_SUITE_P(Configs, GateMateRamTieOffTest, ::testing::ValuesIn(ram_tie_off_cases()),
                         [](const ::testing::TestParamInfo<RamTieOffCase> &info) { return info.param.name; });

// Density: logic constrained to a box of CPEs fills it completely, two LUTs (with or without
// their registers) per CPE, or one cell of a kind that takes a whole CPE.
struct DensityCase
{
    std::string name;
    int count; // cells of the kind built
    int w, h;  // the box, in CPE tiles
};

std::ostream &operator<<(std::ostream &os, const DensityCase &c) { return os << c.name; }

class GateMateDensityTest : public GateMateQorTest, public ::testing::WithParamInterface<DensityCase>
{
  protected:
    // `count` cells of the kind `name`, named "u*", in a chain so that they all stay in use
    void build(const std::string &name, int count)
    {
        std::vector<NetInfo *> pads;
        for (int i = 0; i < 8; i++)
            pads.push_back(input(stringf("p%d", i)));
        NetInfo *clk = clock_pin();
        NetInfo *net = pads[0];
        auto pad = [&](int i) { return pads[1 + i % 7]; };
        for (int i = 0; i < count; i++) {
            std::string u = stringf("u%d", i);
            if (name == "luts") {
                net = lut2(u, 0b0110, net, pad(i));
            } else if (name == "lut_trees") {
                net = l2t4(u, 0b0110, 0b1000, 0b1110, net, pad(i), pad(i + 1), pad(i + 2));
            } else if (name == "luts_with_registers") {
                net = dff(u + "_ff", lut2(u, 0b0110, net, pad(i)), clk)->getPort(id_Q);
            } else if (name == "registers") {
                net = dff(u, net, clk)->getPort(id_Q);
            } else if (name == "mx4") {
                net = mx4(u, net, pad(i), pad(i + 1), pad(i + 2), pad(i + 3), pad(i + 4));
            } else if (name == "l2t5") {
                CellInfo *l = create_cell_ptr(id_CC_L2T5, u);
                l->params[id_INIT_L02] = Property(0b0110, 4);
                l->params[id_INIT_L03] = Property(0b1000, 4);
                l->params[id_INIT_L11] = Property(0b0110, 4);
                l->params[id_INIT_L20] = Property(0b1001, 4);
                NetInfo *in[5] = {net, pad(i), pad(i + 1), pad(i + 2), pad(i + 3)};
                for (int k = 0; k < 5; k++)
                    l->connectPort(ctx->idf("I%d", k), in[k]);
                net = create_net(u + "$o");
                l->connectPort(id_O, net);
            } else if (name == "adder") {
                // One adder bit per cell, the carry from and to pads
                if (i == 0)
                    net = input("ci");
                auto sc = addf(u, pads[i % 8], pad(i * 3), net);
                output(stringf("s%d", i), sc.first);
                net = sc.second;
            }
        }
        // Through a LUT outside the box (a LUT driving an output pad moves into the pad's CPE)
        output("y", lut2("z", 0b1001, net, pads[0]));
    }
};

TEST_P(GateMateDensityTest, fills_box)
{
    const DensityCase &c = GetParam();
    build(c.name, c.count);
    ASSERT_TRUE(packs());
    EXPECT_TRUE(places_in_box("u", c.w, c.h));
}

INSTANTIATE_TEST_SUITE_P(Kinds, GateMateDensityTest,
                         ::testing::Values(DensityCase{"luts", 32, 4, 4}, DensityCase{"lut_trees", 32, 4, 4},
                                           DensityCase{"luts_with_registers", 32, 4, 4},
                                           DensityCase{"registers", 32, 4, 4}, DensityCase{"mx4", 16, 4, 4},
                                           DensityCase{"l2t5", 16, 4, 4},
                                           // 16 CPEs of two adder bits, carry in and carry out
                                           DensityCase{"adder", 32, 1, 18}),
                         [](const ::testing::TestParamInfo<DensityCase> &info) { return info.param.name; });

// The box of the density tests is a real limit: one LUT more than fits does not place.
TEST_F(GateMateQorTest, box_does_not_fit_more)
{
    NetInfo *a = input("a"), *b = input("b");
    NetInfo *net = a;
    for (int i = 0; i < 33; i++)
        net = lut2(stringf("u%d", i), 0b0110, net, b);
    output("y", net);
    ASSERT_TRUE(packs());
    EXPECT_FALSE(places_in_box("u", 4, 4));
}

// Two 20K RAMs share a RAM block: the device takes twice as many 20K RAMs as it has blocks.
TEST_F(GateMateQorTest, two_20k_rams_per_block)
{
    int blocks = 0;
    for (BelId bel : ctx->getBels())
        blocks += ctx->getBelType(bel) == id_RAM;
    ASSERT_GT(blocks, 0);
    NetInfo *clk = clock_pin();
    NetInfo *we = input("we");
    NetInfo *net = input("d");
    std::vector<NetInfo *> addr;
    for (int i = 0; i < 4; i++)
        addr.push_back(input(stringf("a%d", i)));
    for (int r = 0; r < 2 * blocks; r++) {
        CellInfo *ram = ctx->createCell(ctx->idf("ram%d", r), id_CC_BRAM_20K);
        ram->params[id_RAM_MODE] = Property("TDP");
        ram->params[id_A_WR_WIDTH] = Property(1, 32);
        ram->params[id_B_RD_WIDTH] = Property(1, 32);
        connect_input(ram, id_A_CLK, clk);
        connect_input(ram, id_B_CLK, clk);
        connect_input(ram, id_A_EN, vcc());
        connect_input(ram, id_B_EN, vcc());
        connect_input(ram, id_A_WE, we);
        connect_input(ram, id_B_WE, gnd());
        for (int i = 0; i < 4; i++) {
            connect_input(ram, ctx->idf("A_ADDR[%d]", i + 1), addr[i]);
            connect_input(ram, ctx->idf("B_ADDR[%d]", i + 1), addr[i]);
        }
        connect_input(ram, ctx->id("A_DI[0]"), net);
        connect_input(ram, ctx->id("A_BM[0]"), we);
        net = connect_output(ram, ctx->id("B_DO[0]"));
    }
    output("q", net);
    ASSERT_TRUE(packs());
    ctx->place();
    int used = 0;
    for (auto &c : ctx->cells)
        used += c.second->type == id_RAM;
    EXPECT_EQ(used, blocks);
}

// An N bit adder takes N/2 CPEs (two bits per CPE), plus one for a carry input from the fabric.
TEST_F(GateMateQorTest, adder_takes_two_bits_per_cpe)
{
    NetInfo *carry = input("ci");
    for (int i = 0; i < 16; i++) {
        auto sc = addf(stringf("add%d", i), input(stringf("a%d", i)), input(stringf("b%d", i)), carry);
        carry = sc.second;
        output(stringf("s%d", i), sc.first);
    }
    ASSERT_TRUE(packs());
    ctx->place();
    int adders = 0;
    for (auto &c : ctx->cells)
        adders += c.second->type.in(id_CPE_ADDF, id_CPE_ADDF2);
    EXPECT_EQ(adders, 8);
    EXPECT_EQ(tiles(cells_named("add")).size(), 9u) << "adder CPEs and the carry input";
}

// Multiplexers with four inputs and LUT trees with five inputs take one CPE each.
TEST_F(GateMateQorTest, mx4_and_l2t5_take_one_cpe)
{
    NetInfo *in[6];
    for (int i = 0; i < 6; i++)
        in[i] = input(stringf("i%d", i));
    NetInfo *m = mx4("mx4", in[0], in[1], in[2], in[3], in[4], in[5]);
    CellInfo *l = create_cell_ptr(id_CC_L2T5, "l2t5");
    l->params[id_INIT_L02] = Property(0b0110, 4);
    l->params[id_INIT_L03] = Property(0b1000, 4);
    l->params[id_INIT_L11] = Property(0b0110, 4);
    l->params[id_INIT_L20] = Property(0b1001, 4);
    for (int k = 0; k < 5; k++)
        l->connectPort(ctx->idf("I%d", k), in[k]);
    NetInfo *o = create_net("l2t5$o");
    l->connectPort(id_O, o);
    // Into LUTs, so that neither is moved into the CPE of an output pad
    output("ym", lut2("use_m", 0b0110, m, in[5]));
    output("yl", lut2("use_l", 0b0110, o, in[5]));
    ASSERT_TRUE(packs());
    ctx->place();
    EXPECT_EQ(tiles(cells_named("mx4")).size(), 1u);
    EXPECT_EQ(tiles(cells_named("l2t5")).size(), 1u);
}

// A register shares the CPE of the LUT driving it (also of an inverter), and constant enables and
// resets of registers need no logic.
TEST_F(GateMateQorTest, registers_share_cpes_with_their_luts)
{
    NetInfo *clk = clock_pin();
    NetInfo *a = input("a"), *b = input("b");
    for (int i = 0; i < 4; i++)
        output(stringf("q%d", i),
               dff(stringf("ff%d", i), lut2(stringf("lut%d", i), 0b0110 ^ i, a, b), clk)->getPort(id_Q));
    output("qi", dff("ffi", lut1("inv", 0b01, a), clk)->getPort(id_Q));
    output("qc", dff("ffc", lut2("lutc", 0b1000, a, b), clk, vcc(), gnd())->getPort(id_Q));
    ASSERT_TRUE(packs());
    ctx->place();
    // CPE halves with logic: the six LUTs in front of the registers (the inverter is the LUT of
    // its register), and the six that pass the register outputs to the output pads
    int halves = 0;
    for (auto &c : ctx->cells)
        halves += c.second->type == id_CPE_L2T4;
    EXPECT_EQ(halves, 12);
    const std::pair<const char *, const char *> pairs[] = {{"ff0", "lut0"}, {"ff1", "lut1"}, {"ff2", "lut2"},
                                                           {"ff3", "lut3"}, {"ffi", "inv"},  {"ffc", "lutc"}};
    for (auto &p : pairs) {
        CellInfo *ff = ctx->cells.at(ctx->id(p.first)).get();
        CellInfo *lut = ctx->cells.at(ctx->id(p.second)).get();
        EXPECT_EQ(tiles({ff}), tiles({lut})) << p.first << " is not in the CPE of " << p.second;
    }
}

// Each RAM input and each output pad is driven through a CPE at a fixed place; logic with no other
// users in front of them goes into that CPE instead of one of its own.
TEST_F(GateMateQorTest, logic_in_front_of_ram_pins_and_pads_shares_their_cpes)
{
    NetInfo *clk = clock_pin();
    NetInfo *a = input("a"), *b = input("b"), *c = input("c");
    CellInfo *ram = ctx->createCell(ctx->id("ram"), id_CC_BRAM_20K);
    ram->params[id_RAM_MODE] = Property("TDP");
    ram->params[id_A_RD_WIDTH] = Property(1, 32);
    ram->params[id_A_WR_WIDTH] = Property(1, 32);
    connect_input(ram, id_A_CLK, clk);
    connect_input(ram, id_A_EN, lut2("en", 0b1000, a, b));
    connect_input(ram, id_A_WE, mx2("we", a, b, c));
    connect_input(ram, ctx->id("A_ADDR[7]"), lut1("addr", 0b01, a));
    connect_input(ram, ctx->id("A_DI[0]"), l2t4("di", 0b0110, 0b1000, 0b1110, a, b, c, input("e")));
    connect_input(ram, ctx->id("A_BM[0]"), vcc());
    output("q", connect_output(ram, ctx->id("A_DO[0]")));
    output("y", lut2("out", 0b0110, a, c));
    ASSERT_TRUE(packs());
    // One CPE half per RAM input and output pad: the RAM inputs EN, WE, ADDR[7], DI[0] and BM[0],
    // the output pads q and y
    int halves = 0;
    for (auto &cell : ctx->cells)
        halves += cell.second->type == id_CPE_L2T4;
    EXPECT_EQ(halves, 7);
}

// An inverter on a clock: registers and RAMs can invert their clock themselves, so the global
// clock can reach them directly, without a LUT that turns it into a fabric clock. (With the
// inversion given in the registers and the RAM instead, it does.)
class GateMateClockInversionTest : public GateMateQorTest, public ::testing::WithParamInterface<bool>
{
};

TEST_P(GateMateClockInversionTest, global_clock_reaches_registers_and_rams)
{
    bool inverter = GetParam();
    NetInfo *clk = clock_pin();
    NetInfo *nclk = inverter ? lut1("inv", 0b01, clk) : clk;
    CellInfo *ff = dff("ff", input("d"), nclk);
    ff->params[id_CLK_INV] = Property(!inverter, 1);
    output("q", ff->getPort(id_Q));
    CellInfo *ram = ctx->createCell(ctx->id("ram"), id_CC_BRAM_20K);
    ram->params[id_RAM_MODE] = Property("TDP");
    ram->params[id_A_RD_WIDTH] = Property(1, 32);
    ram->params[id_A_WR_WIDTH] = Property(1, 32);
    ram->params[id_A_CLK_INV] = Property(!inverter, 1);
    connect_input(ram, id_A_CLK, nclk);
    connect_input(ram, id_A_EN, vcc());
    connect_input(ram, id_A_WE, gnd());
    connect_input(ram, ctx->id("A_ADDR[7]"), input("a"));
    output("qr", connect_output(ram, ctx->id("A_DO[0]")));
    ASSERT_TRUE(packs());
    EXPECT_EQ(ctx->cells.count(ctx->id("inv")), 0u) << "the clock inverter is a LUT of its own";
    const NetInfo *ff_clk = ctx->cells.at(ctx->id("ff"))->getPort(id_CLK_INT);
    ASSERT_NE(ff_clk, nullptr);
    EXPECT_EQ(ff_clk->driver.cell->type.str(ctx), "GLBOUT") << "register clock " << ff_clk->name.c_str(ctx);
    for (auto &c : ctx->cells) {
        if (!c.second->type.in(id_RAM, id_RAM_HALF))
            continue;
        for (auto &p : c.second->ports)
            if (p.second.net && boost::starts_with(p.first.str(ctx), "CLK"))
                ADD_FAILURE() << "RAM clocked from the fabric through " << p.first.c_str(ctx) << " ("
                              << p.second.net->name.c_str(ctx) << ")";
    }
}

INSTANTIATE_TEST_SUITE_P(Inversion, GateMateClockInversionTest, ::testing::Values(false, true),
                         [](const ::testing::TestParamInfo<bool> &info) {
                             return info.param ? "inverter_lut" : "inversion_in_cells";
                         });

// What cpe_half_bound and cpe_halves_used count (used by the fuzzer, see
// GateMateRandomLogicTest.cpe_use_within_bound), on a small design: four LUTs each only driving a
// register, a LUT driving two registers, and the six registers on output pads.
TEST_F(GateMateQorTest, cpe_bound_and_use)
{
    NetInfo *clk = clock_pin();
    NetInfo *a = input("a"), *b = input("b");
    for (int i = 0; i < 4; i++)
        output(stringf("q%d", i),
               dff(stringf("ff%d", i), lut2(stringf("lut%d", i), 0b0110 ^ i, a, b), clk)->getPort(id_Q));
    NetInfo *shared = lut2("shared", 0b1000, a, b);
    output("qa", dff("ffa", shared, clk)->getPort(id_Q));
    output("qb", dff("ffb", shared, clk)->getPort(id_Q));
    // LUTs 0-3 share their register's half (0), the shared LUT 1, six registers, six output
    // pads and the clock buffer, the constants 2
    EXPECT_EQ(cpe_half_bound(), 0 + 1 + 6 + 6 + 1 + 2);
    ASSERT_TRUE(packs());
    ctx->place();
    // The LUTs with their registers (4), the shared LUT with one register and a pass-through for
    // the other (2 or 3, depending on whether one register shares the LUT's half), the output pads
    // (6); the clock comes from a clock pin
    EXPECT_GE(cpe_halves_used(), 12);
    EXPECT_LE(cpe_halves_used(), 13);
}
