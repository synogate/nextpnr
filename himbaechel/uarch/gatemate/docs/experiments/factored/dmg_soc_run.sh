#!/bin/bash
# run.sh VARIANT SEED  -> dmg_soc.VARIANT.sSEED.log
V=$1; S=$2
NP=/c/Users/mio/source/nextpnr/.claude/worktrees/friendly-mendeleev-6d164f/build/Release/nextpnr-himbaechel.axis2.exe
D=/c/Users/mio/source/synogate/generator/dmg_soc/gatemate
case $V in
  table) X="--vopt lookahead=table --vopt place-delay=table";;
  axis) X="--vopt lookahead=axis --vopt place-delay=table";;
  both) X="--vopt lookahead=axis --vopt place-delay=axis";;
  hybrid) X="";;
esac
cd $D
timeout 3600 $NP --json synth.json --device CCGM1A1 --router router2 --seed $S --sdc top.sdc -o ccf=top.ccf -o fpga_mode=speed -o time_mode=typical --vopt timing-fixes --vopt mux-chains --vopt route-repair=0 --vopt place-repair=0 $X > "$OLDPWD/dmg_soc.$V.s$S.log" 2>&1
echo "exit $?" >> "$OLDPWD/dmg_soc.$V.s$S.log"
