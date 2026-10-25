# usage: npcground_measure.py PREFIX  -> per distance: floor row at the NPC's cell centre vs the lowest sprite row (feet) / shadow rows, from the shots
# of npcground_shots.txt. Rows < 80 (sky: clouds animate between shots) are ignored.
import sys
def rd(fn):
    body=open(fn,'rb').read().split(b'\n',3)[3]
    return [[body[(y*240+x)*3:(y*240+x)*3+3] for x in range(240)] for y in range(160)]
def bbox(a,b):
    rows=[y for y in range(80,160) if any(a[y][x]!=b[y][x] for x in range(240))]
    cols=[x for x in range(240) if any(a[y][x]!=b[y][x] for y in range(80,160))]
    return (rows[0],rows[-1],cols[0],cols[-1]) if rows else None
P=sys.argv[1]
print("cells  z    floor_row  before(rows,cols)      feet-only(rows,cols)   after=feet+shadow(rows,cols)")
for d in range(1,6):
    z=max(d*256,320); fl=80+12800//z
    print(d,max(d*256,0),fl,bbox(rd(f"{P}_d{d}_before.ppm"),rd(f"{P}_d{d}_before_hide.ppm")),bbox(rd(f"{P}_d{d}_feet.ppm"),rd(f"{P}_d{d}_feet_hide.ppm")),bbox(rd(f"{P}_d{d}_after.ppm"),rd(f"{P}_d{d}_feet_hide.ppm")))
