/*
 *  nextpnr -- Next Generation Place and Route
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

#include <sstream>
#include "json_frontend.h"
#include "testing.h"

USING_NEXTPNR_NAMESPACE

// By default (--no-rename-auto-cells turns it off) the frontend names the cells yosys and abc named automatically after the nets they drive.
// The netlist is flat, as synth_gatemate writes it, with the names it gives: "$iopadmap$top.<port>" IO buffers,
// "$abc$..." LUTs, "$auto$ff.cc..." flip-flops, "$auto$alumacc.cc..." adder slices; and the nets with an HDL name
// and the name GHDL gave the expression ("n12_o") on the same bit.
namespace {

std::string design()
{
    return R"({
  "modules": {
    "top": {
      "attributes": {"top": "00000000000000000000000000000001"},
      "ports": {
        "clk": {"direction": "input", "bits": [2]},
        "a": {"direction": "input", "bits": [3]},
        "b": {"direction": "input", "bits": [4]},
        "q": {"direction": "output", "bits": [5]},
        "lt": {"direction": "output", "bits": [6]}
      },
      "cells": {
        "$iopadmap$top.clk": {"type": "CC_IBUF", "port_directions": {"I": "input", "Y": "output"}, "connections": {"I": [2], "Y": [10]}},
        "$iopadmap$top.a": {"type": "CC_IBUF", "port_directions": {"I": "input", "Y": "output"}, "connections": {"I": [3], "Y": [11]}},
        "$iopadmap$top.b": {"type": "CC_IBUF", "port_directions": {"I": "input", "Y": "output"}, "connections": {"I": [4], "Y": [12]}},
        "$abc$1$auto$blifparse.cc:397:parse_blif$2": {"type": "CC_LUT2", "parameters": {"INIT": "0110"}, "port_directions": {"I0": "input", "I1": "input", "O": "output"}, "connections": {"I0": [11], "I1": [12], "O": [13]}},
        "$abc$1$auto$blifparse.cc:397:parse_blif$3": {"type": "CC_LUT2", "parameters": {"INIT": "1000"}, "port_directions": {"I0": "input", "I1": "input", "O": "output"}, "connections": {"I0": [13], "I1": [11], "O": [14]}},
        "$auto$ff.cc:337:slice$4": {"type": "CC_DFF", "parameters": {"CLK_INV": "0", "EN_INV": "0", "INIT": "x", "SR_INV": "0", "SR_VAL": "0"}, "port_directions": {"D": "input", "CLK": "input", "EN": "input", "SR": "input", "Q": "output"}, "connections": {"D": [14], "CLK": [10], "EN": ["1"], "SR": ["0"], "Q": [15]}},
        "$iopadmap$top.q": {"type": "CC_OBUF", "port_directions": {"A": "input", "O": "output"}, "connections": {"A": [15], "O": [5]}},
        "$auto$alumacc.cc:512:replace_alu$5.slice[0].addf_i": {"type": "CC_ADDF", "port_directions": {"A": "input", "B": "input", "CI": "input", "S": "output", "CO": "output"}, "connections": {"A": [11], "B": [12], "CI": ["1"], "S": [20], "CO": [21]}},
        "$auto$alumacc.cc:512:replace_alu$5.slice[1].addf_i": {"type": "CC_ADDF", "port_directions": {"A": "input", "B": "input", "CI": "input", "S": "output", "CO": "output"}, "connections": {"A": [12], "B": [11], "CI": [21], "S": [22], "CO": [23]}},
        "$auto$alumacc.cc:512:replace_alu$5.slice[2].addf_i": {"type": "CC_ADDF", "port_directions": {"A": "input", "B": "input", "CI": "input", "S": "output", "CO": "output"}, "connections": {"A": [15], "B": [11], "CI": [23], "S": [24], "CO": [25]}},
        "$abc$1$auto$blifparse.cc:397:parse_blif$6": {"type": "CC_LUT1", "parameters": {"INIT": "01"}, "port_directions": {"I0": "input", "O": "output"}, "connections": {"I0": [25], "O": [26]}},
        "$iopadmap$top.lt": {"type": "CC_OBUF", "port_directions": {"A": "input", "O": "output"}, "connections": {"A": [26], "O": [6]}}
      },
      "netnames": {
        "clk": {"hide_name": 0, "bits": [2]},
        "a": {"hide_name": 0, "bits": [3]},
        "b": {"hide_name": 0, "bits": [4]},
        "q": {"hide_name": 0, "bits": [5]},
        "lt": {"hide_name": 0, "bits": [6]},
        "$iopadmap$clk": {"hide_name": 1, "bits": [10]},
        "s_a": {"hide_name": 0, "bits": [11]},
        "n5_o": {"hide_name": 0, "bits": [11]},
        "s_b": {"hide_name": 0, "bits": [12]},
        "$abc$1$new_n3": {"hide_name": 1, "bits": [13]},
        "u_core.s_x": {"hide_name": 0, "bits": [14]},
        "u_core.n9_o": {"hide_name": 0, "bits": [14]},
        "u_core.s_q": {"hide_name": 0, "bits": [15]},
        "u_core.n11_q": {"hide_name": 0, "bits": [15]},
        "$auto$alumacc.cc:512:replace_alu$5.Y": {"hide_name": 1, "bits": [20, 22, 24]},
        "$auto$alumacc.cc:512:replace_alu$5.CO": {"hide_name": 1, "bits": [21, 23, 25]},
        "s_lt": {"hide_name": 0, "bits": [26]}
      }
    }
  }
})";
}

} // namespace

class GateMateAutoNamesTest : public GateMateTest
{
  protected:
    void load(bool rename)
    {
        if (!rename)
            ctx->settings[ctx->id("frontend/renameAutoCells")] = false;
        std::istringstream in(design());
        ASSERT_TRUE(parse_json(in, "auto_names.json", ctx));
    }

    bool has_cell(const std::string &name) { return ctx->cells.count(ctx->id(name)) > 0; }
    std::string type_of(const std::string &name) { return ctx->cells.at(ctx->id(name))->type.str(ctx); }
};

TEST_F(GateMateAutoNamesTest, cells_named_after_the_nets_they_drive)
{
    load(true);
    // named after the HDL name of the net, not the GHDL one
    ASSERT_TRUE(has_cell("u_core.s_x"));
    EXPECT_EQ(type_of("u_core.s_x"), "CC_LUT2");
    ASSERT_TRUE(has_cell("u_core.s_q"));
    EXPECT_EQ(type_of("u_core.s_q"), "CC_DFF");
    ASSERT_TRUE(has_cell("s_lt"));
    EXPECT_EQ(type_of("s_lt"), "CC_LUT1");
    // the LUT inside the abc cone after the LUT it feeds
    ASSERT_TRUE(has_cell("u_core.s_x$I0"));
    EXPECT_EQ(type_of("u_core.s_x$I0"), "CC_LUT2");
    // the carry chain without a used sum counts up the CI hops
    EXPECT_EQ(type_of("s_lt$I0"), "CC_ADDF");
    EXPECT_EQ(type_of("s_lt$I0$CI"), "CC_ADDF");
    EXPECT_EQ(type_of("s_lt$I0$CIx2"), "CC_ADDF");
    // IO buffers after their pads; the port cells the frontend makes have the port name already
    EXPECT_EQ(type_of("clk$ibuf"), "CC_IBUF");
    EXPECT_EQ(type_of("a$ibuf"), "CC_IBUF");
    EXPECT_EQ(type_of("q$obuf"), "CC_OBUF");
    EXPECT_EQ(type_of("lt$obuf"), "CC_OBUF");
    for (auto &cell : ctx->cells)
        if (cell.second->type.str(ctx).rfind("CC_", 0) == 0)
            EXPECT_NE(cell.first.c_str(ctx)[0], '$') << cell.first.str(ctx);
}

TEST_F(GateMateAutoNamesTest, nets_take_the_hdl_name)
{
    load(true);
    EXPECT_TRUE(ctx->nets.count(ctx->id("s_a")));
    EXPECT_FALSE(ctx->nets.count(ctx->id("n5_o")));
    EXPECT_TRUE(ctx->nets.count(ctx->id("u_core.s_q")));
    // the other names still find the net
    EXPECT_EQ(ctx->getNetByAlias(ctx->id("n5_o")), ctx->nets.at(ctx->id("s_a")).get());
    EXPECT_EQ(ctx->getNetByAlias(ctx->id("u_core.n11_q")), ctx->nets.at(ctx->id("u_core.s_q")).get());
}

TEST_F(GateMateAutoNamesTest, hierarchy_follows_the_new_names)
{
    load(true);
    auto &top = ctx->hierarchy.at(ctx->top_module);
    for (auto &cell : ctx->cells) {
        ASSERT_TRUE(top.leaf_cells_by_gname.count(cell.first)) << cell.first.str(ctx);
        EXPECT_EQ(top.leaf_cells.at(top.leaf_cells_by_gname.at(cell.first)), cell.first);
    }
    EXPECT_EQ(top.leaf_cells_by_gname.size(), ctx->cells.size());
}

TEST_F(GateMateAutoNamesTest, turned_off_keeps_the_yosys_names)
{
    load(false);
    EXPECT_TRUE(has_cell("$auto$ff.cc:337:slice$4"));
    EXPECT_TRUE(has_cell("$abc$1$auto$blifparse.cc:397:parse_blif$2"));
    EXPECT_FALSE(has_cell("u_core.s_q"));
}

TEST_F(GateMateAutoNamesTest, implementation_matches_reference)
{
    EXPECT_TRUE(implementation_matches_reference([&] { load(true); }, 200));
}
