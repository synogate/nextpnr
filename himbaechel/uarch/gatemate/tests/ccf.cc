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

// The CCF constraints file (--vopt ccf=...): pin locations, IO parameters, defaults, floorplanning
// regions and the error messages for malformed files.

#include "testing.h"
#include "uarch/gatemate/extra_data.h"
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

class GateMateCcfTest : public GateMateTest
{
  protected:
    void SetUp() override
    {
        GateMateTest::SetUp();
        pins = package_pins();
    }

    // A design with two inputs and two outputs, as the frontend creates it
    void small_design()
    {
        NetInfo *a = top_input("a"), *b = top_input("b");
        top_output("x", lut2("and", 0b1000, a, b));
        top_output("y", lut2("or", 0b1110, a, b));
    }

    // Parameter of the IOSEL of a pad after packing
    Property iosel_param(const std::string &port, IdString param)
    {
        CellInfo *iosel = ctx->cells.at(ctx->idf("%s$iopad$iosel", port.c_str())).get();
        if (!iosel->params.count(param))
            return Property();
        return iosel->params.at(param);
    }

    // Value of an IOSEL parameter, -1 if not set
    int iosel_int(const std::string &port, IdString param)
    {
        Property p = iosel_param(port, param);
        return (p.size() > 0 && p.is_fully_def()) ? int(p.as_int64()) : -1;
    }

    // The CCF lines constraining all four pins of small_design() to the first package pins
    std::string all_locations()
    {
        return stringf("NET \"a\" Loc = \"%s\";\nNET \"b\" Loc = \"%s\";\nNET \"x\" Loc = \"%s\";\nNET \"y\" Loc = "
                       "\"%s\";\n",
                       pins[0].c_str(), pins[1].c_str(), pins[2].c_str(), pins[3].c_str());
    }

    std::vector<std::string> pins;
};

TEST_F(GateMateCcfTest, locations)
{
    init_with_ccf(all_locations());
    small_design();
    ASSERT_TRUE(packs());
    EXPECT_EQ(package_pin("a$iopad"), pins[0]);
    EXPECT_EQ(package_pin("b$iopad"), pins[1]);
    EXPECT_EQ(package_pin("x$iopad"), pins[2]);
    EXPECT_EQ(package_pin("y$iopad"), pins[3]);
}

// Keywords and parameter names in any case, comments, statements over several lines and
// without spaces, quoted and unquoted values.
TEST_F(GateMateCcfTest, syntax_variants)
{
    init_with_ccf(stringf("# comment\n"
                          "Pin_in \"a\" Loc = \"%s\"; // trailing comment\n"
                          "pin_in b loc=%s;\n"
                          "PIN_OUT \"x\"\n"
                          "   LOC\n"
                          "   = \"%s\" | slew = fast\n"
                          "   ;\n"
                          "Net \"y\" Loc = \"%s\" ; # another comment\n",
                          pins[0].c_str(), pins[1].c_str(), pins[2].c_str(), pins[3].c_str()));
    small_design();
    ASSERT_TRUE(packs());
    EXPECT_EQ(package_pin("a$iopad"), pins[0]);
    EXPECT_EQ(package_pin("b$iopad"), pins[1]);
    EXPECT_EQ(package_pin("x$iopad"), pins[2]);
    EXPECT_EQ(package_pin("y$iopad"), pins[3]);
    EXPECT_EQ(iosel_int("x", id_SLEW), 0);
}

TEST_F(GateMateCcfTest, two_statements_on_one_line)
{
    init_with_ccf(stringf("NET \"a\" Loc = \"%s\"; NET \"b\" Loc = \"%s\";\n", pins[0].c_str(), pins[1].c_str()),
                  {"--allow-unconstrained"});
    small_design();
    ASSERT_TRUE(packs());
    EXPECT_EQ(package_pin("a$iopad"), pins[0]);
    EXPECT_EQ(package_pin("b$iopad"), pins[1]);
}

TEST_F(GateMateCcfTest, input_parameters)
{
    init_with_ccf(all_locations() + "NET \"a\" Pullup = true | Schmitt_Trigger = 1 | Delay_IBF = 5;\n"
                                    "NET \"b\" Pulldown = TRUE | Keeper = false;\n");
    small_design();
    ASSERT_TRUE(packs());
    EXPECT_EQ(iosel_int("a", id_PULLUP), 1);
    EXPECT_EQ(iosel_int("a", id_SCHMITT_TRIGGER), 1);
    EXPECT_EQ(iosel_int("a", id_DELAY_IBF), 1 << 5);
    EXPECT_EQ(iosel_int("b", id_PULLDOWN), 1);
    EXPECT_NE(iosel_int("b", id_PULLUP), 1);
    // Without a delay the first tap is used
    EXPECT_EQ(iosel_int("b", id_DELAY_IBF), 1);
}

