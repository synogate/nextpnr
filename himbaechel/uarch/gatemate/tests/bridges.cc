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

// Routing through the 8:1 multiplexer of a CPE (CPE.C_SN, a "bridge"). Its C_BR setting drives DIN2
// from MUXOUT, so a register or latch in the upper half of the CPE must keep bridges out of it.

#include "testing.h"

#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

class GateMateBridgeTest : public GateMateTest
{
  protected:
    // The bridge pips (CPE.C_SN) of a tile
    std::vector<PipId> bridge_pips(int tile)
    {
        std::vector<PipId> pips;
        auto &type = chip_tile_info(ctx->chip_info, tile);
        for (int i = 0; i < type.pips.ssize(); i++) {
            PipId pip(tile, i);
            const auto &extra_data = *impl->pip_extra_data(pip);
            if (extra_data.type == PipExtra::PIP_EXTRA_MUX && (extra_data.flags & MUX_ROUTING))
                pips.push_back(pip);
        }
        return pips;
    }

    // Whether the register part of the upper half of the CPE in `tile` holds a cell of `type`
    bool upper_register_is(int tile, IdString type)
    {
        int x, y;
        tile_xy(ctx->chip_info, tile, x, y);
        BelId bel = ctx->getBelByLocation(Loc(x, y, CPE_FF_U_Z));
        CellInfo *ci = bel == BelId() ? nullptr : ctx->getBoundBelCell(bel);
        return ci && ci->type == type;
    }
};

TEST_F(GateMateBridgeTest, upper_latch_blocks_bridges)
{
    // Latches and registers enough that the placer puts some into upper halves
    NetInfo *g = input("g");
    NetInfo *clk = global_clock("clk");
    for (int i = 0; i < 24; i++) {
        NetInfo *d = input(stringf("d%d", i));
        output(stringf("q%d", i), dlt(stringf("latch%d", i), d, g)->getPort(id_Q));
        output(stringf("r%d", i), dff(stringf("ff%d", i), d, clk)->getPort(id_Q));
    }
    ASSERT_TRUE(packs());
    ASSERT_TRUE(ctx->place());

    int latch_tiles = 0, ff_tiles = 0, free_tiles = 0;
    for (int tile = 0; tile < int(impl->used_cpes.size()); tile++) {
        auto pips = bridge_pips(tile);
        if (pips.empty())
            continue;
        bool latch = upper_register_is(tile, id_CPE_LATCH), ff = upper_register_is(tile, id_CPE_FF);
        if (latch || ff) {
            latch_tiles += latch;
            ff_tiles += ff;
            // As postPlace() marks it, and as update_used_cpe() does for a register moved later
            EXPECT_TRUE(impl->used_cpes[tile]) << ctx->getBelName(BelId(tile, 0)).str(ctx);
            impl->used_cpes[tile] = false;
            impl->update_used_cpe(tile);
            EXPECT_TRUE(impl->used_cpes[tile]) << ctx->getBelName(BelId(tile, 0)).str(ctx);
            for (PipId pip : pips)
                EXPECT_FALSE(impl->checkPipAvail(pip)) << ctx->nameOfPip(pip);
        } else if (!impl->used_cpes[tile] && free_tiles++ == 0) {
            // A CPE without such a register keeps its bridges
            for (PipId pip : pips)
                EXPECT_TRUE(impl->checkPipAvail(pip)) << ctx->nameOfPip(pip);
        }
    }
    EXPECT_GT(latch_tiles, 0) << "no latch was placed in an upper half";
    EXPECT_GT(ff_tiles, 0) << "no register was placed in an upper half";
    EXPECT_GT(free_tiles, 0);
}
