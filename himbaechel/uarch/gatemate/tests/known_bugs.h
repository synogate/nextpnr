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

#ifndef GATEMATE_KNOWN_BUGS_H
#define GATEMATE_KNOWN_BUGS_H

// Known bugs that some tests deliberately steer around, so that they do not mask other problems.
//
// Every such restriction is temporary and limits coverage. Each one is guarded by the entry
// below for the bug it avoids; once a bug is fixed, delete its entry and the compiler points at
// every restriction to remove. Each bug also has dedicated tests (listed here) that fail until it
// is fixed, independent of these entries.
enum class KnownBug
{
    // pack.cc optimize_lut2: a LUT input tied to a constant is disconnected, but the LUT
    // configuration is not updated for the constant.
    // Tests: GateMateLut2Test.constant_i0/i1, GateMateL2T4Test.constant_inputs
    LUT_CONSTANT_INPUT,

    // pack.cc optimize_lut2: a LUT with the same net on both inputs of a LUT2 gets a
    // configuration that depends on the input it disconnects. This also happens when the packer
    // removes a LUT that only copies its input, and the copy fed another input of the same cell.
    // Tests: GateMateLut2Test.same_net_on_both_inputs, input_equal_after_buffer_removal,
    // GateMateL2T4Test.repeated_inputs
    LUT_SAME_INPUT,

    // pack.cc cleanup: once the original users of a constant net are folded away (e.g. constant
    // register enables and resets), the constant driver is disconnected; constants created later
    // in the cleanup (LUTs or registers with a constant output) are left undriven.
    // Test: GateMateTest.constants_created_during_cleanup_stay_driven
    CONSTANT_DRIVER_DROPPED,

    // pack_io.cc pack_io_sel: after a double data rate output with an inverter in front of its
    // output buffer, every output packed after it is inverted (is_inverted is not reset per IO).
    // Tests: GateMateOddrInverterTest.other_outputs_not_inverted,
    // GateMateIoRegisterTest.ddr_output_through_inverter
    DDR_OUTPUT_INVERSION_LEAKS,

    // pack_cpe.cc pack_addf: a carry out that feeds the carry inputs of two adders is rejected
    // ("Only one other ADDF can be connected") instead of being packed.
    // Test: GateMateAdderTest.carry_feeding_two_carry_inputs
    CARRY_TO_SEVERAL_CARRY_INPUTS,

    // pack_io.cc pack_io: pins assigned through PIN_NAME parameters of IO buffers (not through the
    // CCF) are not taken out of the pool for unconstrained IOs, so an unconstrained IO can get the
    // same pin ("Can't place ... because it's already taken by ...").
    // Test: GateMateTest.pin_name_and_unconstrained_ios
    PIN_NAME_PAD_REUSED,

    // pack_bram.cc: the FIFO read pointer outputs F_RD_PTR[0], [2] and [5] share a CPE with the
    // fabric read clock, the pop enable and the reset; each gets a CPE of its own at the same
    // location, so the FIFO cannot be placed.
    // Test: GateMateBramTest.fifo_read_pointer_outputs
    FIFO_READ_POINTER_SHARED_CPE,

    // delay.cc getPortTimingClass: the inputs of the output buffers (CPE_OBUF etc. A and T) are
    // TMG_IGNORE, so paths to output pads (and output enables) are not analysed.
    // Tests: Stages/GateMateTimingPathTest.register_to_output_pad, input_pad_to_output_pad
    OUTPUT_PADS_NOT_TIMED,

    // delay.cc getPortTimingClass: all RAM ports starting with 'F' are TMG_IGNORE, among them the
    // FIFO status outputs (F_EMPTY[0], F_FULL[0], ...) and pointers (FRD_ADDR, FWR_ADDR), so
    // paths from them are not analysed.
    // Test: GateMateTimingArcTest.fifo_status_outputs_are_timed
    FIFO_STATUS_NOT_TIMED,

