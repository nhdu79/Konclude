import sys, re, subprocess, collections
def parse(p):
    sites=[]
    for l in open(p):
        if l.startswith('SITE'):
            sites.append([int(re.search(r'live=(\d+)',l).group(1)),[]])
        else:
            sites[-1][1].append(l.strip())
    d=collections.defaultdict(int)
    for live,fr in sites: d[tuple(fr)]+=live
    return d
a=parse(sys.argv[1]); b=parse(sys.argv[2])
deltas=sorted(((b.get(k,0)-a.get(k,0)),k) for k in set(a)|set(b))[::-1]
def sym(frame):
    lib,off=frame.rsplit('+',1)
    if 'libKonclude' not in lib: return lib.split('/')[-1]
    out=subprocess.run(['addr2line','-f','-C','-e','ReleaseEmbedded/libKonclude.so.1.0.0',off],capture_output=True,text=True).stdout.split('\n')
    return out[0][:110]
for dlt,k in deltas[:10]:
    print(f"delta {dlt/1024:.0f} kB  (15 states: {a.get(k,0)/1024:.0f} kB)")
    shown=0
    for fr in k:
        s=sym(fr)
        if s.startswith('lib') and shown==0: continue
        print('    ',s); shown+=1
        if shown>=7: break
