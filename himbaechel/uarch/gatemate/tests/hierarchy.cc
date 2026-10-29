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
#include <sstream>
#include "json_frontend.h"
#include "jsonwrite.h"
#include "testing.h"
#include "timing.h"
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

// The hierarchical flow: a netlist synthesised without flattening (yosys synth_gatemate
// -noflatten -noiopad -noclkbuf, with the IO and clock buffers instantiated in submodules; the
// cells as yosys 0.70 writes them), which the JSON frontend flattens, giving the cells hierarchical names
// ("u_core.u_ram.ram") and recording the hierarchy in ctx->hierarchy. The design puts the parts
// the packer has to see across module boundaries into different modules: the IO buffers (in a
// module instantiated twice) away from the top level ports, the clock inverter away from the
// registers and the RAM it clocks, a carry chain, and a RAM with a LOC parameter and constant
// inputs it ignores.
namespace {

// A JSON bit list: the signals `first`.. `first + n - 1`
std::string bits(int first, int n)
{
    std::string s;
    for (int i = 0; i < n; i++)
        s += (i ? ", " : "") + std::to_string(first + i);
    return s;
}

// `n` constant bits
std::string consts(char value, int n)
{
    std::string s;
    for (int i = 0; i < n; i++)
        s += std::string(i ? ", " : "") + "\"" + value + "\"";
    return s;
}

std::string param(int value)
{
    std::string s;
    for (int i = 31; i >= 0; i--)
        s += (value >> i) & 1 ? '1' : '0';
    return "\"" + s + "\"";
}

// A 20K RAM, port A 5 bits wide, port B disabled. Module ports: clk 2, addr[2:0] 3-5, di[3:0] 6-9,
// we 10, do[1:0] 11-12. The address bits below bit 3 (unused at this width) and bit 6 (not there
// in a 20K RAM) are tied to 0.
std::string ram_module(const std::string &loc)
{
    std::string dirs, conns;
    auto port = [&](const std::string &name, const char *dir, const std::string &conn) {
        dirs += std::string(dirs.empty() ? "" : ", ") + "\"" + name + "\": \"" + dir + "\"";
        conns += std::string(conns.empty() ? "" : ", ") + "\"" + name + "\": [" + conn + "]";
    };
    port("A_CLK", "input", "2");
    port("A_EN", "input", consts('1', 1));
    port("A_WE", "input", "10");
    port("A_ADDR", "input", consts('0', 3) + ", " + bits(3, 3) + ", " + consts('0', 10));
    port("A_DI", "input", bits(6, 4) + ", " + consts('0', 16));
    port("A_BM", "input", consts('1', 20));
    port("A_DO", "output", bits(11, 2) + ", " + bits(100, 18));
    port("B_CLK", "input", consts('0', 1));
    port("B_EN", "input", consts('0', 1));
    port("B_WE", "input", consts('0', 1));
    port("B_ADDR", "input", consts('0', 16));
    port("B_DI", "input", consts('0', 20));
    port("B_BM", "input", consts('0', 20));
    port("B_DO", "output", bits(200, 20));
    return R"(
    "ram_blk": {
      "ports": {
        "clk": {"direction": "input", "bits": [2]},
        "addr": {"direction": "input", "bits": [3, 4, 5]},
        "di": {"direction": "input", "bits": [6, 7, 8, 9]},
        "we": {"direction": "input", "bits": [10]},
        "do": {"direction": "output", "bits": [11, 12]}
      },
      "cells": {
        "ram": {
          "type": "CC_BRAM_20K",
          "parameters": {"RAM_MODE": "TDP", "LOC": ")" +
           loc + R"(", "A_RD_WIDTH": )" + param(5) + R"(, "A_WR_WIDTH": )" + param(5) +
           R"(, "B_RD_WIDTH": )" + param(0) + R"(, "B_WR_WIDTH": )" + param(0) + R"(},
          "port_directions": {)" +
           dirs + R"(},
          "connections": {)" +
           conns + R"(}
        }
      },
      "netnames": {"addr": {"bits": [3, 4, 5]}, "do": {"bits": [11, 12]}}
    })";
}

