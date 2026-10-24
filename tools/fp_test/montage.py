#!/usr/bin/env python3
# montage.py out.png cols in1.ppm in2.ppm ...   (contact sheet, 1x)
import sys, zlib, struct
def rd(p):
    d = open(p,'rb').read(); parts = d.split(b'\n',3)
    w,h = map(int, parts[1].split()); return w,h,parts[3]
cols = int(sys.argv[2]); ims = [rd(p) for p in sys.argv[3:]]
w,h = ims[0][0], ims[0][1]; rows = (len(ims)+cols-1)//cols
raw = bytearray()
for r in range(rows):
    for y in range(h):
        raw += b'\0'
        for c in range(cols):
            i = r*cols+c
            raw += ims[i][2][y*w*3:(y+1)*w*3] if i < len(ims) else bytes(w*3)
def ch(t,b): c=struct.pack('>I',len(b))+t+b; return c+struct.pack('>I',zlib.crc32(t+b))
open(sys.argv[1],'wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',w*cols,h*rows,8,2,0,0,0))+ch(b'IDAT',zlib.compress(bytes(raw)))+ch(b'IEND',b''))