TEST_F(GateMateCcfTest, keeper_enables_pull_up_and_down)
{
    init_with_ccf(all_locations() + "NET \"a\" Keeper = true;\n");
    small_design();
    ASSERT_TRUE(packs());
    EXPECT_EQ(iosel_int("a", id_PULLUP), 1);
    EXPECT_EQ(iosel_int("a", id_PULLDOWN), 1);
}

TEST_F(GateMateCcfTest, output_parameters)
{
    init_with_ccf(all_locations() + "NET \"x\" Slew = fast | Drive = 12 | Delay_OBF = 15;\n"
                                    "NET \"y\" Slew = slow | Drive = 6;\n");
    small_design();
    ASSERT_TRUE(packs());
    EXPECT_EQ(iosel_int("x", id_SLEW), 0);
    EXPECT_EQ(iosel_int("x", id_DRIVE), 3);
    EXPECT_EQ(iosel_int("x", id_DELAY_OBF), 1 << 15);
    EXPECT_EQ(iosel_int("y", id_SLEW), 1);
    EXPECT_EQ(iosel_int("y", id_DRIVE), 1);
    EXPECT_EQ(iosel_int("y", id_DELAY_OBF), 1);
}

TEST_F(GateMateCcfTest, drive_strengths)
{
    for (int drive : {3, 6, 9, 12}) {
        SCOPED_TRACE(stringf("drive %d", drive));
        init_with_ccf(all_locations() + stringf("NET \"x\" Drive = %d;\n", drive));
        small_design();
        ASSERT_TRUE(packs());
        EXPECT_EQ(iosel_int("x", id_DRIVE), (drive - 3) / 3);
    }
}

// Outputs without a slew rate setting are slow
TEST_F(GateMateCcfTest, default_slew_is_slow)
{
    init_with_ccf(all_locations());
    small_design();
    ASSERT_TRUE(packs());
    EXPECT_EQ(iosel_int("x", id_SLEW), 1);
}

// default_GPIO applies to the pins defined after it
TEST_F(GateMateCcfTest, default_gpio)
{
    init_with_ccf(stringf("NET \"x\" Loc = \"%s\";\n"
                          "default_GPIO | Drive = 9 | Slew = fast;\n"
                          "NET \"y\" Loc = \"%s\";\n"
                          "NET \"a\" Loc = \"%s\";\n"
                          "NET \"b\" Loc = \"%s\";\n",
                          pins[2].c_str(), pins[3].c_str(), pins[0].c_str(), pins[1].c_str()));
    small_design();
    ASSERT_TRUE(packs());
    EXPECT_EQ(iosel_int("x", id_DRIVE), -1) << "defaults must not apply to earlier pins";
    EXPECT_EQ(iosel_int("x", id_SLEW), 1);
    EXPECT_EQ(iosel_int("y", id_DRIVE), 2);
    EXPECT_EQ(iosel_int("y", id_SLEW), 0);
}

// A parameter given for a pin overrides the default_GPIO value
TEST_F(GateMateCcfTest, pin_parameter_overrides_default)
{
    init_with_ccf(stringf("default_GPIO | Drive = 9 | Slew = fast;\n"
                          "NET \"x\" Loc = \"%s\" | Drive = 3;\n"
                          "NET \"y\" Loc = \"%s\";\n"
                          "NET \"y\" Slew = slow;\n",
                          pins[2].c_str(), pins[3].c_str()),
                  {"--allow-unconstrained"});
    small_design();
    ASSERT_TRUE(packs());
    EXPECT_EQ(iosel_int("x", id_DRIVE), 0);
    EXPECT_EQ(iosel_int("x", id_SLEW), 0);
    EXPECT_EQ(iosel_int("y", id_DRIVE), 2);
    EXPECT_EQ(iosel_int("y", id_SLEW), 1);
}

