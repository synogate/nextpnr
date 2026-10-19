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
