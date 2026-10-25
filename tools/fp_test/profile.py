#!/usr/bin/env python3
# profile.py: aggregate "PROF addr" lines (stdin) by function using syms.txt
import sys, bisect, collections
syms=[]
for l in open(__file__.rsplit('/',1)[0]+'/syms.txt'):
    p=l.split()
    if len(p)==3 and p[1] in 'tTwW':
        try: syms.append((int(p[0],16),p[2]))
        except: pass
syms.sort(); addrs=[a for a,_ in syms]
c=collections.Counter(); tot=0
for l in sys.stdin:
    if 'PROF' in l:
        v=int(l.split('PROF')[1].split()[0],16)&~1
        i=bisect.bisect_right(addrs,v)-1
        c[syms[i][1] if i>=0 and v>=0x08000000 or 0x03000000<=v<0x03008000 else '?']+=1; tot+=1
for k,v in c.most_common(12): print('%5.1f%% %s'%(100.0*v/tot,k))
