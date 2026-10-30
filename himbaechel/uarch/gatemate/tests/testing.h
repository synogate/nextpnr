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

#ifndef GATEMATE_TESTING_H
#define GATEMATE_TESTING_H

#include <functional>
#include <map>
#include <sstream>
#include "gtest/gtest.h"
#include "log.h"
#include "nextpnr.h"
#include "uarch/gatemate/gatemate.h"

NEXTPNR_NAMESPACE_BEGIN

class GateMateTest : public ::testing::Test
{
  protected:
    virtual void SetUp() override;

    virtual void TearDown() override;

    // (Re)create the context for the given device and --vopt options; any existing netlist is dropped.
    void init_context(const std::string &device = "CCGM1A1",
                      const std::vector<std::string> &vopts = {"--allow-unconstrained"});

    CellInfo *create_cell_ptr(IdString type, std::string name);
    void direct_connect(CellInfo *o_cell, IdString o_port, CellInfo *i_cell, IdString i_port);

    // Netlist building helpers, producing the same pre-pack netlist structure as the JSON frontend.
    NetInfo *create_net(const std::string &name);
    // Connect an input port, creating the port if the cell does not have it yet.
    void connect_input(CellInfo *cell, IdString port, NetInfo *net);
    // Create a net driven by an output port, creating the port if the cell does not have it yet.
    NetInfo *connect_output(CellInfo *cell, IdString port, const std::string &net_name = "");
    // Constant nets, driven by the VCC/GND cells the frontend creates for constant bits.
    NetInfo *vcc();
    NetInfo *gnd();
    // A CC_IBUF called `name`; returns the net it drives into the fabric.
    NetInfo *input(const std::string &name);
    // A CC_OBUF called `name` driven by `src`.
    CellInfo *output(const std::string &name, NetInfo *src);
    // A CC_TOBUF called `name`, driving `src` while `t` is low.
    CellInfo *tristate_output(const std::string &name, NetInfo *src, NetInfo *t);
    // A CC_IOBUF called `name`, driving `src` while `t` is low; returns the net with the pad value.
    NetInfo *bidirectional(const std::string &name, NetInfo *src, NetInfo *t);
    // CC_IDDR; returns {Q0, Q1}
    std::pair<NetInfo *, NetInfo *> iddr(const std::string &name, NetInfo *d, NetInfo *clk, bool clk_inv = false);
    // CC_ODDR; returns Q
    NetInfo *oddr(const std::string &name, NetInfo *d0, NetInfo *d1, NetInfo *clk, NetInfo *ddr, bool clk_inv = false);
    NetInfo *lut1(const std::string &name, int init, NetInfo *i0);
    NetInfo *lut2(const std::string &name, int init, NetInfo *i0, NetInfo *i1);
    NetInfo *l2t4(const std::string &name, int init_l00, int init_l01, int init_l10, NetInfo *i0, NetInfo *i1,
                  NetInfo *i2, NetInfo *i3);
    NetInfo *mx2(const std::string &name, NetInfo *d0, NetInfo *d1, NetInfo *s0);
    NetInfo *mx4(const std::string &name, NetInfo *d0, NetInfo *d1, NetInfo *d2, NetInfo *d3, NetInfo *s0,
                 NetInfo *s1);
    // A CC_MX8 with data d[0..7] and selects s[0..2]
    NetInfo *mx8(const std::string &name, const std::vector<NetInfo *> &d, const std::vector<NetInfo *> &s);
    CellInfo *dff(const std::string &name, NetInfo *d, NetInfo *clk, NetInfo *en = nullptr, NetInfo *sr = nullptr);
    CellInfo *dlt(const std::string &name, NetInfo *d, NetInfo *g, NetInfo *sr = nullptr);
    // A CC_ADDF; returns {S, CO}.
    std::pair<NetInfo *, NetInfo *> addf(const std::string &name, NetInfo *a, NetInfo *b, NetInfo *ci);
    // A CC_BUFG driven by input `name`; returns the global clock net.
    NetInfo *global_clock(const std::string &name);

    // Top level ports as created by the JSON frontend: a port cell named `name` ($nextpnr_ibuf /
    // $nextpnr_obuf, which constraints refer to) connected to the pad pin of the IO buffer from
    // synthesis (named `name$iopad`). Return the fabric side net / the output buffer.
    NetInfo *top_input(const std::string &name);
    CellInfo *top_output(const std::string &name, NetInfo *src);

