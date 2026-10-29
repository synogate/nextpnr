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

// Multiplexers, five-input LUT trees and randomly generated netlists mixing all logic cells,
// compared against the reference models after packing and placement.

#include <algorithm>
#include <random>
#include <set>
#include "known_bugs.h"
#include "netlist_eval.h"
#include "testing.h"
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

TEST_F(GateMateTest, mx4_signals)
{
    EXPECT_TRUE(implementation_matches_reference([&] {
        NetInfo *d[4];
        for (int i = 0; i < 4; i++)
            d[i] = input(stringf("d%d", i));
        output("y", mx4("mux", d[0], d[1], d[2], d[3], input("s0"), input("s1")));
    }));
}

// Constant data inputs are masked and inverted inside the MX4 CPE; every pattern of constant
// and signal inputs is checked.
TEST_F(GateMateTest, mx4_constant_data)
{
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                NetInfo *s0 = input("s0"), *s1 = input("s1");
                NetInfo *sig[4];
                for (int i = 0; i < 4; i++)
                    sig[i] = input(stringf("d%d", i));
                for (int pattern = 0; pattern < 81; pattern++) {
                    // Each data input is a signal, 0 or 1 (base 3 digits of the pattern)
                    NetInfo *d[4];
                    int p = pattern;
                    for (int i = 0; i < 4; i++, p /= 3) {
                        int kind = p % 3;
                        d[i] = kind == 0 ? sig[i] : kind == 1 ? gnd() : vcc();
                    }
                    output(stringf("y%d", pattern), mx4(stringf("mux%d", pattern), d[0], d[1], d[2], d[3], s0, s1));
                }
            },
            200));
}

TEST_F(GateMateTest, mx4_one_constant_select)
{
    EXPECT_TRUE(implementation_matches_reference([&] {
        NetInfo *d[4];
        for (int i = 0; i < 4; i++)
            d[i] = input(stringf("d%d", i));
        NetInfo *s = input("s");
        output("y0", mx4("mux0", d[0], d[1], d[2], d[3], gnd(), s));
        output("y1", mx4("mux1", d[0], d[1], d[2], d[3], vcc(), s));
        output("y2", mx4("mux2", d[0], d[1], d[2], d[3], s, gnd()));
        output("y3", mx4("mux3", d[0], d[1], d[2], d[3], s, vcc()));
    }));
}

TEST_F(GateMateTest, mx4_registered)
{
    EXPECT_TRUE(implementation_matches_reference([&] {
        NetInfo *d[4];
        for (int i = 0; i < 4; i++)
            d[i] = input(stringf("d%d", i));
        NetInfo *y = mx4("mux", d[0], d[1], d[2], d[3], input("s0"), input("s1"));
        output("q", dff("ff", y, global_clock("clk"))->getPort(id_Q));
    }));
}

// Bug 38: after placement, the repack merges the upper half of a 4:1 multiplexer (and of an adder)
// into the lower one and deletes it; its cluster root must not keep a pointer to it
// (getClusterPlacement() follows them).
TEST_F(GateMateTest, cluster_children_are_live_cells_after_placement)
{
    NetInfo *d[4];
    for (int i = 0; i < 4; i++)
        d[i] = input(stringf("d%d", i));
    NetInfo *y = mx4("mux", d[0], d[1], d[2], d[3], input("s0"), input("s1"));
    output("q", dff("ff", y, global_clock("clk"))->getPort(id_Q));
    output("sum", addf("add", d[0], d[1], gnd()).first);
    ctx->pack();
    ctx->place();
    std::set<const CellInfo *> live;
    for (auto &cell : ctx->cells)
        live.insert(cell.second.get());
    int children = 0;
    for (auto &cell : ctx->cells)
        for (CellInfo *child : cell.second->constr_children) {
            // (Comparing the pointer only: a deleted cell must not be read)
            EXPECT_TRUE(live.count(child)) << "a child of " << cell.first.c_str(ctx) << " was deleted";
            children++;
        }
    EXPECT_GT(children, 0);
}

