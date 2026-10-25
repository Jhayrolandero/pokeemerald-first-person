#!/bin/sh
# regress_ledge.sh: item 5. On Route 103 free movement pushes into a south-facing ledge; the real ledge jump must run
# (the player lands 2 tiles south in one go, never standing on the ledge tile). Needs route103.state + syms.txt.
cd "$(dirname "$0")"
./run.sh ledge_test.txt | grep -a "^PLAYER" | python3 -c "
import re, sys
ys = [int(re.search(r'tile=-?\d+,(-?\d+)', l).group(1)) for l in sys.stdin]
start = ys[0]
# the ledge is the tile directly south of the start tile (start+1): the player must never be there, and must reach start+2
ok = (start + 2) in ys and (start + 1) not in ys
print('ledge jump: %s (player rows %s, ledge row %d)' % ('ok' if ok else 'FAIL', ys, start + 1))
sys.exit(0 if ok else 1)
"
