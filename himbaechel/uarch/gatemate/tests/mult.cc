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

#include <cstdlib>
#include "testing.h"
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

class GateMateMultTest : public GateMateTest
{
  protected:
    // A CC_MULT with P_WIDTH = A_WIDTH + B_WIDTH whose product bits all feed registers;
    // returns the registers in bit order.
    std::vector<CellInfo *> registered_mult(int a_width, int b_width, NetInfo *clk)
    {
        CellInfo *mult = ctx->createCell(ctx->id("mult"), id_CC_MULT);
        mult->params[id_A_WIDTH] = Property(a_width, 32);
        mult->params[id_B_WIDTH] = Property(b_width, 32);
        mult->params[id_P_WIDTH] = Property(a_width + b_width, 32);
        for (int i = 0; i < a_width; i++)
            connect_input(mult, ctx->idf("A[%d]", i), input(stringf("a%d", i)));
        for (int i = 0; i < b_width; i++)
            connect_input(mult, ctx->idf("B[%d]", i), input(stringf("b%d", i)));
        std::vector<CellInfo *> regs;
        for (int i = 0; i < a_width + b_width; i++) {
            NetInfo *p = connect_output(mult, ctx->idf("P[%d]", i));
            regs.push_back(dff(stringf("p_reg%d", i), p, clk));
            output(stringf("q%d", i), regs.back()->getPort(id_Q));
        }
        return regs;
    }
};

// Packing the product registers must not crash when only some of them have a reset; such
// registers are simply left unpacked.
TEST_F(GateMateMultTest, output_registers_with_mismatched_reset)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
            {
                NetInfo *clk = input("clk");
                auto regs = registered_mult(2, 2, clk);
                // Replace the reset of P[1]'s register with a real signal
                regs[1]->disconnectPort(id_SR);
                regs[1]->connectPort(id_SR, input("rst"));
                pack();
                std::exit(0);
            },
            ::testing::ExitedWithCode(0), "");
}

TEST_F(GateMateMultTest, output_registers_with_mismatched_enable)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
            {
                NetInfo *clk = input("clk");
                auto regs = registered_mult(2, 2, clk);
                regs[1]->disconnectPort(id_EN);
                regs[1]->connectPort(id_EN, input("en"));
                pack();
                std::exit(0);
            },
            ::testing::ExitedWithCode(0), "");
}