TEST_F(GateMateTest, l2t5_random_inits)
{
    EXPECT_TRUE(implementation_matches_reference([&] {
        std::mt19937 rng(5);
        NetInfo *in[5];
        for (int i = 0; i < 5; i++)
            in[i] = input(stringf("i%d", i));
        for (int n = 0; n < 16; n++) {
            CellInfo *lut = create_cell_ptr(id_CC_L2T5, stringf("lut%d", n));
            for (auto p : {id_INIT_L02, id_INIT_L03, id_INIT_L11, id_INIT_L20})
                lut->params[p] = Property(int(rng() & 0xf), 4);
            lut->connectPort(id_I0, in[0]);
            lut->connectPort(id_I1, in[1]);
            lut->connectPort(id_I2, in[2]);
            lut->connectPort(id_I3, in[3]);
            lut->connectPort(id_I4, in[4]);
            lut->connectPort(id_O, create_net(stringf("lut%d$o", n)));
            output(stringf("o%d", n), lut->getPort(id_O));
        }
    }));
}

struct RandomLogicCase
{
    int seed;
    // Avoid the known bugs (known_bugs.h), so that the test can find others
    bool avoid_known_bugs;
    // Also block RAMs (a separate variant, so that the netlists of the others stay the same)
    bool rams = false;
};

