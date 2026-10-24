#!/bin/sh
# regress_loop.sh: regression for the "stuck between two frames" flicker loop (view on -> script starts -> reload -> view on -> ...).
# house.state + fuzz seed 66/181: the player ends up at Mom's mat after a warp with the view on. Before the fix the field was reloaded
# about every 50 frames forever (white flash / half-drawn room) and never settled. Counts reloads (CB2_ReturnToFieldLocal entries)
# over 500 frames after the mashing: must be <= 2. Needs house.state + syms.txt for the CURRENT build.
cd "$(dirname "$0")"
rt=$(grep -a " CB2_ReturnToFieldLocal$" syms.txt | awk '{print $1}')
fail=0
for seed in 66 181; do
    printf "load house.state\nrun 60\nfuzz $seed 30\nframelog 500\n" > /private/tmp/fp_loop.txt
    n=$(FUZZ_NOIDLE=1 ./fp_harness ../../pokeemerald.gba /private/tmp/fp_loop.txt 2>&1 | grep -a -o "FRAME.*cb2=[0-9a-f]*" | awk -v rt="$(printf %08x $((0x$rt + 1)))" '{c=$NF; sub("cb2=","",c); if (c==rt && p!=rt) n++; p=c} END{print n+0}')
    if [ "$n" -gt 2 ]; then echo "house seed $seed: LOOP ($n reloads in 500 frames)"; fail=1; else echo "house seed $seed: ok ($n reloads)"; fi
done
exit $fail
