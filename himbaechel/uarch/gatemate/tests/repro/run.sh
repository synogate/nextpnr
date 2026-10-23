#!/bin/bash
# Reproductions of the bugs in ../BUGS.md with the normal yosys + nextpnr flow, each with the
# output of nextpnr that shows the bug. Where it helps, a variant that differs in one detail
# (<design>_ok.v) shows what nextpnr does when the bug is not involved.
#
# Usage: ./run.sh <bug>...      e.g. ./run.sh 31, ./run.sh all
# Needs yosys and jq on the PATH. NEXTPNR defaults to the build of this checkout.
set -u
cd "$(dirname "$0")"
NEXTPNR=${NEXTPNR:-../../../../../build/nextpnr-himbaechel}
mkdir -p out

# synth <design> [synth_gatemate options]: <design>.v -> out/<design>.json
synth() {
    yosys -q -p "read_verilog $1.v; synth_gatemate -top top -luttree -nomx8 ${2:-}; write_json out/$1.json" \
        >out/$1.yosys.log 2>&1 || { echo "yosys failed on $1.v, see out/$1.yosys.log"; exit 1; }
}
# pnr <design> [nextpnr options]: nextpnr on out/<design>.json, log in out/<design>.log
pnr() {
    local d=$1
    shift
    { "$NEXTPNR" --device CCGM1A1 --json out/$d.json --vopt allow-unconstrained --freq 100 "$@" >/dev/null 2>out/$d.log; } 2>/dev/null
    status=$?
}
# pack <design> [nextpnr options]: pack only, netlist after packing in out/<design>.packed.json
pack() { local d=$1; shift; pnr $d --pack-only --write out/$d.packed.json "$@"; }
title() { echo "== $*"; }
# The result of the run: the exit status if nextpnr did not finish normally
result() {
    case $status in
    0) ;;
    139) echo "nextpnr: segmentation fault" ;;
    134) echo "nextpnr: aborted: $(grep -m1 -E "what\(\)|terminate" out/$1.log | sed 's/^ *//')" ;;
    *) grep -m1 ERROR out/$1.log | sed 's/^/nextpnr: /' ;;
    esac
}
# The Fmax line of the final timing report
fmax() { grep -E "No Fmax|Max frequency" out/$1.log | tail -1 | sed 's/^Info: //'; }
# critical <design> <pattern>: lines of the final critical path report for the clock that match
critical() {
    sed -n '/Info: Router2 time/,$p' out/$1.log | sed -n '/Critical path report for clock/,/ns logic/p' |
        grep -E "$2" | sed 's/^Info: *//'
}
# inputs <packed json> <cell regex> [port regex]: what drives the inputs of the matching cells
inputs() {
    jq -r --arg cell "$2" --arg port "${3:-.}" '
        .modules.top.cells as $cells
        | ([$cells | to_entries[] | .key as $c | .value as $v | $v.connections | to_entries[]
            | select($v.port_directions[.key] == "output" and (.value | length) > 0)
            | {key: (.value[0] | tostring), value: "\($c).\(.key)"}] | from_entries) as $drv
        | $cells | to_entries[] | select(.key | test($cell)) | .key as $c | .value as $v
        | $v.connections | to_entries[] | select($v.port_directions[.key] == "input" and (.key | test($port)))
        | "\($c).\(.key) <- " + (if (.value | length) == 0 then "not connected"
                                  else ($drv[.value[0] | tostring] // "nothing (no driver)") end)' "$1"
}
# The register behind an output port of the yosys netlist
register_of() {
    jq -r --arg port "$2" '.modules.top as $m | ($m.netnames[$port].bits[0]) as $pad
        | ($m.cells[] | select(.type == "CC_OBUF" and .connections.O == [$pad]) | .connections.A) as $q
        | $m.cells | to_entries[] | select(.value.type == "CC_DFF" and .value.connections.Q == $q) | .key' "$1"
}
# timed <report json> <cell>: the paths into a cell that the timing analysis found
timed() {
    jq -r --arg cell "$2" '[.detailed_net_timings[] | .event as $from | .endpoints[] | select(.cell == $cell)
        | "\(.port): a path from \($from) to \(.event)"] | unique
        | if length == 0 then "no path into it" else .[] end' "$1"
}

bug() {
    case $1 in
    1)
        for d in b01_same_input b01_same_input_ok; do
            title $d.v
            synth $d; pack $d
            inputs out/$d.packed.json 'top\.o\$iosel$' '^OUT1$'
        done ;;
    2)
        d=b02_constant_input
        title $d.v
        synth $d; pack $d
        jq -r '"lut: INIT_L10 = \(.modules.top.cells.lut.parameters.INIT_L10) (D0_10 AND D1_10)"' out/$d.packed.json
        inputs out/$d.packed.json '^lut$' '^D' ;;
    3)
        for d in b03_ddr_inverter b03_ddr_inverter_ok; do
            title $d.v
            synth $d; pack $d
            jq -r '.modules.top.cells["$iopadmap$top.plain$iosel$OUT1_cpe"].parameters.INIT_L10
                | "LUT in front of the pad plain: INIT_L10 = \(.) (1010: plain = p, 0101: plain = ~p)"' out/$d.packed.json
        done ;;
    4)
        d=b04_mult
        title $d.v
        synth $d; pack $d; result $d ;;
    5)
        d=b05_output
        title $d.v
        synth $d; pnr $d
        echo "paths in the final timing report:"
        sed -n '/Info: Router2 time/,$p' out/$d.log | grep -E "Critical path report|Max delay|No Fmax" | sed 's/^Info: */  /' ;;
    6)
        d=b06_sdf
        title $d.v
        synth $d; pnr $d --sdf out/$d.sdf --write out/$d.routed.json
        r=$(jq -r '.modules.top.cells | to_entries[] | select(.value.type == "CPE_FF") | .key' out/$d.routed.json)
        echo "ports of the register $r after routing: $(jq -r --arg r "$r" '.modules.top.cells[$r].connections
            | to_entries | map(select(.value != []) | .key) | join(" ")' out/$d.routed.json)"
        echo "its SDF entries:"
        sed 's/\\//g' out/$d.sdf | awk -v r="(INSTANCE $r)" 'index($0, r) {p = 1; next} p && /\(CELL/ {exit}
            p && /IOPATH|SETUPHOLD/ {sub(/^ +/, "  "); print}' ;;
    7)
        d=b07_upper
        title $d.v
        synth $d; pnr $d --sdf out/$d.sdf --write out/$d.routed.json
        echo "clock-to-out of the registers in the SDF, by the half of the CPE they are in:"
        jq -r '.modules.top.cells | to_entries[] | select(.value.type == "CPE_FF") | "\(.key) \(.value.attributes.NEXTPNR_BEL)"' out/$d.routed.json |
            while read -r cell bel; do
                printf '  %-8s %s\n' "$([ "${bel##*_}" = U ] && echo upper || echo lower)" \
                    "$(sed 's/\\//g' out/$d.sdf | grep -A4 "(INSTANCE $cell)" | grep -m1 IOPATH | sed 's/^ *//')"
            done | sort | uniq -c ;;
    8)
        d=b08_globals
        title "$d.v on CCGM1A2 with --vopt strategy=full"
        synth $d
        "$NEXTPNR" --device CCGM1A2 --json out/$d.json --vopt allow-unconstrained --vopt strategy=full --pack-only \
            >/dev/null 2>out/$d.log
        grep -E "BUFG" out/$d.log | grep -v "Sort\|Packing" | sed 's/\$auto\$clkbufmap.cc:261:execute//' ;;
    9)
        d=b09_cascade
        title $d.v
        synth $d; pack $d; result $d
        grep -E "ERROR|Cascade|finished" out/$d.log ;;
    11)
        d=b11_constant_lut
        title $d.v
        synth $d; pack $d
        echo "the multiplexer m after packing (INIT_L00 = $(jq -r '.modules.top.cells.m.parameters.INIT_L00' out/$d.packed.json): D0_00 is the constant input)"
        inputs out/$d.packed.json '^m$' '^D' ;;
    12)
        d=b12_two_carries
        title $d.v
        synth $d; pack $d; result $d ;;
    13)
        d=b13_ccf
        title "$d.v with $d.ccf"
        cat $d.ccf | sed 's/^/  /'
        synth $d; pack $d --vopt ccf=$d.ccf
        echo "after packing (DRIVE: 00 = 3 mA, 01 = 6 mA, 10 = 9 mA, 11 = 12 mA; SLEW: 0 = fast, 1 = slow):"
        for p in x y; do
            jq -r --arg c "\$iopadmap\$top.$p\$iosel" '.modules.top.cells[$c].parameters | "  '$p': DRIVE = \(.DRIVE), SLEW = \(.SLEW)"' out/$d.packed.json
        done ;;
    14)
        for d in b14_region b14_region_ok; do
            title "$d.v with b14_region.ccf (region x60y60x70y70, i.e. bel grid X62..72, Y62..72)"
            synth $d -scopename; pnr $d --vopt ccf=b14_region.ccf --write out/$d.routed.json
            jq -r '.modules.top.cells | to_entries[] | select(.key == "u.lut") | "u.lut placed at \(.value.attributes.NEXTPNR_BEL)"' out/$d.routed.json
        done ;;
    15)
        d=b15_pin_name
        title $d.v
        synth $d; pnr $d
        grep -E "Constraining.*IO_SA_A0|ERROR" out/$d.log | sed 's/^Info: *//' ;;
    16)
        for d in b16_pll b16_pll_ok; do
            synth $d; pack $d --vopt ccf=b16_pll.ccf
            jq -S '.modules.top.cells | to_entries[] | select(.value.type == "PLL") | .value.parameters' out/$d.packed.json >out/$d.params
        done
        title "PLL configuration of b16_pll.v (<) against b16_pll_ok.v (>)"
        diff out/b16_pll.params out/b16_pll_ok.params | grep '^[<>]' ;;
    17)
        for d in b17_keeper b17_keeper_ok b17_pullup; do
            title $d.v
            synth $d; pack $d; result $d
            jq -r '.modules.top.cells["a_buf$iosel"].parameters | "pad a: PULLUP = \(.PULLUP // "0"), PULLDOWN = \(.PULLDOWN // "0")"' out/$d.packed.json
        done ;;
    18)
        d=b18_noiopad
        title "$d.v synthesized with -noiopad"
        synth $d -noiopad; pack $d; result $d ;;
    19)
        d=b19_no_iobuf
        title "$d.v synthesized with -noiopad"
        synth $d -noiopad; pack $d; result $d
        grep -E "ERROR|Warning" out/$d.log
        inputs out/$d.packed.json 'parse_blif' '^D' ;;
    20)
        d=b20_ram_loc
        title "$d.v (RAM blocks: columns X35, X67, X99, X131; rows Y3, Y19, ..., Y115)"
        synth $d; pnr $d --write out/$d.routed.json
        for r in ram0 ram1; do
            echo "$r: LOC = $(jq -r --arg r $r '.modules.top.cells[$r].parameters.LOC' out/$d.json), placed at $(jq -r --arg r "$r\$full" '.modules.top.cells[$r].attributes.NEXTPNR_BEL' out/$d.routed.json)"
        done ;;
    21)
        for d in b21_fifo_rd_ptr b21_fifo_rd_ptr_ok; do
            title $d.v
            synth $d; pnr $d; result $d
            [ $status = 0 ] && grep -m1 finished out/$d.log | sed 's/^Info: /nextpnr: /'
        done ;;
    23)
        title "b23_adder.v with W = 128"
        yosys -q -p "read_verilog b23_adder.v; chparam -set W 128 top; synth_gatemate -top top -luttree -nomx8; write_json out/b23_adder.json" >/dev/null 2>&1
        pnr b23_adder
        fmax b23_adder
        echo "adder CPEs on the critical path: $(critical b23_adder 'Source.*slice\[' | grep -o 'slice\[[0-9]*\]' | sort -u | tr '\n' ' ')"
        critical b23_adder 'clk-to-q|logic|setup|ns routing' ;;
    24 | 25)
        for d in b24_ram_read b25_ram_do_reg; do
            title $d.v
            synth $d; pnr $d
            critical $d 'clk-skew|clk-to-q|logic|setup'
        done ;;
    26)
        d=b26_enable
        title $d.v
        synth $d; pnr $d
        critical $d 'setup' ;;
    27)
        for d in b27_fifo b27_fifo_ok; do
            title $d.v
            synth $d; pnr $d; fmax $d
        done ;;
    28)
        d=b28_carry_in
        title $d.v
        synth $d; pnr $d --sdf out/$d.sdf
        echo "delays of the CP lines in the SDF:"
        grep -A4 '"CPE_CPLINES"' out/$d.sdf | grep IOPATH | sed 's/^ *//' | sort | uniq -c ;;
    30)
        for d in b30_inout b30_inout_ok; do
            title $d.v
            synth $d; pnr $d
            echo "paths in the final timing report:"
            sed -n '/Info: Router2 time/,$p' out/$d.log | grep -E "Critical path report|No Fmax|Max delay" | sed 's/^Info: */  /'
        done ;;
    31)
        for d in b31_compare b31_compare_ok b31_lut5 b31_lut5_ok b31_shared b31_shared_ok; do
            title $d.v
            synth $d; pnr $d --report out/$d.report.json --detailed-timing-report
            fmax $d
            r=$(register_of out/$d.json q)
            [ -z "$r" ] && r=$(register_of out/$d.json q2)
            echo "register of $( [ "$d" = "${d#b31_shared}" ] && echo q || echo q2) ($r) in the timing report: $(timed out/$d.report.json "$r" | tr '\n' ' ')"
        done ;;
    33)
        for d in b33_mid_carry b33_mid_carry_ok; do
            title $d.v
            synth $d; pnr $d; fmax $d
        done ;;
    34)
        d=b34_ram_tieoff
        title $d.v
        synth $d; pack $d
        echo "CPEs in front of the RAM's inputs: $(jq '[.modules.top.cells | keys[] | select(test("^mem.0.0\\$.*_cpe$") and (test("\\$DO") | not))] | length' out/$d.packed.json)"
        echo "of them with a constant and no input (the pins the RAM ignores at 20 bit width):"
        jq -r '.modules.top.cells | to_entries[] | select(.key | test("^mem.0.0\\$.*_cpe$") and (test("\\$DO") | not))
            | select([.value.connections | to_entries[] | select((.key | startswith("D")) and .value != [])] | length == 0)
            | "\(.key | sub("mem.0.0\\$"; "") | sub("_cpe"; ""))=\(.value.parameters.INIT_L10 | if . == "0000" then 0 else 1 end)"' \
            out/$d.packed.json | sort -V | tr '\n' ' ' | fold -s -w 96 | sed 's/^/  /'
        echo ;;
    35)
        for d in b35_inverted_clock b35_inverted_clock_ok; do
            title $d.v
            synth $d; pack $d
            inputs out/$d.packed.json 'slice' 'CLK'
            inputs out/$d.packed.json '^inv$' '^D'
        done ;;
    36)
        for d in b36_carry_operand b36_carry_operand_ok; do
            title $d.v
            synth $d; pnr $d; result $d
            [ $status = 0 ] && grep -m1 finished out/$d.log | sed 's/^Info: /nextpnr: /'
        done ;;
    37)
        for d in b37_adder_sum_operand b37_adder_sum_operand_ok; do
            title $d.v
            synth $d; pnr $d; result $d
            grep -A3 "combinational loops:" out/$d.log | sed 's/^Info: */nextpnr: /'
            [ $status = 0 ] && grep -m1 finished out/$d.log | sed 's/^Info: /nextpnr: /'
        done ;;
    o1 | O1)
        d=o1_fmax_crash
        title "$d.v synthesized with -noclkbuf"
        synth $d -noclkbuf; pnr $d; result $d ;;
    *)
        echo "no reproduction for bug $1 with the command line tools (see ../BUGS.md for its unit test)" ;;
    esac
}

[ $# = 0 ] && { sed -n '2,7p' "$0" | sed 's/^# \?//'; exit 1; }
[ "$1" = all ] && set -- 1 2 3 4 5 6 7 8 9 11 12 13 14 15 16 17 18 19 20 21 23 24 26 27 28 30 31 33 34 35 36 37 o1
for b in "$@"; do
    echo "######## bug $b"
    bug $b
done
