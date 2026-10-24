#!/usr/bin/env python3
# resolve.py: annotate hex addresses in harness output with function names from syms.txt (arm-none-eabi-nm -n)
import sys, bisect, re
syms=[]
for l in open(__file__.rsplit('/',1)[0]+'/syms.txt'):
    p=l.split()
    if len(p)==3 and p[1] in 'tTwW': syms.append((int(p[0],16),p[2]))
addrs=[a for a,_ in syms]
def name(v):
    v&=~1
    i=bisect.bisect_right(addrs,v)-1
    return syms[i][1] if i>=0 and v>=0x08000000 or (i>=0 and 0x03000000<=v<0x03008000) else '?'
for l in sys.stdin:
    if l.startswith(("GMAIN","FIELDCB")):
        l=re.sub(r'=([0-9a-f]{8})',lambda m:'=%s(%s)'%(m.group(1),name(int(m.group(1),16))),l)
    sys.stdout.write(l)
