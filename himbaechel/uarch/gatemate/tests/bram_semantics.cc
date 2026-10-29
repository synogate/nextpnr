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

// Block RAM behaviour: CC_BRAM_20K / CC_BRAM_40K designs, simulated before packing (yosys
// models) and after placement (the RAM cell with its configuration, the CPEs feeding the RAM
// pins and taking its outputs, and two 20K RAMs merged into one block).

#include <random>
#include "netlist_eval.h"
#include "known_bugs.h"
#include "testing.h"
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"
#include "uarch/gatemate/extra_data.h"

USING_NEXTPNR_NAMESPACE

namespace {

struct BramDesign
{
    std::string name;
    bool is_40k = false;
    bool sdp = false;
    // TDP: the read and write width of each port. SDP: the write width of port A and the read
    // width of port B (40 for a 20K, 80 for a 40K).
    int width[2] = {0, 0};
    bool write_through[2] = {false, false};
    bool do_reg[2] = {false, false};
    bool clk_inv[2] = {false, false};
    bool en_inv[2] = {false, false};
    bool we_inv[2] = {false, false};
    bool global_clock = true;
    bool shared_clock = true; // both ports on the same clock
    bool controls_from_pads = false; // enables and write enables never constant
    // Two cascaded 40K blocks (TDP, 1 bit) sharing all inputs, the outputs from the upper block
    bool cascade = false;
    // Choice of the logic driving the RAM inputs, and of the initial memory contents
    uint64_t seed = 1;
};

std::ostream &operator<<(std::ostream &os, const BramDesign &d) { return os << d.name; }

BramDesign tdp(const std::string &name, bool is_40k, int width_a, int width_b)
{
    BramDesign d;
    d.name = name;
    d.is_40k = is_40k;
    d.width[0] = width_a;
    d.width[1] = width_b;
    return d;
}

BramDesign sdp(const std::string &name, bool is_40k)
{
    BramDesign d;
    d.name = name;
    d.is_40k = is_40k;
    d.sdp = true;
    d.width[0] = d.width[1] = is_40k ? 80 : 40;
    return d;
}

// Address bits a port with the given width uses (cells_sim.v)
bool address_bit_used(const BramDesign &d, int width, int bit)
{
    if (d.sdp)
        return bit >= 7;
    int lsb = width <= 1 ? 1 : width <= 2 ? 2 : width <= 5 ? 3 : width <= 10 ? 4 : width <= 20 ? 5 : 6;
    if (!d.is_40k && bit == 6)
        return false;
    return bit >= lsb;
}

class GateMateBramSemanticsTest : public GateMateTest
{
  protected:
    // A RAM called `prefix` configured as `d`, driven by input pads `<prefix>_*`, with its data
    // outputs on output pads. The RAM inputs come from a few shared input pads, directly or
    // through LUTs, multiplexers or constants, so that all address bits vary while the same
    // addresses come up often. The memory starts with random contents. With `clock`, both ports
    // use that clock.
    void build_bram(const std::string &prefix, const BramDesign &d, NetInfo *clock = nullptr)
    {
        std::mt19937_64 rng(d.seed);
        std::vector<CellInfo *> cells;
        if (d.cascade) {
            for (const char *which : {"LOWER", "UPPER"}) {
                CellInfo *cell = ctx->createCell(ctx->idf("%s_%s", prefix.c_str(), which), id_CC_BRAM_40K);
                cell->params[id_CAS] = Property(which);
                cells.push_back(cell);
            }
        } else {
            cells.push_back(ctx->createCell(ctx->id(prefix), d.is_40k ? id_CC_BRAM_40K : id_CC_BRAM_20K));
        }
        // The outputs come from the upper block of a cascade
        CellInfo *ram = cells.back();
        auto set_param = [&](IdString name, const Property &value) {
            for (CellInfo *cell : cells)
                cell->params[name] = value;
        };
        auto connect = [&](IdString port, NetInfo *net) {
            for (CellInfo *cell : cells)
                connect_input(cell, port, net);
        };
        set_param(id_RAM_MODE, Property(d.sdp ? "SDP" : "TDP"));
        int rd[2], wr[2];
        if (d.sdp) {
            wr[0] = d.width[0];
            rd[0] = 0;
            wr[1] = 0;
            rd[1] = d.width[1];
        } else {
            for (int p = 0; p < 2; p++)
                rd[p] = wr[p] = d.width[p];
        }
        for (int p = 0; p < 2; p++) {
            char port = "AB"[p];
            set_param(ctx->idf("%c_RD_WIDTH", port), Property(rd[p], 32));
            set_param(ctx->idf("%c_WR_WIDTH", port), Property(wr[p], 32));
            set_param(ctx->idf("%c_WR_MODE", port), Property(d.write_through[p] ? "WRITE_THROUGH" : "NO_CHANGE"));
            set_param(ctx->idf("%c_DO_REG", port), Property(d.do_reg[p], 1));
            set_param(ctx->idf("%c_CLK_INV", port), Property(d.clk_inv[p], 1));
            set_param(ctx->idf("%c_EN_INV", port), Property(d.en_inv[p], 1));
            set_param(ctx->idf("%c_WE_INV", port), Property(d.we_inv[p], 1));
        }
        int init_count = d.is_40k ? 128 : 64;
        for (CellInfo *cell : cells) {
            for (int i = 0; i < init_count; i++) {
                std::string bits;
                for (int j = 0; j < 320; j++)
                    bits.push_back((rng() & 1) ? '1' : '0');
                cell->params[ctx->idf("INIT_%02X", i)] = Property::from_string(bits);
            }
        }

        std::vector<NetInfo *> pads;
        for (int i = 0; i < 6; i++)
            pads.push_back(input(stringf("%s_i%d", prefix.c_str(), i)));
        std::vector<NetInfo *> signals;
        int count = 0;
        // A signal derived from the pads (all random choices in statement order, so that both
        // builds of the design are the same)
        auto signal = [&]() -> NetInfo * {
            std::string name = stringf("%s_s%d", prefix.c_str(), count++);
            int kind = rng() % 8;
            int n = int(pads.size());
            int ia = rng() % n, ib = rng() % n, ic = rng() % n, id = rng() % n;
            NetInfo *a = pads[ia], *b = pads[ib], *c = pads[ic];
            int init = rng() % 16;
            NetInfo *result;
            switch (kind) {
            case 0:
            case 1:
                result = a;
                break;
            case 2:
            case 3:
                // XOR-like functions so that the result depends on both inputs
                result = lut2(name, (init & 1) ? 0b0110 : 0b1001, a, b);
                break;
            case 4:
                result = mx2(name, a, b, c);
                break;
            case 5:
                result = l2t4(name, 0b0110, init | 0b0110, 0b0110, a, b, c, pads[id]);
                break;
            case 6:
                // Fanout: a signal that also drives other RAM inputs
                result = signals.empty() ? a : signals[rng() % signals.size()];
                break;
            default:
                result = (init & 1) ? vcc() : gnd();
                break;
            }
            signals.push_back(result);
            return result;
        };

        NetInfo *clk[2];
        if (clock) {
            clk[0] = clk[1] = clock;
        } else if (d.global_clock) {
            clk[0] = global_clock(prefix + "_clka");
            clk[1] = d.shared_clock ? clk[0] : global_clock(prefix + "_clkb");
        } else {
            clk[0] = input(prefix + "_clka");
            clk[1] = d.shared_clock ? clk[0] : input(prefix + "_clkb");
        }
        int data_width = d.is_40k ? 40 : 20;
        for (int p = 0; p < 2; p++) {
            char port = "AB"[p];
            connect(ctx->idf("%c_CLK", port), clk[p]);
            // Enables: from a pad, sometimes a constant that keeps the port enabled
            int en_kind = d.controls_from_pads ? 1 : rng() % 4;
            connect(ctx->idf("%c_EN", port),
                          en_kind == 0 ? (d.en_inv[p] ? gnd() : vcc()) : input(stringf("%s_en%c", prefix.c_str(), port)));
            int we_kind = d.controls_from_pads ? 1 : rng() % 4;
            connect(ctx->idf("%c_WE", port),
                          we_kind == 0 ? gnd() : input(stringf("%s_we%c", prefix.c_str(), port)));
            int width = std::max(rd[p], wr[p]);
            for (int i = 0; i < 16; i++) {
                bool used = width > 0 && address_bit_used(d, d.sdp ? d.width[0] : width, i);
                int kind = rng() % 4;
                if (used || kind == 0)
                    connect(ctx->idf("%c_ADDR[%d]", port, i), signal());
                else
                    connect(ctx->idf("%c_ADDR[%d]", port, i), gnd());
            }
            for (int i = 0; i < data_width; i++) {
                connect(ctx->idf("%c_DI[%d]", port, i), signal());
                int bm_kind = rng() % 4;
                connect(ctx->idf("%c_BM[%d]", port, i), bm_kind == 0 ? signal() : vcc());
            }
        }
        if (d.cascade) {
            connect_input(cells[1], id_A_CI, connect_output(cells[0], id_A_CO));
            connect_input(cells[1], id_B_CI, connect_output(cells[0], id_B_CO));
        }
        // Data outputs in use: TDP up to the width of each port; SDP the read width, the lower
        // half on port A's outputs, the upper half on port B's
        for (int p = 0; p < 2; p++) {
            char port = "AB"[p];
            int used = d.sdp ? data_width : d.width[p];
            for (int i = 0; i < used; i++)
                output(stringf("%s_o%c%d", prefix.c_str(), port, i),
                       connect_output(ram, ctx->idf("%c_DO[%d]", port, i)));
        }
    }
};

class GateMateBramConfigTest : public GateMateBramSemanticsTest, public ::testing::WithParamInterface<BramDesign>
{
};

TEST_P(GateMateBramConfigTest, matches_reference)
{
    BramDesign d = GetParam();
    for (uint64_t seed : {1, 2, 3}) {
        d.seed = seed;
        EXPECT_TRUE(implementation_matches_reference([&] { build_bram("ram", d); }, 400, seed)) << "seed " << seed;
    }
}

std::vector<BramDesign> bram_configs()
{
    std::vector<BramDesign> configs;
    for (bool is_40k : {false, true}) {
        std::string size = is_40k ? "40k" : "20k";
        for (int w : {1, 2, 5, 10, 20, 40}) {
            if (w == 40 && !is_40k)
                continue;
            configs.push_back(tdp(stringf("tdp%s_w%d", size.c_str(), w), is_40k, w, w));
            BramDesign wt = tdp(stringf("tdp%s_w%d_write_through", size.c_str(), w), is_40k, w, w);
            wt.write_through[0] = wt.write_through[1] = true;
            configs.push_back(wt);
        }
        // Ports of different widths access the same memory
        configs.push_back(tdp(stringf("tdp%s_w1_w20", size.c_str()), is_40k, 1, 20));
        configs.push_back(tdp(stringf("tdp%s_w5_w2", size.c_str()), is_40k, 5, 2));
        configs.push_back(tdp(stringf("tdp%s_w10_w5", size.c_str()), is_40k, 10, 5));
        configs.push_back(tdp(stringf("tdp%s_only_a", size.c_str()), is_40k, 10, 0));
        configs.push_back(tdp(stringf("tdp%s_only_b", size.c_str()), is_40k, 0, 10));
        configs.push_back(sdp(stringf("sdp%s", size.c_str()), is_40k));
        BramDesign reg = tdp(stringf("tdp%s_output_registers", size.c_str()), is_40k, 10, 5);
        reg.do_reg[0] = reg.do_reg[1] = true;
        configs.push_back(reg);
        BramDesign reg_a = tdp(stringf("tdp%s_output_register_a", size.c_str()), is_40k, 5, 5);
        reg_a.do_reg[0] = true;
        configs.push_back(reg_a);
        BramDesign sdp_reg = sdp(stringf("sdp%s_output_registers", size.c_str()), is_40k);
        sdp_reg.do_reg[0] = sdp_reg.do_reg[1] = true;
        configs.push_back(sdp_reg);
        for (int p = 0; p < 2; p++) {
            char port = "ab"[p];
            BramDesign clk_inv = tdp(stringf("tdp%s_clk_inv_%c", size.c_str(), port), is_40k, 5, 5);
            clk_inv.clk_inv[p] = true;
            configs.push_back(clk_inv);
            BramDesign en_inv = tdp(stringf("tdp%s_en_inv_%c", size.c_str(), port), is_40k, 5, 5);
            en_inv.en_inv[p] = true;
            configs.push_back(en_inv);
            BramDesign we_inv = tdp(stringf("tdp%s_we_inv_%c", size.c_str(), port), is_40k, 5, 5);
            we_inv.we_inv[p] = true;
            configs.push_back(we_inv);
        }
        BramDesign fabric = tdp(stringf("tdp%s_fabric_clock", size.c_str()), is_40k, 5, 10);
        fabric.global_clock = false;
        configs.push_back(fabric);
        BramDesign two = tdp(stringf("tdp%s_two_clocks", size.c_str()), is_40k, 5, 10);
        two.shared_clock = false;
        configs.push_back(two);
        BramDesign two_fabric = tdp(stringf("tdp%s_two_fabric_clocks", size.c_str()), is_40k, 5, 10);
        two_fabric.shared_clock = false;
        two_fabric.global_clock = false;
        configs.push_back(two_fabric);
        BramDesign sdp_two = sdp(stringf("sdp%s_two_clocks", size.c_str()), is_40k);
        sdp_two.shared_clock = false;
        configs.push_back(sdp_two);
    }
    BramDesign cascade = tdp("cascade", true, 1, 1);
    cascade.cascade = true;
    configs.push_back(cascade);
    BramDesign cascade_wt = cascade;
    cascade_wt.name = "cascade_write_through";
    cascade_wt.write_through[0] = cascade_wt.write_through[1] = true;
    configs.push_back(cascade_wt);
    BramDesign cascade_a = tdp("cascade_only_a", true, 1, 0);
    cascade_a.cascade = true;
    configs.push_back(cascade_a);
    BramDesign cascade_reg = cascade;
    cascade_reg.name = "cascade_output_registers";
    cascade_reg.do_reg[0] = cascade_reg.do_reg[1] = true;
    configs.push_back(cascade_reg);
    return configs;
}

INSTANTIATE_TEST_SUITE_P(Configs, GateMateBramConfigTest, ::testing::ValuesIn(bram_configs()),
                         [](const ::testing::TestParamInfo<BramDesign> &info) { return info.param.name; });

// Two 20K RAMs merged into one RAM block after placement
class GateMateBramPairTest : public GateMateBramSemanticsTest,
                             public ::testing::WithParamInterface<std::pair<BramDesign, BramDesign>>
{
};

TEST_P(GateMateBramPairTest, matches_reference)
{
    BramDesign lower = GetParam().first, upper = GetParam().second;
    for (uint64_t seed : {1, 2}) {
        lower.seed = seed;
        upper.seed = seed + 100;
        EXPECT_TRUE(implementation_matches_reference(
                [&] {
                    build_bram("lower", lower);
                    build_bram("upper", upper);
                },
                400, seed, [&] { ASSERT_TRUE(place_ram_pair("lower", "upper")); }))
                << "seed " << seed;
        int rams = 0;
        for (auto &cell : ctx->cells)
            if (cell.second->type == id_RAM)
                rams++;
        EXPECT_EQ(rams, 1) << "the two 20K RAMs should share one block";
    }
}

std::vector<std::pair<BramDesign, BramDesign>> bram_pairs()
{
    std::vector<std::pair<BramDesign, BramDesign>> pairs;
    pairs.emplace_back(tdp("tdp_w5_and_w20", false, 5, 5), tdp("", false, 20, 20));
    pairs.emplace_back(tdp("tdp_w1_and_w10", false, 1, 2), tdp("", false, 10, 5));
    BramDesign wt = tdp("", false, 10, 10);
    wt.write_through[0] = true;
    wt.do_reg[1] = true;
    pairs.emplace_back(tdp("tdp_w20_and_write_through", false, 20, 1), wt);
    pairs.emplace_back(sdp("sdp_and_sdp", false), sdp("", false));
    BramDesign fabric = tdp("", false, 5, 10);
    fabric.global_clock = false;
    pairs.emplace_back(tdp("tdp_global_and_fabric_clock", false, 10, 5), fabric);
    BramDesign inv = tdp("", false, 5, 5);
    inv.clk_inv[0] = inv.en_inv[1] = inv.we_inv[0] = true;
    pairs.emplace_back(tdp("tdp_and_inverted_controls", false, 5, 5), inv);
    return pairs;
}

INSTANTIATE_TEST_SUITE_P(Pairs, GateMateBramPairTest, ::testing::ValuesIn(bram_pairs()),
                         [](const ::testing::TestParamInfo<std::pair<BramDesign, BramDesign>> &info) {
                             return info.param.first.name;
                         });

// Random configurations
class GateMateBramRandomTest : public GateMateBramSemanticsTest, public ::testing::WithParamInterface<int>
{
};

TEST_P(GateMateBramRandomTest, matches_reference)
{
    std::mt19937_64 rng(GetParam());
    BramDesign d;
    d.name = "random";
    d.seed = GetParam();
    d.is_40k = rng() & 1;
    d.sdp = (rng() % 4) == 0;
    static const int widths[] = {0, 1, 2, 5, 10, 20, 40};
    for (int p = 0; p < 2; p++) {
        if (d.sdp)
            d.width[p] = d.is_40k ? 80 : 40;
        else
            d.width[p] = widths[rng() % (d.is_40k ? 7 : 6)];
        d.write_through[p] = !d.sdp && (rng() & 1);
        d.do_reg[p] = (rng() % 4) == 0;
        d.clk_inv[p] = (rng() % 4) == 0;
        d.en_inv[p] = (rng() % 4) == 0;
        d.we_inv[p] = (rng() % 4) == 0;
    }
    d.global_clock = rng() & 1;
    d.shared_clock = rng() & 1;
    if (d.width[0] == 0 && d.width[1] == 0)
        d.width[0] = 1;
    EXPECT_TRUE(implementation_matches_reference([&] { build_bram("ram", d); }, 400, GetParam()))
            << (d.is_40k ? "40K " : "20K ") << (d.sdp ? "SDP" : "TDP") << " widths " << d.width[0] << "/"
            << d.width[1] << " write_through " << d.write_through[0] << d.write_through[1] << " do_reg "
            << d.do_reg[0] << d.do_reg[1] << " inv clk/en/we " << d.clk_inv[0] << d.clk_inv[1] << d.en_inv[0]
            << d.en_inv[1] << d.we_inv[0] << d.we_inv[1] << " global " << d.global_clock << " shared "
            << d.shared_clock;
}

INSTANTIATE_TEST_SUITE_P(Seeds, GateMateBramRandomTest, ::testing::Range(1, 31));

// Success if `result` reports outputs that differ (not a design that cannot be simulated)
::testing::AssertionResult differs(const ::testing::AssertionResult &result)
{
    std::string message = result.message();
    if (!result && message.find(", expected ") != std::string::npos)
        return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure() << "expected different outputs, got: " << (result ? "outputs match" : message);
}

// Negative controls: the comparison notices a packed RAM that is wired or configured wrongly
class GateMateBramMutationTest : public GateMateBramSemanticsTest
{
  protected:
    // Whether the implementation still matches after `mutate` changes the packed RAM "ram"
    ::testing::AssertionResult matches_after(BramDesign d, const std::function<void(CellInfo *)> &mutate)
    {
        d.controls_from_pads = true;
        return implementation_matches_reference([&] { build_bram("ram", d); }, 400, 1,
                                                [&] { mutate(ctx->cells.at(ctx->id("ram")).get()); });
    }



