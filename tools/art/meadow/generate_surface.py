"""Author the bounded meadow surface and static water from the same depression records.
Run from repository root. No external packages, no edits to other environment meshes.
"""
import json, math, sys, struct
from functools import lru_cache
MESHES_ONLY = "--meshes-only" in sys.argv
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mesh_indexing import index_mesh
ROOT=Path(__file__).resolve().parents[3]
OUT=ROOT/'ContentSource/Models/Environment'
records=[]
def add(kind,x,z,rx,rz,depth,yaw=0,fill=0):
    records.append(dict(kind=kind,x=x,z=z,rx=rx,rz=rz,depth=depth,yaw=yaw,fill=fill))
def cart(z): return math.sin(z*.085)*6.5
for z in (-8,-4,0,4,8):
    for side in (-1,1): add(0,cart(z)+side*.82,z,.115,1.55,.045,math.atan(.5525*math.cos(z*.085)),.48 if z in (-4,4) else 0)
for i in range(9):
    z=-6+i*.63
    add(1,cart(z)-1.85+(-.12 if i%2 else .12),z,.065,.145,.027,math.atan(.5525*math.cos(z*.085)) + (-.06 if i%2 else .06), .45 if i in (2,5,6) else 0)
for i in range(8):
    z=2+i*.55
    add(2,6.9-i*.16+(-.085 if i%2 else .085),z,.048,.075,.025,-.26,.5 if i in (2,3,5) else 0)
add(3,4.0,5.8,.72,1.05,.065,.3,.4)
add(3,-2.6,-7.0,.55,.8,.045,-.3,0)
# Supply-cart spur: paired wheel depressions turn out from the main cart route.
for x in (6.5,8.5):
    for side in (-1,1): add(0,x,3+side*.82,.115,.92,.032,math.pi/2,.35 if x==6.5 else 0)
source=dict(version=1,bounds=[-12,12,-12,12],seed=41703,animal_assumption='roe deer, cloven hoof; static traces only',records=records)
if (OUT/'meadow_surface.json').exists():
    source=json.loads((OUT/'meadow_surface.json').read_text());records=source['records']
else:
    if not MESHES_ONLY: (OUT/'meadow_surface.json').write_text(json.dumps(source,indent=2)+'\n')

for i,r in enumerate(records):
    r.setdefault('side',-1 if i%2 else 1)
    r['c']=math.cos(r['yaw']);r['s']=math.sin(r['yaw'])

relief=source['relief']
def base(x,z):
    # A squared envelope joins the slab at zero height and zero slope.
    f=max(0,1-(x/12)**2)**2*max(0,1-(z/12)**2)**2
    return (relief['broad_amplitude']*math.sin(x*.22+.5)*math.cos(z*.18-.35)+relief['meso_amplitude']*math.sin(x*.48+z*.31-.6))*f

def shape(r,x,z):
    dx=x-r['x']; dz=z-r['z']
    if abs(dx)>2*(r['rx']+r['rz']) or abs(dz)>2*(r['rx']+r['rz']):return 100
    c=math.cos(r['yaw']);s=math.sin(r['yaw'])
    u=(c*dx-s*dz)/r['rx'];v=(s*dx+c*dz)/r['rz']
    if r['kind']==0: u-=.24*r['side']*max(0,1-v*v)*(v+.35)
    if r['kind']==1:
        u-=r['side']*.16*max(0,1-4*v*v)
        u/= .72+.28*max(0,min(1,(v+1)*.5))
    if r['kind']==2: u=(abs(u)-.48)/.43
    return u*u+v*v

def depth(r,x,z):
    q=shape(r,x,z)
    return r['depth']*max(0,1-q)**2*(1+.22*max(-1,min(1,(z-r['z'])/r['rz']))) if q<1 else 0

def ridge(r,q):
    # Bank and cut integrals balance; the original warped support avoids more fine cells.
    return r['depth']*relief['rut_bank_scale']*max(0,1-((q-1.05)/.65)**2)**2 if r['kind']==0 else 0