class GateMateRandomLogicTest : public GateMateTest, public ::testing::WithParamInterface<RandomLogicCase>
{
  protected:
    // A random netlist of LUTs, LUT trees, multiplexers, adders, registers, IO registers, tristate
    // and bidirectional pads (and block RAMs) with random configurations, constant inputs, repeated
    // inputs and fan-out.
    //
    // The variant that avoids known bugs restricts the netlists; each restriction names the bug it
    // avoids (see known_bugs.h) and goes away with it.
    void build()
    {
        int seed = GetParam().seed;
        bool avoiding = GetParam().avoid_known_bugs;
        bool with_rams = GetParam().rams;
        auto avoiding_bug = [&](KnownBug bug) { return avoiding && avoid(bug); };
        std::mt19937 rng(seed);
        auto pick = [&](int n) { return int(rng() % unsigned(n)); };
        NetInfo *clk = global_clock("clk");
        // A clock from the fabric, and the shared select of the double data rate outputs
        NetInfo *fabric_clk = input("fclk");
        NetInfo *ddr_sel = input("ddr_sel");
        // IO registers and RAMs are clocked from the global or the fabric clock
        auto io_clock = [&] {
            bool global = pick(2);
            if (avoiding_bug(KnownBug::FMAX_OF_RELATED_CLOCKS_WITHOUT_PATHS))
                global = true;
            return global ? clk : fabric_clk;
        };
        std::vector<NetInfo *> nets;
        for (int i = 0; i < 8; i++)
            nets.push_back(input(stringf("in%d", i)));
        // A net of the netlist, sometimes a constant
        auto operand = [&]() -> NetInfo * {
            int r = pick(20);
            if (r == 0)
                return gnd();
            if (r == 1)
                return vcc();
            return nets.at(pick(int(nets.size())));
        };
        auto lut_input = [&] { return operand(); };
        auto other_input = [&] { return operand(); };
        // LUT configurations
        auto lut1_init = [&] { return pick(4); };
        auto lut2_init = [&] { return pick(16); };
        auto lut_tree_root_init = [&] { return pick(16); };
        // Random choices are made in statement order (not as several arguments of one call,
        // whose evaluation order is unspecified), so that the netlist is the same with
        // every compiler.
        auto inputs = [&](int count, bool lut) {
            std::vector<NetInfo *> v;
            for (int i = 0; i < count; i++)
                v.push_back(lut ? lut_input() : other_input());
            return v;
        };
        int rams = 0;
        for (int n = 0; n < 60; n++) {
            std::string name = stringf("c%d", n);
            int kind = pick(with_rams ? 13 : 12);
            if (kind == 12 && rams == 3)
                kind = 0;
            switch (kind) {
            case 12: {
                // A block RAM: inputs from the netlist, data outputs used further on
                rams++;
                bool is_40k = pick(2);
                bool sdp = pick(5) == 0;
                CellInfo *ram = ctx->createCell(ctx->id(name), is_40k ? id_CC_BRAM_40K : id_CC_BRAM_20K);
                ram->params[id_RAM_MODE] = Property(sdp ? "SDP" : "TDP");
                static const int widths[] = {1, 2, 5, 10, 20, 40};
                int width[2], data_width = is_40k ? 40 : 20;
                for (int p = 0; p < 2; p++) {
                    char port = "AB"[p];
                    width[p] = sdp ? 2 * data_width : widths[pick(is_40k ? 6 : 5)];
                    int rd = sdp ? (p ? width[p] : 0) : width[p];
                    int wr = sdp ? (p ? 0 : width[p]) : width[p];
                    ram->params[ctx->idf("%c_RD_WIDTH", port)] = Property(rd, 32);
                    ram->params[ctx->idf("%c_WR_WIDTH", port)] = Property(wr, 32);
                    bool write_through = !sdp && pick(2);
                    ram->params[ctx->idf("%c_WR_MODE", port)] =
                            Property(write_through ? "WRITE_THROUGH" : "NO_CHANGE");
                    ram->params[ctx->idf("%c_DO_REG", port)] = Property(pick(4) == 0, 1);
                    for (const char *inv : {"CLK_INV", "EN_INV", "WE_INV"})
                        ram->params[ctx->idf("%c_%s", port, inv)] = Property(pick(4) == 0, 1);
                }
                for (int i = 0; i < (is_40k ? 128 : 64); i++) {
                    std::string bits;
                    for (int j = 0; j < 320; j++)
                        bits.push_back(pick(2) ? '1' : '0');
                    ram->params[ctx->idf("INIT_%02X", i)] = Property::from_string(bits);
                }
                bool shared_clock = pick(2);
                NetInfo *ram_clk[2];
                ram_clk[0] = io_clock();
                ram_clk[1] = shared_clock ? ram_clk[0] : io_clock();
                for (int p = 0; p < 2; p++) {
                    char port = "AB"[p];
                    connect_input(ram, ctx->idf("%c_CLK", port), ram_clk[p]);
                    connect_input(ram, ctx->idf("%c_EN", port), other_input());
                    connect_input(ram, ctx->idf("%c_WE", port), other_input());
                    for (int i = 0; i < 16; i++)
                        connect_input(ram, ctx->idf("%c_ADDR[%d]", port, i), other_input());
                    for (int i = 0; i < data_width; i++) {
                        connect_input(ram, ctx->idf("%c_DI[%d]", port, i), other_input());
                        connect_input(ram, ctx->idf("%c_BM[%d]", port, i), pick(3) ? vcc() : other_input());
                    }
                }
                for (int p = 0; p < 2; p++) {
                    int used_outputs = sdp ? data_width : width[p];
                    for (int i = 0; i < used_outputs; i++)
                        nets.push_back(connect_output(ram, ctx->idf("%c_DO[%d]", "AB"[p], i)));
                }
                break;
            }
            case 8: {
                // An input pad with a register (merged into the IOSEL) or a DDR input
                NetInfo *pad = input(name + "_pad");
                NetInfo *iclk = io_clock();
                bool clk_inv = pick(4) == 0;
                if (pick(2)) {
                    ctx->cells.at(ctx->id(name + "_pad"))->params[id_FF_IBF] = Property(1, 1);
                    CellInfo *ff = dff(name, pad, iclk);
                    ff->params[id_CLK_INV] = Property(clk_inv, 1);
                    nets.push_back(ff->getPort(id_Q));
                } else {
                    auto [q0, q1] = iddr(name, pad, iclk, clk_inv);
                    nets.push_back(q0);
                    nets.push_back(q1);
                }
                break;
            }
            case 9: {
                // An output pad with a register (merged into the IOSEL) or a DDR output
                NetInfo *oclk = io_clock();
                bool clk_inv = pick(4) == 0;
                if (pick(2)) {
                    CellInfo *ff = dff(name, other_input(), oclk);
                    ff->params[id_CLK_INV] = Property(clk_inv, 1);
                    output(name + "_pad", ff->getPort(id_Q))->params[id_FF_OBF] = Property(1, 1);
                    // Sometimes also used in the fabric, then it cannot move into the IOSEL
                    if (pick(4) == 0)
                        nets.push_back(ff->getPort(id_Q));
                } else {
                    auto in = inputs(2, false);
                    NetInfo *q = oddr(name, in[0], in[1], oclk, ddr_sel, clk_inv);
                    if (pick(3) == 0)
                        q = lut1(name + "_inv", 0b01, q);
                    output(name + "_pad", q);
                }
                break;
            }
            case 10: {
                // A tristate or bidirectional pad
                auto in = inputs(2, false);
                if (pick(2))
                    tristate_output(name + "_pad", in[0], in[1]);
                else
                    nets.push_back(bidirectional(name + "_pad", in[0], in[1]));
                break;
            }
            case 0: {
                int init = lut1_init();
                nets.push_back(lut1(name, init, lut_input()));
                break;
            }
            case 1: {
                int init = lut2_init();
                auto in = inputs(2, true);
                nets.push_back(lut2(name, init, in[0], in[1]));
                break;
            }
            case 2: {
                int l00 = lut2_init();
                int l01 = lut2_init();
                int l10 = lut_tree_root_init();
                auto in = inputs(4, true);
                nets.push_back(l2t4(name, l00, l01, l10, in[0], in[1], in[2], in[3]));
                break;
            }
            case 3: {
                auto in = inputs(3, false);
                nets.push_back(mx2(name, in[0], in[1], in[2]));
                break;
            }
            case 4: {
                auto in = inputs(6, false);
                nets.push_back(mx4(name, in[0], in[1], in[2], in[3], in[4], in[5]));
                break;
            }
            case 5: {
                // A short carry chain
                NetInfo *carry = other_input();
                int width = 1 + pick(4);
                for (int i = 0; i < width; i++) {
                    auto in = inputs(2, false);
                    auto [s, co] = addf(stringf("%s_%d", name.c_str(), i), in[0], in[1], carry);
                    nets.push_back(s);
                    carry = co;
                }
                nets.push_back(carry);
                break;
            }
            default: {
                // Registers without enable or reset have them tied to constants, which the
                // packer folds into the configuration
                NetInfo *d = other_input();
                NetInfo *en = !pick(3) ? other_input() : nullptr;
                NetInfo *sr = !pick(3) ? other_input() : nullptr;
                CellInfo *ff = dff(name, d, clk, en, sr);
                ff->params[id_CLK_INV] = Property(pick(4) == 0, 1);
                ff->params[id_SR_VAL] = Property(pick(2), 1);
                if (pick(2))
                    ff->params[id_INIT] = Property(pick(2), 1);
                nets.push_back(ff->getPort(id_Q));
                break;
            }
            }
        }
        // Observe the last 24 nets plus a few random ones
        for (int i = 0; i < 24; i++)
            output(stringf("o%d", i), nets.at(nets.size() - 1 - i));
        for (int i = 0; i < 8; i++)
            output(stringf("r%d", i), nets.at(8 + pick(int(nets.size()) - 8)));
    }
};