// top(clk, a[3:0], b[3:0], we) -> sum[4:0] (registered a + b), q_neg (a[1] ^ sum[0] registered
// on the falling edge), rdo[1:0] (RAM written with a at address b[2:0], clocked on the falling edge)
std::string hierarchical_design(const std::string &ram_loc = "D0X1Y2")
{
    return R"({
  "creator": "hand written, in the form of yosys write_json",
  "modules": {
    "in_buf4": {
      "ports": {
        "pad": {"direction": "input", "bits": [2, 3, 4, 5]},
        "y": {"direction": "output", "bits": [6, 7, 8, 9]}
      },
      "cells": {
        "ibuf0": {"type": "CC_IBUF", "port_directions": {"I": "input", "Y": "output"}, "connections": {"I": [2], "Y": [6]}},
        "ibuf1": {"type": "CC_IBUF", "port_directions": {"I": "input", "Y": "output"}, "connections": {"I": [3], "Y": [7]}},
        "ibuf2": {"type": "CC_IBUF", "port_directions": {"I": "input", "Y": "output"}, "connections": {"I": [4], "Y": [8]}},
        "ibuf3": {"type": "CC_IBUF", "port_directions": {"I": "input", "Y": "output"}, "connections": {"I": [5], "Y": [9]}}
      },
      "netnames": {"pad": {"bits": [2, 3, 4, 5]}, "y": {"bits": [6, 7, 8, 9]}}
    },
    "in_buf1": {
      "ports": {"pad": {"direction": "input", "bits": [2]}, "y": {"direction": "output", "bits": [3]}},
      "cells": {
        "ibuf": {"type": "CC_IBUF", "port_directions": {"I": "input", "Y": "output"}, "connections": {"I": [2], "Y": [3]}}
      },
      "netnames": {"pad": {"bits": [2]}, "y": {"bits": [3]}}
    },
    "out_buf8": {
      "ports": {
        "i": {"direction": "input", "bits": [2, 3, 4, 5, 6, 7, 8, 9]},
        "pad": {"direction": "output", "bits": [10, 11, 12, 13, 14, 15, 16, 17]}
      },
      "cells": {
        "obuf0": {"type": "CC_OBUF", "port_directions": {"A": "input", "O": "output"}, "connections": {"A": [2], "O": [10]}},
        "obuf1": {"type": "CC_OBUF", "port_directions": {"A": "input", "O": "output"}, "connections": {"A": [3], "O": [11]}},
        "obuf2": {"type": "CC_OBUF", "port_directions": {"A": "input", "O": "output"}, "connections": {"A": [4], "O": [12]}},
        "obuf3": {"type": "CC_OBUF", "port_directions": {"A": "input", "O": "output"}, "connections": {"A": [5], "O": [13]}},
        "obuf4": {"type": "CC_OBUF", "port_directions": {"A": "input", "O": "output"}, "connections": {"A": [6], "O": [14]}},
        "obuf5": {"type": "CC_OBUF", "port_directions": {"A": "input", "O": "output"}, "connections": {"A": [7], "O": [15]}},
        "obuf6": {"type": "CC_OBUF", "port_directions": {"A": "input", "O": "output"}, "connections": {"A": [8], "O": [16]}},
        "obuf7": {"type": "CC_OBUF", "port_directions": {"A": "input", "O": "output"}, "connections": {"A": [9], "O": [17]}}
      },
      "netnames": {"i": {"bits": [2, 3, 4, 5, 6, 7, 8, 9]}, "pad": {"bits": [10, 11, 12, 13, 14, 15, 16, 17]}}
    },
    "clkgen": {
      "ports": {"pad": {"direction": "input", "bits": [2]}, "clk": {"direction": "output", "bits": [4]}},
      "cells": {
        "ibuf": {"type": "CC_IBUF", "port_directions": {"I": "input", "Y": "output"}, "connections": {"I": [2], "Y": [3]}},
        "bufg": {"type": "CC_BUFG", "port_directions": {"I": "input", "O": "output"}, "connections": {"I": [3], "O": [4]}}
      },
      "netnames": {"pad": {"bits": [2]}, "clk_in": {"bits": [3]}, "clk": {"bits": [4]}}
    },
    "inv": {
      "ports": {"i": {"direction": "input", "bits": [2]}, "o": {"direction": "output", "bits": [3]}},
      "cells": {
        "lut": {"type": "CC_LUT1", "parameters": {"INIT": "01"}, "port_directions": {"I0": "input", "O": "output"}, "connections": {"I0": [2], "O": [3]}}
      },
      "netnames": {"i": {"bits": [2]}, "o": {"bits": [3]}}
    },
    "adder4": {
      "ports": {
        "a": {"direction": "input", "bits": [2, 3, 4, 5]},
        "b": {"direction": "input", "bits": [6, 7, 8, 9]},
        "s": {"direction": "output", "bits": [10, 11, 12, 13, 14]}
      },
      "cells": {
        "add0": {"type": "CC_ADDF", "port_directions": {"A": "input", "B": "input", "CI": "input", "S": "output", "CO": "output"}, "connections": {"A": [2], "B": [6], "CI": ["0"], "S": [10], "CO": [15]}},
        "add1": {"type": "CC_ADDF", "port_directions": {"A": "input", "B": "input", "CI": "input", "S": "output", "CO": "output"}, "connections": {"A": [3], "B": [7], "CI": [15], "S": [11], "CO": [16]}},
        "add2": {"type": "CC_ADDF", "port_directions": {"A": "input", "B": "input", "CI": "input", "S": "output", "CO": "output"}, "connections": {"A": [4], "B": [8], "CI": [16], "S": [12], "CO": [17]}},
        "add3": {"type": "CC_ADDF", "port_directions": {"A": "input", "B": "input", "CI": "input", "S": "output", "CO": "output"}, "connections": {"A": [5], "B": [9], "CI": [17], "S": [13], "CO": [18]}},
        "add4": {"type": "CC_ADDF", "port_directions": {"A": "input", "B": "input", "CI": "input", "S": "output", "CO": "output"}, "connections": {"A": ["0"], "B": ["0"], "CI": [18], "S": [14], "CO": [19]}}
      },
      "netnames": {"a": {"bits": [2, 3, 4, 5]}, "b": {"bits": [6, 7, 8, 9]}, "s": {"bits": [10, 11, 12, 13, 14]}, "carry": {"bits": [15, 16, 17, 18]}}
    },
    "reg5": {
      "ports": {
        "clk": {"direction": "input", "bits": [2]},
        "d": {"direction": "input", "bits": [3, 4, 5, 6, 7]},
        "q": {"direction": "output", "bits": [8, 9, 10, 11, 12]}
      },
      "cells": {
        "ff0": {"type": "CC_DFF", "parameters": {"CLK_INV": "0", "EN_INV": "0", "INIT": "x", "SR_INV": "0", "SR_VAL": "0"}, "port_directions": {"D": "input", "CLK": "input", "EN": "input", "SR": "input", "Q": "output"}, "connections": {"D": [3], "CLK": [2], "EN": ["1"], "SR": ["0"], "Q": [8]}},
        "ff1": {"type": "CC_DFF", "parameters": {"CLK_INV": "0", "EN_INV": "0", "INIT": "x", "SR_INV": "0", "SR_VAL": "0"}, "port_directions": {"D": "input", "CLK": "input", "EN": "input", "SR": "input", "Q": "output"}, "connections": {"D": [4], "CLK": [2], "EN": ["1"], "SR": ["0"], "Q": [9]}},
        "ff2": {"type": "CC_DFF", "parameters": {"CLK_INV": "0", "EN_INV": "0", "INIT": "x", "SR_INV": "0", "SR_VAL": "0"}, "port_directions": {"D": "input", "CLK": "input", "EN": "input", "SR": "input", "Q": "output"}, "connections": {"D": [5], "CLK": [2], "EN": ["1"], "SR": ["0"], "Q": [10]}},
        "ff3": {"type": "CC_DFF", "parameters": {"CLK_INV": "0", "EN_INV": "0", "INIT": "x", "SR_INV": "0", "SR_VAL": "0"}, "port_directions": {"D": "input", "CLK": "input", "EN": "input", "SR": "input", "Q": "output"}, "connections": {"D": [6], "CLK": [2], "EN": ["1"], "SR": ["0"], "Q": [11]}},
        "ff4": {"type": "CC_DFF", "parameters": {"CLK_INV": "0", "EN_INV": "0", "INIT": "x", "SR_INV": "0", "SR_VAL": "0"}, "port_directions": {"D": "input", "CLK": "input", "EN": "input", "SR": "input", "Q": "output"}, "connections": {"D": [7], "CLK": [2], "EN": ["1"], "SR": ["0"], "Q": [12]}}
      },
      "netnames": {"clk": {"bits": [2]}, "d": {"bits": [3, 4, 5, 6, 7]}, "q": {"bits": [8, 9, 10, 11, 12]}}
    },
    "negreg": {
      "ports": {
        "clk": {"direction": "input", "bits": [2]},
        "a": {"direction": "input", "bits": [3]},
        "b": {"direction": "input", "bits": [4]},
        "q": {"direction": "output", "bits": [6]}
      },
      "cells": {
        "xor": {"type": "CC_LUT2", "parameters": {"INIT": "0110"}, "port_directions": {"I0": "input", "I1": "input", "O": "output"}, "connections": {"I0": [3], "I1": [4], "O": [5]}},
        "ff": {"type": "CC_DFF", "parameters": {"CLK_INV": "0", "EN_INV": "0", "INIT": "x", "SR_INV": "0", "SR_VAL": "0"}, "port_directions": {"D": "input", "CLK": "input", "EN": "input", "SR": "input", "Q": "output"}, "connections": {"D": [5], "CLK": [2], "EN": ["1"], "SR": ["0"], "Q": [6]}}
      },
      "netnames": {"clk": {"bits": [2]}, "x": {"bits": [5]}, "q": {"bits": [6]}}
    },)" + ram_module(ram_loc) +
           R"(,
    "core": {
      "ports": {
        "clk": {"direction": "input", "bits": [2]},
        "clk_n": {"direction": "input", "bits": [3]},
        "a": {"direction": "input", "bits": [4, 5, 6, 7]},
        "b": {"direction": "input", "bits": [8, 9, 10, 11]},
        "we": {"direction": "input", "bits": [12]},
        "sum": {"direction": "output", "bits": [13, 14, 15, 16, 17]},
        "q_neg": {"direction": "output", "bits": [18]},
        "rdo": {"direction": "output", "bits": [19, 20]}
      },
      "cells": {
        "u_add": {"type": "adder4", "connections": {"a": [4, 5, 6, 7], "b": [8, 9, 10, 11], "s": [21, 22, 23, 24, 25]}},
        "u_reg": {"type": "reg5", "connections": {"clk": [2], "d": [21, 22, 23, 24, 25], "q": [13, 14, 15, 16, 17]}},
        "u_neg": {"type": "negreg", "connections": {"clk": [3], "a": [5], "b": [13], "q": [18]}},
        "u_ram": {"type": "ram_blk", "connections": {"clk": [3], "addr": [8, 9, 10], "di": [4, 5, 6, 7], "we": [12], "do": [19, 20]}}
      },
      "netnames": {"s": {"bits": [21, 22, 23, 24, 25]}}
    },
    "top": {
      "attributes": {"top": "00000000000000000000000000000001"},
      "ports": {
        "clk": {"direction": "input", "bits": [2]},
        "a": {"direction": "input", "bits": [3, 4, 5, 6]},
        "b": {"direction": "input", "bits": [7, 8, 9, 10]},
        "we": {"direction": "input", "bits": [11]},
        "sum": {"direction": "output", "bits": [12, 13, 14, 15, 16]},
        "q_neg": {"direction": "output", "bits": [17]},
        "rdo": {"direction": "output", "bits": [18, 19]}
      },
      "cells": {
        "u_clk": {"type": "clkgen", "connections": {"pad": [2], "clk": [20]}},
        "u_inv": {"type": "inv", "connections": {"i": [20], "o": [21]}},
        "u_ina": {"type": "in_buf4", "connections": {"pad": [3, 4, 5, 6], "y": [22, 23, 24, 25]}},
        "u_inb": {"type": "in_buf4", "connections": {"pad": [7, 8, 9, 10], "y": [26, 27, 28, 29]}},
        "u_inwe": {"type": "in_buf1", "connections": {"pad": [11], "y": [30]}},
        "u_core": {"type": "core", "connections": {"clk": [20], "clk_n": [21], "a": [22, 23, 24, 25], "b": [26, 27, 28, 29], "we": [30], "sum": [31, 32, 33, 34, 35], "q_neg": [36], "rdo": [37, 38]}},
        "u_out": {"type": "out_buf8", "connections": {"i": [31, 32, 33, 34, 35, 36, 37, 38], "pad": [12, 13, 14, 15, 16, 17, 18, 19]}}
      },
      "netnames": {"gclk": {"bits": [20]}, "gclk_n": {"bits": [21]}}
    }
  }
})";
}

} // namespace