// The last value given for a parameter of a pin is used
TEST_F(GateMateCcfTest, repeated_pin_parameter)
{
    init_with_ccf(all_locations() + "NET \"x\" Drive = 12;\nNET \"x\" Drive = 6;\n");
    small_design();
    ASSERT_TRUE(packs());
    EXPECT_EQ(iosel_int("x", id_DRIVE), 1);
}

TEST_F(GateMateCcfTest, parameter_without_value_is_one)
{
    init_with_ccf(all_locations() + "NET \"a\" Pullup;\n");
    small_design();
    LogCapture warnings(LogLevel::WARNING_MSG);
    ASSERT_TRUE(packs());
    EXPECT_EQ(iosel_int("a", id_PULLUP), 1);
    EXPECT_NE(warnings.str().find("Parameter 'PULLUP' missing value, defaulting to '1'"), std::string::npos);
}

TEST_F(GateMateCcfTest, unknown_pin_is_a_warning)
{
    init_with_ccf(all_locations() + "NET \"nonexistent\" Pullup = true;\n");
    small_design();
    LogCapture warnings(LogLevel::WARNING_MSG);
    ASSERT_TRUE(packs());
    EXPECT_NE(warnings.str().find("Pad with name 'nonexistent' not found in netlist."), std::string::npos);
}

TEST_F(GateMateCcfTest, unconstrained_pins_are_an_error)
{
    init_with_ccf(stringf("NET \"a\" Loc = \"%s\";\n", pins[0].c_str()));
    small_design();
    EXPECT_LOG_ERROR(pack(), "is unconstrained in CCF (override this error with --vopt allow-unconstrained)");
}

TEST_F(GateMateCcfTest, unconstrained_pins_allowed)
{
    init_with_ccf(stringf("NET \"a\" Loc = \"%s\";\n", pins[0].c_str()), {"--allow-unconstrained"});
    small_design();
    ASSERT_TRUE(packs());
    EXPECT_EQ(package_pin("a$iopad"), pins[0]);
    for (auto port : {"b", "x", "y"}) {
        std::string pin = package_pin(stringf("%s$iopad", port));
        EXPECT_NE(pin, "") << port;
        EXPECT_NE(pin, pins[0]) << port;
    }
}

// Constraints given as parameters of the IO buffers (from the Verilog source) are used as well.
TEST_F(GateMateCcfTest, location_from_buffer_parameter)
{
    init_context();
    NetInfo *a = input("a");
    ctx->cells.at(ctx->id("a"))->params[id_PIN_NAME] = Property(pins[5]);
    output("o", a)->params[id_PIN_NAME] = Property(pins[6]);
    ASSERT_TRUE(packs());
    EXPECT_EQ(package_pin("a"), pins[5]);
    EXPECT_EQ(package_pin("o"), pins[6]);
}

TEST_F(GateMateCcfTest, ccf_location_overrides_buffer_parameter)
{
    init_with_ccf(all_locations());
    NetInfo *a = top_input("a"), *b = top_input("b");
    ctx->cells.at(ctx->id("a$iopad"))->params[id_PIN_NAME] = Property(pins[7]);
    top_output("x", lut2("and", 0b1000, a, b));
    top_output("y", lut2("or", 0b1110, a, b));
    LogCapture warnings(LogLevel::WARNING_MSG);
    ASSERT_TRUE(packs());
    EXPECT_EQ(package_pin("a$iopad"), pins[0]);
}

// Floorplanning: the cells of a scope are kept inside the given placement box.
TEST_F(GateMateCcfTest, floorplanning_region)
{
    init_with_ccf("start floorplanning\n"
                  "1 : top sub ; pB : x10y10x20y20\n"
                  "end floorplanning\n",
                  {"--allow-unconstrained"});
    NetInfo *a = input("a"), *b = input("b");
    std::vector<std::string> scoped;
    NetInfo *net = a;
    for (int i = 0; i < 8; i++) {
        net = lut2(stringf("lut%d", i), 0b0110, net, b);
        ctx->cells.at(ctx->idf("lut%d", i))->attrs[ctx->id("scopename")] = Property("sub");
        scoped.push_back(stringf("lut%d", i));
    }
    // An unconstrained LUT in front of the output (a LUT driving an output pad directly is moved
    // next to the pad, see region_of_lut_driving_output)
    output("o", lut2("last", 0b1001, net, a));
    ASSERT_TRUE(packs());
    ctx->place();
    for (auto &name : scoped) {
        CellInfo *cell = ctx->cells.at(ctx->id(name)).get();
        ASSERT_NE(cell->bel, BelId()) << name;
        Loc loc = ctx->getBelLocation(cell->bel);
        // pB coordinates are offset by 2 from the bel grid
        EXPECT_TRUE(loc.x >= 12 && loc.x <= 22 && loc.y >= 12 && loc.y <= 22)
                << name << " placed at " << loc.x << ", " << loc.y;
    }
}

