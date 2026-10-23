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

// Carry chains (CC_ADDF): after packing into ADDF/ADDF2 CPEs and placement, every sum and carry
// output must match a ripple-carry adder.

#include "netlist_eval.h"
#include "testing.h"
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

namespace {

enum class CarryIn
{
    ZERO,
    ONE,
    SIGNAL,
};

struct AdderCase
{
    int width;
    CarryIn ci;
    bool co_used;
};

} // namespace

class GateMateAdderTest : public GateMateTest, public ::testing::WithParamInterface<AdderCase>
{
  protected:
    // A `width` bit ripple-carry chain over operand bits a[i], b[i]; returns the carry out.
    NetInfo *adder(const std::string &prefix, const std::vector<NetInfo *> &a, const std::vector<NetInfo *> &b,
                   NetInfo *ci, std::vector<NetInfo *> &sum)
    {
        NetInfo *carry = ci;
        for (size_t i = 0; i < a.size(); i++) {
            auto [s, co] = addf(stringf("%s%d", prefix.c_str(), int(i)), a[i], b[i], carry);
            sum.push_back(s);
            carry = co;
        }
        return carry;
    }
};

TEST_P(GateMateAdderTest, ripple_carry)
{
    auto c = GetParam();
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                std::vector<NetInfo *> a, b, sum;
                for (int i = 0; i < c.width; i++) {
                    a.push_back(input(stringf("a%d", i)));
                    b.push_back(input(stringf("b%d", i)));
                }
                NetInfo *ci = c.ci == CarryIn::ZERO ? gnd() : c.ci == CarryIn::ONE ? vcc() : input("ci");
                NetInfo *co = adder("add", a, b, ci, sum);
                for (int i = 0; i < c.width; i++)
                    output(stringf("s%d", i), sum[i]);
                if (c.co_used)
                    output("co", co);
            },
            400));
}

INSTANTIATE_TEST_SUITE_P(
        Widths, GateMateAdderTest,
        ::testing::Values(AdderCase{1, CarryIn::ZERO, false}, AdderCase{1, CarryIn::ZERO, true},
                          AdderCase{1, CarryIn::SIGNAL, true}, AdderCase{2, CarryIn::ZERO, true},
                          AdderCase{2, CarryIn::ONE, true}, AdderCase{3, CarryIn::ZERO, false},
                          AdderCase{3, CarryIn::SIGNAL, true}, AdderCase{4, CarryIn::ZERO, true},
                          AdderCase{4, CarryIn::ONE, false}, AdderCase{5, CarryIn::SIGNAL, true},
                          AdderCase{8, CarryIn::ZERO, true}, AdderCase{8, CarryIn::SIGNAL, false},
                          AdderCase{9, CarryIn::ONE, true}, AdderCase{16, CarryIn::SIGNAL, true}),
        [](const ::testing::TestParamInfo<AdderCase> &info) {
            const char *ci = info.param.ci == CarryIn::ZERO ? "ci0" : info.param.ci == CarryIn::ONE ? "ci1" : "cisig";
            return stringf("w%d_%s_%s", info.param.width, ci, info.param.co_used ? "co" : "noco");
        });

// Chains longer than 64 bits are split into several carry chains.
TEST_F(GateMateAdderTest, long_chain_is_split)
{
    for (int width : {64, 65, 70}) {
        SCOPED_TRACE(stringf("width %d", width));
        EXPECT_TRUE(implementation_matches_reference(
                [&] {
                    // Few pins: operand bits repeat a small set of inputs, only the upper sum
                    // bits and the carry are observed, which depend on the whole chain.
                    std::vector<NetInfo *> in, a, b, sum;
                    for (int i = 0; i < 8; i++)
                        in.push_back(input(stringf("in%d", i)));
                    for (int i = 0; i < width; i++) {
                        a.push_back(in[i % 8]);
                        b.push_back(in[(i * 3 + 1) % 8]);
                    }
                    NetInfo *co = adder("add", a, b, input("ci"), sum);
                    for (int i = width - 8; i < width; i++)
                        output(stringf("s%d", i), sum[i]);
                    output("mid30", sum[30]);
                    output("co", co);
                },
                400));
    }
}