class GateMateHierarchyTest : public GateMateTest
{
  protected:
    void load(const std::string &json)
    {
        std::istringstream in(json);
        ASSERT_TRUE(parse_json(in, "hierarchy.json", ctx));
    }

    CellInfo *cell(const std::string &name)
    {
        IdString id = ctx->id(name);
        return ctx->cells.count(id) ? ctx->cells.at(id).get() : nullptr;
    }
};

TEST_F(GateMateHierarchyTest, frontend_keeps_the_hierarchy)
{
    load(hierarchical_design());
    for (auto path : {"top", "top/u_core", "top/u_core/u_ram", "top/u_ina", "top/u_inb"})
        EXPECT_TRUE(ctx->hierarchy.count(ctx->id(path))) << path;
    CellInfo *ram = cell("u_core.u_ram.ram");
    ASSERT_NE(ram, nullptr);
    EXPECT_EQ(ram->hierpath, ctx->id("top/u_core/u_ram"));
    // The module instantiated twice gives two sets of buffers
    EXPECT_NE(cell("u_ina.ibuf0"), nullptr);
    EXPECT_NE(cell("u_inb.ibuf0"), nullptr);
}

TEST_F(GateMateHierarchyTest, implementation_matches_reference)
{
    EXPECT_TRUE(implementation_matches_reference([&] { load(hierarchical_design()); }, 400));
}

