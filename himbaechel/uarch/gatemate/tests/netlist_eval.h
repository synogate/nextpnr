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

#ifndef GATEMATE_NETLIST_EVAL_H
#define GATEMATE_NETLIST_EVAL_H

#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "gtest/gtest.h"
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

// Three-valued logic: X is an unknown/undriven value.
enum class Tri : uint8_t
{
    F = 0,
    T = 1,
    X = 2,
};

const char *tri_str(Tri v);

// Thrown when the simulator meets a cell or configuration it has no model for, so that a test
// fails loudly instead of silently checking something else.
struct EvalUnsupported : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// Cycle-based simulator for GateMate netlists, at two levels:
//
//  - the unpacked netlist of CC_* cells, following the yosys simulation models
//    (techlibs/gatemate/cells_sim.v); this is the reference;
//  - the packed (and placed, but not yet routed) netlist of CPE_* cells. The CPE models follow
//    the CPE schematic of Project Peppercorn (schematics/cpe/cpe.pdf, sheet "Cologne Processing
//    Element"); where that is not conclusive the assumption is documented at the model.
//
// The pads are the IO buffer cells (CC_IBUF, CC_OBUF, CC_TOBUF, CC_IOBUF before packing, the
// CPE_* buffers after, with the same names). Values driven into the device are assigned per
// buffer name; for a bidirectional pad the assigned value is seen while the pad does not drive.
// Unconnected inputs evaluate as X, so a function that depends on a disconnected input is
// reported as unknown.
struct NetlistEvaluator
{
    explicit NetlistEvaluator(Context *ctx);

    void set_input(IdString buffer, bool value);
    void set_input(IdString buffer, Tri value);
    Tri eval(const NetInfo *net);
    // Value driven by an output pad (meaningful while eval_output_enable() is 1).
    Tri eval_output(IdString buffer);
    // Whether an output pad drives its value (always 1 for plain outputs).
    Tri eval_output_enable(IdString buffer);
    // Propagate the current input values into the registers and latches: registers whose
    // clock rose since the previous call load their data input, latches follow their data
    // input while transparent, and asynchronous set/reset are applied. With `power_up`, the
    // current clock levels are taken as the starting point instead of being edges.
    void settle(bool power_up = false);

    // Give the registers in `names` that have no defined initial value a pseudo-random one,
    // derived from the name and `seed`. The packer keeps register names, so the reference and the
    // implementation start from the same state, and unknown values cannot hide differences (the
    // gate level CPE models are more pessimistic about unknowns than the CC_* reference models).
    void resolve_unknown_init(uint64_t seed, const std::set<std::string> &names);
    // Names of all registers and latches (with "#n" for further state bits of a cell)
    std::set<std::string> register_names() const;
    // Direct access to state bits: the cell and the index of the state bit within the cell
    // (CC_IDDR/CC_ODDR: 0 rising edge, 1 falling edge register; IOSEL: 0/1 input, 2/3 output
    // registers).
    std::vector<std::pair<const CellInfo *, int>> state_bits() const;
    Tri state(const CellInfo *cell, int index) const;
    void set_state(const CellInfo *cell, int index, Tri value);

    // Describes where an unknown value on `net` comes from: the first cell (following unknown
    // inputs backwards) whose known inputs produce an unknown output, with its unconnected inputs.
    std::string explain_unknown(const NetInfo *net);

    // Value of output `port` of `cell` on its own, with the given values on its input nets
    // (instead of the values the netlist gives them): for finding which inputs an output depends
    // on. The current state of registers and RAMs is not changed.
    Tri eval_cell(const CellInfo *cell, IdString port, const std::unordered_map<const NetInfo *, Tri> &inputs);

    // Evaluate a 2-input LUT with Kleene semantics: unknown inputs only matter if the
    // result depends on them.
    static Tri lut2(int init, Tri d0, Tri d1);