TEST_F(GateMateCcfTest, floorplanning_wildcard)
{
    init_with_ccf("start floorplanning\n"
                  "top s?b* ; pB : x30y30x40y40\n"
                  "end floorplanning\n",
                  {"--allow-unconstrained"});
    NetInfo *a = input("a");
    NetInfo *net = lut2("lut", 0b0110, a, input("b"));
    ctx->cells.at(ctx->id("lut"))->attrs[ctx->id("scopename")] = Property("sub_unit");
    output("o", lut2("last", 0b1001, net, a));
    ASSERT_TRUE(packs());
    ctx->place();
    Loc loc = ctx->getBelLocation(ctx->cells.at(ctx->id("lut"))->bel);
    EXPECT_TRUE(loc.x >= 32 && loc.x <= 42 && loc.y >= 32 && loc.y <= 42) << loc.x << ", " << loc.y;
}

// A LUT that drives an output pad is packed into the CPE next to the pad; its region constraint
// must still be respected (or reported).
TEST_F(GateMateCcfTest, region_of_lut_driving_output)
{
    init_with_ccf("start floorplanning\n"
                  "top sub ; pB : x30y30x40y40\n"
                  "end floorplanning\n",
                  {"--allow-unconstrained"});
    NetInfo *net = lut2("lut", 0b0110, input("a"), input("b"));
    ctx->cells.at(ctx->id("lut"))->attrs[ctx->id("scopename")] = Property("sub");
    output("o", net);
    LogCapture warnings(LogLevel::WARNING_MSG);
    ASSERT_TRUE(packs());
    ctx->place();
    Loc loc = ctx->getBelLocation(ctx->cells.at(ctx->id("lut"))->bel);
    bool inside = loc.x >= 32 && loc.x <= 42 && loc.y >= 32 && loc.y <= 42;
    EXPECT_TRUE(inside || warnings.str().find("lut") != std::string::npos)
            << "placed at " << loc.x << ", " << loc.y << " without a warning";
}

// On multi-die devices the DIE parameter selects the die of a pin.
TEST_F(GateMateCcfTest, die_parameter)
{
    // A pin that exists on both dies
    init_context("CCGM1A2");
    std::string pin;
    for (auto &p : package_pins())
        if (impl->locations.count({ctx->id(p), 0}) && impl->locations.count({ctx->id(p), 1})) {
            pin = p;
            break;
        }
    ASSERT_NE(pin, "") << "no pin on both dies";
    for (auto die : {"1A", "1B"}) {
        SCOPED_TRACE(die);
        init_with_ccf(stringf("NET \"a\" Loc = \"%s\" | DIE = %s;\n", pin.c_str(), die),
                      {"--allow-unconstrained"}, "CCGM1A2");
        output("o", lut1("inv", 0b01, top_input("a")));
        ASSERT_TRUE(packs());
        CellInfo *ibuf = ctx->cells.at(ctx->id("a$iopad")).get();
        ASSERT_NE(ibuf->bel, BelId());
        int die_index = reinterpret_cast<const GateMateTileExtraDataPOD *>(
                                ctx->chip_info->tile_insts[ibuf->bel.tile].extra_data.get())
                                ->die;
        EXPECT_EQ(impl->index_to_die.at(die_index).str(ctx), die);
    }
}

// Malformed constraint files
struct CcfErrorCase
{
    const char *name;
    // CCF content; %s is replaced with a valid package pin
    const char *ccf;
    const char *error;
};

class GateMateCcfErrorTest : public GateMateCcfTest, public ::testing::WithParamInterface<CcfErrorCase>
{
};

TEST_P(GateMateCcfErrorTest, rejected)
{
    auto c = GetParam();
    init_with_ccf(stringf(c.ccf, pins[0].c_str()), {"--allow-unconstrained"});
    small_design();
    EXPECT_LOG_ERROR(pack(), c.error);
}

