#!/bin/sh
# rebuild.sh: build the ROM, regenerate syms.txt and all savestates (in order)
export PATH="$HOME/pokemon/toolchain/arm-gnu-toolchain-15.3.rel2-darwin-arm64-arm-none-eabi/bin:$PATH"
cd "$(dirname "$0")/../.." || exit 1
make -j8 2>&1 | grep -E "error|warning: .*first_person|Error" 
arm-none-eabi-nm -n pokeemerald.elf > tools/fp_test/syms.txt
cd tools/fp_test || exit 1
for s in make_state make_follower_state make_route_state make_route102_state make_route103_state; do ./fp_harness ../../pokeemerald.gba $s.txt >/dev/null 2>&1; done
