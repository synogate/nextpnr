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

// User errors in options, netlists and cell parameters must be reported with a clear message
// (and no crash or silent misbehaviour).

#include "known_bugs.h"
#include "testing.h"
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

// --vopt options

TEST_F(GateMateTest, performance_and_timing_modes)
{
    struct Case
    {
        const char *fpga_mode, *time_mode;
        int fpga, timing;
    };
    for (auto c : {Case{"1", "1", 1, 1}, Case{"economy", "typical", 2, 2}, Case{"speed", "worst", 3, 3},
                   Case{"lowpower", "3", 1, 3}}) {
        SCOPED_TRACE(stringf("fpga_mode=%s time_mode=%s", c.fpga_mode, c.time_mode));
        init_context("CCGM1A1", {stringf("--fpga_mode=%s", c.fpga_mode), stringf("--time_mode=%s", c.time_mode)});
        EXPECT_EQ(impl->fpga_mode, c.fpga);
        EXPECT_EQ(impl->timing_mode, c.timing);
    }
}

TEST_F(GateMateTest, invalid_performance_mode)
{
    EXPECT_LOG_ERROR(init_context("CCGM1A1", {"--fpga_mode=turbo"}),
                     "Invalid mode='turbo', performance valid values are {1:lowpower, 2:economy, 3:speed}.");
    EXPECT_LOG_ERROR(init_context("CCGM1A1", {"--fpga_mode=4"}), "Invalid mode='4'");
}

TEST_F(GateMateTest, invalid_timing_mode)
{
    EXPECT_LOG_ERROR(init_context("CCGM1A1", {"--time_mode=fast"}),
                     "Invalid mode='fast', timing valid values are {1:best, 2:typical, 3:worst}.");
}

TEST_F(GateMateTest, invalid_forced_die)
{
    EXPECT_LOG_ERROR(init_context("CCGM1A1", {"--force_die=1B"}), "Unable to select forced die '1B'.");
}

TEST_F(GateMateTest, invalid_strategy)
{
    init_context("CCGM1A2", {"--allow-unconstrained", "--strategy=everywhere"});
    output("o", input("i"));
    EXPECT_LOG_ERROR(pack(), "Unknown value='everywhere' for 'strategy' option.");
}

TEST_F(GateMateTest, forced_die_with_full_strategy)
{
    init_context("CCGM1A2", {"--allow-unconstrained", "--strategy=full", "--force_die=1B"});
    output("o", input("i"));
    EXPECT_LOG_ERROR(pack(), "Not allowed to use forced die in FULL USE mode.");
}

TEST_F(GateMateTest, clk1_strategy_without_clock)
{
    init_context("CCGM1A2", {"--allow-unconstrained", "--strategy=clk1"});
    output("o", input("i"));
    EXPECT_LOG_ERROR(pack(), "Unable to use REUSE CLK1 strategy when there is no clock/PLL.");
}

TEST_F(GateMateTest, clk1_strategy_with_two_clocks)
{
    init_context("CCGM1A2", {"--allow-unconstrained", "--strategy=clk1"});
    output("q1", dff("ff1", input("d"), global_clock("clk1"))->getPort(id_Q));
    output("q2", dff("ff2", input("e"), global_clock("clk2"))->getPort(id_Q));
    EXPECT_LOG_ERROR(pack(), "Unable to use REUSE CLK1 strategy when there is more than one clock/PLL.");
}

// Top level ports

TEST_F(GateMateTest, port_without_buffer)
{
    PortInfo &pi = ctx->ports[ctx->id("p")];
    pi.name = ctx->id("p");
    pi.type = PORT_IN;
    EXPECT_LOG_ERROR(pack(), "Port 'p' doesn't seem to have a corresponding top level IO.");
}

// pack_io.cc: the frontend's cell for a top level input that drives logic directly (no IO buffer
// from synthesis, e.g. synth_gatemate -noiopad) is removed, which leaves the logic's input
// undriven without any message. Expected: an error (or an inserted IO buffer).
TEST_F(GateMateTest, port_driving_logic_without_buffer)
{
    NetInfo *pad = create_net("p$pad");
    CellInfo *port = ctx->createCell(ctx->id("p"), ctx->id("$nextpnr_ibuf"));
    port->addOutput(id_O);
    port->connectPort(id_O, pad);
    PortInfo &pi = ctx->ports[ctx->id("p")];
    pi.name = ctx->id("p");
    pi.type = PORT_IN;
    pi.net = pad;
    output("o", lut1("inv", 0b01, pad));
    bool failed = false;
    try {
        pack();
    } catch (const log_execution_error_exception &) {
        failed = true;
    }
    if (!failed) {
        // Packed: then the inverter must still be driven by something
        NetInfo *in = nullptr;
        for (auto &cell : ctx->cells)
            for (auto &p : cell.second->ports)
                if (p.second.net && p.second.net->name == ctx->id("p$pad") && p.second.type == PORT_IN)
                    in = p.second.net;
        ASSERT_NE(in, nullptr);
        EXPECT_NE(in->driver.cell, nullptr) << "the logic's input is left undriven";
    }
}

