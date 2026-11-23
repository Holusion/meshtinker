#!/usr/bin/env python3
"""Simulates a GPU-memory budget on tools/experiments/optimize/survey.sh's slow output, for
docs/experiments/optimize-survey.md#budgets:

    python3 tools/experiments/optimize/budget.py OUT_DIR DENSITY MAX_FACES BUDGET_MB

Faces: simplify's result at the survey's error, capped at MAX_FACES. When the model is over BUDGET_MB, and has
at least a megatexel of color texture, faces go down first to the count that gives DENSITY used color texels
a face (never below 100,000; 0 disables this). Then one maximum side for the whole model goes down by
256 until geometry and textures fit: every image above it is scaled to it, so images of the same size (a
material's color and normal maps) stay alike, as with ktx's --max-size. Geometry on the GPU: 16 bytes a quantized vertex (position, normal, UV), 12 bytes a face (32-bit
indices). Textures as summary.py estimates them, with mipmaps.
"""
import json,glob,os,sys
d=sys.argv.pop(1)
MIPS=4/3; MB=2**20
D=float(sys.argv[1]); FMAX=float(sys.argv[2]); BUDGET=float(sys.argv[3])*MB; FLOOR=100_000
def per(e): return 1.0 if e['codec']=='UASTC' or e.get('alpha') else 0.5
out=[]
for f in sorted(glob.glob(d+'/*.optimize.json')):
    name=os.path.basename(f)[:-14]
    r=json.load(open(f)); st={s['command']:s.get('report') or {} for s in r.get('steps',[])}
    ins=json.load(open(f.replace('.optimize.','.inspect.')))
    Fin=ins.get('geometry',{}).get('faces',0)
    s=st.get('simplify',{}); Ferr=s.get('output',{}).get('faces',Fin); Verr=s.get('output',{}).get('vertices',0)
    vpf=Verr/Ferr if Ferr else 0.6
    k=st.get('ktx',{}).get('images',[])
    occ=ins.get('texels',{}).get('occupancy') or 0
    col=sum(e['outputWidth']*e['outputHeight'] for e in k if e['transferFunction']=='sRGB')
    F=min(Ferr,FMAX)
    why=''
    over0=(min(Ferr,FMAX)*vpf*16+min(Ferr,FMAX)*12+sum(e["outputWidth"]*e["outputHeight"]*per(e)*MIPS for e in k))>BUDGET
    if col>=1_000_000 and D>0 and over0:
        Fd=max(FLOOR,col*occ/D)
        if Fd<F: F=Fd; why='density'
    if F<min(Ferr,FMAX) and not why: why='cap'
    if Ferr>FMAX and F==FMAX: why='cap'
    geo=F*vpf*16+F*12
    imgs=[[e['outputWidth'],e['outputHeight'],per(e)] for e in k if max(e['outputWidth'],e['outputHeight'])>512]
    small=sum(e['outputWidth']*e['outputHeight']*per(e)*MIPS for e in k if max(e['outputWidth'],e['outputHeight'])<=512)
    def tex(): return small+sum(w*h*p*MIPS for w,h,p in imgs)
    t0=tex()
    full=[im[:] for im in imgs]
    cap=max([max(w,h) for w,h,_ in full],default=0)
    while tex()>BUDGET-geo and cap>512:
        cap-=256
        imgs=[[w*min(1,cap/max(w,h))//1,h*min(1,cap/max(w,h))//1,p] for w,h,p in full]
    side=int(max([max(w,h) for w,h,_ in imgs],default=0))
    out.append((name[:36],Fin,Ferr,int(F),why,round(geo/MB),round(t0/MB),round(tex()/MB),side,round((geo+tex())/MB)))
changed=[o for o in out if o[3]<o[2] or o[7]<o[6]]
print(f"D={D} Fmax={FMAX:.0f} budget={BUDGET/MB:.0f}MB: {len(changed)} of {len(out)} changed; over budget after: {sum(o[9]>BUDGET/MB for o in out)}")
for o in changed: print(f"  {o[0]:36} faces {o[2]:>8}->{o[3]:>8} {o[4]:7} geoGPU {o[5]:3} tex {o[6]:4}->{o[7]:3}MB maxside {o[8]:5} total {o[9]}")
