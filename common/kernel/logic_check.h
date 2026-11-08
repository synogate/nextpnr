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

/*
 * Logic check: does the design still compute what a reference copy of its netlist computes?
 *
 * For changing the logic by hand (new cells, LUTs merged, other cell configurations): the reference is a copy of the
 * netlist taken before (LogicSnapshot, e.g. right after packing), and the check compares, between the two netlists,
 * everything the logic ends in: the inputs of registers, RAMs, output buffers and every other cell the arch does not
 * evaluate as combinational logic ("sinks", ArchAPI::evaluateCell). Their values are functions of the "free
 * variables": the outputs of such cells and nets without a driver, which are the same in both netlists where their
 * cell and port (or net) names are.
 *
 *  1. Simulation: both netlists are evaluated for many random values of the free variables (64 patterns per word);
 *     a sink whose values differ is a mismatch, with a counterexample.
 *  2. Proof: sinks and nets that agree in simulation are proven equal one after another, in topological order. A net
 *     of the design with a net of the same name in the reference is compared through its cone back to nets already
 *     proven (or free variables), in both netlists, for all values of those (at most LogicCheckOptions::max_cut).
 *     Logic left as it was proves in one step per cell; changed logic is compared as a whole from the proven nets
 *     around it. What agrees in simulation but cannot be proven like that (cones too wide, or the nets the cut stops
 *     at are not independent) is reported as unproven.
 *
 * Only the logical netlist is compared: routing is taken as connecting what the nets say (on GateMate the arch's
 * evaluateCell does follow the renames of postRoute, so a routed design compares with its packed reference).
 */

#ifndef LOGIC_CHECK_H
#define LOGIC_CHECK_H

#include <iosfwd>
#include <memory>
#include <string>
#include <vector>
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

// A copy of the logic of a netlist: cells (type, parameters, attributes, ports) without nets, and their connections
// by net name.
struct LogicSnapshot
{
    struct Cell
    {
        std::unique_ptr<CellInfo> info; // ports have no nets
        dict<IdString, IdString> port_nets; // port -> net name, for connected ports
    };
    std::vector<Cell> cells;
    int64_t taken_at = 0; // seconds since the epoch, for showing

    bool empty() const { return cells.empty(); }
    static LogicSnapshot take(Context *ctx);
    // A JSON file of its own (not the design's JSON). read() returns "" or the error.
    void write(std::ostream &out) const;
    std::string read(Context *ctx, std::istream &in);
};

struct LogicCheckOptions
{
    int sim_words = 32;     // simulation: random patterns, 64 per word
    int max_cut = 16;       // proof: at most this many variables per cone (2^max_cut patterns)
    int max_cone_cells = 256;
    uint64_t seed = 1;
};

struct LogicIssue
{
    enum Kind
    {
        MISMATCH,     // a sink computes something else (counterexample in message)
        UNPROVEN,     // agrees in simulation, not proven
        MISSING_SINK, // a sink of the reference is not in the design (e.g. a register removed)
        NEW_SINK,     // a sink of the design is not in the reference
        NEW_VARIABLE, // a free variable of the design (register output, undriven net) the reference does not have
        LOOP,         // combinational loop: its outputs are taken as free variables
    };
    Kind kind;
    IdString cell;      // the cell (of the design, or of the reference for MISSING_SINK)
    IdString sink;      // sink name (MISMATCH, UNPROVEN, *_SINK), port (NEW_VARIABLE, LOOP)
    IdString net;       // NEW_VARIABLE: the net
    std::string message;
    std::vector<IdString> cone; // MISMATCH / UNPROVEN: the cells of the design between the sink and the proven nets
};

struct LogicCheckResult
{
    int sinks = 0, proven = 0, unproven = 0, mismatched = 0;
    int nets_proven = 0;
    std::vector<LogicIssue> issues;
    double seconds = 0;
    std::string summary() const;
};

// Compare the current netlist of ctx with ref. The caller holds the context lock (nothing is changed).
LogicCheckResult checkLogic(Context *ctx, const LogicSnapshot &ref, const LogicCheckOptions &opts = {});

NEXTPNR_NAMESPACE_END

#endif // LOGIC_CHECK_H
