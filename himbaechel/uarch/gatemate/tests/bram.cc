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

#include <set>
#include "testing.h"
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

class GateMateBramTest : public GateMateTest
{
  protected:
    // A cascaded pair of CC_BRAM_40K in TDP mode, with the given widths on all ports
    // (parameters as written by yosys).
    void cascaded_pair(int width)
    {
        CellInfo *lower = ctx->createCell(ctx->id("ram_lower"), id_CC_BRAM_40K);
        CellInfo *upper = ctx->createCell(ctx->id("ram_upper"), id_CC_BRAM_40K);
        for (CellInfo *ram : {lower, upper}) {
            ram->params[id_RAM_MODE] = Property("TDP");
            for (auto p : {id_A_RD_WIDTH, id_A_WR_WIDTH, id_B_RD_WIDTH, id_B_WR_WIDTH})
                ram->params[p] = Property(width, 32);
        }
        lower->params[id_CAS] = Property("LOWER");
        upper->params[id_CAS] = Property("UPPER");
        connect_input(upper, id_A_CI, connect_output(lower, id_A_CO));
        connect_input(upper, id_B_CI, connect_output(lower, id_B_CO));
    }
};

TEST_F(GateMateBramTest, cascade_one_bit_width)
{
    cascaded_pair(1);
    EXPECT_NO_THROW(pack());
}

TEST_F(GateMateBramTest, cascade_rejects_wide_ports)
{
    cascaded_pair(40);
    EXPECT_LOG_ERROR(pack(), "Cascade feature only supported in 1 bit data width mode");
}

TEST_F(GateMateBramTest, cascade_rejects_sdp)
{
    cascaded_pair(1);
    ctx->cells.at(ctx->id("ram_lower"))->params[id_RAM_MODE] = Property("SDP");
    ctx->cells.at(ctx->id("ram_upper"))->params[id_RAM_MODE] = Property("SDP");
    EXPECT_LOG_ERROR(pack(), "Cascade feature only supported in TDP mode");
}

TEST_F(GateMateBramTest, cascade_only_for_40k)
{
    CellInfo *ram = ctx->createCell(ctx->id("ram"), id_CC_BRAM_20K);
    ram->params[id_CAS] = Property("UPPER");
    EXPECT_LOG_ERROR(pack(), "Cascade feature only supported for CC_BRAM_40K");
}

TEST_F(GateMateBramTest, unknown_ram_mode)
{
    CellInfo *ram = ctx->createCell(ctx->id("ram"), id_CC_BRAM_20K);
    ram->params[id_RAM_MODE] = Property("XDP");
    EXPECT_LOG_ERROR(pack(), "Unknown RAM_MODE parameter value 'XDP' for cell ram");
}

// pack_cpe.cc move_ram_io: a RAM input on a net without a driver (an undriven wire in the
// design) dereferences the missing driver: segmentation fault. Packing should either succeed or
// report an error.
TEST_F(GateMateBramTest, input_from_undriven_net)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
            {
                CellInfo *ram = ctx->createCell(ctx->id("ram"), id_CC_BRAM_20K);
                ram->params[id_RAM_MODE] = Property("TDP");
                for (auto p : {id_A_RD_WIDTH, id_A_WR_WIDTH})
                    ram->params[p] = Property(5, 32);
                connect_input(ram, id_A_CLK, global_clock("clk"));
                connect_input(ram, ctx->id("A_ADDR[7]"), create_net("undriven"));
                output("o", connect_output(ram, ctx->id("A_DO[0]")));
                try {
                    pack();
                } catch (const log_execution_error_exception &) {
                }
                std::exit(0);
            },
            ::testing::ExitedWithCode(0), "");
}

// Bug 39: after placement, repack_ram merges each block's RAM halves into a new RAM cell and
// deletes them, but the CPEs at the RAM's pins keep the deleted half's name as their cluster, so
// getClusterRootCell() throws std::out_of_range for them.
TEST_F(GateMateBramTest, ram_pin_cpes_keep_a_live_cluster_root_after_placement)
{
    CellInfo *ram = ctx->createCell(ctx->id("ram"), id_CC_BRAM_20K);
    ram->params[id_RAM_MODE] = Property("TDP");
    for (auto p : {id_A_RD_WIDTH, id_A_WR_WIDTH})
        ram->params[p] = Property(5, 32);
    connect_input(ram, id_A_CLK, global_clock("clk"));
    connect_input(ram, ctx->id("A_ADDR[7]"), input("a"));
    output("o", connect_output(ram, ctx->id("A_DO[0]")));
    ctx->pack();
    ctx->place();
    int clustered = 0;
    for (auto &cell : ctx->cells) {
        if (cell.second->cluster == ClusterId())
            continue;
        clustered++;
        EXPECT_TRUE(ctx->cells.count(cell.second->cluster))
                << cell.first.c_str(ctx) << " is in the cluster of " << cell.second->cluster.c_str(ctx) << ", deleted";
    }
    EXPECT_GT(clustered, 0);
}

