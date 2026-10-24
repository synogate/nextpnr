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

// The extra router cost of the scarce switch box muxes (--vopt sb-y-cost, sb-ydiag-cost).

#include "router2.h"
#include "testing.h"

USING_NEXTPNR_NAMESPACE

class GateMateRouterCostTest : public GateMateTest
{
  protected:
    // A pip of the tile at (x, y) driving the tile wire called `dst`
    PipId pip_to(int x, int y, const std::string &dst)
    {
        int tile = tile_by_xy(ctx->chip_info, x, y);
        auto &type = chip_tile_info(ctx->chip_info, tile);
        for (int i = 0; i < type.pips.ssize(); i++)
            if (IdString(type.wires[type.pips[i].dst_wire].name).str(ctx) == dst)
                return PipId(tile, i);
        return PipId();
    }

    // Extra cost of entering pip's wire, at the given criticality weight
    float extra(const Router2Cfg &cfg, PipId pip, float crit_weight)
    {
        WireId wire = ctx->getPipDstWire(pip);
        return cfg.get_base_cost(ctx, wire, pip, crit_weight) - default_base_cost(ctx, wire, pip, crit_weight);
    }
};

TEST_F(GateMateRouterCostTest, scarce_switch_box_muxes_cost_more_for_uncritical_nets)
{
    init_context("CCGM1A1", {"--sb-y-cost=0.3", "--sb-ydiag-cost=0.2"});
    Router2Cfg cfg(ctx);
    impl->configureRouter2(cfg);
    // (83, 67) has a big switch box, (85, 67) a small one
    PipId big_y = pip_to(83, 67, "SB_BIG.P05.Y2"), small_y = pip_to(85, 67, "SB_SML.P05.Y2_int");
    PipId ydiag = pip_to(85, 67, "SB_SML.P05.YDIAG_int"), im = pip_to(83, 67, "IM.P05.Y");
    ASSERT_NE(big_y, PipId());
    ASSERT_NE(small_y, PipId());
    ASSERT_NE(ydiag, PipId());
    ASSERT_NE(im, PipId());
    EXPECT_FLOAT_EQ(extra(cfg, big_y, 1.0f), 0.3f);
    EXPECT_FLOAT_EQ(extra(cfg, small_y, 1.0f), 0.3f);
    EXPECT_FLOAT_EQ(extra(cfg, ydiag, 1.0f), 0.2f);
    EXPECT_FLOAT_EQ(extra(cfg, im, 1.0f), 0.0f);
    // The router weights congestion, and with it this cost, down for critical connections
    EXPECT_FLOAT_EQ(extra(cfg, big_y, 0.05f), 0.015f);
}

TEST_F(GateMateRouterCostTest, no_extra_cost_by_default)
{
    Router2Cfg cfg(ctx);
    impl->configureRouter2(cfg);
    PipId big_y = pip_to(83, 67, "SB_BIG.P05.Y2");
    ASSERT_NE(big_y, PipId());
    EXPECT_FLOAT_EQ(extra(cfg, big_y, 1.0f), 0.0f);
}
