# GateMate bugs found by these tests

Bugs found while writing the tests in this directory, against nextpnr commit 44a6c739. Each
one has at least one test that fails until it is fixed. Bugs that other tests deliberately steer
around also have an entry in `known_bugs.h`; delete the entry once the bug is fixed, and the
compiler points at every restriction to lift.

`repro/` has a small Verilog design for every bug that shows up in the normal yosys + nextpnr flow
(all except 10, 22, 29, 32 and the second one outside the GateMate code), mostly with a variant
that differs in one detail and does not trigger the bug. `repro/run.sh <number>` (or `all`)
synthesizes them, runs nextpnr and prints the part of its output that shows the bug: for the
timing bugs, typically that nextpnr finds no register-to-register path in a design that has
exactly one, or the delay it uses for one arc. It needs yosys and jq, and writes into `repro/out/`.

| # | Where | What goes wrong | Failing test(s) | `KnownBug` |
|---|---|---|---|---|
| 1 | `pack.cc` `optimize_lut2` | A LUT with the same net on two inputs gets a configuration for the input it disconnects; `AND(x, x)` leaves the pin undriven. Also happens after a buffer LUT is removed. | `GateMateLut2Test.same_net_on_both_inputs`, `input_equal_after_buffer_removal`, `GateMateL2T4Test.repeated_inputs` | `LUT_SAME_INPUT` |
| 2 | `pack.cc` `optimize_lut2` | A LUT input tied to a constant is disconnected without updating the LUT configuration. A disconnected CPE input reads an arbitrary wire. | `GateMateLut2Test.constant_i0`, `constant_i1`, `GateMateL2T4Test.constant_inputs` | `LUT_CONSTANT_INPUT` |
| 3 | `pack_io.cc` `pack_io_sel` | After a DDR output with an inverter in front of its buffer, every output packed later is inverted (`is_inverted[]` is never reset). | `GateMateOddrInverterTest.other_outputs_not_inverted`, `GateMateIoRegisterTest.ddr_output_through_inverter` | `DDR_OUTPUT_INVERSION_LEAKS` |
| 4 | `pack_mult.cc:967` | Reads `id_EN` where it means `id_SR`; segfaults on multiplier output registers whose resets differ. | `GateMateMultTest.output_registers_with_mismatched_reset` | |
| 5 | `delay.cc` | Output pads are never timing endpoints (`CPE_OBUF` `A`/`T` are `TMG_IGNORE`), so paths into output pads are not analysed. | `GateMateTimingPathTest.register_to_output_pad`, `input_pad_to_output_pad` | `OUTPUT_PADS_NOT_TIMED` |
| 6 | `postRoute` | CPE ports are renamed (`CLK_INT`→`CLK`, `D0_10`→`INx`) after routing, so timing analysis afterwards finds no paths, and `--sdf` writes `CLK` as data against a nonexistent `CLK_INT`. | `GateMateTimingPathTest.*/routed` | |
| 7 | `delay.cc:472` | Clock-to-Q of an upper-half CPE flip-flop uses the lower half's `Q1_OUT1` delay (10 ps off). | `GateMateFfTimingTest.clock_to_q_uses_output_of_own_half/upper` | |
| 8 | `pack_clocking.cc:67` | Hardcodes 4 instead of `max_num`, so `--strategy=full` on CCGM1A2 keeps 4 global clocks instead of 8. | `GateMateClockingTest.full_strategy_uses_globals_of_all_dies` | |
| 9 | `pack_bram.cc` | The cascade width check reads `A_WIDTH`/`B_WIDTH`, which `CC_BRAM_40K` doesn't have, so cascades with wide ports are accepted. | `GateMateBramTest.cascade_rejects_wide_ports` | |
| 10 | `config.h` | `CfgLoc::operator!=` is wrong (currently unused). | `GateMateConfig.cfgloc_comparison` | |
| 11 | `pack.cc` cleanup | Once constant register enables and resets are folded away, the constant drivers are disconnected; constants created later in the cleanup are left undriven. | `GateMateTest.constants_created_during_cleanup_stay_driven` | `CONSTANT_DRIVER_DROPPED` |
| 12 | `pack_cpe.cc` `pack_addf` | A carry out feeding the carry inputs of two adders is rejected ("Only one other ADDF can be connected"). | `GateMateAdderTest.carry_feeding_two_carry_inputs` | `CARRY_TO_SEVERAL_CARRY_INPUTS` |
| 13 | `ccf.cc` `parse_params` | Uses `dict::emplace`, so a pin's parameter never overrides a `default_GPIO` value or an earlier value for the same pin. | `GateMateCcfTest.pin_parameter_overrides_default`, `repeated_pin_parameter` | |
| 14 | `pack_io.cc` `pack_io_sel` | A LUT driving an output pad is absorbed into the pad's CPE at a fixed location, silently ignoring its floorplanning region. (abc drops the scope attribute from the LUTs it creates, so in a yosys flow only instantiated LUTs have a region.) | `GateMateCcfTest.region_of_lut_driving_output` | |
| 15 | `pack_io.cc` `pack_io` | Pins assigned through `PIN_NAME` parameters stay in the pool for unconstrained IOs, so an unconstrained IO can get the same pin. | `GateMateTest.pin_name_and_unconstrained_ios` | `PIN_NAME_PAD_REUSED` |
| 16 | `pack_clocking.cc:457`, `:462` | A missing `LOCK_REQ`/`LOW_JITTER` is read as 0; the primitives library (UG1001, `CC_PLL` parameters) and yosys's `cells_bb.v` default both to 1, and yosys does not write default parameters. Besides `LOCK_REQ = 0`, the PLL may then get a reference divider that low-jitter mode excludes (`K = 9` for 10 → 123.456 MHz). | `GateMatePllTest.primitive_defaults` | |
| 17 | `pack_io.cc:247` | `KEEPER` together with `PULLUP` (or `PULLDOWN`) alone is accepted: the check only fires when both pulls are set (`> 1`), and the keeper encoding then silently replaces the requested pull. The primitives library (UG1001) says pull-up, pull-down and keeper are mutually exclusive. | `GateMateTest.keeper_with_pull_up` | |
| 18 | `pack_cpe.cc:790` `move_ram_io` | A RAM input on an undriven net dereferences the missing driver (segfault). yosys turns undriven wires into `x`, but a RAM fed from an input port with `-noiopad` gets one through bug 19. | `GateMateBramTest.input_from_undriven_net` | |
| 19 | `pack_io.cc:113` | A top-level input that feeds logic without an IO buffer silently loses its driver. | `GateMateTest.port_driving_logic_without_buffer` | |
| 20 | `pack_bram.cc:287` | The `LOC` parameter of the RAM primitives is read but ignored. | `GateMateBramTest.location_parameter` | |
| 21 | `pack_bram.cc` | The FIFO read pointer outputs `F_RD_PTR[0]`, `[2]` and `[5]` share a CPE with the fabric read clock, pop enable and reset; the packer gives each its own cell at the same location, so the FIFO cannot be placed. | `GateMateBramTest.fifo_read_pointer_outputs` | `FIFO_READ_POINTER_SHARED_CPE` |
| 22 | — | *Withdrawn.* nextpnr treats a `CC_FIFO_40K` without `RAM_MODE` as SDP, which is the default the GateMate primitives library (UG1001, table 6.15) gives. The difference is in yosys's simulation model, see below. | (`GateMateBramTest.fifo_ram_mode_defaults_to_sdp` now checks the vendor behaviour and passes) | |
| 23 | `delay.cc:169` | The carry arc of the adders is looked up as `timing__ADDF2Y1_CINY1_COUTY1`, but the timing database calls it `_ADDF2Y1_CINY1_COUTY` (prjpeppercorn only renames the `_ROUTING_` variant). So `CPE_ADDF`/`CPE_ADDF2` have no carry-in to carry-out arc, and timing analysis cuts every carry chain into single stages: the critical path of a registered 16-bit adder is reported as two CPEs. | `GateMateTimingArcTest.carry_in_to_carry_out`, `critical_path_runs_through_carry_chain`, `Designs/GateMateArcCoverageTest.every_dependency_has_an_arc/adder` | `CARRY_ARC_MISSING` |
| 24 | `delay.cc` `getCellDelay` | The CPE that takes a RAM output into the fabric (`CPE_RAMI`/`CPE_RAMIO`, `RAM_I`→`OUT`) has zero delay; the timing database has `_ARBLUT_RAM_I1_OUT1`/`_ARBLUT_RAM_I2_OUT2` (about 0.1 ns). Every RAM read path is reported slightly too fast. | `GateMateTimingArcTest.ram_output_feed_through` | |
| 25 | `delay.cc` `getPortClockingInfo` | RAMs with output registers (`A_DO_REG`/`B_DO_REG`) use the clock-to-out of the unregistered RAM (`RAM_NOECC_IOPATH_*`, about 3.0 ns); the `RAM_REG_*` entries of the timing database (about 1.35 ns) are never used. Registered RAM reads are reported about 1.7 ns too slow. | `OutputRegisters/GateMateRamTimingTest.clock_to_out/registered` | |
| 26 | `delay.cc:469` | The packer renames the enable of CPE registers to `EN_INT` (`pack_cpe.cc:196`, `:416`), but the setup time is only given to `DIN`, `EN` and `SR`: register enables have a setup time of 0. | `GateMateTimingArcTest.register_enable_setup_time`, `Pairs/GateMateTimingMatrixTest.path_is_timed/pad_to_ff_controls` | `REGISTER_ENABLE_SETUP_MISSING` |
| 27 | `delay.cc:392` | All RAM ports starting with `F` are `TMG_IGNORE`, among them the FIFO status outputs (`F_EMPTY[0]`, `F_FULL[0]`, ...) and pointers: paths from FIFO flags into the fabric (e.g. back into the push/pop logic) are not analysed. | `GateMateTimingArcTest.fifo_status_outputs_are_timed` | `FIFO_STATUS_NOT_TIMED` |
| 28 | `delay.cc:185` | All arcs of `CPE_CPLINES` have zero delay. They carry the carry input into every carry chain (`pack_cpe.cc:597`) and the signals between multiplier CPEs; the timing database has `comb12_*` (about 0.4 ns from the LUT output) and `_ROUTING_*` (0.06 to 0.65 ns) for them. | `GateMateTimingArcTest.carry_lines_delays` | |
| 29 | `delay.cc:304` | The fabric side output of the RAM pin CPEs (`CPE_RAMI`/`CPE_RAMIO`) is called `OUT`, but only `O` and `RAM_O` are classed as outputs, so `OUT` is `TMG_IGNORE`. Arrival times still pass through it, but required times do not: slack and criticality are not propagated back to the RAM outputs. | `GateMateTimingArcTest.ram_output_slack_is_known`, `Designs/GateMateArcCoverageTest.every_dependency_has_an_arc/ram` | `RAM_OUTPUT_CPE_IGNORED` |
| 30 | `delay.cc:230`, `:232` | For an IOSEL with both an output and an output enable (tristate and bidirectional pads), an arc is rejected unless its source is both the output's and the enable's source, so none is left: no data or enable path, and no input path (`GPIO_IN`→`IN1`). Paths from bidirectional pads into the fabric are not analysed; paths to tristate outputs would not be even once bug 5 is fixed. | `GateMateTimingArcTest.bidirectional_pad_input_is_timed`, `Designs/GateMateArcCoverageTest.every_dependency_has_an_arc/io` | `IOSEL_ARCS_WITH_OUTPUT_ENABLE` |
| 31 | `delay.cc:162`, `:254` | LUT CPEs fed from the CPE's internal inputs `COMBIN` and `CINY1` have no arc to `OUT` (the lookups `timing__ARBLUT_COMBIN_OUT*` / `_CINY1_OUT*` are not in the timing database), so paths through them are not analysed. The packer uses `COMBIN` where a LUT drives a register **and** further logic (`pack_cpe.cc:281`) and for the fifth input of every 5-input LUT tree (`pack_cpe.cc:309`), and `CINY1` for the carry out of adder chains (`pack_cpe.cc:685`), i.e. comparisons like `a < b`. On picorv32 synthesized by yosys, 396 of 495 registers have a path into them that is not analysed, and with the missing arcs added (at zero delay) the critical path grows from 12.15 to 14.93 ns: 82.3 MHz reported, at most 67.0 MHz. In a 32-bit `lt <= a < b`, the result register is not timed at all. (A LUT fed only from `CINX`/`PINX`/`PINY1`, as in multipliers, has arcs, but its output is `TMG_IGNORE`, which stops required times.) | `GateMateLaunchTest.*`, `Designs/GateMateArcCoverageTest.every_dependency_has_an_arc/luts`, `/adder`, `/multiplier` | `CPE_INTERNAL_INPUT_ARCS_MISSING` |
| 32 | `delay.cc` `predictDelay` | Before routing, the delay of a global clock net is estimated from the distance to each sink like any other net: 5.5 to 15.8 ns to 40 registers, where the routed clock tree has 1.1 to 3.3 ns. The timing report after placement therefore contains up to 10 ns of clock skew that does not exist. (The routers route the clocks first, so routing is not affected.) | `GateMateTimingArcTest.global_clock_estimate_matches_routed_tree` | |
| 33 | `delay.cc:267`, `:169` | A carry from the middle of a chain that is also used as an ordinary signal leaves the adder CPE through `CPOUT2` (`pack_cpe.cc:717`, merged in `pack.cc:464`). For `CPE_ADDF`/`CPE_ADDF2`, `CPOUT1`/`CPOUT2` are `TMG_IGNORE` and have no arcs (only multipliers use the database's `cpout_OUT*`), so paths from such a carry into the fabric are not analysed. | `Designs/GateMateArcCoverageTest.every_dependency_has_an_arc/adder_carry_used` | `ADDER_CPOUT_NOT_TIMED` |
| 34 | `pack_bram.cc`, `pack_cpe.cc:853` `move_ram_io` (QoR) | Every RAM input tied to a constant gets a CPE at its fixed place, also the inputs the RAM ignores in its configuration: address bits below the lowest one used at the port's width and bit 6 of a 20K RAM (which yosys ties to 0: 12 CPEs for a 20 bit wide 20K RAM), the address bits [6:0] in SDP mode (14 CPEs), and all data, bit mask and address inputs of a port that never writes or is disabled. | `Configs/GateMateRamTieOffTest.ignored_inputs_take_no_cpe/*` | |
| 35 | packer (QoR) | An inverter on a clock stays a LUT: the registers and RAMs behind it are clocked from the fabric through it instead of from the global clock with their own clock inversion. Each such clock also takes a global clock network of its own, fed from the fabric. (yosys folds inferred inverters into `CLK_INV` in flat designs, so this matters for instantiated LUTs and netlists from elsewhere.) | `Inversion/GateMateClockInversionTest.global_clock_reaches_registers_and_rams/inverter_lut` | |
| 36 | `pack_cpe.cc:708` | Looking for the adder that continues a chain among the users of a mid-chain carry, the condition is `usr.cell->type == id_CC_ADDF \|\| usr.port == id_CI` (meant: `&&`). If a single-bit adder (one not merged into a two-bit CPE) uses the carry as an operand and comes first among the users, the packer disconnects that adder's own carry input and connects the chain's carry to it; the chain's next bit is left on a `CPOUT2` output. In the full flow the router then fails ("Failed to route arc … CPOUT2 → CINY1"); with pack and place only, the netlist computes the wrong result. Whether it happens depends on the order of the cells (their names, in the JSON frontend). | `GateMateAdderTest.carry_used_as_operand_before_next_bit` | |

## Outside the GateMate code

| Where | What goes wrong | Failing test(s) | `KnownBug` |
|---|---|---|---|
| `common/kernel/timing_log.cc:304` `log_fmax` | `clock_fmax.at()` throws `std::out_of_range` for a path between two related clocks that have no Fmax entry of their own, once any other clock has one. With GateMate, fabric-clocked RAMs and IO registers create such clocks. (`repro/run.sh o1`) | `GateMateTimingTest.related_clocks_without_own_paths`, fuzz seeds `full_seed20`, `full_with_rams_*` | `FMAX_OF_RELATED_CLOCKS_WITHOUT_PATHS` |
| `common/kernel/timing_log.cc:43` | A static lambda keeps the first `Context`, so critical path reports of later contexts use a deleted one. The test fixture works around it (`pin_critical_path_log_context` in `testing.cc`). | — | |
| yosys `techlibs/gatemate/cells_sim.v` `CC_FIFO_40K` | Defaults `RAM_MODE` to `"TDP"`, while the primitives library (UG1001, table 6.15) and nextpnr default to `"SDP"`. A FIFO that leaves `RAM_MODE` at its default simulates as TDP and is rejected by nextpnr unless it is 80 bits wide. | — | |
| yosys `techlibs/gatemate/cells_sim.v` `CC_FIFO_40K` | Stores word n at `n * 2^(15-tp)`, so words of 5 bits or more overlap (a written `aaaaaaaaaa` reads back as `55aaaaaaaa`). Verified with iverilog (`fifo_trace_tb.v`). The test simulator stores words at `n * width`. | — | |

## Latent or unclear

- `pack_bram.cc` `ram_clk_signal` and the `CLOCK%d` handling in `pack_io.cc` only cover global
  clocks 0–3. Clocks 4–7 (second die on CCGM1A2) can't occur while bug 8 holds;
  `GateMateBramSemanticsTest.six_global_clocks_on_two_dies` covers them once it is fixed.
- A FIFO in SDP mode configures `input_config_b0`/`output_config_a0`, while a RAM in SDP mode
  uses a0 for writes and b0 for reads. Possibly swapped; needs hardware or the vendor tool to
  decide.
- The prjpeppercorn CPE schematic and nextpnr disagree on which mux (IN1–4 or IN5–8) feeds
  `L20.D1` in MX4 mode. The test simulator follows nextpnr.
- `delay.cc:522`, `:558` give RAM ports whose names start with `CLOCK` the database's
  `RAM_*_IOPATH_1` and `RAM_*_SETUPHOLD_2` (with a TODO about `CLOCK1`). In the vendor's RAM
  tables these are the delay from the global clock inputs to the forwarded clock outputs and a
  check between the RAM's clock inputs, not data timing. The code has no effect: clock ports
  are `TMG_CLOCK_INPUT`, so the timing analysis never asks for it.
- `common/kernel/timing.cc` `identify_related_domains`: the delay between two clocks derived from
  the same source only adds up cell delays, not routing. With GateMate, every fabric clock that
  reaches a RAM or an IO register through a CPE of its own is such a derived clock. Not covered
  by a test.

## Checked and found correct

- The clock each RAM pin is timed against (`delay.cc` `clock()`/`ram_clock_index`): 20K and 40K,
  TDP and SDP, global and fabric clocks, inverted clocks, both halves of a block shared by two
  20K RAMs, cascades (`Cases/GateMateRamClockTest`).
- Launch and capture clock edges, clock-to-out and setup for paths between pads, CPE registers
  and latches, IO registers (global and fabric clock), DDR registers, RAMs (both ports, SDP,
  fabric clock) and FIFO inputs (`Pairs/GateMateTimingMatrixTest`, except the cases skipped for
  known bugs).
- Clock-to-out of IO input registers on each of the four global clocks; the setup times of the RAM
  inputs against the vendor's grouping of RAM pins; the clock-to-out of unregistered RAM outputs.

## Mapping quality checked and found good

`qor.cc`: logic constrained to a box of CPEs fills it completely (two LUTs, LUT trees, LUTs with
registers or registers per CPE; one MX4 or 5-input LUT tree per CPE; a 32 bit adder in 18 CPEs of
a column), adders take two bits per CPE, registers share the CPE of their LUT, constant register
controls need no logic, logic in front of RAM inputs and output pads goes into their CPEs, and two
20K RAMs share a block, so the device takes twice as many 20K RAMs as it has blocks.

`random_logic.cc` `cpe_use_within_bound`: every fuzzer design uses no more CPE halves than a bound
computed from its netlist with what a CPE can hold (`cpe_half_bound` in `testing.cc`); the designs
use 78 to 97% of it.