// A RAM input is fed through a CPE at the pin, whose LUT half takes the LUT driving the input. Where that LUT
// fed other cells too, the half only passed its result on: one more level of logic and one more connection on
// every path into the pin (on dmg_soc the address of the instruction cache, which ends the worst path in 11 of
// 16 seeds, comes from such a LUT). The pin then gets a copy of the LUT, and the LUT stays for its other users.
class GateMatePinLutTest : public GateMateBramTest
{
  protected:
    CellInfo *ram = nullptr;
    NetInfo *a = nullptr, *b = nullptr;

    // A RAM with the address bits `bits` from one LUT2 `f` of the inputs a and b, which feeds another LUT
    // too if `other_user`
    void build(const std::vector<int> &bits, bool other_user, bool keep = false)
    {
        ram = ctx->createCell(ctx->id("ram"), id_CC_BRAM_20K);
        ram->params[id_RAM_MODE] = Property("TDP");
        for (auto p : {id_A_RD_WIDTH, id_A_WR_WIDTH})
            ram->params[p] = Property(5, 32);
        connect_input(ram, id_A_CLK, global_clock("clk"));
        a = input("a");
        b = input("b");
        NetInfo *f = lut2("f", 0b0110, a, b);
        if (keep)
            ctx->cells.at(ctx->id("f"))->attrs[ctx->id("keep")] = Property(1, 1);
        for (int bit : bits)
            connect_input(ram, ctx->idf("A_ADDR[%d]", bit), f);
        if (other_user)
            output("g_out", lut2("g", 0b1000, f, input("c")));
        output("o", connect_output(ram, ctx->id("A_DO[0]")));
        pack();
    }

    // The LUT half of the CPE at the pin of address bit `bit`
    const CellInfo *half_at(int bit)
    {
        NetInfo *pin = ram->getPort(ctx->idf("ADDRA0[%d]", bit));
        if (!pin || !pin->driver.cell || pin->driver.cell->type != id_CPE_RAMO)
            return nullptr;
        NetInfo *in = pin->driver.cell->getPort(id_I);
        return in ? in->driver.cell : nullptr;
    }

    // The half computes f from a and b itself
    void expect_the_lut(const CellInfo *half, const std::string &what)
    {
        ASSERT_NE(half, nullptr) << what;
        EXPECT_EQ(half->type, id_CPE_L2T4) << what;
        EXPECT_EQ(half->getPort(id_D0_10), a) << what;
        EXPECT_EQ(half->getPort(id_D1_10), b) << what;
        EXPECT_EQ(int_or_default(half->params, id_INIT_L10, -1), 0b0110) << what;
        EXPECT_EQ(half->cluster, ram->cluster) << what;
    }

    int cells_fed_by(const NetInfo *net, IdString type)
    {
        int n = 0;
        for (auto &user : net->users)
            n += user.cell->type == type;
        return n;
    }
};

TEST_F(GateMatePinLutTest, lut_feeding_a_pin_and_another_cell_is_copied_to_the_pin)
{
    build({7}, true);
    const CellInfo *half = half_at(7);
    expect_the_lut(half, "the half at the pin");
    // The LUT is still there for its other user, free to be placed
    ASSERT_TRUE(ctx->cells.count(ctx->id("f")));
    const CellInfo *f = ctx->cells.at(ctx->id("f")).get();
    EXPECT_NE(half, f);
    EXPECT_EQ(f->cluster, ClusterId());
    ASSERT_NE(f->getPort(id_OUT), nullptr);
    EXPECT_EQ(f->getPort(id_OUT)->users.entries(), 1u);
    EXPECT_EQ(cells_fed_by(a, id_CPE_L2T4), 2);
}

TEST_F(GateMatePinLutTest, lut_feeding_two_pins_is_at_both)
{
    build({7, 8}, false);
    expect_the_lut(half_at(7), "the half at the first pin");
    expect_the_lut(half_at(8), "the half at the second pin");
    EXPECT_NE(half_at(7), half_at(8));
    // One of the two is the LUT itself: no third one is left
    EXPECT_EQ(cells_fed_by(a, id_CPE_L2T4), 2);
    EXPECT_EQ(cells_fed_by(b, id_CPE_L2T4), 2);
}

TEST_F(GateMatePinLutTest, lut_feeding_one_pin_alone_moves_to_the_pin)
{
    build({7}, false);
    const CellInfo *half = half_at(7);
    expect_the_lut(half, "the half at the pin");
    EXPECT_EQ(half->name, ctx->id("f"));
    EXPECT_EQ(cells_fed_by(a, id_CPE_L2T4), 1);
}