TEST_P(GateMateRandomLogicTest, matches_reference)
{
    EXPECT_TRUE(implementation_matches_reference([&] { build(); }, 300, GetParam().seed));
}

// --vopt lut-input-swap: swapping the input pairs of every LUT tree keeps the function. (The full
// seeds already fail matches_reference on known bugs.)
TEST_P(GateMateRandomLogicTest, swapped_lut_inputs_match_reference)
{
    if (!GetParam().avoid_known_bugs)
        GTEST_SKIP() << "fails on known bugs anyway, see matches_reference";
    int swapped = 0;
    EXPECT_TRUE(implementation_matches_reference([&] { build(); }, 300, GetParam().seed, {},
                                                 [&] { swapped = impl->swap_lut_inputs(true); }));
    EXPECT_GT(swapped, 0);
}

// --vopt replicate-fanout: copying LUT trees that drive several sinks next to groups of them keeps
// the function. (The full seeds already fail matches_reference on known bugs.)
TEST_P(GateMateRandomLogicTest, replicated_drivers_match_reference)
{
    if (!GetParam().avoid_known_bugs)
        GTEST_SKIP() << "fails on known bugs anyway, see matches_reference";
    int copies = 0;
    EXPECT_TRUE(implementation_matches_reference([&] { build(); }, 300, GetParam().seed, {},
                                                 [&] { copies = impl->replicate_drivers(2, true); }));
    EXPECT_GT(copies, 0);
}

