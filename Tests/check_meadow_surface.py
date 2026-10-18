"""Structural validation of the authored meadow geometry and actual partial water fills."""
import ast, json, math, struct, sys, time
from collections import Counter
started=time.perf_counter()
from pathlib import Path
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'tools/art'))
from mesh_indexing import index_mesh

def check_index_mesh():
 # These source values all cook to float32 zero, but are distinct binary64 keys.
 a=[0.0]*13;b=a.copy();b[0]=-0.0;c=a.copy();c[0]=1e-50
 original={'name':'indexing-fixture','vertices':[a,b,c,c,a,b]}
 indexed=index_mesh(original)
 assert len(indexed['vertices'])==3 and indexed['indices']==[0,1,2,2,0,1]
 def packed(mesh):
  return b''.join(struct.pack('<13d',*mesh['vertices'][i]) for i in mesh.get('indices',range(len(mesh['vertices']))))
 assert packed(indexed)==packed(original)
 repeated=index_mesh(indexed)
 assert repeated['indices']==indexed['indices'] and packed(repeated)==packed(original)
 restored=json.loads(json.dumps(indexed))
 assert packed(restored)==packed(original) and len(index_mesh(restored)['vertices'])==3
 for invalid in (True,0.0,1.5,-1,3):
  try:index_mesh({'vertices':[a,b,c],'indices':[0,1,invalid]})
  except ValueError:pass
  else:raise AssertionError(('index_mesh accepted invalid index',invalid))
check_index_mesh()
src=json.loads((root/'ContentSource/Models/Environment/meadow_surface.json').read_text())
mesh_path=Path(sys.argv[1]) if len(sys.argv)>1 else root/'ContentSource/Models/Environment/meadow_surface_meshes.json'
meshes=json.loads(mesh_path.read_text())['meshes']
# Preserve the existing triangle-stream oracle for both source encodings.
for mesh in meshes:
 if 'indices' in mesh:
  vertices=mesh['vertices'];indices=mesh['indices']
  assert isinstance(vertices,list) and 0<len(vertices)<=3000000
  assert all(isinstance(v,list) and len(v)==13 and all(type(n) in (int,float) and math.isfinite(n) for n in v) for v in vertices)
  assert isinstance(indices,list) and 0<len(indices)<=3000000 and len(indices)%3==0
  assert all(type(i) is int and 0<=i<len(vertices) for i in indices)
  assert len({struct.pack('<13d',*v) for v in vertices})==len(vertices), 'duplicate complete source vertex'
  assert len(set(indices))==len(vertices), 'unused source vertex'
  mesh['vertices']=[vertices[i] for i in indices]
assert len(meshes)==6
rows=src['records'];counts={};water_types=set()
for mesh in meshes:
 vertices=mesh['vertices'];assert len(vertices)>0 and len(vertices)%3==0
 water=mesh['name'].startswith('meadow_water')
 for i in range(0,len(vertices),3):
  a,b,c=vertices[i:i+3]
  area=(b[0]-a[0])*(c[2]-a[2])-(c[0]-a[0])*(b[2]-a[2]);assert area < -1e-11,(mesh['name'],i,area)
  for v in (a,b,c):
   assert len(v)==13 and all(math.isfinite(n) for n in v)
   assert -12.00001<=v[0]<=12.00001 and -12.00001<=v[2]<=12.00001
   assert abs(sum(n*n for n in v[3:6])-1)<1e-5
   assert v[12]==0
  if water:
   def f(v):return struct.unpack('f',struct.pack('f',v))[0]
   pa,pb,pc=[(f(v[0]),f(v[2])) for v in (a,b,c)]
   ux=f(pb[0]-pa[0]);uz=f(pb[1]-pa[1]);vx=f(pc[0]-pa[0]);vz=f(pc[1]-pa[1])
   cross=f(f(uz*vx)-f(ux*vz))
   assert f(cross*cross)>f(1e-16), 'water fails float32 cooker degeneracy gate'
   ids=[round(v[7])-100 for v in (a,b,c)];assert ids[0]==ids[1]==ids[2]
   record=rows[ids[0]];assert record['fill']>0
   assert all(abs(v[1]-record['level'])<1e-6 for v in (a,b,c)), 'water is not horizontal'
   assert all(0<=v[6]<.1 for v in (a,b,c)), 'invalid baked water optical depth'
   assert max(v[6] for v in (a,b,c))>0, 'water covers no depth'
   water_types.add(record['kind']);counts[ids[0]]=counts.get(ids[0],0)+1
  elif any(abs(abs(v[0])-12)<1e-6 or abs(abs(v[2])-12)<1e-6 for v in (a,b,c)):
   for v in (a,b,c):
    if abs(abs(v[0])-12)<1e-6 or abs(abs(v[2])-12)<1e-6:assert abs(v[1])<1e-6, 'slab boundary seam'