TEST_F(GateMateTest, port_with_wrong_cell_type)
{
    output("o", lut1("p", 0b01, input("i")));
    PortInfo &pi = ctx->ports[ctx->id("p")];
    pi.name = ctx->id("p");
    pi.type = PORT_IN;
    EXPECT_LOG_ERROR(pack(), "Port 'p' doesn't seem to have a corresponding top level IO (internal cell type mismatch).");
}

TEST_F(GateMateTest, port_with_two_input_buffers)
{
    NetInfo *y = top_input("p");
    CellInfo *second = create_cell_ptr(id_CC_IBUF, "second_ibuf");
    second->connectPort(id_I, ctx->nets.at(ctx->id("p$pad")).get());
    second->connectPort(id_Y, create_net("second$y"));
    output("o", y);
    EXPECT_LOG_ERROR(pack(), "Top level pin 'p' has multiple input buffers.");
}

// IO buffer parameters (from the Verilog source)

TEST_F(GateMateTest, unknown_pin_name)
{
    output("o", input("a"));
    ctx->cells.at(ctx->id("a"))->params[id_PIN_NAME] = Property("IO_XX_Z9");
    EXPECT_LOG_ERROR(pack(), "Unknown PIN_NAME 'IO_XX_Z9' for cell 'a'.");
}

// Pins given by PIN_NAME parameters must not be used for IOs placed automatically
TEST_F(GateMateTest, pin_name_and_unconstrained_ios)
{
    // The pins unconstrained IOs get first
    std::vector<std::string> first;
    for (auto pad : impl->available_pads)
        if (pad.str(ctx).rfind("SER_CLK", 0) != 0 && first.size() < 2)
            first.push_back(pad.str(ctx));
    ASSERT_EQ(first.size(), 2u);
    NetInfo *a = input("a");
    NetInfo *b = input("b");
    output("o", lut2("and", 0b1000, a, b));
    ctx->cells.at(ctx->id("b"))->params[id_PIN_NAME] = Property(first[0]);
    ctx->cells.at(ctx->id("o"))->params[id_PIN_NAME] = Property(first[1]);
    ASSERT_TRUE(packs());
    EXPECT_EQ(package_pin("b"), first[0]);
    EXPECT_EQ(package_pin("o"), first[1]);
    EXPECT_NE(package_pin("a"), first[0]);
    EXPECT_NE(package_pin("a"), first[1]);
}

TEST_F(GateMateTest, two_buffers_on_one_pin)
{
    auto pins = package_pins();
    output("o", lut2("and", 0b1000, input("a"), input("b")));
    ctx->cells.at(ctx->id("a"))->params[id_PIN_NAME] = Property(pins[0]);
    ctx->cells.at(ctx->id("b"))->params[id_PIN_NAME] = Property(pins[0]);
    EXPECT_LOG_ERROR(pack(), "because it's already taken by");
}

TEST_F(GateMateTest, invalid_slew_parameter)
{
    output("o", input("a"))->params[id_SLEW] = Property("MEDIUM");
    EXPECT_LOG_ERROR(pack(), "Unknown value 'MEDIUM' for SLEW parameter of 'o' cell.");
}

TEST_F(GateMateTest, invalid_drive_parameter)
{
    output("o", input("a"))->params[id_DRIVE] = Property(5, 32);
    EXPECT_LOG_ERROR(pack(), "Unsupported value '5' for DRIVE parameter of 'o' cell.");
}

TEST_F(GateMateTest, invalid_pull_parameter)
{
    output("o", input("a"));
    ctx->cells.at(ctx->id("a"))->params[id_PULLUP] = Property(2, 32);
    EXPECT_LOG_ERROR(pack(), "Unsupported value '2' for PULLUP parameter of 'a' cell.");
}

// KEEPER is implemented with both pull resistors, so it cannot be combined with either of them
// (as the error message says). Currently only the combination with both is rejected; a KEEPER with
// one pull resistor silently becomes a keeper. Whether that is intended is an open question.
TEST_F(GateMateTest, keeper_with_pull_up)
{
    output("o", input("a"));
    CellInfo *ibuf = ctx->cells.at(ctx->id("a")).get();
    ibuf->params[id_KEEPER] = Property(1, 1);
    ibuf->params[id_PULLUP] = Property(1, 1);
    EXPECT_LOG_ERROR(pack(), "PULLUP/PULLDOWN and KEEPER are mutually exclusive parameters, issue for 'a' cell.");
}

