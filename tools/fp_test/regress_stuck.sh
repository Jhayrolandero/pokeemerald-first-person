#!/bin/sh
# regress_stuck.sh: regression for "stuck between two frames" (hang in CB2_ReturnToFieldLocal).
# Replays button-mash seeds that used to hang (a text printer node alive across the heap wipe done by CB2_ReturnToField):
#   littleroot seeds 48 57 138 185 (first-person leaves the view while a script's text box starts printing),
#   littleroot seed 79 (Bag closed while a text printer exists - an engine bug, not first-person specific).
# Needs littleroot.state from make_state.txt for the CURRENT build and syms.txt (arm-none-eabi-nm -n ../../pokeemerald.elf > syms.txt).
# Pass: after the mashing and 600 idle frames every run must be in a normal screen (CB2_Overworld, vblank callback set) or a menu.
cd "$(dirname "$0")"
fail=0
for seed in 48 57 79 138 185; do
    ./fuzz_one.sh littleroot $seed 40 /private/tmp/fp_regress
    log=/private/tmp/fp_regress/littleroot_$seed.log
    if grep -aq "cb2=[0-9a-f]*(CB2_ReturnToFieldLocal)\|vblank=00000000" "$log"; then echo "seed $seed: STUCK"; grep -a GMAIN "$log"; fail=1; else echo "seed $seed: ok"; fi
done
exit $fail