// A carry from the middle of a chain used as a normal signal (and as an operand of another
// adder) must keep its value, and the chain must stay intact.
TEST_F(GateMateAdderTest, carry_used_mid_chain)
{
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                std::vector<NetInfo *> a, b, sum1, sum2;
                for (int i = 0; i < 4; i++) {
                    a.push_back(input(stringf("a%d", i)));
                    b.push_back(input(stringf("b%d", i)));
                }
                // First chain with the carry out of bit 1 tapped
                NetInfo *carry = gnd();
                NetInfo *tap = nullptr;
                for (int i = 0; i < 4; i++) {
                    auto [s, co] = addf(stringf("x%d", i), a[i], b[i], carry);
                    sum1.push_back(s);
                    carry = co;
                    if (i == 1)
                        tap = co;
                }
                output("carry_tap", tap);
                output("co1", carry);
                // Second chain using the tapped carry as an operand bit
                std::vector<NetInfo *> c = {tap, a[3], b[0]}, d = {b[2], tap, a[1]};
                NetInfo *co2 = adder("y", c, d, input("ci2"), sum2);
                output("co2", co2);
                for (int i = 0; i < 4; i++)
                    output(stringf("s1_%d", i), sum1[i]);
                for (int i = 0; i < 3; i++)
                    output(stringf("s2_%d", i), sum2[i]);
            },
            400));
}

// As above, but a single bit adder takes the carry as an operand before the next bit of the
// chain takes it as its carry input. pack_cpe.cc pack_addf looks for the adder that continues the
// chain among the users of the carry; it must not take an adder using the carry on another port.
// (The order matters: the cells are created so that the chain is packed first, and the single
// adder is the first user of the carry, as the JSON frontend can order them by name.)
TEST_F(GateMateAdderTest, carry_used_as_operand_before_next_bit)
{
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                std::vector<NetInfo *> a, b, sum;
                for (int i = 0; i < 4; i++) {
                    a.push_back(input(stringf("a%d", i)));
                    b.push_back(input(stringf("b%d", i)));
                }
                // The single adder, its operand A connected below
                CellInfo *single = create_cell_ptr(id_CC_ADDF, "y");
                single->connectPort(id_B, b[2]);
                single->connectPort(id_CI, input("ci2"));
                NetInfo *s2 = create_net("y$s"), *co2 = create_net("y$co");
                single->connectPort(id_S, s2);
                single->connectPort(id_CO, co2);
                NetInfo *carry = gnd();
                for (int i = 0; i < 2; i++) {
                    auto [s, co] = addf(stringf("x%d", i), a[i], b[i], carry);
                    sum.push_back(s);
                    carry = co;
                }
                single->connectPort(id_A, carry);
                for (int i = 2; i < 4; i++) {
                    auto [s, co] = addf(stringf("x%d", i), a[i], b[i], carry);
                    sum.push_back(s);
                    carry = co;
                }
                output("co1", carry);
                output("co2", co2);
                output("s2", s2);
                for (int i = 0; i < 4; i++)
                    output(stringf("s1_%d", i), sum[i]);
            },
            400));
}

// A chain whose second bit adds the first bit's sum: an ordinary design without a loop. The packer
// merges both bits into one two-bit adder CPE, whose first sum then feeds its own operand input;
// timing analysis must not take that for a combinational loop (the first bit's sum does not depend
// on the second bit's operands).
TEST_F(GateMateAdderTest, sum_into_next_bit)
{
    auto [s0, c0] = addf("x0", input("a"), input("b"), input("ci"));
    auto [s1, co] = addf("x1", s0, input("c"), c0);
    output("s0", s0);
    output("s1", s1);
    output("co", co);
    // The whole flow: the timing analysis that stops it runs while routing
    EXPECT_NO_THROW(place_and_route());
}