TEST_F(GateMateTest, keeper_with_pull_up_and_down)
{
    output("o", input("a"));
    CellInfo *ibuf = ctx->cells.at(ctx->id("a")).get();
    ibuf->params[id_KEEPER] = Property(1, 1);
    ibuf->params[id_PULLUP] = Property(1, 1);
    ibuf->params[id_PULLDOWN] = Property(1, 1);
    EXPECT_LOG_ERROR(pack(), "PULLUP/PULLDOWN and KEEPER are mutually exclusive parameters, issue for 'a' cell.");
}

TEST_F(GateMateTest, ser_clk_on_output)
{
    output("o", input("a"))->params[id_PIN_NAME] = Property("SER_CLK");
    EXPECT_LOG_ERROR(pack(), "SER_CLK and SER_CLK_N pins can only be used on input port.");
}

TEST_F(GateMateTest, more_ios_than_pads)
{
    int pads = int(package_pins().size());
    NetInfo *a = input("a");
    for (int i = 0; i < pads; i++)
        output(stringf("o%d", i), a);
    EXPECT_LOG_ERROR(pack(), "No more pads available.");
}

// LVDS pairs

class GateMateLvdsTest : public GateMateTest
{
  protected:
    void lvds_input(const std::string &p, const std::string &n)
    {
        CellInfo *ibuf = create_cell_ptr(id_CC_LVDS_IBUF, "lvds");
        if (!p.empty())
            ibuf->params[id_PIN_NAME_P] = Property(p);
        if (!n.empty())
            ibuf->params[id_PIN_NAME_N] = Property(n);
        NetInfo *y = create_net("lvds$y");
        ibuf->connectPort(id_Y, y);
        output("o", lut1("inv", 0b01, y));
    }
};

TEST_F(GateMateLvdsTest, valid_pair)
{
    lvds_input("IO_SA_A0", "IO_SA_B0");
    ASSERT_TRUE(packs());
    CellInfo *iosel = ctx->cells.at(ctx->id("lvds$iosel")).get();
    EXPECT_TRUE(bool_or_default(iosel->params, id_LVDS_EN, false));
    EXPECT_TRUE(bool_or_default(iosel->params, id_LVDS_IE, false));
    EXPECT_EQ(package_pin("lvds"), "IO_SA_A0");
}

TEST_F(GateMateLvdsTest, missing_pin)
{
    lvds_input("IO_SA_A0", "");
    EXPECT_LOG_ERROR(pack(), "Both LVDS pins must be set to a valid locations.");
}

TEST_F(GateMateLvdsTest, pins_of_different_pairs)
{
    lvds_input("IO_SA_A0", "IO_SA_B1");
    EXPECT_LOG_ERROR(pack(), "LVDS pads 'IO_SA_A0' and 'IO_SA_B1' do not match.");
}

TEST_F(GateMateLvdsTest, positive_pin_of_type_b)
{
    lvds_input("IO_SA_B0", "IO_SA_A0");
    EXPECT_LOG_ERROR(pack(), "LVDS positive pad must be from type A.");
}

// Double data rate cells

TEST_F(GateMateTest, ddr_cell_not_at_pin)
{
    NetInfo *q = oddr("oddr", input("d0"), input("d1"), input("clk"), input("sel"));
    output("o", lut2("and", 0b1000, q, input("e")));
    EXPECT_LOG_ERROR(pack(), "Cell 'oddr' of type CC_ODDR is not connected to GPIO pin.");
}

TEST_F(GateMateTest, ddr_outputs_of_a_bank_need_the_same_select)
{
    NetInfo *clk = input("clk");
    output("o1", oddr("oddr1", input("a0"), input("a1"), clk, input("sel1")))->params[id_PIN_NAME] =
            Property("IO_SA_A0");
    output("o2", oddr("oddr2", input("b0"), input("b1"), clk, input("sel2")))->params[id_PIN_NAME] =
            Property("IO_SA_B0");
    EXPECT_LOG_ERROR(pack(), "DDR port use signal different than already occupied DDR source.");
}

// Die assignment by attribute

TEST_F(GateMateTest, unknown_die_attribute)
{
    output("o", lut1("lut", 0b01, input("a")));
    ctx->cells.at(ctx->id("lut"))->attrs[id_GATEMATE_DIE] = Property("9Z");
    EXPECT_LOG_ERROR(pack(), "Trying to assign cell 'lut' to non existing die '9Z'.");
}