// A LUT marked to be kept is not copied, and with --vopt no-pin-lut-copies none is: the half at the pin passes
// the LUT's result on
TEST_F(GateMatePinLutTest, kept_lut_is_not_copied)
{
    build({7}, true, true);
    const CellInfo *half = half_at(7);
    ASSERT_NE(half, nullptr);
    ASSERT_TRUE(ctx->cells.count(ctx->id("f")));
    EXPECT_EQ(half->getPort(id_D0_10), ctx->cells.at(ctx->id("f"))->getPort(id_OUT));
    EXPECT_EQ(half->getPort(id_D1_10), nullptr);
    EXPECT_EQ(cells_fed_by(a, id_CPE_L2T4), 1);
}

TEST_F(GateMatePinLutTest, no_copies_with_the_option_off)
{
    init_context("CCGM1A1", {"--allow-unconstrained", "--no-pin-lut-copies"});
    build({7, 8}, true);
    ASSERT_TRUE(ctx->cells.count(ctx->id("f")));
    const NetInfo *f = ctx->cells.at(ctx->id("f"))->getPort(id_OUT);
    for (int bit : {7, 8}) {
        const CellInfo *half = half_at(bit);
        ASSERT_NE(half, nullptr) << bit;
        EXPECT_EQ(half->getPort(id_D0_10), f) << bit;
    }
    EXPECT_EQ(cells_fed_by(a, id_CPE_L2T4), 1);
}

// The copies compute what the LUT computes: the placed and routed design against the netlist as it was read
TEST_F(GateMatePinLutTest, design_with_copies_matches_reference)
{
    auto design = [&] {
        CellInfo *mem = ctx->createCell(ctx->id("ram"), id_CC_BRAM_20K);
        mem->params[id_RAM_MODE] = Property("TDP");
        for (auto p : {id_A_RD_WIDTH, id_A_WR_WIDTH})
            mem->params[p] = Property(5, 32);
        NetInfo *clk = global_clock("clk");
        connect_input(mem, id_A_CLK, clk);
        connect_input(mem, id_A_EN, vcc());
        connect_input(mem, id_A_WE, input("we"));
        NetInfo *x = input("x"), *y = input("y"), *z = input("z");
        // Three LUTs, each to two address bits or to an address bit and a data input, and to an output
        NetInfo *f = lut2("f", 0b0110, x, y);
        NetInfo *g = l2t4("g", 0b1000, 0b0110, 0b1110, x, y, z, x);
        NetInfo *h = mx2("h", x, y, z);
        connect_input(mem, ctx->id("A_ADDR[3]"), f);
        connect_input(mem, ctx->id("A_ADDR[4]"), f);
        connect_input(mem, ctx->id("A_ADDR[5]"), g);
        connect_input(mem, ctx->id("A_DI[0]"), g);
        connect_input(mem, ctx->id("A_ADDR[7]"), h);
        connect_input(mem, ctx->id("A_DI[1]"), h);
        for (int i = 2; i < 5; i++)
            connect_input(mem, ctx->idf("A_DI[%d]", i), i == 2 ? x : i == 3 ? y : z);
        for (int i = 0; i < 5; i++) {
            connect_input(mem, ctx->idf("A_BM[%d]", i), vcc());
            output(stringf("o%d", i), connect_output(mem, ctx->idf("A_DO[%d]", i)));
        }
        output("f_out", f);
        output("g_out", g);
        output("h_out", h);
    };
    EXPECT_TRUE(implementation_matches_reference(design, 400, 1));
    int copies = 0;
    for (auto &cell : ctx->cells)
        copies += cell.second->type == id_CPE_L2T4 && cell.second->cluster != ClusterId() &&
                  cell.first.str(ctx).find("_cpe") != std::string::npos && cell.second->getPort(id_D1_10) != nullptr;
    EXPECT_GT(copies, 0);
}

