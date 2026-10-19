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

// Checks of the test harness itself: the simulator must agree with the CC_* reference models
// and must detect a wrong implementation.

#include <memory>
#include "netlist_eval.h"
#include "testing.h"
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

TEST(GateMateSimulator, lut2_kleene)
{
    // AND with one unknown input is unknown unless the other input is 0
    EXPECT_EQ(NetlistEvaluator::lut2(0b1000, Tri::F, Tri::X), Tri::F);
    EXPECT_EQ(NetlistEvaluator::lut2(0b1000, Tri::T, Tri::X), Tri::X);
    // A LUT that ignores D1 does not depend on it
    EXPECT_EQ(NetlistEvaluator::lut2(0b1010, Tri::T, Tri::X), Tri::T);
    EXPECT_EQ(NetlistEvaluator::lut2(0b0000, Tri::X, Tri::X), Tri::F);
}

TEST_F(GateMateTest, reference_register_follows_cells_sim)
{
    NetInfo *clk = input("clk");
    CellInfo *ff = dff("ff", input("d"), clk, input("en"), input("sr"));
    ff->params[id_SR_VAL] = Property(1, 1);
    output("q", ff->getPort(id_Q));
    NetlistEvaluator ev(ctx);
    auto apply = [&](bool c, bool d, bool en, bool sr) {
        ev.set_input(ctx->id("clk"), c);
        ev.set_input(ctx->id("d"), d);
        ev.set_input(ctx->id("en"), en);
        ev.set_input(ctx->id("sr"), sr);
        ev.settle();
        return ev.eval_output(ctx->id("q"));
    };
    EXPECT_EQ(apply(false, true, true, false), Tri::X);  // no INIT
    EXPECT_EQ(apply(true, true, true, false), Tri::T);   // rising edge loads D
    EXPECT_EQ(apply(false, false, true, false), Tri::T); // falling edge does nothing
    EXPECT_EQ(apply(true, false, false, false), Tri::T); // disabled
    EXPECT_EQ(apply(false, false, true, false), Tri::T);
    EXPECT_EQ(apply(true, false, true, false), Tri::F);  // enabled edge
    EXPECT_EQ(apply(true, false, true, true), Tri::T);   // asynchronous set (SR_VAL = 1)
    EXPECT_EQ(apply(true, false, true, false), Tri::T);  // still set, no edge
}

TEST_F(GateMateTest, reference_latch_follows_cells_sim)
{
    CellInfo *latch = dlt("latch", input("d"), input("g"));
    output("q", latch->getPort(id_Q));
    NetlistEvaluator ev(ctx);
    auto apply = [&](bool g, bool d) {
        ev.set_input(ctx->id("g"), g);
        ev.set_input(ctx->id("d"), d);
        ev.settle();
        return ev.eval_output(ctx->id("q"));
    };
    EXPECT_EQ(apply(false, true), Tri::X);
    EXPECT_EQ(apply(true, true), Tri::T);  // transparent
    EXPECT_EQ(apply(true, false), Tri::F); // follows D
    EXPECT_EQ(apply(false, true), Tri::F); // holds
}

TEST_F(GateMateTest, matching_detects_wrong_implementation)
{
    // Reference computes AND, the "implementation" computes OR
    output("o", lut2("lut", 0b1000, input("x"), input("y")));
    std::unique_ptr<Context> reference(ctx);
    ctx = nullptr;
    init_context();
    output("o", lut2("lut", 0b1110, input("x"), input("y")));
    pack();
    ctx->place();
    EXPECT_FALSE(outputs_match(reference.get(), ctx, 32, 1));
}

TEST_F(GateMateTest, matching_combinational)
{
    EXPECT_TRUE(implementation_matches_reference([&] {
        NetInfo *x = input("x"), *y = input("y"), *z = input("z");
        output("o1", lut2("and", 0b1000, x, y));
        output("o2", l2t4("tree", 0b0110, 0b1000, 0b1110, x, y, z, x));
        output("o3", mx2("mux", x, y, z));
    }));
}

TEST_F(GateMateTest, matching_register)
{
    EXPECT_TRUE(implementation_matches_reference([&] {
        CellInfo *ff = dff("ff", input("d"), input("clk"));
        output("q", ff->getPort(id_Q));
    }));
}
