#!/bin/sh
# fuzz_one.sh STATE SEED N [OUTDIR]: fuzz buttons from a savestate; writes OUTDIR/STATE_SEED.log (regs/callbacks) and .ppm
st=$1; seed=$2; n=${3:-40}; out=${4:-/private/tmp/fuzz}
mkdir -p "$out"; cd "$(dirname "$0")"
s="$out/${st}_$seed.txt"
printf "load $st.state\nrun 60\nfuzz $seed $n\nregs\nshot $out/${st}_$seed.ppm\n" > "$s"
./fp_harness ../../pokeemerald.gba "$s" 2>&1 | grep -a -o "REGS.*\|SHADOW.*\|CAMERA.*\|GMAIN.*\|FIELDCB.*" | python3 resolve.py > "$out/${st}_$seed.log"
