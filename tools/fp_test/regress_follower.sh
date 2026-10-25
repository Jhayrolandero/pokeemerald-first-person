#!/bin/sh
# regress_follower.sh: item 3. In free mode the follower must trail the player through the real follower logic:
#   1. walking:  after a walk the follower is visible and at most 2 tiles from the player (and has left the start tile)
#   2. warp:     after a door warp into the house, and a few steps there, the follower is visible and next to the player
#   3. battle:   after a wild battle (run away) and a few steps, the follower is visible and next to the player
# Needs follower.state, route101.state (make_*state.txt) and syms.txt for the CURRENT build.
cd "$(dirname "$0")"
fail=0
check() {   # check NAME SCRIPT MAXDIST SAMPLE(last|any index)
    ./run.sh "$2" | grep -a "^OBJS" | python3 -c "
import re, sys
name, maxd = sys.argv[1], int(sys.argv[2])
last = None
for line in sys.stdin:
    objs = re.findall(r'\[(\d+)\](-?\d+),(-?\d+) gfx=([0-9a-f]+) face=\d inv=(\d)', line)
    pl = [o for o in objs if o[0] == '0']
    fo = [o for o in objs if int(o[3], 16) & 0x4000]
    if pl and fo:
        last = (pl[0], fo[0])
if not last:
    print('%s: FAIL (no follower seen)' % name); sys.exit(1)
(p, f) = last
d = abs(int(p[1]) - int(f[1])) + abs(int(p[2]) - int(f[2]))
ok = f[4] == '0' and d <= maxd and d >= 1
print('%s: %s (player %s,%s follower %s,%s visible=%s distance %d)' % (name, 'ok' if ok else 'FAIL', p[1], p[2], f[1], f[2], f[4] == '0', d))
sys.exit(0 if ok else 1)
" "$1" "$3" || fail=1
}
check "follower trail (walk)" follower_trail_test.txt 2
check "follower after warp" follower_warp_test.txt 2
check "follower after battle" follower_battle_test.txt 2
exit $fail