def height(x,z): return base(x,z)+max([0]+[ridge(r,shape(r,x,z)) for r in records])-max([0]+[depth(r,x,z) for r in records])
height=lru_cache(maxsize=262144)(height)
def basin_point(r,u,v):
    if r['kind']==0:u+=.24*r['side']*max(0,1-v*v)*(v+.35)
    if r['kind']==1:u=u*(.72+.28*max(0,min(1,(v+1)*.5)))+r['side']*.16*max(0,1-4*v*v)
    return r['x']+r['c']*r['rx']*u+r['s']*r['rz']*v,r['z']-r['s']*r['rx']*u+r['c']*r['rz']*v

# Connected cuts share a spill height and a horizontal water plane.
rims=[];interiors=[]
for r in records:
    rim=[];inside=[]
    for i in range(128):
        angle=(i+.5)*2*math.pi/128
        for side in ((-1,1) if r['kind']==2 else (0,)):
            for j in range(17):
                radius=j/16;u=radius*math.cos(angle);v=radius*math.sin(angle)
                if side:u=side*(.48+.43*u)
                point=basin_point(r,u,v);inside.append(point)
                if j==16:rim.append(point)
    rims.append(rim);interiors.append(inside)
parents=list(range(len(records)))
def group(i):
    while parents[i]!=i:i=parents[i]
    return i
for i,r in enumerate(records):
    for j in range(i):
        other=records[j]
        if any(shape(other,*p)<1 for p in rims[i]) or any(shape(r,*p)<1 for p in rims[j]):parents[group(i)]=group(j)
water_groups={}
for i,r in enumerate(records):water_groups.setdefault(group(i),[]).append(i)
for key,ids in water_groups.items():
    fill=max(records[i]['fill'] for i in ids)
    for i in ids:records[i]['level']=-100;records[i]['water_group']=key
    if not fill:continue
    rim=[height(*p) for i in ids for p in rims[i] if not any(shape(records[j],*p)<1-1e-6 for j in ids if i!=j)]
    low=min(height(*p) for i in ids for p in interiors[i]);spill=min(rim)
    assert spill>low+.001, ('no closed connected water basin on slope',ids,low,spill)
    for i in ids:
        records[i]['level']=low+(spill-low)*fill
        if not records[i]['fill']:records[i]['fill']=fill

water_members={}
for i,r in enumerate(records):
 if r['level']>-10:water_members.setdefault(r['level'],[]).append(i)
if not MESHES_ONLY: (OUT/'meadow_surface.json').write_text(json.dumps(source,indent=2)+'\n')