// --vopt place-repair: moving a LUT tree to another free position (preferably in the other half
// of a CPE, which changes how it passes its result), and a register with the LUT tree feeding it to
// another free half, keeps the function, and the placement valid.
TEST_P(GateMateRandomLogicTest, relocated_cells_match_reference)
{
    if (!GetParam().avoid_known_bugs)
        GTEST_SKIP() << "fails on known bugs anyway, see matches_reference";
    int moved = 0, moved_registers = 0;
    EXPECT_TRUE(implementation_matches_reference([&] { build(); }, 300, GetParam().seed, {}, [&] {
        std::vector<CellInfo *> trees;
        for (auto &cell : ctx->cells)
            if (impl->relocatable_lut_tree(cell.second.get()))
                trees.push_back(cell.second.get());
        for (CellInfo *ci : trees) {
            Loc loc = ctx->getBelLocation(ci->bel);
            BelId to;
            for (int d = 1; d < 10 && to == BelId(); d++)
                for (auto [dx, dy] : {std::make_pair(d, 0), std::make_pair(0, d), std::make_pair(-d, 0),
                                      std::make_pair(0, -d)})
                    for (int z : {loc.z ^ 1, loc.z}) {
                        BelId bel = ctx->getBelByLocation(Loc(loc.x + dx, loc.y + dy, z));
                        if (to == BelId() && impl->free_lut_tree_bel(ci, bel))
                            to = bel;
                    }
            if (to != BelId()) {
                impl->relocate_lut_tree(ci, to);
                moved++;
            }
        }
        std::vector<CellInfo *> registers;
        for (auto &cell : ctx->cells)
            if (impl->relocatable_register(cell.second.get()))
                registers.push_back(cell.second.get());
        for (CellInfo *ff : registers) {
            CellInfo *lut = impl->relocatable_register(ff);
            Loc loc = ctx->getBelLocation(lut->bel);
            BelId to;
            for (int d = 0; d < 10 && to == BelId(); d++)
                for (auto [dx, dy] : {std::make_pair(d, 0), std::make_pair(0, d), std::make_pair(-d, 0),
                                      std::make_pair(0, -d)})
                    for (int z : {loc.z ^ 1, loc.z}) {
                        BelId bel = ctx->getBelByLocation(Loc(loc.x + dx, loc.y + dy, z));
                        if (to == BelId() && bel != lut->bel && impl->free_register_half(lut, ff, bel))
                            to = bel;
                    }
            if (to != BelId()) {
                impl->relocate_register(lut, ff, to);
                moved_registers++;
            }
        }
        for (auto &cell : ctx->cells) {
            if (cell.second->bel != BelId()) {
                EXPECT_TRUE(ctx->isBelLocationValid(cell.second->bel)) << cell.first.c_str(ctx);
            }
        }
    }));
    EXPECT_GT(moved, 0);
    EXPECT_GT(moved_registers, 0);
}