  private:
    struct SeqInputs
    {
        Tri clk, en, d, rst, set;
        bool latch;
    };
    struct SeqState
    {
        Tri q, last_clk;
    };
    // A state bit: a cell and the index of the state bit within the cell
    using SeqKey = std::pair<const CellInfo *, int>;

    Tri eval_port(const CellInfo *cell, IdString port);
    Tri input(const CellInfo *cell, IdString port);
    bool connected(const CellInfo *cell, IdString port) const { return cell->getPort(port) != nullptr; }
    Tri external(const CellInfo *buffer);
    Tri eval_l2t4(const CellInfo *cell);
    Tri eval_cpe(const CellInfo *cell, IdString port);
    Tri eval_mx8(const CellInfo *cell);
    Tri eval_cplines(const CellInfo *cell, IdString port);
    Tri eval_iosel(const CellInfo *cell, IdString port);
    Tri eval_clocking(const CellInfo *cell, IdString port);
    Tri eval_reference(const CellInfo *cell, IdString port);
    std::vector<int> seq_elements(const CellInfo *cell) const;
    bool is_sequential(const CellInfo *cell) const { return !seq_elements(cell).empty(); }
    bool is_latch(const CellInfo *cell) const;
    std::string seq_name(const SeqKey &key) const;
    SeqInputs seq_inputs(const CellInfo *cell, int index, bool sync);
    Tri seq_output(const CellInfo *cell, int index);

    // Block RAM. A RamBlock is one logical CC_BRAM_20K or CC_BRAM_40K with the behaviour of the
    // yosys simulation model: a reference cell, or in the placed netlist a RAM cell (a 40K block,
    // or two 20K halves merged into one RAM).
    struct RamPin
    {
        IdString port; // empty for a constant
        Tri constant = Tri::X;
        bool invert = false;
    };
    struct RamPort
    {
        RamPin clk, en, we;
        int rd_width = 0, wr_width = 0;
        bool write_through = false, do_reg = false;
        // Pins of the address, data in, bit mask and data out bits
        std::vector<IdString> addr, di, bm, dout;
        Tri last_clk = Tri::X;
        std::vector<Tri> out, reg; // the data output and the optional output register
    };
    struct RamBlock
    {
        const CellInfo *cell = nullptr;
        // The memory of the cell is a flat bit vector as in the yosys models. A 20K half of a
        // placed RAM uses 40 of every 80 bits of the 40K memory: -1 for the whole memory.
        int half = -1;
        bool is_40k = false, sdp = false;
        // Cascade of two 40K blocks (1 bit wide): 0 none, 1 the lower block (A_ADDR[0] = 0),
        // 2 the upper one (A_ADDR[0] = 1), and the block index of the other one
        int cascade = 0, partner = -1;
        std::string unsupported; // why the block cannot be simulated, if it cannot
        RamPort port[2];
    };
    struct RamUpdate
    {
        Tri clk[2];
        std::vector<Tri> out[2], reg[2];
        // Memory bit, value, and whether the write happens (X: maybe)
        std::vector<std::tuple<int, Tri, Tri>> writes;
    };
    void add_reference_ram(const CellInfo *cell);
    void add_placed_ram(const CellInfo *cell);
    void add_ram(RamBlock block, int memory_bits);
    Tri pin_value(const CellInfo *cell, const RamPin &pin);
    Tri ram_pin(const RamBlock &block, const RamPin &pin) { return pin_value(block.cell, pin); }
    std::vector<int> ram_addresses(const RamBlock &block, int p, int width);
    int ram_bit(const RamBlock &block, int bit) const;
    Tri ram_read(const RamBlock &block, const std::vector<int> &firsts, int bit) const;
    Tri ram_cascade_out(const RamBlock &lower, int p);
    void link_ram_cascades();
    RamUpdate ram_sample(const RamBlock &block, bool power_up);
    void ram_commit(RamBlock &block, const RamUpdate &update);
    Tri eval_ram(const CellInfo *cell, IdString port);