    void swap_nets(CellInfo *cell, IdString a, IdString b)
    {
        NetInfo *na = cell->getPort(a), *nb = cell->getPort(b);
        ASSERT_NE(na, nullptr);
        ASSERT_NE(nb, nullptr);
        cell->disconnectPort(a);
        cell->disconnectPort(b);
        cell->connectPort(a, nb);
        cell->connectPort(b, na);
    }
};

TEST_F(GateMateBramMutationTest, unchanged)
{
    for (bool is_40k : {false, true})
        EXPECT_TRUE(matches_after(tdp("", is_40k, 5, 10), [](CellInfo *) {}));
}

TEST_F(GateMateBramMutationTest, swapped_address_pins)
{
    for (bool is_40k : {false, true})
        EXPECT_TRUE(differs(matches_after(tdp("", is_40k, 5, 10), [&](CellInfo *ram) {
            swap_nets(ram, ctx->id("ADDRA0[8]"), ctx->id("ADDRA0[10]"));
        })));
}

TEST_F(GateMateBramMutationTest, swapped_data_inputs)
{
    for (bool is_40k : {false, true})
        EXPECT_TRUE(differs(matches_after(tdp("", is_40k, 5, 10), [&](CellInfo *ram) {
            // Rotated, as some data inputs may carry the same signal
            for (int i = 0; i < 4; i++)
                swap_nets(ram, ctx->idf("DIA[%d]", i), ctx->idf("DIA[%d]", i + 1));
        })));
}

TEST_F(GateMateBramMutationTest, swapped_data_outputs)
{
    for (bool is_40k : {false, true})
        EXPECT_TRUE(differs(matches_after(tdp("", is_40k, 5, 10),
                                   [&](CellInfo *ram) { swap_nets(ram, ctx->id("DOB[0]"), ctx->id("DOB[7]")); })));
}

TEST_F(GateMateBramMutationTest, inverted_contents)
{
    for (bool is_40k : {false, true})
        EXPECT_TRUE(differs(matches_after(tdp("", is_40k, 5, 10), [&](CellInfo *ram) {
            for (auto &param : ram->params) {
                if (param.first.str(ctx).rfind("INIT_", 0) != 0)
                    continue;
                std::string bits = param.second.str;
                for (char &c : bits)
                    c = c == '1' ? '0' : '1';
                param.second = Property::from_string(std::string(bits.rbegin(), bits.rend()));
            }
        })));
}

TEST_F(GateMateBramMutationTest, wrong_write_mode)
{
    for (bool is_40k : {false, true})
        EXPECT_TRUE(differs(matches_after(tdp("", is_40k, 5, 10), [&](CellInfo *ram) {
            ram->params[id_RAM_cfg_a0_writemode] = Property(1, 1);
            ram->params[id_RAM_cfg_b0_writemode] = Property(1, 1);
        })));
}

TEST_F(GateMateBramMutationTest, wrong_width)
{
    for (bool is_40k : {false, true})
        EXPECT_TRUE(differs(matches_after(tdp("", is_40k, 5, 10), [&](CellInfo *ram) {
            ram->params[id_RAM_cfg_input_config_a0] = Property(4, 3);
            ram->params[id_RAM_cfg_output_config_a0] = Property(4, 3);
        })));
}

TEST_F(GateMateBramMutationTest, wrong_clock_edge)
{
    for (bool is_40k : {false, true})
        EXPECT_TRUE(differs(matches_after(tdp("", is_40k, 5, 10), [&](CellInfo *ram) {
            ram->params[id_RAM_cfg_inversion_b0] = Property(0b100, 3);
        })));
}

TEST_F(GateMateBramMutationTest, cascade_not_configured)
{
    BramDesign d = tdp("", true, 1, 1);
    d.cascade = true;
    d.controls_from_pads = true;
    EXPECT_TRUE(implementation_matches_reference([&] { build_bram("ram", d); }, 400, 1));
    // The upper block no longer takes the lower block's data
    ::testing::AssertionResult result =
            implementation_matches_reference([&] { build_bram("ram", d); }, 400, 1, [&] {
                ctx->cells.at(ctx->id("ram_UPPER"))->params[id_RAM_cfg_cascade_enable] = Property(0, 2);
            });
    EXPECT_TRUE(differs(result));
}

// The simulator's RAM model against the cells_sim.v behaviour it follows: a write of port A is
// read back on port B, of a different width, at the corresponding address.
TEST_F(GateMateBramSemanticsTest, model_mixed_widths_share_memory)
{
    CellInfo *ram = ctx->createCell(ctx->id("ram"), id_CC_BRAM_20K);
    ram->params[id_RAM_MODE] = Property("TDP");
    for (auto p : {id_A_RD_WIDTH, id_A_WR_WIDTH})
        ram->params[p] = Property(10, 32);
    for (auto p : {id_B_RD_WIDTH, id_B_WR_WIDTH})
        ram->params[p] = Property(5, 32);
    NetInfo *clk = input("clk");
    connect_input(ram, id_A_CLK, clk);
    connect_input(ram, id_B_CLK, clk);
    connect_input(ram, id_A_EN, vcc());
    connect_input(ram, id_B_EN, vcc());
    NetInfo *we = input("we");
    connect_input(ram, id_A_WE, we);
    connect_input(ram, id_B_WE, gnd());
    // Port A writes 10 bits at A_ADDR = 0b1 << 7 (word 2 of 10 bits); port B reads 5 bits at the
    // same position: word 4 of 5 bits (B_ADDR[15:7] = 1, B_ADDR[5:3] = 0) and word 5
    for (int i = 0; i < 16; i++) {
        connect_input(ram, ctx->idf("A_ADDR[%d]", i), i == 7 ? vcc() : gnd());
        connect_input(ram, ctx->idf("B_ADDR[%d]", i), i == 7 ? vcc() : (i == 3 ? input("b3") : gnd()));
    }
    for (int i = 0; i < 10; i++) {
        connect_input(ram, ctx->idf("A_DI[%d]", i), (0b1011001110 >> i) & 1 ? vcc() : gnd());
        connect_input(ram, ctx->idf("A_BM[%d]", i), vcc());
    }
    for (int i = 0; i < 5; i++)
        output(stringf("o%d", i), connect_output(ram, ctx->idf("B_DO[%d]", i)));
    NetlistEvaluator ev(ctx);
    ev.set_input(ctx->id("clk"), false);
    ev.set_input(ctx->id("we"), true);
    ev.set_input(ctx->id("b3"), false);
    ev.settle(true);
    ev.set_input(ctx->id("clk"), true);
    ev.settle(); // write, and B reads the old contents (0)
    ev.set_input(ctx->id("clk"), false);
    ev.set_input(ctx->id("we"), false);
    ev.settle();
    auto word = [&]() {
        int v = 0;
        for (int i = 0; i < 5; i++) {
            Tri b = ev.eval_output(ctx->idf("o%d", i));
            EXPECT_NE(b, Tri::X);
            v |= (b == Tri::T) << i;
        }
        return v;
    };
    EXPECT_EQ(word(), 0);
    ev.set_input(ctx->id("clk"), true);
    ev.settle();
    EXPECT_EQ(word(), 0b01110);
    ev.set_input(ctx->id("clk"), false);
    ev.set_input(ctx->id("b3"), true);
    ev.settle();
    ev.set_input(ctx->id("clk"), true);
    ev.settle();
    EXPECT_EQ(word(), 0b10110);
}

// FIFOs (CC_FIFO_40K)
struct FifoDesign
{
    std::string name;
    bool sdp = false, async = false;
    int width = 40;
    bool do_reg[2] = {false, false};
    bool dyn_offsets = false;
    bool clk_inv[2] = {false, false};
    bool en_inv[2] = {false, false};
    bool we_inv = false;
    bool global_clock = true;
    // Reset only at the start (instead of now and then), so that the FIFO can fill up
    bool reset_once = false;
    uint64_t seed = 1;
};

std::ostream &operator<<(std::ostream &os, const FifoDesign &d) { return os << d.name; }

FifoDesign fifo(const std::string &name, int width)
{
    FifoDesign d;
    d.name = name;
    d.width = width;
    d.sdp = width == 80;
    return d;
}

class GateMateFifoTest : public GateMateTest, public ::testing::WithParamInterface<FifoDesign>
{
  protected:
    // A FIFO "fifo" with all status outputs, pointers and data outputs on pads. Pushes and pops
    // happen with random rates; with reset_once a register releases the reset after the first
    // clock edge, otherwise a combination of pads resets the FIFO now and then.
    void build_fifo(const FifoDesign &d)
    {
        std::mt19937_64 rng(d.seed);
        CellInfo *f = ctx->createCell(ctx->id("fifo"), id_CC_FIFO_40K);
        f->params[id_RAM_MODE] = Property(d.sdp ? "SDP" : "TDP");
        f->params[id_FIFO_MODE] = Property(d.async ? "ASYNC" : "SYNC");
        f->params[id_A_WIDTH] = Property(d.width, 32);
        f->params[id_B_WIDTH] = Property(d.width, 32);
        f->params[id_DYN_STAT_SELECT] = Property(d.dyn_offsets, 1);
        int tp = d.width == 1 ? 15 : d.width == 2 ? 14 : d.width == 5 ? 13 : d.width == 10 ? 12 : d.width == 20 ? 11
                 : d.width == 40                                                                     ? 10
                                                                                                     : 9;
        int depth = 1 << tp;
        // Almost full when a few words are free, or (as the FIFOs here rarely fill up) already
        // with a few words in it; almost empty with a few words in it
        int almost_full = 2 + int(rng() % 3);
        if (rng() % 2)
            almost_full = depth - almost_full;
        int almost_empty = 1 + int(rng() % 3);
        f->params[id_ALMOST_FULL_OFFSET] = Property(almost_full, 15);
        f->params[id_ALMOST_EMPTY_OFFSET] = Property(almost_empty, 15);
        for (int p = 0; p < 2; p++) {
            char port = "AB"[p];
            f->params[ctx->idf("%c_CLK_INV", port)] = Property(d.clk_inv[p], 1);
            f->params[ctx->idf("%c_EN_INV", port)] = Property(d.en_inv[p], 1);
            f->params[ctx->idf("%c_DO_REG", port)] = Property(d.do_reg[p], 1);
        }
        f->params[id_B_WE_INV] = Property(d.we_inv, 1);

        std::vector<NetInfo *> pads;
        for (int i = 0; i < 8; i++)
            pads.push_back(input(stringf("i%d", i)));
        int count = 0;
        auto name = [&]() { return stringf("s%d", count++); };
        // A signal that is high with probability 1/4, 1/2 or 3/4
        auto biased = [&](int quarters) {
            int a = rng() % 8, b = (a + 1 + rng() % 7) % 8;
            if (quarters == 2)
                return pads[a];
            return lut2(name(), quarters == 1 ? 0b1000 : 0b1110, pads[a], pads[b]);
        };
        auto data = [&]() -> NetInfo * {
            int kind = rng() % 4;
            int a = rng() % 8, b = (a + 1 + rng() % 7) % 8;
            if (kind == 0)
                return lut2(name(), 0b0110, pads[a], pads[b]);
            if (kind == 1)
                return (rng() & 1) ? vcc() : gnd();
            return pads[a];
        };
        NetInfo *rd_clk = d.global_clock ? global_clock("rclk") : input("rclk");
        NetInfo *wr_clk = d.async ? (d.global_clock ? global_clock("wclk") : input("wclk")) : rd_clk;
        connect_input(f, id_A_CLK, rd_clk);
        connect_input(f, id_B_CLK, wr_clk);
        NetInfo *rst_n;
        if (d.reset_once) {
            CellInfo *ff = dff("rst", vcc(), rd_clk);
            ff->params[id_INIT] = Property(0, 1);
            rst_n = ff->getPort(id_Q);
        } else {
            // Low with probability 1/16
            NetInfo *r0 = lut2(name(), 0b1110, pads[0], pads[1]);
            NetInfo *r1 = lut2(name(), 0b1110, pads[2], pads[3]);
            rst_n = lut2(name(), 0b1110, r0, r1);
        }
        connect_input(f, id_F_RST_N, rst_n);
        // Push with probability 1/2 or 3/4, pop with 1/4 or 1/2 (filling up), or the other way.
        // To fill up: push always, pop with probability 1/8.
        int push = 2 + int(rng() % 2), pop = 1 + int(rng() % 2);
        if (!d.reset_once && rng() % 3 == 0)
            std::swap(push, pop);
        NetInfo *push_net = d.reset_once ? vcc() : biased(push);
        connect_input(f, id_B_EN, d.en_inv[1] ? lut1(name(), 0b01, push_net) : push_net);
        connect_input(f, id_B_WE, d.we_inv ? gnd() : vcc());
        NetInfo *pop_net = d.reset_once ? lut2(name(), 0b1000, biased(1), pads[7]) : biased(pop);
        connect_input(f, id_A_EN, d.en_inv[0] ? lut1(name(), 0b01, pop_net) : pop_net);
        for (int p = 0; p < 2; p++) {
            char port = "AB"[p];
            // TDP pushes on port B only
            if (!d.sdp && p == 0)
                continue;
            for (int i = 0; i < std::min(d.width, 40); i++) {
                connect_input(f, ctx->idf("%c_DI[%d]", port, i), data());
                connect_input(f, ctx->idf("%c_BM[%d]", port, i), rng() % 8 ? vcc() : data());
            }
        }
        if (d.dyn_offsets) {
            // Mostly constant offsets, the lowest bits from pads
            for (int i = 0; i < 15; i++) {
                connect_input(f, ctx->idf("F_ALMOST_FULL_OFFSET[%d]", i),
                              i < 2 ? pads[4 + i] : ((almost_full >> i) & 1) ? vcc() : gnd());
                connect_input(f, ctx->idf("F_ALMOST_EMPTY_OFFSET[%d]", i),
                              i < 2 ? pads[6 + i] : ((almost_empty >> i) & 1) ? vcc() : gnd());
            }
        }
        for (IdString flag :
             {id_F_FULL, id_F_EMPTY, id_F_ALMOST_FULL, id_F_ALMOST_EMPTY, id_F_RD_ERROR, id_F_WR_ERROR})
            output("o_" + flag.str(ctx), connect_output(f, flag));
        for (int i = 0; i < 16; i++) {
            output(stringf("rd_ptr%d", i), connect_output(f, ctx->idf("F_RD_PTR[%d]", i)));
            output(stringf("wr_ptr%d", i), connect_output(f, ctx->idf("F_WR_PTR[%d]", i)));
        }
        for (int k = 0; k < d.width; k++)
            output(stringf("q%d", k), connect_output(f, ctx->idf("%c_DO[%d]", "AB"[k / 40], k % 40)));
    }
};

TEST_P(GateMateFifoTest, matches_reference)
{
    FifoDesign d = GetParam();
    int steps = d.reset_once ? 4000 : 600;
    for (uint64_t seed : {1, 2}) {
        d.seed = seed;
        EXPECT_TRUE(implementation_matches_reference([&] { build_fifo(d); }, steps, seed)) << "seed " << seed;
    }
}

std::vector<FifoDesign> fifo_configs()
{
    std::vector<FifoDesign> configs;
    for (int w : {1, 2, 5, 10, 20, 40})
        configs.push_back(fifo(stringf("tdp_w%d", w), w));
    configs.push_back(fifo("sdp_w80", 80));
    FifoDesign async = fifo("async_w10", 10);
    async.async = true;
    configs.push_back(async);
    FifoDesign async_sdp = fifo("async_sdp_w80", 80);
    async_sdp.async = true;
    configs.push_back(async_sdp);
    FifoDesign reg = fifo("output_registers", 20);
    reg.do_reg[0] = reg.do_reg[1] = true;
    configs.push_back(reg);
    FifoDesign sdp_reg = fifo("sdp_output_registers", 80);
    sdp_reg.do_reg[0] = sdp_reg.do_reg[1] = true;
    configs.push_back(sdp_reg);
    FifoDesign dyn = fifo("dynamic_offsets", 10);
    dyn.dyn_offsets = true;
    configs.push_back(dyn);
    FifoDesign inv = fifo("inverted_controls", 10);
    inv.clk_inv[0] = inv.en_inv[0] = inv.en_inv[1] = inv.we_inv = true;
    configs.push_back(inv);
    FifoDesign async_inv = fifo("async_inverted_write_clock", 10);
    async_inv.async = true;
    async_inv.clk_inv[1] = true;
    configs.push_back(async_inv);
    FifoDesign fabric = fifo("fabric_clock", 10);
    fabric.global_clock = false;
    configs.push_back(fabric);
    FifoDesign async_fabric = fifo("async_fabric_clocks", 10);
    async_fabric.async = true;
    async_fabric.global_clock = false;
    configs.push_back(async_fabric);
    FifoDesign full = fifo("fills_up", 80);
    full.reset_once = true;
    configs.push_back(full);
    FifoDesign full_async = fifo("async_fills_up", 80);
    full_async.reset_once = true;
    full_async.async = true;
    configs.push_back(full_async);
    return configs;
}

INSTANTIATE_TEST_SUITE_P(Configs, GateMateFifoTest, ::testing::ValuesIn(fifo_configs()),
                         [](const ::testing::TestParamInfo<FifoDesign> &info) { return info.param.name; });

// Negative controls for the FIFO comparison
class GateMateFifoMutationTest : public GateMateFifoTest
{
  protected:
    ::testing::AssertionResult matches_after(const FifoDesign &d, const std::function<void(CellInfo *)> &mutate,
                                             int steps = 600)
    {
        return implementation_matches_reference([&] { build_fifo(d); }, steps, 1,
                                                [&] { mutate(ctx->cells.at(ctx->id("fifo")).get()); });
    }