TEST_F(GateMateHierarchyTest, packer_fixes_work_across_modules)
{
    load(hierarchical_design());
    ASSERT_TRUE(packs());
    // Bug 35: the inverter in its own module folds into the registers and the RAM it clocks
    EXPECT_EQ(cell("u_inv.lut"), nullptr) << "the clock inverter is a LUT of its own";
    const NetInfo *ff_clk = cell("u_core.u_neg.ff")->getPort(id_CLK_INT);
    ASSERT_NE(ff_clk, nullptr);
    EXPECT_EQ(ff_clk->driver.cell->type, id_GLBOUT) << "register clock " << ff_clk->name.c_str(ctx);
    // Bug 34: the constant RAM inputs the RAM ignores (address bits 0-2 and 6, port B) have no CPE
    // (each constant bit comes from a GND/VCC cell of its own from the frontend)
    std::set<std::string> pins;
    const std::string prefix = "u_core.u_ram.ram$";
    for (auto &c : ctx->cells) {
        std::string name = c.first.str(ctx);
        if (c.second->type.in(id_CPE_RAMI, id_CPE_RAMO, id_CPE_RAMIO) && boost::starts_with(name, prefix))
            pins.insert(name.substr(prefix.size(), name.rfind("_ramo") - prefix.size()));
    }
    EXPECT_TRUE(pins.count("ADDRA0[3]"));
    for (auto pin : {"ADDRA0[0]", "ADDRA0[1]", "ADDRA0[2]", "ADDRA0[6]"})
        EXPECT_FALSE(pins.count(pin)) << pin;
    for (auto &pin : pins)
        EXPECT_EQ(pin.find("B"), std::string::npos) << "port B pin " << pin;

    ctx->place();
    // Bug 20: the LOC parameter of the RAM in the submodule places it
    std::set<int> xs, ys;
    for (BelId bel : ctx->getBels())
        if (ctx->getBelType(bel) == id_RAM) {
            xs.insert(ctx->getBelLocation(bel).x);
            ys.insert(ctx->getBelLocation(bel).y);
        }
    std::vector<int> col(xs.begin(), xs.end()), row(ys.begin(), ys.end());
    const CellInfo *ram = nullptr;
    for (auto &c : ctx->cells)
        if (c.second->type.in(id_RAM, id_RAM_HALF))
            ram = c.second.get();
    ASSERT_NE(ram, nullptr);
    ASSERT_NE(ram->bel, BelId());
    Loc loc = ctx->getBelLocation(ram->bel);
    EXPECT_EQ(loc.x, col.at(1));
    EXPECT_GE(loc.y, row.at(2));
    EXPECT_LT(loc.y, row.at(3));
    // Bugs 38/39: after repack_ram all clusters have a live root
    for (auto &c : ctx->cells)
        if (c.second->cluster != ClusterId())
            EXPECT_TRUE(ctx->cells.count(c.second->cluster))
                    << c.first.c_str(ctx) << " is in the deleted cluster " << c.second->cluster.c_str(ctx);

    ctx->route();
    // Bugs 6/32: after routing, the timing analysis finds the clocks of the registers on both edges
    timing_analysis(ctx, false, true, false, false, true);
    auto &result = ctx->timing_result;
    ASSERT_TRUE(result.clock_fmax.count(ctx->id("gclk")));
    EXPECT_GT(result.clock_fmax.at(ctx->id("gclk")).achieved, 0);
    // The path from the adder's register to the register on the falling edge
    ASSERT_TRUE(result.clock_paths.count(ctx->id("gclk")));
    const CriticalPath &path = result.clock_paths.at(ctx->id("gclk"));
    ASSERT_FALSE(path.segments.empty());
    EXPECT_EQ(path.clock_pair.start.edge, RISING_EDGE);
    EXPECT_EQ(path.clock_pair.end.edge, FALLING_EDGE);

    // The hierarchy can be brought up to date with the packed netlist, and written out
    ctx->fixupHierarchy();
    for (auto &h : ctx->hierarchy)
        for (auto &lc : h.second.leaf_cells)
            EXPECT_TRUE(ctx->cells.count(lc.second)) << lc.second.c_str(ctx);
    std::ostringstream out;
    std::string filename = "routed.json";
    EXPECT_TRUE(write_json_file(out, filename, ctx));
    EXPECT_NE(out.str().find("u_core.u_ram"), std::string::npos);
}

// Bug 19 through the hierarchy: a top level input that reaches a LUT in a submodule without an IO
// buffer on the way
TEST_F(GateMateHierarchyTest, top_level_port_without_io_buffer_in_submodule)
{
    load(R"({
  "modules": {
    "sub": {
      "ports": {"i": {"direction": "input", "bits": [2]}, "o": {"direction": "output", "bits": [4]}},
      "cells": {
        "lut": {"type": "CC_LUT1", "parameters": {"INIT": "01"}, "port_directions": {"I0": "input", "O": "output"}, "connections": {"I0": [2], "O": [3]}},
        "obuf": {"type": "CC_OBUF", "port_directions": {"A": "input", "O": "output"}, "connections": {"A": [3], "O": [4]}}
      }
    },
    "top": {
      "attributes": {"top": "00000000000000000000000000000001"},
      "ports": {"a": {"direction": "input", "bits": [2]}, "y": {"direction": "output", "bits": [3]}},
      "cells": {"u_sub": {"type": "sub", "connections": {"i": [2], "o": [3]}}}
    }
  }
})");
    EXPECT_LOG_ERROR(pack(), "without an IO buffer");
}
