#!/usr/bin/env python3
# fuzz_report.py [DIR]: classify fuzz results; anomalies = stuck callbacks or scroll registers that do not match the camera
import re, sys, glob, os
from collections import Counter
d = sys.argv[1] if len(sys.argv) > 1 else '/private/tmp/fuzz'
legit = {'CB2_Overworld','CB2_BagMenuRun','MainCB2','CB2_TrainerCard','CB2_PartyMenu','CB2_Pokedex','CB2_PokeNav','CB2_MainMenu'}
c = Counter(); bad = []
for f in sorted(glob.glob(d + '/*.log')):
    b = open(f).read(); name = os.path.basename(f)[:-4]
    disp = re.search(r'REGS\s+DISPCNT=(\w+)', b); cb2 = re.search(r'cb2=\w+\((\w+)\)', b); vb = re.search(r'vblank=(\w+)', b)
    sh = re.search(r'SHADOW.*HOFS=(\w+)/(\w+)/(\w+)/(\w+) VOFS=(\w+)/(\w+)/(\w+)/(\w+)', b)
    cam = re.search(r'CAMERA off=(-?\d+),(-?\d+) tile=(-?\d+),(-?\d+) pan=(-?\d+),(-?\d+)', b)
    if not (disp and cb2 and sh and cam): c['incomplete'] += 1; bad.append((name, 'incomplete')); continue
    cb = cb2.group(1); dc = int(disp.group(1), 16); c[cb] += 1
    if cb == 'CB2_Overworld' and (dc & 7) == 0:
        xo, yo, _, _, hp, vp = map(int, cam.groups())
        ex, ey = (xo + hp) & 0xffff, (yo + vp + 8) & 0xffff
        h = [int(sh.group(i), 16) for i in (2, 3, 4)]; v = [int(sh.group(i), 16) for i in (6, 7, 8)]
        if any(x != ex for x in h) or any(y != ey for y in v): bad.append((name, 'SCROLL-MISMATCH', h, v, (ex, ey)))
    if cb not in legit or (vb and int(vb.group(1), 16) == 0): bad.append((name, 'STUCK?', cb, 'vblank=' + vb.group(1)))
    sd = re.search(r'SHADOW DISPCNT=(\w+)', b)
    if sd and sd.group(1) != disp.group(1) and cb == 'CB2_Overworld': bad.append((name, 'DISPCNT-HW!=SHADOW', disp.group(1), sd.group(1)))
print(sum(c.values()), 'runs', dict(c))
for x in bad: print(x)
