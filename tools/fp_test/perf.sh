#!/bin/sh
# perf.sh: fps summary for free_perf.txt + scenery_perf.txt
cd "$(dirname "$0")"
for s in free_perf scenery_perf; do ./fp_harness ../../pokeemerald.gba $s.txt 2>&1 | grep -a -o "PERF [a-z_]*: [0-9]* frames, [0-9]* page flips ([0-9.]* fps), [0-9]* renders, avg [0-9]* scanlines"; done
