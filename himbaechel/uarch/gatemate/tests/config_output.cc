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

// The textual configuration written with --vopt out=... and read by gmpack.

#include <sstream>
#include "testing.h"
#include "uarch/gatemate/config.h"

USING_NEXTPNR_NAMESPACE

TEST(GateMateConfig, cfgloc_comparison)
{
    CfgLoc a{0, 1, 2};
    EXPECT_TRUE(a == (CfgLoc{0, 1, 2}));
    EXPECT_FALSE(a != (CfgLoc{0, 1, 2}));
    EXPECT_TRUE(a != (CfgLoc{1, 1, 2}));
    EXPECT_TRUE(a != (CfgLoc{0, 2, 2}));
    EXPECT_TRUE(a != (CfgLoc{0, 1, 3}));
}

TEST(GateMateConfig, cfgloc_order_is_die_then_row_then_column)
{
    EXPECT_TRUE((CfgLoc{0, 5, 0}) < (CfgLoc{0, 0, 1}));
    EXPECT_TRUE((CfgLoc{0, 0, 1}) < (CfgLoc{0, 1, 1}));
    EXPECT_TRUE((CfgLoc{0, 9, 9}) < (CfgLoc{1, 0, 0}));
    EXPECT_FALSE((CfgLoc{0, 1, 1}) < (CfgLoc{0, 1, 1}));
}

TEST(GateMateConfig, word_is_written_msb_first)
{
    std::ostringstream out;
    out << ConfigWord{"CPE1.INIT_L00", {true, false, false, false}};
    EXPECT_EQ(out.str(), "CPE1.INIT_L00 0001\n");
}

TEST(GateMateConfig, words_are_sorted_by_name)
{
    TileConfig tc;
    tc.add_word("ZZ", {true});
    tc.add_word("AA", {false, true});
    EXPECT_EQ(tc.to_string(), "AA 10\nZZ 1\n");
}

TEST(GateMateConfig, same_word_twice_with_same_value)
{
    TileConfig tc;
    tc.add_word("A", {true, false});
    tc.add_word("A", {true, false});
    EXPECT_EQ(tc.cwords.size(), 1u);
}

TEST(GateMateConfig, same_word_twice_with_different_value)
{
    TileConfig tc;
    tc.add_word("A", {true, false});
    EXPECT_LOG_ERROR(tc.add_word("A", {false, false}, "cell"),
                     "Trying to add value to already assigned word A for cell");
}

TEST(GateMateConfig, chip_config_sections)
{
    ChipConfig cc;
    cc.chip_name = "CCGM1A1";
    cc.configs[0].add_word("GPIO.BANK_N1", {true});
    cc.tiles[CfgLoc{0, 3, 4}].add_word("CPE1.C_O1", {false, true});
    cc.brams[CfgLoc{0, 1, 2}].add_word("RAM_cfg_ecc_enable", {false, false});
    cc.tiles[CfgLoc{0, 5, 6}]; // empty sections are left out
    std::ostringstream out;
    out << cc;
    EXPECT_EQ(out.str(), ".device CCGM1A1\n\n"
                         ".config 0 \nGPIO.BANK_N1 1\n\n"
                         ".tile 0 3 4\nCPE1.C_O1 10\n\n"
                         ".bram 0 1 2\nRAM_cfg_ecc_enable 00\n\n");
}

TEST(GateMateConfig, bram_init_layout)
{
    ChipConfig cc;
    cc.chip_name = "CCGM1A1";
    auto &data = cc.bram_data[CfgLoc{0, 0, 1}];
    for (int i = 0; i < 80; i++)
        data.push_back(uint8_t(i * 3));
    std::ostringstream out;
    out << cc;
    std::string text = out.str();
    std::string header = ".bram_init 0 0 1\n";
    auto pos = text.find(header);
    ASSERT_NE(pos, std::string::npos) << text;
    std::istringstream body(text.substr(pos + header.size()));
    // 40 bytes per line, two lower-case hex digits each
    for (int line = 0; line < 2; line++) {
        std::string l;
        std::getline(body, l);
        std::istringstream bytes(l);
        for (int i = 0; i < 40; i++) {
            std::string byte;
            bytes >> byte;
            EXPECT_EQ(byte, stringf("%02x", (line * 40 + i) * 3 & 0xff)) << "line " << line << " byte " << i;
        }
    }
}
