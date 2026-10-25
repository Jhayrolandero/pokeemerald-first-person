#!/bin/sh
# run.sh SCRIPT: run a harness script and show REGS/GMAIN/wait lines (resolved)
cd "$(dirname "$0")"
./fp_harness ../../pokeemerald.gba "$1" 2>&1 | grep -a -o "REGS.*\|GMAIN.*\|PLAYER.*\|FIELDCB.*\|SHADOW.*\|CAMERA.*\|wait[a-z]*[ 0-9]*: [0-9]* frames\|walkloop.*\|pressflip.*" | python3 resolve.py