    void swap_nets(CellInfo *cell, IdString a, IdString b)
    {
        NetInfo *na = cell->getPort(a), *nb = cell->getPort(b);
        ASSERT_NE(na, nullptr);
        ASSERT_NE(nb, nullptr);
        cell->disconnectPort(a);
        cell->disconnectPort(b);
        cell->connectPort(a, nb);
        cell->connectPort(b, na);
    }
};

TEST_F(GateMateFifoMutationTest, unchanged)
{
    EXPECT_TRUE(matches_after(fifo("", 10), [](CellInfo *) {}));
}

TEST_F(GateMateFifoMutationTest, swapped_status_outputs)
{
    EXPECT_TRUE(differs(matches_after(fifo("", 10), [&](CellInfo *f) {
        swap_nets(f, ctx->id("F_EMPTY[0]"), ctx->id("F_AL_EMPTY[0]"));
    })));
}

TEST_F(GateMateFifoMutationTest, swapped_pointer_outputs)
{
    EXPECT_TRUE(differs(matches_after(fifo("", 10), [&](CellInfo *f) {
        swap_nets(f, ctx->id("FWR_ADDR[3]"), ctx->id("FWR_ADDR[4]"));
    })));
}

TEST_F(GateMateFifoMutationTest, rotated_data_inputs)
{
    EXPECT_TRUE(differs(matches_after(fifo("", 10), [&](CellInfo *f) {
        for (int i = 0; i < 4; i++)
            swap_nets(f, ctx->idf("DIB[%d]", i), ctx->idf("DIB[%d]", i + 1));
    })));
}

TEST_F(GateMateFifoMutationTest, asynchronous_instead_of_synchronous)
{
    FifoDesign d = fifo("", 10);
    EXPECT_TRUE(differs(matches_after(d, [&](CellInfo *f) {
        f->unsetParam(id_RAM_cfg_fifo_sync_enable);
        f->params[id_RAM_cfg_fifo_async_enable] = Property(1, 1);
    })));
}

// The following use a FIFO that fills up, going through all fill levels once
TEST_F(GateMateFifoMutationTest, wrong_almost_empty_offset)
{
    FifoDesign d = fifo("", 80);
    d.reset_once = true;
    EXPECT_TRUE(differs(matches_after(
            d,
            [&](CellInfo *f) {
                int offset = int_or_default(f->params, id_RAM_cfg_almost_empty_offset, 0);
                f->params[id_RAM_cfg_almost_empty_offset] = Property(offset + 2, 15);
            },
            4000)));
}

TEST_F(GateMateFifoMutationTest, swapped_full_and_almost_full)
{
    FifoDesign d = fifo("", 80);
    d.reset_once = true;
    EXPECT_TRUE(differs(matches_after(
            d, [&](CellInfo *f) { swap_nets(f, ctx->id("F_FULL[0]"), ctx->id("F_AL_FULL[0]")); }, 4000)));
}

// With the FULL_USE strategy on a two-die device, globals 5-8 are the four of the second die. The
// code that connects their users only numbers the four of one die: pack_bram.cc ram_clk_signal
// (no RAM clock selection for index 4-7) and pack_io.cc set_in_clk/set_out_clk (CLOCK5-8 ports).
// As long as only four globals are kept (pack_clocking.cc, see
// GateMateClockingTest.full_strategy_uses_globals_of_all_dies) these are not reached; once that
// is fixed, this covers them: a RAM, IO registers and fabric registers on each of six clocks.
TEST_F(GateMateBramSemanticsTest, six_global_clocks_on_two_dies)
{
    init_context("CCGM1A2", {"--allow-unconstrained", "--strategy=full"});
    auto build = [&] {
        NetInfo *clk = nullptr;
        for (int i = 0; i < 6; i++) {
            clk = global_clock(stringf("clk%d", i));
            NetInfo *d = input(stringf("d%d", i));
            ctx->cells.at(ctx->idf("d%d", i))->params[id_FF_IBF] = Property(1, 1);
            CellInfo *ireg = dff(stringf("ireg%d", i), d, clk);
            CellInfo *ff = dff(stringf("ff%d", i), lut1(stringf("inv%d", i), 0b01, ireg->getPort(id_Q)), clk);
            CellInfo *oreg = dff(stringf("oreg%d", i), lut1(stringf("buf%d", i), 0b10, ff->getPort(id_Q)), clk);
            CellInfo *obuf = output(stringf("q%d", i), oreg->getPort(id_Q));
            obuf->params[id_FF_OBF] = Property(1, 1);
        }
        BramDesign d = tdp("", true, 5, 5);
        d.controls_from_pads = true;
        build_bram("ram", d, clk);
    };
    EXPECT_TRUE(implementation_matches_reference(build, 300, 1));
    // The registers of the IOs on global clocks are in the IOSELs
    int in_regs = 0, out_regs = 0;
    for (auto &cell : ctx->cells) {
        in_regs += bool_or_default(cell.second->params, id_IN1_FF);
        out_regs += bool_or_default(cell.second->params, id_OUT1_FF);
    }
    EXPECT_GE(in_regs, 4);
    EXPECT_GE(out_regs, 4);
}

} // namespace