    // FIFO (CC_FIFO_40K, or a placed RAM configured as FIFO), with the behaviour of the yosys
    // simulation model. The state is either known or (before a reset, or after an update with
    // unknown controls) entirely unknown.
    enum FifoOutput
    {
        FIFO_FULL,
        FIFO_EMPTY,
        FIFO_ALMOST_FULL,
        FIFO_ALMOST_EMPTY,
        FIFO_RD_ERROR,
        FIFO_WR_ERROR,
        FIFO_RD_PTR,
        FIFO_WR_PTR,
        FIFO_DO_A,
        FIFO_DO_B,
    };
    struct FifoBlock
    {
        const CellInfo *cell = nullptr;
        bool sdp = false, async = false, dyn_offsets = false;
        int width = 0, tp = 0, depth = 0, counter_max = 0;
        int almost_full_offset = 0, almost_empty_offset = 0;
        bool do_reg[2] = {false, false};
        std::string unsupported;
        RamPin rd_clk, wr_clk, a_en, b_en, b_we, rst_n;
        std::vector<IdString> almost_full_pins, almost_empty_pins;
        std::vector<IdString> di[2], bm[2];
        // Pins of the outputs
        std::map<IdString, std::pair<FifoOutput, int>> outputs;
        // State
        bool known = false;
        int rd_pointer = 0, rd_pointer_int = 0, wr_pointer = 0, wr_pointer_int = 0;
        int rd_sync_0 = 0, rd_sync_1 = 0, wr_sync_0 = 0, wr_sync_1 = 0;
        Tri last_rd_clk = Tri::X, last_wr_clk = Tri::X;
        std::vector<Tri> out[2], reg[2];
    };
    struct FifoFlags
    {
        bool full, empty, almost_full, almost_empty;
        int rdaddr, wraddr;
    };
    void add_reference_fifo(const CellInfo *cell);
    void add_placed_fifo(const CellInfo *cell);
    void add_fifo(FifoBlock block);
    Tri fifo_offsets_known(const FifoBlock &block, int &almost_full, int &almost_empty);
    FifoFlags fifo_flags(const FifoBlock &block, int almost_full, int almost_empty) const;
    FifoBlock fifo_sample(const FifoBlock &block, bool power_up, std::vector<std::pair<int, Tri>> &writes);
    Tri eval_fifo(const FifoBlock &block, IdString port);

    Context *ctx;
    dict<IdString, Tri> inputs;
    std::unordered_map<const NetInfo *, Tri> memo;
    std::unordered_set<const NetInfo *> visiting;
    std::map<SeqKey, SeqState> seq;
    std::vector<RamBlock> rams;
    std::vector<FifoBlock> fifos;
    std::unordered_map<const CellInfo *, int> fifo_index;
    std::unordered_map<const CellInfo *, std::vector<Tri>> ram_memory;
    // (cell, data output pin) -> (block, port, bit)
    std::map<std::pair<const CellInfo *, IdString>, std::tuple<int, int, int>> ram_outputs;

  public:
    // Contents of the memory of a RAM cell, as a flat bit vector (yosys model bit order for the
    // reference cells, the 40K memory for a placed RAM).
    const std::vector<Tri> &ram_contents(const CellInfo *cell) const { return ram_memory.at(cell); }
};

// Checks that output pad `obuf` computes `expected` for every assignment of `inputs`
// (the names of the input buffers). `expected` gets the input values in the same order.
::testing::AssertionResult output_computes(Context *ctx, const std::string &obuf, const std::vector<std::string> &inputs,
                                           const std::function<bool(const std::vector<bool> &)> &expected);

// Drives the reference (unpacked) and the implemented netlist with the same `steps` random input
// vectors, and checks that every output pad of the implementation matches the reference whenever
// the reference value is known: whether it drives, and the value while it drives. Both netlists
// must have the same IO buffer names.
::testing::AssertionResult outputs_match(Context *reference, Context *implementation, int steps, uint64_t seed);

NEXTPNR_NAMESPACE_END
#endif