# A single source table feeds C++ contact height and shader wetness/transport.
rows=[]
for r in records: rows.append(', '.join(f'{r[k]:.9f}' for k in ('x','z','rx','rz','depth','c','s','level','kind','side')))
cpp='''#pragma once
// Generated by Tools/art/meadow/generate_surface.py from the authored meadow records.
#include <hs/core/math.hpp>
#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
namespace hs {
struct MeadowDepression { float x,z,rx,rz,depth,c,s,level,kind,side; };
inline constexpr std::array<MeadowDepression, COUNT> kMeadowDepressions{{
ROWS
}};
inline bool MeadowContains(Float2 p) noexcept { return std::abs(p.x)<=12 && std::abs(p.y)<=12; }
inline float MeadowBaseHeight(Float2 p) noexcept {
 float x=std::max(0.0f,1-p.x*p.x/144),z=std::max(0.0f,1-p.y*p.y/144);
 return (BROADf*std::sin(p.x*.22f+.5f)*std::cos(p.y*.18f-.35f)+MESOf*std::sin(p.x*.48f+p.y*.31f-.6f))*x*x*z*z;
}
inline float MeadowDepressionQ(const MeadowDepression& r, Float2 p) noexcept {
 const float dx=p.x-r.x,dz=p.y-r.z,c=r.c,s=r.s;
 if(std::abs(dx)>2*(r.rx+r.rz)||std::abs(dz)>2*(r.rx+r.rz))return 100;
 float u=(c*dx-s*dz)/r.rx,v=(s*dx+c*dz)/r.rz;
 if(r.kind==0)u-=.24f*r.side*std::max(0.0f,1-v*v)*(v+.35f);
 if(r.kind==1){u-=r.side*.16f*std::max(0.0f,1-4*v*v);u/=.72f+.28f*std::clamp((v+1)*.5f,0.0f,1.0f);}
 if(r.kind==2)u=(std::abs(u)-.48f)/.43f;
 return u*u+v*v;
}
inline float MeadowGroundHeight(Float2 p) noexcept {
 if(!MeadowContains(p))return 0;
 float d=0,b=0; for(const auto&r:kMeadowDepressions){float q=MeadowDepressionQ(r,p);if(r.kind==0){float t=std::max(0.0f,1-((q-1.05f)/.65f)*((q-1.05f)/.65f));b=std::max(b,r.depth*BANKf*t*t);}if(q<1)d=std::max(d,r.depth*(1-q)*(1-q)*(1+.22f*std::clamp((p.y-r.z)/r.rz,-1.0f,1.0f)));}
 return MeadowBaseHeight(p)+b-d;
}
inline float MeadowWetness(Float2 p) noexcept {
 float w=0; for(const auto&r:kMeadowDepressions)if(r.level>-10)w=std::max(w,std::clamp(1-MeadowDepressionQ(r,p)/2.5f,0.0f,1.0f)); return w;
}
inline float MeadowWaterLevel(Float2 p) noexcept {
 float level=std::numeric_limits<float>::quiet_NaN(); const float h=MeadowGroundHeight(p);
 for(const auto&r:kMeadowDepressions)if(r.level>-10 && MeadowDepressionQ(r,p)<1 && h<r.level)level=r.level;
 return level;
}
inline float MeadowCartCenter(float z) noexcept { return std::sin(z*.085f)*6.5f; }
inline float MeadowRoadCoverage(Float2 p,float edge=0) noexcept {
 const auto smooth=[](float a,float b,float x){float t=std::clamp((x-a)/(b-a),0.0f,1.0f);return t*t*(3-2*t);};
 const float main=1-smooth(1.15f,1.65f,std::abs(p.x-MeadowCartCenter(p.y))+edge);
 const float spur=(1-smooth(1.15f,1.65f,std::abs(p.y-3)+edge))*smooth(1.4f+edge,2.2f+edge,p.x)*(1-smooth(11-edge,11.7f-edge,p.x));
 return std::max(main,spur);
}
inline float MeadowWalkCenter(float z) noexcept { return MeadowCartCenter(z)-1.85f; }
inline float MeadowCartWear(Float2 p) noexcept {if(!MeadowContains(p))return 0;float a=std::abs(std::abs(p.x-MeadowCartCenter(p.y))-.82f);float spur=(p.x>4.5f&&p.x<11.5f)?std::abs(std::abs(p.y-3)-.82f):100;return std::clamp(1-std::min(a,spur)/.35f,0.0f,1.0f);}
inline float MeadowWalkWear(Float2 p) noexcept {if(!MeadowContains(p))return 0;return std::clamp(1-std::abs(p.x-MeadowWalkCenter(p.y))/.7f,0.0f,1.0f);}
inline float MeadowPressed(Float2 p) noexcept {return std::max(MeadowCartWear(p),MeadowWalkWear(p));}
}
'''.replace('COUNT',str(len(rows))).replace('ROWS','\n'.join(' {'+', '.join(x+'f' for x in row.split(', '))+'},' for row in rows))
# The project Float2 contract is defined here, independent of renderer ownership.
cpp=cpp.replace('hs/core/math.hpp','hs/core/render_snapshot.hpp')
for key,value in [('BROAD',relief['broad_amplitude']),('MESO',relief['meso_amplitude']),('BANK',relief['rut_bank_scale'])]:cpp=cpp.replace(key,f'{value:.12f}')
if not MESHES_ONLY: (ROOT/'Source/Presentation/Public/hs/presentation/meadow_surface.hpp').write_text(cpp)
hlsl='''#ifndef HS_MEADOW_SURFACE
#define HS_MEADOW_SURFACE
struct MeadowDepression { float x,z,rx,rz,depth,c,s,level,kind,side; };
static const MeadowDepression MeadowRecords[COUNT]={
ROWS
};
float MeadowQ(MeadowDepression r,float2 p){float c=r.c,s=r.s;float2 d=p-float2(r.x,r.z);if(any(abs(d)>2*(r.rx+r.rz)))return 100;float u=(c*d.x-s*d.y)/r.rx,v=(s*d.x+c*d.y)/r.rz;if(r.kind==0)u-=.24*r.side*max(0,1-v*v)*(v+.35);if(r.kind==1){u-=r.side*.16*max(0,1-4*v*v);u/=.72+.28*saturate((v+1)*.5);}if(r.kind==2)u=(abs(u)-.48)/.43;return u*u+v*v;}
float MeadowBaseHeight(float2 p){float2 f=max(0,1-p*p/144);return (BROAD*sin(p.x*.22+.5)*cos(p.y*.18-.35)+MESO*sin(p.x*.48+p.y*.31-.6))*f.x*f.x*f.y*f.y;}
float MeadowGroundHeight(float2 p){if(any(abs(p)>12))return 0;float d=0,b=0;[loop]for(uint i=0;i<COUNT;++i){MeadowDepression r=MeadowRecords[i];float q=MeadowQ(r,p);if(r.kind==0){float t=max(0,1-((q-1.05)/.65)*((q-1.05)/.65));b=max(b,r.depth*BANK*t*t);}if(q<1)d=max(d,r.depth*(1-q)*(1-q)*(1+.22*clamp((p.y-r.z)/r.rz,-1,1)));}return MeadowBaseHeight(p)+b-d;}
float MeadowWetness(float2 p){if(any(abs(p)>12))return 0;float w=0;[loop]for(uint i=0;i<COUNT;++i)if(MeadowRecords[i].level>-10)w=max(w,saturate(1-MeadowQ(MeadowRecords[i],p)/2.5));return w;}
float MeadowRoadCoverage(float2 p,float edge){float main=1-smoothstep(1.15,1.65,abs(p.x-sin(p.y*.085)*6.5)+edge);float spur=(1-smoothstep(1.15,1.65,abs(p.y-3)+edge))*smoothstep(1.4+edge,2.2+edge,p.x)*(1-smoothstep(11-edge,11.7-edge,p.x));return max(main,spur);}
float MeadowCartWear(float2 p){if(any(abs(p)>12))return 0;float a=abs(abs(p.x-sin(p.y*.085)*6.5)-.82);float spur=p.x>4.5&&p.x<11.5?abs(abs(p.y-3)-.82):100;return saturate(1-min(a,spur)/.35);}
float MeadowWalkWear(float2 p){if(any(abs(p)>12))return 0;return saturate(1-abs(p.x-(sin(p.y*.085)*6.5-1.85))/.7);}
float MeadowPressed(float2 p){return max(MeadowCartWear(p),MeadowWalkWear(p));}
#endif
'''.replace('COUNT',str(len(rows))).replace('ROWS','\n'.join(' {'+row+'},' for row in rows))
for key,value in [('BROAD',relief['broad_amplitude']),('MESO',relief['meso_amplitude']),('BANK',relief['rut_bank_scale'])]:hlsl=hlsl.replace(key,f'{value:.12f}')
hlsl=hlsl.replace('#endif','float MeadowSoilDryness(float2 p,float wetness){return MeadowRoadCoverage(p,0)*(1-saturate(wetness*1.4))*lerp(.55,1,saturate(.5+MeadowBaseHeight(p)*2));}\n#endif')