INSTANTIATE_TEST_SUITE_P(
        Errors, GateMateCcfErrorTest,
        ::testing::Values(
                CcfErrorCase{"unknown_location", "NET \"a\" Loc = \"IO_XX_Z99\";\n",
                             "Unknown location 'IO_XX_Z99' used in line 1."},
                CcfErrorCase{"location_used_twice", "NET \"a\" Loc = \"%1$s\";\nNET \"b\" Loc = \"%1$s\";\n",
                             "used in line 2 not available."},
                CcfErrorCase{"location_in_defaults", "default_GPIO | Loc = \"%s\";\n",
                             "Value 'LOC' can not be defined for default GPIO in line 1."},
                CcfErrorCase{"location_without_value", "NET \"a\" Loc;\n",
                             "Parameter must be in form NAME=VALUE (on line 1)."},
                CcfErrorCase{"drive_value", "NET \"x\" Drive = 5;\n",
                             "Parameter 'DRIVE' must have value 3,6,9 or 12 in line 1."},
                CcfErrorCase{"drive_not_a_number", "NET \"x\" Drive = strong;\n",
                             "Parameter 'DRIVE' must be number in line 1."},
                CcfErrorCase{"delay_range", "NET \"a\" Delay_IBF = 16;\n",
                             "Parameter 'DELAY_IBF' must have value from 0 to 15 in line 1."},
                CcfErrorCase{"delay_not_a_number", "NET \"x\" Delay_OBF = long;\n",
                             "Parameter 'DELAY_OBF' must be number in line 1."},
                CcfErrorCase{"boolean_value", "NET \"a\" Pullup = maybe;\n",
                             "Unknown value 'MAYBE' for parameter 'PULLUP' in line 1, must be TRUE or FALSE."},
                CcfErrorCase{"slew_value", "NET \"x\" Slew = medium;\n",
                             "Unknown value 'MEDIUM' for parameter 'SLEW' in line 1, must be SLOW or FAST."},
                CcfErrorCase{"unknown_parameter", "NET \"a\" Colour = red;\n",
                             "Unknown parameter name 'COLOUR' in line 1."},
                CcfErrorCase{"unknown_die", "NET \"a\" Loc = \"%s\" | Die = 3C;\n",
                             "Unknown value '3C' for parameter 'DIE' in line 1."},
                CcfErrorCase{"unknown_statement", "FOO \"a\" Loc = \"%s\";\n", "Unknown type 'foo' in line 1."},
                CcfErrorCase{"pin_without_name", "NET;\n", "Pin definition line not properly formed (in line 1)."},
                CcfErrorCase{"defaults_with_name", "default_GPIO \"a\" | Drive = 3;\n",
                             "Line with default_GPIO should not contain only parameters (in line 1)."},
                CcfErrorCase{"unterminated_quote", "NET \"a Loc = \"%s\";\n", "Expected '\"' at end of string"},
                CcfErrorCase{"missing_semicolon", "NET \"a\" Loc = \"%s\"\n", "Unexpected end of CCF file."},
                CcfErrorCase{"unknown_command", "start somethingelse;\n", "Unknown command 'somethingelse' in line 1."},
                CcfErrorCase{"unknown_scope", "start floorplanning\ntop nothing ; pB : x1y1x2y2\nend floorplanning\n",
                             "Unknown scope name or pattern: 'top nothing' in line 2."},
                CcfErrorCase{"placebox_format", "start floorplanning\ntop ; pB : 1,1,2,2\nend floorplanning\n",
                             "Placebox format invalid: 1,1,2,2 in line 2."},
                CcfErrorCase{"placebox_range", "start floorplanning\ntop ; pB : x1y1x9999y2\nend floorplanning\n",
                             "Placebox coordinates out of range 'x1y1x9999y2' in line 2."},
                CcfErrorCase{"placebox_missing", "start floorplanning\ntop ; pB\nend floorplanning\n",
                             "Missing data for pB (in line 2)."},
                CcfErrorCase{"floorplanning_segment", "start floorplanning\ntop ; zz : x1y1x2y2\nend floorplanning\n",
                             "Unexpected content in last segment (in line 2)."}),
        [](const ::testing::TestParamInfo<CcfErrorCase> &info) { return std::string(info.param.name); });

TEST_F(GateMateCcfTest, missing_file)
{
    init_context("CCGM1A1", {"--allow-unconstrained", "--ccf=/nonexistent/constraints.ccf"});
    small_design();
    EXPECT_LOG_ERROR(pack(), "Failed to open CCF file '/nonexistent/constraints.ccf'");
}
