#!/bin/sh
# regress_billboard.sh: proves that NPC billboards pick front/back/side frames from the angle between camera and facing (item 1).
# Stands 3 tiles from a Littleroot NPC, looks south and then north, forces the NPC to face S/N/W/E in turn and reads the billboard log
# (gFpBillLog = the last 8 draws: graphicsId, facing, sprite animNum, chosen animNum, image, hflip, z, u). Needs littleroot.state + syms.txt.
cd "$(dirname "$0")"
A=$(grep " gFpBillLog$" syms.txt | cut -d' ' -f1)
T=/private/tmp/claude-501
fail=0
for heading in south north; do
    if [ $heading = south ]; then X=14; Y=15; TURN=""; FACE=1; else X=14; Y=21; TURN=""; FACE=2; fi
    {
        printf "load littleroot.state\nrun 30\nteleport $X $Y $FACE\nrun 220\npress L 4\nwaitmode 4 400\nrun 20\n$TURN\nrun 30\n"
        for dir in 1 2 3 4; do
            printf "pokeface 3 $dir\nrun 8\ndump $A 256 $T/bill_${heading}_$dir.bin\n"
        done
    } > $T/bill_$heading.txt
    ./fp_harness ../../pokeemerald.gba $T/bill_$heading.txt >/dev/null 2>&1
    python3 - "$heading" <<'PY' || fail=1
import struct, sys
heading = sys.argv[1]
# facing: 1 S, 2 N, 3 W, 4 E ; frame group: 0 S (front), 1 N (back), 2 W, 3 E
# camera looking south: NPC facing S -> back, N -> front, W is the camera's right -> east frame, E -> west frame; north: mirrored
exp = {'south': {1: 1, 2: 0, 3: 3, 4: 2}, 'north': {1: 0, 2: 1, 3: 2, 4: 3}}[heading]
names = {0: 'front', 1: 'back', 2: 'left-facing', 3: 'right-facing'}
ok = True
for face in (1, 2, 3, 4):
    d = struct.unpack('<64I', open('/private/tmp/claude-501/bill_%s_%d.bin' % (heading, face), 'rb').read())
    hits = [d[i*8:i*8+8] for i in range(8) if d[i*8] == 9 and d[i*8+1] == face]
    if not hits:
        print('  facing %d: NPC not drawn' % face); ok = False; continue
    chosen = hits[-1][3] & 3
    if chosen != exp[face]:
        print('  facing %d -> %s, expected %s' % (face, names[chosen], names[exp[face]])); ok = False
print('billboard frame (camera %s): %s' % (heading, 'ok (S/N/W/E facings all pick the expected frame)' if ok else 'FAIL'))
sys.exit(0 if ok else 1)
PY
done
exit $fail