dry_rows=[]
for patch in source.get('dry_grass_patches',[]):
    x,z=patch['center'];rx,rz=patch['radius'];strength=patch['strength']
    assert rx>0 and rz>0 and 0<=strength<=1
    for ix in range(21):
        for iz in range(21):
            px=x+(ix/10-1)*rx;pz=z+(iz/10-1)*rz
            if ((px-x)/rx)**2+((pz-z)/rz)**2>1:continue
            assert max(abs(px),abs(pz))<12
            assert abs(px-cart(pz)+1.85)>.7 and abs(abs(px-cart(pz))-.82)>.35
            assert not(4.5<px<11.5 and abs(abs(pz-3)-.82)<.35)
            assert not any(r['fill'] and shape(r,px,pz)<2.5 for r in records)
    dry_rows.append(f'    float2 q{len(dry_rows)}=(p-float2({x:.8f},{z:.8f}))/float2({rx:.8f},{rz:.8f}); mask=max(mask,{strength:.8f}*(1-smoothstep(.1,1.0,dot(q{len(dry_rows)},q{len(dry_rows)}))));')
dry_hlsl='float3 MeadowDryGrassGain(float2 p){\n    if(any(abs(p)>12))return float3(1,1,1);\n    float mask=0;\n'+'\n'.join(dry_rows)+'\n    return lerp(float3(1,1,1),float3(1.65,.95,2.5),mask);\n}\n'
hlsl=hlsl.replace('#endif',dry_hlsl+'#endif')
if not MESHES_ONLY: (ROOT/'Content/Shaders/meadow_surface.hlsli').write_text(hlsl)