// A carry out feeding the carry inputs of two adders: the next bit of its own chain and the first
// bit of another chain.
TEST_F(GateMateAdderTest, carry_feeding_two_carry_inputs)
{
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                std::vector<NetInfo *> a, b, c, d, sum1, sum2;
                for (int i = 0; i < 3; i++) {
                    a.push_back(input(stringf("a%d", i)));
                    b.push_back(input(stringf("b%d", i)));
                    c.push_back(input(stringf("c%d", i)));
                    d.push_back(input(stringf("d%d", i)));
                }
                NetInfo *carry = gnd();
                NetInfo *tap = nullptr;
                for (int i = 0; i < 3; i++) {
                    auto [s, co] = addf(stringf("x%d", i), a[i], b[i], carry);
                    sum1.push_back(s);
                    carry = co;
                    if (i == 0)
                        tap = co;
                }
                NetInfo *co2 = adder("y", c, d, tap, sum2);
                for (int i = 0; i < 3; i++) {
                    output(stringf("s1_%d", i), sum1[i]);
                    output(stringf("s2_%d", i), sum2[i]);
                }
                output("co1", carry);
                output("co2", co2);
            },
            400));
}

// Operands computed by a LUT used only by the adder are merged into the adder CPE.
TEST_F(GateMateAdderTest, lut_operands_are_merged)
{
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                std::vector<NetInfo *> a, b, sum;
                for (int i = 0; i < 4; i++) {
                    NetInfo *x = input(stringf("x%d", i)), *y = input(stringf("y%d", i));
                    a.push_back(lut2(stringf("la%d", i), 0b0110, x, y));
                    b.push_back(lut1(stringf("lb%d", i), 0b01, y));
                }
                NetInfo *co = adder("add", a, b, gnd(), sum);
                for (int i = 0; i < 4; i++)
                    output(stringf("s%d", i), sum[i]);
                output("co", co);
            },
            400));
}

// Constant operand bits (e.g. adding a constant) are folded into the adder LUTs.
TEST_F(GateMateAdderTest, constant_operands)
{
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                std::vector<NetInfo *> a, b, sum;
                for (int i = 0; i < 6; i++) {
                    a.push_back(input(stringf("a%d", i)));
                    b.push_back((0b101101 >> i) & 1 ? vcc() : gnd());
                }
                b[4] = input("b4");
                NetInfo *co = adder("add", a, b, vcc(), sum);
                for (int i = 0; i < 6; i++)
                    output(stringf("s%d", i), sum[i]);
                output("co", co);
            },
            400));
}

// Registered sums: the registers are packed into the adder CPEs.
TEST_F(GateMateAdderTest, registered_sum)
{
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                NetInfo *clk = global_clock("clk");
                std::vector<NetInfo *> a, b, sum;
                for (int i = 0; i < 5; i++) {
                    a.push_back(input(stringf("a%d", i)));
                    b.push_back(input(stringf("b%d", i)));
                }
                NetInfo *co = adder("add", a, b, gnd(), sum);
                for (int i = 0; i < 5; i++)
                    output(stringf("q%d", i), dff(stringf("ff%d", i), sum[i], clk)->getPort(id_Q));
                output("qco", dff("ffco", co, clk)->getPort(id_Q));
            },
            400));
}

// A counter: register outputs feed back into the adder.
TEST_F(GateMateAdderTest, counter)
{
    EXPECT_TRUE(implementation_matches_reference(
            [&] {
                NetInfo *clk = global_clock("clk");
                NetInfo *rst = input("rst");
                const int width = 6;
                std::vector<CellInfo *> regs;
                std::vector<NetInfo *> q;
                for (int i = 0; i < width; i++) {
                    NetInfo *d = create_net(stringf("cnt_d%d", i));
                    regs.push_back(dff(stringf("cnt%d", i), d, clk, nullptr, rst));
                    q.push_back(regs.back()->getPort(id_Q));
                }
                std::vector<NetInfo *> zero, sum;
                for (int i = 0; i < width; i++)
                    zero.push_back(gnd());
                adder("inc", q, zero, vcc(), sum);
                for (int i = 0; i < width; i++) {
                    // Rewire the register input to the incremented value
                    regs[i]->disconnectPort(id_D);
                    regs[i]->connectPort(id_D, sum[i]);
                    output(stringf("q%d", i), q[i]);
                }
            },
            400));
}