    // Writes `content` to a file in a temporary directory for this test; returns its path.
    std::string write_temp_file(const std::string &filename, const std::string &content);
    // (Re)create the context with a CCF file containing `ccf` (plus further --vopt options).
    void init_with_ccf(const std::string &ccf, const std::vector<std::string> &vopts = {},
                       const std::string &device = "CCGM1A1");
    // Package pin a placed IO buffer is bound to ("" if none).
    std::string package_pin(const std::string &buffer);
    // Package pins of the device (excluding the SERDES clock), optionally only those that can be
    // used as clock inputs.
    std::vector<std::string> package_pins(bool clock_capable_only = false);

    // After packing: places the 20K RAM `lower` at the RAM bel of a block and `upper` at its other
    // half, in the first block where the CPEs for their pins are free (the IO pads have fixed CPEs
    // already), so that they end up merged into one RAM. Fails if no such block is free, or if the
    // two RAMs cannot share a block.
    ::testing::AssertionResult place_ram_pair(const std::string &lower, const std::string &upper);

    // Run the packer through the Arch entry point (as the command line flow does).
    void pack();
    // Run the packer, reporting an error message as test failure (ASSERT_TRUE(packs())).
    ::testing::AssertionResult packs();
    // Run pack, place and route with default settings.
    void place_and_route();

    // Builds the design twice with `build`: as the reference netlist, which stays unpacked, and
    // as the implementation in `ctx`, which is packed and placed. Then drives both with `steps`
    // random input vectors and checks that the implementation produces the same outputs.
    // `after_pack`, if given, runs between packing and placement (e.g. to place cells by hand), and
    // `after_place` after placement (e.g. to transform the placed netlist).
    ::testing::AssertionResult implementation_matches_reference(const std::function<void()> &build, int steps = 200,
                                                               uint64_t seed = 1,
                                                               const std::function<void()> &after_pack = {},
                                                               const std::function<void()> &after_place = {});

    // After placement: the combinational dependencies inside cells (an output whose value
    // depends on an input, found with the simulator's cell models and random input values) that
    // have no timing arc, so that paths through them are not analysed. `from` are the ports the
    // input net is connected to (none of them has an arc). Cells the simulator has no model for
    // are skipped.
    struct MissingArc
    {
        const CellInfo *cell;
        std::vector<IdString> from;
        IdString to;
    };
    std::vector<MissingArc> missing_timing_arcs(uint64_t seed = 1);
    // One line per kind of missing arc (cell type and ports), with an example cell
    std::string describe_arcs(const std::vector<MissingArc> &arcs);

    // Before packing: an upper bound on the CPE halves (a LUT tree with its register) the design
    // needs when the packer makes use of what a CPE can do, from the CC_* cells: one per LUT, LUT
    // tree and 2:1 multiplexer (none for one that only copies its input, or only drives a
    // register, an output pad (or its enable) or a RAM pin, whose half it shares), one per register (for its LUT or a
    // pass-through; for an IO register only to bring in a fabric clock), two per 4:1 multiplexer or 5-input
    // LUT tree (a whole CPE), one per adder bit (two if its carry out is also used elsewhere) and
    // per carry chain two to bring the carry in, two to take it out if used, one for an odd bit, one per output pad (two
    // with an output enable), clock buffer and RAM pin (the data input and output of a bit sharing
    // one), two per DDR output register (its data, and one more for a fabric clock) and one per
    // DDR select, and two for the constants. Fails the test for cells it has no rule for.
    // (`by_type`: the bound broken down by cell type)
    int cpe_half_bound(std::map<std::string, int> *by_type = nullptr);
    // After placement: the CPE halves in use (a cell in the LUT, register or RAM pin part of a
    // half; a cell taking a whole CPE counts twice)
    int cpe_halves_used();

    ArchArgs chipArgs;
    Context *ctx = nullptr;
    GateMateImpl *impl = nullptr;

  private:
    NetInfo *const_net(bool value);

    std::string device;
    std::vector<std::string> vopts;
};

// Captures everything logged at `level` or above while it is alive.
struct LogCapture
{
    explicit LogCapture(LogLevel level = LogLevel::LOG_MSG);
    ~LogCapture();
    std::string str() const { return stream.str(); }

  private:
    std::ostringstream stream;
};

// Expect `statement` to fail with log_error, with a message containing `message`.
#define EXPECT_LOG_ERROR(statement, message)                                                                           \
    do {                                                                                                               \
        LogCapture capture_(LogLevel::ERROR_MSG);                                                                      \
        EXPECT_THROW(statement, log_execution_error_exception);                                                       \
        EXPECT_NE(capture_.str().find(message), std::string::npos)                                                     \
                << "expected error containing '" << (message) << "', got: '" << capture_.str() << "'";                 \
    } while (0)

NEXTPNR_NAMESPACE_END
#endif