def road_coverage(x,z,edge=0):
 def smooth(a,b,value):
  t=max(0,min(1,(value-a)/(b-a)));return t*t*(3-2*t)
 main=1-smooth(1.15,1.65,abs(x-cart(z))+edge)
 spur=(1-smooth(1.15,1.65,abs(z-3)+edge))*smooth(1.4+edge,2.2+edge,x)*(1-smooth(11-edge,11.7-edge,x))
 return max(main,spur)

def wetness(x,z):
 if max(abs(x),abs(z))>12:return 0
 return max([0]+[max(0,min(1,1-shape(r,x,z)/2.5)) for r in records if r['level']>-10])

def soil_dryness(x,z,wet):
 return road_coverage(x,z)*(1-max(0,min(1,wet*1.4)))*(.55+.45*max(0,min(1,.5+base(x,z)*2)))

# Adaptive cells share edge samples with their neighbours; no T junction holes.
N=48; S=.5; fine={}
for iz in range(N):
 for ix in range(N):
  x=-12+(ix+.5)*S;z=-12+(iz+.5)*S
  near=[r for r in records if abs(x-r['x'])<(abs(r['c'])*r['rx']+abs(r['s'])*r['rz'])*(1.6 if r['kind']==0 else 1)+.28 and abs(z-r['z'])<(abs(r['s'])*r['rx']+abs(r['c'])*r['rz'])*(1.6 if r['kind']==0 else 1)+.28]
  if near:fine[(ix,iz)]=max(32 if r['kind']==2 else 16 if r['kind']==1 else 8 if r['kind']==0 else 4 for r in near)
ground=[];water=[];water_count=[0]*len(records)
@lru_cache(maxsize=262144)
def vertex(x,z,y=None,normal=None):
 ground_vertex=y is None
 y=height(x,z) if ground_vertex else y
 if normal is None:
  e=.002;dx=(height(x+e,z)-height(x-e,z))/(2*e);dz=(height(x,z+e)-height(x,z-e))/(2*e);l=math.sqrt(dx*dx+1+dz*dz);normal=(-dx/l,1/l,-dz/l)
 # Tangent perpendicular to normal, standard UV orientation.
 tx=normal[1];ty=-normal[0];l=math.sqrt(tx*tx+ty*ty)
 wet=wetness(x,z) if ground_vertex else 0
 return (x,y,z,*normal,wet,soil_dryness(x,z,wet) if ground_vertex else 0,tx/l,ty/l,0,-1,0)