namespace {
// Not a test: prints the outputs of the FIFO model for a pseudo-random stimulus, for comparison
// with a simulation of cells_sim.v (FIFO_TRACE_WIDTH, FIFO_TRACE_ASYNC).
TEST_F(GateMateBramSemanticsTest, DISABLED_fifo_trace)
{
    int width = getenv("FIFO_TRACE_WIDTH") ? atoi(getenv("FIFO_TRACE_WIDTH")) : 2;
    bool async = getenv("FIFO_TRACE_ASYNC") != nullptr;
    bool resets = getenv("FIFO_TRACE_NORESET") == nullptr;
    int steps = getenv("FIFO_TRACE_STEPS") ? atoi(getenv("FIFO_TRACE_STEPS")) : 3000;
    int depth = width == 1 ? 32768 : width == 2 ? 16384 : 512;
    CellInfo *f = ctx->createCell(ctx->id("fifo"), id_CC_FIFO_40K);
    f->params[id_RAM_MODE] = Property(width == 80 ? "SDP" : "TDP");
    f->params[id_FIFO_MODE] = Property(async ? "ASYNC" : "SYNC");
    f->params[id_A_WIDTH] = Property(width, 32);
    f->params[id_B_WIDTH] = Property(width, 32);
    f->params[id_ALMOST_FULL_OFFSET] = Property(depth - 3, 15);
    f->params[id_ALMOST_EMPTY_OFFSET] = Property(2, 15);
    std::vector<std::string> ins = {"rclk", "wclk", "pop", "push", "rst_n", "d0", "d1"};
    std::map<std::string, NetInfo *> pads;
    for (auto &n : ins)
        pads[n] = input(n);
    connect_input(f, id_A_CLK, pads["rclk"]);
    connect_input(f, id_B_CLK, pads["wclk"]);
    connect_input(f, id_A_EN, pads["pop"]);
    connect_input(f, id_B_EN, pads["push"]);
    connect_input(f, id_B_WE, vcc());
    connect_input(f, id_F_RST_N, pads["rst_n"]);
    for (int i = 0; i < std::min(width, 2); i++) {
        IdString port = width == 80 ? id_A_DI : id_B_DI;
        connect_input(f, ctx->idf("%s[%d]", port.c_str(ctx), i), pads[stringf("d%d", i)]);
    }
    for (int i = 0; i < std::min(width, 40); i++)
        connect_input(f, ctx->idf("%s[%d]", width == 80 ? "A_BM" : "B_BM", i), vcc());
    std::vector<std::string> outs;
    for (const char *flag : {"F_FULL", "F_EMPTY", "F_ALMOST_FULL", "F_ALMOST_EMPTY", "F_RD_ERROR", "F_WR_ERROR"})
        outs.push_back(flag);
    for (int i = 15; i >= 0; i--)
        outs.push_back(stringf("F_RD_PTR[%d]", i));
    for (int i = 15; i >= 0; i--)
        outs.push_back(stringf("F_WR_PTR[%d]", i));
    for (int i = std::min(width, 2) - 1; i >= 0; i--)
        outs.push_back(stringf("A_DO[%d]", i));
    for (auto &o : outs)
        output("o_" + o, connect_output(f, ctx->id(o)));
    NetlistEvaluator ev(ctx);
    uint32_t lcg = 12345;
    auto next = [&]() {
        lcg = lcg * 1103515245u + 12345u;
        return (lcg >> 16) & 0x7fff;
    };
    for (int step = 0; step < steps; step++) {
        uint32_t r = next();
        // Reset now and then; push more often than pop at first, then the other way
        bool fill = (step / (steps / 6)) % 2 == 0;
        std::map<std::string, bool> v = {{"rclk", bool(r & 1)},
                                         {"wclk", bool(r & 2)},
                                         {"pop", fill ? (resets ? (r & 12) == 12 : (r & 0x700) == 0x700) : (r & 12) != 0},
                                         {"push", fill ? (!resets || (r & 48) != 0) : (r & 48) == 48},
                                         {"rst_n", step < 2 ? false : (!resets || (r & 0x7f00) != 0)},
                                         {"d0", bool(r & 64)},
                                         {"d1", bool(r & 128)}};
        // Data and reset first, then the clocks (as in the testbench, to avoid races there)
        for (auto &n : ins)
            if (n != "rclk" && n != "wclk")
                ev.set_input(ctx->id(n), v[n]);
        if (step == 0) {
            ev.set_input(ctx->id("rclk"), false);
            ev.set_input(ctx->id("wclk"), false);
        }
        ev.settle(step == 0);
        ev.set_input(ctx->id("rclk"), v["rclk"]);
        ev.set_input(ctx->id("wclk"), v["wclk"]);
        ev.settle();
        std::string line;
        for (auto &o : outs)
            line += tri_str(ev.eval_output(ctx->id("o_" + o)));
        printf("%d %s r=%u\n", step, line.c_str(), r);
    }
}
} // namespace
