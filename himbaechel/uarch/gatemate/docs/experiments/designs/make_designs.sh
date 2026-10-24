#!/bin/sh
# The picorv32 test designs: N attosoc cores (ice40/smoketest/attosoc, with a per-core din input
# so no core is optimised away) whose LEDs are XORed. 1/8/12/15 cores fill 6/48/71/89% of a CCGM1A1.
# Run from this directory; writes multiN.json. Yosys 0.67 was used for the measurements.
set -e
REPO=$(git rev-parse --show-toplevel)
for n in 1 8 12 15; do
    yosys -q -l synth_$n.log -p "read_verilog attosoc_din.v $REPO/ice40/smoketest/attosoc/picorv32.v multi.v; \
        chparam -set N $n multi; synth_gatemate -top multi -luttree -nomx8; write_json multi$n.json"
done