def water_triangle_cooks(pts):
 # Match serialized decimal positions and the cooker's float32 cross-product gate.
 def f(v):return struct.unpack('f',struct.pack('f',v))[0]
 a,b,c=[(f(round(p[0],7)),f(round(p[1],7))) for p in pts]
 ux=f(b[0]-a[0]);uz=f(b[1]-a[1]);vx=f(c[0]-a[0]);vz=f(c[1]-a[1])
 cross=f(f(uz*vx)-f(ux*vz))
 return f(cross*cross)>f(1e-16)

def triangle(a,b,c):
 vs=[vertex(*p) for p in (a,b,c)]; ground.extend(vs)
 # Clip each selected basin against its common horizontal level. Ground and water agree at shoreline.
 cx=sum(p[0] for p in (a,b,c))/3;cz=sum(p[1] for p in (a,b,c))/3
 used_levels=set()
 for ri,r in enumerate(records):
  if r['level'] in used_levels or r['level']<-10 or min(shape(r,*p) for p in (a,b,c))>1.3:continue
  used_levels.add(r['level'])
  level=r['level'];poly=[(v[0],v[2],v[1]) for v in vs];clipped=[]
  for j,p in enumerate(poly):
   q=poly[(j+1)%3]; pin=p[2]<level;qin=q[2]<level
   if pin:clipped.append(p)
   if pin!=qin:
    t=(level-p[2])/(q[2]-p[2]);clipped.append((p[0]+t*(q[0]-p[0]),p[1]+t*(q[1]-p[1]),level))
  for j in range(1,len(clipped)-1):
   pts=[clipped[0],clipped[j],clipped[j+1]]
   area=abs((pts[1][0]-pts[0][0])*(pts[2][1]-pts[0][1])-(pts[2][0]-pts[0][0])*(pts[1][1]-pts[0][1]))
   if area<1e-10 or not water_triangle_cooks(pts):continue
   center=(sum(p[0] for p in pts)/3,sum(p[1] for p in pts)/3)
   owner=min(water_members[level],key=lambda i:shape(records[i],*center))
   for p in pts:
    v=list(vertex(p[0],p[1],level,(0,1,0)));v[6]=max(0,level-p[2]);v[7]=owner+100;v[12]=0;water.append(v)
   water_count[owner]+=1
