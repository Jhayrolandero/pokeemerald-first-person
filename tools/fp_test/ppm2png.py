#!/usr/bin/env python3
# ppm2png.py in.ppm out.png [scale]
import sys, zlib, struct
d = open(sys.argv[1], 'rb').read()
sc = int(sys.argv[3]) if len(sys.argv) > 3 else 2
parts = d.split(b'\n', 3)
w, h = map(int, parts[1].split()); px = parts[3]
raw = bytearray()
for y in range(h):
    row = px[y*w*3:(y+1)*w*3]
    big = b''.join(row[x*3:x*3+3]*sc for x in range(w))
    for _ in range(sc): raw += b'\0' + big
def ch(t, b): c = struct.pack('>I', len(b)) + t + b; return c + struct.pack('>I', zlib.crc32(t + b))
open(sys.argv[2], 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w*sc, h*sc, 8, 2, 0, 0, 0)) + ch(b'IDAT', zlib.compress(bytes(raw))) + ch(b'IEND', b''))