    // delay.cc getCellDelay: an IOSEL with both an output and an output enable (tristate and
    // bidirectional pads) has no timing arcs at all, so paths from bidirectional pads are not
    // analysed.
    // Tests: GateMateTimingArcTest.bidirectional_pad_input_is_timed,
    // Designs/GateMateArcCoverageTest.every_dependency_has_an_arc/io
    IOSEL_ARCS_WITH_OUTPUT_ENABLE,

    // delay.cc getCellDelay: the carry arc of CPE_ADDF/CPE_ADDF2 (CINY1 -> COUTY1) is looked up
    // under a name the timing database doesn't have, so carry chains are cut into single stages.
    // Tests: GateMateTimingArcTest.carry_in_to_carry_out, critical_path_runs_through_carry_chain,
    // Designs/GateMateArcCoverageTest.every_dependency_has_an_arc/adder
    CARRY_ARC_MISSING,

    // delay.cc: a carry from the middle of a chain that is also used as an ordinary signal leaves
    // the adder CPE through CPOUT2, which has neither a timing class nor arcs for
    // CPE_ADDF/CPE_ADDF2.
    // Test: Designs/GateMateArcCoverageTest.every_dependency_has_an_arc/adder_carry_used
    ADDER_CPOUT_NOT_TIMED,

    // delay.cc getCellDelay: CPE_L2T4 cells fed from the CPE's internal inputs COMBIN and CINY1
    // have no arcs to OUT (no such entries in the timing database): paths through a LUT that
    // drives a register and further logic, through the fifth input of 5-input LUT trees, through
    // the carry out of adder chains (comparators) and multiplier carry logic are not analysed.
    // (Fed only from CINX/PINX/PINY1, the output is TMG_IGNORE, which stops required times.)
    // Tests: GateMateLaunchTest.*, Designs/GateMateArcCoverageTest.every_dependency_has_an_arc/
    // luts, adder, multiplier
    CPE_INTERNAL_INPUT_ARCS_MISSING,

    // delay.cc getPortTimingClass: the fabric side output (OUT) of CPE_RAMI/CPE_RAMIO is
    // TMG_IGNORE, so required times are not propagated back to the RAM outputs.
    // Tests: GateMateTimingArcTest.ram_output_slack_is_known,
    // Designs/GateMateArcCoverageTest.every_dependency_has_an_arc/ram
    RAM_OUTPUT_CPE_IGNORED,

    // delay.cc getPortClockingInfo: the enable of a CPE register (EN_INT after packing) has no
    // setup time.
    // Test: GateMateTimingArcTest.register_enable_setup_time
    REGISTER_ENABLE_SETUP_MISSING,

    // common/kernel/timing_log.cc log_fmax: a path between two related clock domains that both
    // have no Fmax entry (no register-to-register paths of their own) throws std::out_of_range
    // from clock_fmax.at(), here during placement, once any other clock has an Fmax entry. With
    // GateMate, IO registers and RAMs clocked from the fabric get their clock through a CPE of
    // their own, giving such related clocks.
    // Tests: GateMateTimingTest.related_clocks_without_own_paths,
    // Seeds/GateMateRandomLogicTest.matches_reference/full_seed20 (and full_with_rams_*)
    FMAX_OF_RELATED_CLOCKS_WITHOUT_PATHS,

    // delay.cc getCellDelay: a CPE_ADDF2 gets arcs from all operand inputs to both sums, also from
    // the second bit's operands to the first bit's sum (OUT1). When the second bit of a chain adds
    // the first bit's sum, the packer puts both in one CPE, and timing analysis stops with a
    // combinational loop.
    // Test: GateMateAdderTest.sum_into_next_bit
    ADDER_SUM_INTO_NEXT_BIT,
};

// Whether tests still avoid the given bug. Kept as a function so that a restriction reads as
// `if (avoid(KnownBug::X))` at the place where coverage is limited.
constexpr bool avoid(KnownBug) { return true; }

// Harness workarounds for problems outside the GateMate code, also temporary:
//
//  - common/kernel/timing_log.cc log_crit_paths() keeps the first Context in a static lambda, so
//    critical path reports of later contexts use a deleted one. testing.cc
//    (pin_critical_path_log_context) prints the first report with a context that is never
//    deleted. Remove it once the static is fixed.

#endif