for iz in range(N):
 for ix in range(N):
  x=-12+ix*S;z=-12+iz*S;n=fine.get((ix,iz),1)
  for j in range(n):
   for i in range(n):
    x0=x+i*S/n;z0=z+j*S/n;step=S/n;ring=[]
    for start,end,neighbor,border in [((x0,z0),(x0,z0+step),(ix-1,iz),i==0),((x0,z0+step),(x0+step,z0+step),(ix,iz+1),j==n-1),((x0+step,z0+step),(x0+step,z0),(ix+1,iz),i==n-1),((x0+step,z0),(x0,z0),(ix,iz-1),j==0)]:
     steps=max(1,fine.get(neighbor,1)//n) if border else 1
     ring.extend((start[0]+(end[0]-start[0])*t/steps,start[1]+(end[1]-start[1])*t/steps) for t in range(steps))
    for t in range(len(ring)):triangle((x0+step/2,z0+step/2),ring[t],ring[(t+1)%len(ring)])
assert all(any(water_count[i]>0 for i in ids) for ids in water_groups.values() if any(records[i]['fill'] for i in ids))
assert all(abs(v[1])<.4 for v in ground)
# Only empty parts of the old fine cells may coarsen; authored depressions keep
# their exact original tessellation, and wet macro-cells remain byte-identical.
def support(r):
 rx=r['rx']*(1.6 if r['kind']==0 else 1.2 if r['kind']==1 else 1)
 rz=r['rz']*(1.31 if r['kind']==0 else 1)
 ex=abs(r['c'])*rx+abs(r['s'])*rz
 ez=abs(r['s'])*rx+abs(r['c'])*rz
 return (r['x']-ex,r['x']+ex,r['z']-ez,r['z']+ez)
supports=[support(r) for r in records]
def overlaps(box,x0,z0,x1,z1):
 return box[0]<=x1 and box[1]>=x0 and box[2]<=z1 and box[3]>=z0
# Clipping never crosses a macro-cell. Include every cell touched by water,
# including shoreline vertices exactly on the boundary, rather than the basin AABB.
wet_cells=set()
for v in water:
 for dx in (-1e-7,1e-7):
  for dz in (-1e-7,1e-7):
   wet_cells.add((min(47,max(0,math.floor((v[0]+12+dx)*2))),min(47,max(0,math.floor((v[2]+12+dz)*2)))))

def rut_overlaps(r,x0,z0,x1,z1):
 # Interval lower bound includes finite-difference normal support; it can retain
 # extra cells, but cannot discard a nonzero rut cut or displaced-earth bank.
 e=.002;x0-=e;z0-=e;x1+=e;z1+=e
 uv=[((r['c']*(x-r['x'])-r['s']*(z-r['z']))/r['rx'],(r['s']*(x-r['x'])+r['c']*(z-r['z']))/r['rz']) for x in (x0,x1) for z in (z0,z1)]
 ulo=min(p[0] for p in uv);uhi=max(p[0] for p in uv)
 vlo=min(p[1] for p in uv);vhi=max(p[1] for p in uv)
 points=[vlo,vhi]+[v for v in (-1,1,(-.7-math.sqrt(12.49))/6,(-.7+math.sqrt(12.49))/6) if vlo<=v<=vhi]
 shifts=[.24*r['side']*max(0,1-v*v)*(v+.35) for v in points]
 ulo-=max(shifts);uhi-=min(shifts)
 def nearest(a,b):return 0 if a<=0<=b else min(abs(a),abs(b))
 return nearest(ulo,uhi)**2+nearest(vlo,vhi)**2<=1.7

def rectangle_without_center(center, ring):
 # Attribute error bounds also preserve shallow water and authored marks.
 if len(ring)!=4:return None
 if any(v[11]!=center[11] or v[12]!=center[12] for v in ring):return None
 if len({v[0] for v in ring})!=2 or len({v[2] for v in ring})!=2:return None
 choices=[]
 for diagonal in (0,1):
  a,b=ring[diagonal],ring[diagonal+2];maximum=0.;valid=True
  for packed in (False,True):
   def encode(v):
    q=[round(x,7) for x in v]
    return struct.unpack('<13f',struct.pack('<13f',*q)) if packed else q
   c,p,q=map(encode,(center,a,b));errors=[abs(c[i]-(p[i]+q[i])*.5) for i in range(13)]
   valid &= c[0]==(p[0]+q[0])*.5 and c[2]==(p[2]+q[2])*.5
   valid &= max(errors[:3])<=.00005 and max(errors[3:6])<=1e-5 and max(errors[8:11])<=1e-5
   valid &= max(errors[6:8])<=1e-7 and c[11]==p[11]==q[11] and c[12]==p[12]==q[12]
   maximum=max(maximum,max(errors))
  if valid:choices.append((maximum,diagonal))
 if not choices:return None
 diagonal=min(choices)[1]
 q=ring[diagonal:]+ring[:diagonal]
 return [q[0],q[1],q[2],q[0],q[2],q[3]]

def optimized_ground(lod):
 result=[]
 for iz in range(N):
  for ix in range(N):
   x=-12+ix*S;z=-12+iz*S;n=fine.get((ix,iz),1);unit=S/32
   minimum=32//n;maximum=max(minimum,8 if max(abs(x+S/2),abs(z+S/2))<(6,3,0)[lod] else 32);leaves=[]
   nearby=[(r,b) for r,b in zip(records,supports) if overlaps(b,x-.002,z-.002,x+S+.002,z+S+.002)]
   def divide(a,b,size):
    preserve=(ix,iz) in wet_cells or any(rut_overlaps(r,x+a*unit,z+b*unit,x+(a+size)*unit,z+(b+size)*unit) if r['kind']==0 else overlaps(q,x+a*unit,z+b*unit,x+(a+size)*unit,z+(b+size)*unit) for r,q in nearby)
    # Coarsening smooth hills must also preserve the 100 micrometre oracle.
    corners=[base(x+(a+dx)*unit,z+(b+dz)*unit) for dx,dz in ((0,0),(size,0),(size,size),(0,size))]
    center_h=base(x+(a+size/2)*unit,z+(b+size/2)*unit)
    curvature=abs(center_h-sum(corners)/4)
    for k,(u,v) in enumerate(((0,0),(1,0),(1,1),(0,1))):
     quarter=base(x+(a+size*(.25+.5*u))*unit,z+(b+size*(.25+.5*v))*unit)
     next_u,next_v=((1,0),(1,1),(0,1),(0,0))[k]
     edge=base(x+(a+size*(u+next_u)/2)*unit,z+(b+size*(v+next_v)/2)*unit)
     curvature=max(curvature,abs(quarter-(center_h+corners[k])/2),abs(edge-(corners[k]+corners[(k+1)%4])/2))
    if size>minimum and (preserve or size>maximum or curvature>.00007):
     h=size//2
     for dz,dx in ((0,0),(0,h),(h,0),(h,h)):divide(a+dx,b+dz,h)
    else:leaves.append((a,b,size))
   divide(0,0,32)
   horizontal={};vertical={}
   def corner(a,b):
    horizontal.setdefault(b,set()).add(a);vertical.setdefault(a,set()).add(b)
   for a,b,size in leaves:
    for dx,dz in ((0,0),(0,size),(size,0),(size,size)):corner(a+dx,b+dz)
   # Keep the original macro-cell boundary samples, including finer neighbours.
   for axis,edge,neighbor in ((0,0,(ix-1,iz)),(0,32,(ix+1,iz)),(1,0,(ix,iz-1)),(1,32,(ix,iz+1))):
    count=max(n,fine.get(neighbor,1))
    for t in range(count+1):
     along=t*32//count
     corner(edge,along) if axis==0 else corner(along,edge)
   for a,b,size in sorted(leaves,key=lambda item:(item[1],item[0])):
    ring=[(a,t) for t in sorted(vertical[a]) if b<=t<b+size]
    ring += [(t,b+size) for t in sorted(horizontal[b+size]) if a<=t<a+size]
    ring += [(a+size,t) for t in sorted(vertical[a+size],reverse=True) if b<t<=b+size]
    ring += [(t,b) for t in sorted(horizontal[b],reverse=True) if a<t<=a+size]
    center=vertex(x+(a+size/2)*unit,z+(b+size/2)*unit)
    boundary=[vertex(x+u*unit,z+v*unit) for u,v in ring]
    reduced=rectangle_without_center(center,boundary)
    if reduced is not None:result.extend(reduced)
    else:
     for i in range(len(boundary)):result.extend((center,boundary[i],boundary[(i+1)%len(boundary)]))
 return result
meshes=[];counts=[]
for lod in range(3):
 verts=optimized_ground(lod);counts.append(len(verts)//3)
 meshes.append(dict(name='meadow_ground'+('' if lod==0 else '_lod'+str(lod)),vertices=[[round(v,7) for v in row] for row in verts]))
for lod in range(3):
 meshes.append(dict(name='meadow_water'+('' if lod==0 else '_lod'+str(lod)),vertices=[[round(v,7) for v in row] for row in water]))
assert all(count<len(ground)//3 for count in counts)
meshes=[index_mesh(mesh) for mesh in meshes]
(OUT/'meadow_surface_meshes.json').write_text(json.dumps(dict(version=1,meshes=meshes),separators=(',',':')))
print(json.dumps(dict(original_ground_triangles=len(ground)//3,ground_lod_triangles=counts,water_triangles=len(water)//3,preserved_wet_cells=len(wet_cells),filled_basins=sum(n>0 for n in water_count),records=len(records))))