// Block RAM and FIFO parameters

class GateMateRamParamTest : public GateMateTest
{
  protected:
    CellInfo *ram(IdString type, const std::vector<std::pair<IdString, Property>> &params)
    {
        CellInfo *cell = ctx->createCell(ctx->id("ram"), type);
        for (auto &p : params)
            cell->params[p.first] = p.second;
        return cell;
    }
};

TEST_F(GateMateRamParamTest, unknown_write_mode)
{
    ram(id_CC_BRAM_20K, {{id_A_WR_MODE, Property("READ_FIRST")}});
    EXPECT_LOG_ERROR(pack(), "Unknown A_WR_MODE parameter value 'READ_FIRST' for cell ram.");
    init_context();
    ram(id_CC_BRAM_20K, {{id_B_WR_MODE, Property("READ_FIRST")}});
    EXPECT_LOG_ERROR(pack(), "Unknown B_WR_MODE parameter value 'READ_FIRST' for cell ram.");
}

TEST_F(GateMateRamParamTest, unsupported_width)
{
    ram(id_CC_BRAM_40K, {{id_A_RD_WIDTH, Property(81, 32)}});
    EXPECT_LOG_ERROR(pack(), "Unsupported width '81'.");
}

TEST_F(GateMateRamParamTest, unknown_cascade)
{
    ram(id_CC_BRAM_40K, {{id_CAS, Property("MIDDLE")}, {id_RAM_MODE, Property("TDP")}});
    EXPECT_LOG_ERROR(pack(), "Unknown CAS parameter value 'MIDDLE' for cell ram.");
}

TEST_F(GateMateRamParamTest, cascade_upper_without_lower)
{
    ram(id_CC_BRAM_40K, {{id_CAS, Property("UPPER")}, {id_RAM_MODE, Property("TDP")}});
    EXPECT_LOG_ERROR(pack(), "Port A_CI of 'ram' must be driven by other CC_BRAM_40K.");
}

TEST_F(GateMateRamParamTest, cascade_lower_without_upper)
{
    ram(id_CC_BRAM_40K, {{id_CAS, Property("LOWER")}, {id_RAM_MODE, Property("TDP")}});
    EXPECT_LOG_ERROR(pack(), "Port A_CO of 'ram' must be driving one other CC_BRAM_40K.");
}

TEST_F(GateMateRamParamTest, fifo_widths_must_match)
{
    ram(id_CC_FIFO_40K, {{id_A_WIDTH, Property(40, 32)}, {id_B_WIDTH, Property(20, 32)}});
    EXPECT_LOG_ERROR(pack(), "The FIFO configuration of A_WIDTH and B_WIDTH must be equal.");
}

TEST_F(GateMateRamParamTest, fifo_sdp_needs_80_bits)
{
    ram(id_CC_FIFO_40K, {{id_A_WIDTH, Property(40, 32)}, {id_B_WIDTH, Property(40, 32)}, {id_RAM_MODE, Property("SDP")}});
    EXPECT_LOG_ERROR(pack(), "FIFO SDP is only supported in 80 bit mode.");
}

TEST_F(GateMateRamParamTest, unknown_fifo_mode)
{
    ram(id_CC_FIFO_40K, {{id_A_WIDTH, Property(40, 32)},
                         {id_B_WIDTH, Property(40, 32)},
                         {id_RAM_MODE, Property("TDP")},
                         {id_FIFO_MODE, Property("SEMISYNC")}});
    EXPECT_LOG_ERROR(pack(), "Unknown FIFO_MODE parameter value 'SEMISYNC' for cell ram.");
}

TEST_F(GateMateRamParamTest, invalid_dynamic_offset_select)
{
    ram(id_CC_FIFO_40K, {{id_A_WIDTH, Property(40, 32)},
                         {id_B_WIDTH, Property(40, 32)},
                         {id_RAM_MODE, Property("TDP")},
                         {id_DYN_STAT_SELECT, Property(2, 32)}});
    EXPECT_LOG_ERROR(pack(), "DYN_STAT_SELECT must be 0 or 1.");
}

TEST_F(GateMateRamParamTest, dynamic_offsets_not_in_sdp)
{
    ram(id_CC_FIFO_40K, {{id_A_WIDTH, Property(80, 32)},
                         {id_B_WIDTH, Property(80, 32)},
                         {id_RAM_MODE, Property("SDP")},
                         {id_DYN_STAT_SELECT, Property(1, 32)}});
    EXPECT_LOG_ERROR(pack(), "Dynamic FIFO offset configuration is not supported in SDP mode.");
}