// --vopt fuse-luts: folding inverters (forward, backward and through registers) and fusing LUTs
// into the LUTs they feed keeps the function. The netlists (the reference too) get inverters on the
// outputs of half their LUTs, multiplexers and registers, so there is much to fold (tests/fuse.cc
// tests each rule on its own).
TEST_P(GateMateRandomLogicTest, fused_luts_match_reference)
{
    if (!GetParam().avoid_known_bugs)
        GTEST_SKIP() << "fails on known bugs anyway, see matches_reference";
    init_context("CCGM1A1", {"--allow-unconstrained", "--fuse-luts"});
    auto build_with_inverters = [&] {
        build();
        std::mt19937 rng(GetParam().seed);
        std::vector<NetInfo *> outputs;
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (!ci->type.in(id_CC_LUT2, id_CC_L2T4, id_CC_MX2, id_CC_DFF))
                continue;
            NetInfo *out = ci->getPort(ci->type == id_CC_MX2 ? id_Y : ci->type == id_CC_DFF ? id_Q : id_O);
            if (!out || out->users.empty())
                continue;
            bool plain = true;
            for (auto &user : out->users)
                plain &= user.cell->type.in(id_CC_LUT1, id_CC_LUT2, id_CC_L2T4, id_CC_L2T5, id_CC_MX2, id_CC_MX4,
                                            id_CC_DFF, id_CC_ADDF) &&
                         !user.port.in(id_CLK, id_EN, id_SR);
            if (plain)
                outputs.push_back(out);
        }
        int n = 0;
        for (NetInfo *net : outputs) {
            if (rng() % 2)
                continue;
            std::vector<PortRef> users;
            for (auto &user : net->users)
                users.push_back(user);
            NetInfo *inverted = lut1(stringf("inv%d", n++), 0b01, net);
            for (auto &user : users) {
                user.cell->disconnectPort(user.port);
                user.cell->connectPort(user.port, inverted);
            }
        }
    };
    EXPECT_TRUE(implementation_matches_reference(build_with_inverters, 300, GetParam().seed));
}

// --vopt enable-to-lut: moving register enables into the registers' LUTs (every register that
// qualifies) keeps the function.
TEST_P(GateMateRandomLogicTest, enables_in_luts_match_reference)
{
    if (!GetParam().avoid_known_bugs)
        GTEST_SKIP() << "fails on known bugs anyway, see matches_reference";
    init_context("CCGM1A1", {"--allow-unconstrained", "--enable-to-lut=1"});
    EXPECT_TRUE(implementation_matches_reference([&] { build(); }, 300, GetParam().seed));
}

// --vopt mux-chains: merging chains of 2:1 multiplexers into 4:1 multiplexers keeps the function.
TEST_P(GateMateRandomLogicTest, merged_mux_chains_match_reference)
{
    if (!GetParam().avoid_known_bugs)
        GTEST_SKIP() << "fails on known bugs anyway, see matches_reference";
    init_context("CCGM1A1", {"--allow-unconstrained", "--mux-chains"});
    EXPECT_TRUE(implementation_matches_reference([&] { build(); }, 300, GetParam().seed));
}

// --vopt cpe-merge=2: merging pairs of LUT levels into one CPE (with copies) keeps the function.
TEST_P(GateMateRandomLogicTest, merged_cpes_match_reference)
{
    if (!GetParam().avoid_known_bugs)
        GTEST_SKIP() << "fails on known bugs anyway, see matches_reference";
    init_context("CCGM1A1", {"--allow-unconstrained", "--cpe-merge=2"});
    EXPECT_TRUE(implementation_matches_reference([&] { build(); }, 300, GetParam().seed));
}