// The LOC parameter of the RAM primitives ("D<die>X<column>Y<row>", cells_sim.v) places the RAM
// in that block. pack_bram.cc reads it, but does not use it.
TEST_F(GateMateBramTest, location_parameter)
{
    // The RAM blocks of the die, by column and row
    std::set<int> xs, ys;
    for (BelId bel : ctx->getBels()) {
        if (ctx->getBelType(bel) == id_RAM) {
            xs.insert(ctx->getBelLocation(bel).x);
            ys.insert(ctx->getBelLocation(bel).y);
        }
    }
    ASSERT_EQ(xs.size(), 4u);
    ASSERT_EQ(ys.size(), 8u);
    std::vector<int> col(xs.begin(), xs.end()), row(ys.begin(), ys.end());
    NetInfo *clk = global_clock("clk");
    std::vector<std::pair<std::string, std::pair<int, int>>> rams = {{"D0X0Y0", {0, 0}}, {"D0X3Y7", {3, 7}}};
    for (size_t i = 0; i < rams.size(); i++) {
        CellInfo *ram = ctx->createCell(ctx->idf("ram%d", int(i)), id_CC_BRAM_40K);
        ram->params[id_RAM_MODE] = Property("TDP");
        ram->params[id_LOC] = Property(rams[i].first);
        for (auto p : {id_A_RD_WIDTH, id_A_WR_WIDTH})
            ram->params[p] = Property(5, 32);
        connect_input(ram, id_A_CLK, clk);
        connect_input(ram, id_A_EN, vcc());
        connect_input(ram, ctx->id("A_ADDR[5]"), input(stringf("a%d", int(i))));
        output(stringf("o%d", int(i)), connect_output(ram, ctx->id("A_DO[0]")));
    }
    pack();
    ctx->place();
    for (size_t i = 0; i < rams.size(); i++) {
        CellInfo *ram = ctx->cells.at(ctx->idf("ram%d", int(i))).get();
        ASSERT_NE(ram->bel, BelId());
        Loc loc = ctx->getBelLocation(ram->bel);
        EXPECT_EQ(loc.x, col[rams[i].second.first]) << rams[i].first;
        EXPECT_EQ(loc.y, row[rams[i].second.second]) << rams[i].first;
    }
}

// The RAM block shares some CPEs between an output and an input pin: F_RD_PTR[0] (FRD_ADDR[0])
// with the read clock from the fabric (CLKA[0]), F_RD_PTR[2] with the pop enable (ENA[0]) and
// F_RD_PTR[5] with the reset (F_RSTN). pack_bram.cc moves each into a CPE of its own (as it does
// with the data pins DOx/DIx, which it pairs), constrained to the same location, so the FIFO
// cannot be placed ("Unable to find legal placement").
TEST_F(GateMateBramTest, fifo_read_pointer_outputs)
{
    for (bool fabric_clock : {false, true}) {
        init_context();
        CellInfo *f = ctx->createCell(ctx->id("fifo"), id_CC_FIFO_40K);
        f->params[id_RAM_MODE] = Property("TDP");
        f->params[id_A_WIDTH] = Property(10, 32);
        f->params[id_B_WIDTH] = Property(10, 32);
        NetInfo *clk = fabric_clock ? input("clk") : global_clock("clk");
        connect_input(f, id_A_CLK, clk);
        connect_input(f, id_B_CLK, clk);
        connect_input(f, id_A_EN, input("pop"));
        connect_input(f, id_B_EN, input("push"));
        connect_input(f, id_B_WE, vcc());
        connect_input(f, id_F_RST_N, input("rst_n"));
        connect_input(f, ctx->id("B_DI[0]"), input("d"));
        output("q", connect_output(f, ctx->id("A_DO[0]")));
        for (int i = 0; i < 16; i++)
            output(stringf("rd_ptr%d", i), connect_output(f, ctx->idf("F_RD_PTR[%d]", i)));
        ASSERT_TRUE(packs());
        EXPECT_NO_THROW(ctx->place()) << (fabric_clock ? "fabric clock" : "global clock");
    }
}

// CC_FIFO_40K defaults to RAM_MODE "SDP" (GateMate primitives library UG1001, table 6.15), as in
// pack_bram.cc, so a FIFO narrower than 80 bits that does not set RAM_MODE is rejected. (The yosys
// simulation model cells_sim.v defaults to "TDP" instead, and yosys does not write default
// parameters into the netlist: such a FIFO simulates as TDP and is rejected by nextpnr.)
TEST_F(GateMateBramTest, fifo_ram_mode_defaults_to_sdp)
{
    CellInfo *f = ctx->createCell(ctx->id("fifo"), id_CC_FIFO_40K);
    f->params[id_A_WIDTH] = Property(40, 32);
    f->params[id_B_WIDTH] = Property(40, 32);
    NetInfo *clk = global_clock("clk");
    connect_input(f, id_A_CLK, clk);
    connect_input(f, id_B_CLK, clk);
    connect_input(f, id_A_EN, input("pop"));
    connect_input(f, id_B_EN, input("push"));
    connect_input(f, id_B_WE, vcc());
    connect_input(f, id_F_RST_N, input("rst_n"));
    connect_input(f, ctx->id("B_DI[0]"), input("d"));
    output("q", connect_output(f, ctx->id("A_DO[0]")));
    EXPECT_LOG_ERROR(pack(), "FIFO SDP is only supported in 80 bit mode");
}