assert {0,1,2,3}<=water_types
assert all(i in counts for i,r in enumerate(rows) if r['fill']>0)
assert all(any(r['kind']==kind and r['fill']==0 for r in rows) for kind in (0,1,2))
assert all(0<r['fill']<1 for r in rows if r['fill'])
# Validate the emitted mesh, independently of the generator's subdivision code.
grounds=[m for m in meshes if m['name'].startswith('meadow_ground')]
waters=[m for m in meshes if m['name'].startswith('meadow_water')]
assert len(grounds)==len(waters)==3
triangles=[len(m['vertices'])//3 for m in grounds]
# Rut banks now require 6.25 cm samples; retain a bounded budget and distinct LODs.
assert 125000>triangles[0]>triangles[1]>triangles[2], ('ground LOD reduction',triangles)
assert waters[0]['vertices']==waters[1]['vertices']==waters[2]['vertices'], 'water changed across LODs'
def cell(triangle):
 # Each clipped water triangle remains inside one original half-metre cell.
 return tuple(min(47,max(0,math.floor((sum(v[k] for v in triangle)/3+12)*2))) for k in (0,2))
def boundary(p):
 return abs(abs(p[0])-12)<1e-6 or abs(abs(p[2])-12)<1e-6
wet_vertices=waters[0]['vertices']
wet_cells={cell(wet_vertices[i:i+3]) for i in range(0,len(wet_vertices),3)}
perimeters=[];wet_support=[]
for mesh in grounds:
 vertices=mesh['vertices'];edges=Counter();perimeter=set();support=set();area2=0
 for i in range(0,len(vertices),3):
  tri=vertices[i:i+3]
  points=[tuple(round(x,7) for x in v[:3]) for v in tri]
  a,b,c=points
  area2+=(c[0]-a[0])*(b[2]-a[2])-(b[0]-a[0])*(c[2]-a[2])
  for p,q in zip(points,points[1:]+points[:1]):
   edges[tuple(sorted((p,q)))]+=1
  for v,p in zip(tri,points):
   if boundary(p):
    assert abs(p[1])<1e-6, 'nonzero perimeter height'
    perimeter.add(tuple(round(x,7) for x in v[:6]))
  if cell(tri) in wet_cells:
   support.add(tuple(sorted(tuple(v) for v in tri)))
 for (p,q),count in edges.items():
  exterior=(abs(abs(p[0])-12)<1e-6 and p[0]==q[0]) or (abs(abs(p[2])-12)<1e-6 and p[2]==q[2])
  assert count==(1 if exterior else 2), (mesh['name'],'unmatched/overlapping edge',p,q,count)
 assert abs(area2/2-576)<1e-6, (mesh['name'],'ground coverage',area2/2)
 perimeters.append(perimeter);wet_support.append(support)
assert perimeters[0]==perimeters[1]==perimeters[2], 'LOD perimeter positions/normals differ'
assert wet_support[0]==wet_support[1]==wet_support[2], 'ground support under wet cells differs'
# Sample emitted geometry against the shared authored height field without executing generation.
generator=ast.parse((root/'tools/art/meadow/generate_surface.py').read_text(encoding='utf-8-sig'))
functions=ast.Module(body=[n for n in generator.body if isinstance(n,ast.FunctionDef) and n.name in ('base','shape','depth','ridge','height')],type_ignores=[])
field={'math':math,'records':rows};exec(compile(functions,'meadow_height_contract','exec'),field)
height_error=0
for mesh in grounds:
 for v in mesh['vertices'][::max(1,len(mesh['vertices'])//1500)]:
  height_error=max(height_error,abs(v[1]-field['height'](v[0],v[2])))
assert height_error<1e-6, ('mesh/contact height mismatch',height_error)
for r in rows:
 if r['kind']!=0:continue
 assert .18<=r['rx']<=.24 and .05<=r['depth']<=.09
 # Earth displacement peaks outside the cut, stays low, and fades to zero.
 assert 0<field['ridge'](r,1.05)<.03
 assert field['ridge'](r,0)==field['ridge'](r,1.8)==0
assert all(field['height'](x,z)==0 for x,z in [(-12,0),(12,0),(0,-12),(0,12)])
print(json.dumps({'ground_lod_triangles':triangles,'water_triangles':len(waters[0]['vertices'])//3,'filled_basins':len(counts),'wet_trace_types':sorted(water_types),'wet_cells_checked':len(wet_cells),'wet_support_triangles':len(wet_support[0]),'perimeter_vertices':len(perimeters[0]),'coverage_m2':576,'elapsed_seconds':round(time.perf_counter()-started,3),'sampled_height_error_m':height_error,'valid':True}))