// The same netlists: the packed and placed design uses no more CPE halves than the netlist can
// need (see cpe_half_bound), so the packer neither duplicates logic nor adds needless CPEs.
TEST_P(GateMateRandomLogicTest, cpe_use_within_bound)
{
    build();
    std::map<std::string, int> by_type;
    int bound = cpe_half_bound(&by_type);
    try {
        ctx->pack();
        ctx->place();
    } catch (const std::exception &e) {
        GTEST_SKIP() << "implementation failed (see matches_reference): " << e.what();
    }
    std::string breakdown;
    for (auto &k : by_type)
        breakdown += stringf(" %s %d", k.first.c_str(), k.second);
    EXPECT_LE(cpe_halves_used(), bound) << "CPE halves, the bound by cell type:" << breakdown;
}

// The same netlists, placed: every combinational dependency inside a cell has a timing arc (see
// missing_timing_arcs). The variant that avoids known bugs leaves out the arcs known to be
// missing.
TEST_P(GateMateRandomLogicTest, timing_arcs_cover_dependencies)
{
    build();
    try {
        ctx->pack();
        ctx->place();
    } catch (const std::exception &e) {
        GTEST_SKIP() << "implementation failed (see matches_reference): " << e.what();
    }
    bool avoiding = GetParam().avoid_known_bugs;
    auto avoiding_bug = [&](KnownBug bug) { return avoiding && avoid(bug); };
    auto missing = missing_timing_arcs(GetParam().seed);
    auto known = [&](const MissingArc &arc) {
        IdString type = arc.cell->type;
        if (avoiding_bug(KnownBug::ADDER_CPOUT_NOT_TIMED) && type.in(id_CPE_ADDF, id_CPE_ADDF2) &&
            arc.to.in(id_CPOUT1, id_CPOUT2))
            return true;
        if (avoiding_bug(KnownBug::CPE_INTERNAL_INPUT_ARCS_MISSING) && type == id_CPE_L2T4)
            for (IdString port : arc.from)
                if (port.in(id_CINX, id_CINY1, id_CINY2, id_PINX, id_PINY1, id_COMBIN))
                    return true;
        if (avoiding_bug(KnownBug::RAM_OUTPUT_CPE_IGNORED) && type.in(id_CPE_RAMI, id_CPE_RAMIO) &&
            arc.to == id_OUT)
            return true;
        if (avoiding_bug(KnownBug::IOSEL_ARCS_WITH_OUTPUT_ENABLE) && type == id_IOSEL &&
            bool_or_default(arc.cell->params, id_OUT_SIGNAL) && bool_or_default(arc.cell->params, id_OE_ENABLE))
            return true;
        return false;
    };
    missing.erase(std::remove_if(missing.begin(), missing.end(), known), missing.end());
    EXPECT_TRUE(missing.empty()) << "dependencies without a timing arc:" << describe_arcs(missing);
}

static std::vector<RandomLogicCase> random_logic_cases()
{
    std::vector<RandomLogicCase> cases;
    for (int seed = 1; seed <= 20; seed++) {
        cases.push_back({seed, true});
        cases.push_back({seed, false});
    }
    for (int seed = 1; seed <= 10; seed++) {
        cases.push_back({seed, true, true});
        cases.push_back({seed, false, true});
    }
    return cases;
}

INSTANTIATE_TEST_SUITE_P(Seeds, GateMateRandomLogicTest, ::testing::ValuesIn(random_logic_cases()),
                         [](const ::testing::TestParamInfo<RandomLogicCase> &info) {
                             return stringf("%s%s_seed%d", info.param.avoid_known_bugs ? "avoiding_known_bugs" : "full",
                                            info.param.rams ? "_with_rams" : "", info.param.seed);
                         });
