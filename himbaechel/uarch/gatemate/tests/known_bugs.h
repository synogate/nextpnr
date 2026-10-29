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
