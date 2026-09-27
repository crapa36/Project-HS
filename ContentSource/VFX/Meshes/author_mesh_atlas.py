import bpy, math, json
from pathlib import Path
root=Path(__file__).parent
scene=bpy.data.scenes.new('HS_VFX_MeshAtlas')
bpy.context.window.scene=scene
scene.unit_settings.scale_length=1.0
specs=[('arrowhead_mesh',6,[(0,.035),(.22,.10),(.36,.23),(.94,.025),(1,.001)],0),('shard_mesh',7,[(0,.08),(.18,.19),(.65,.15),(1,.01)],.33),('debris_shard_mesh',5,[(0,.16),(.20,.27),(.68,.22),(1,.12)],.51),('small_shard_mesh',5,[(0,.06),(.28,.20),(.80,.11),(1,.005)],.26),('enemy_thorn_mesh',5,[(0,.21),(.16,.22),(.62,.10),(1,.005)],.12),('boss_crest_lance_mesh',8,[(0,.045),(.12,.07),(.27,.30),(.40,.10),(.72,.16),(1,.001)],0),('needle_shard_mesh',5,[(0,.025),(.15,.06),(.68,.045),(1,.002)],.15)]
atlas={'version':1,'axis_forward':'+Z','unit_meters':1,'meshes':[]}
for idx,(name,n,rings,twist) in enumerate(specs):
 verts=[]
 for ri,(z,r) in enumerate(rings):
  for k in range(n):
   a=2*math.pi*k/n+twist*ri
   irregular=1+(.14*math.sin(k*2.4+ri*1.7) if twist else 0)
   verts.append((math.cos(a)*r*irregular,math.sin(a)*r*(.65 if name=='arrowhead_mesh' else 1),z))
 faces=[(0,k+1,k) for k in range(1,n-1)]
 for ri in range(len(rings)-1):
  for k in range(n):
   a=ri*n+k;b=ri*n+(k+1)%n;c=(ri+1)*n+(k+1)%n;d=(ri+1)*n+k
   faces.extend([(a,b,c),(a,c,d)])
 base=(len(rings)-1)*n
 faces.extend([(base,base+k,base+k+1) for k in range(1,n-1)])
 mesh=bpy.data.meshes.new(name);mesh.from_pydata(verts,[],faces);mesh.update()
 uv=mesh.uv_layers.new(name='UV0')
 for poly in mesh.polygons:
  for corner,li in enumerate(poly.loop_indices): uv.data[li].uv=((0,0),(1,0),(0,1))[corner]
 mesh.calc_loop_triangles();mesh.calc_tangents(uvmap='UV0')
 obj=bpy.data.objects.new(name,mesh);scene.collection.objects.link(obj)
 data=[]
 for tri in mesh.loop_triangles:
  for li in tri.loops:
   co=mesh.vertices[mesh.loops[li].vertex_index].co
   data.append([*co,*tri.normal,*uv.data[li].uv,*mesh.loops[li].tangent,mesh.loops[li].bitangent_sign])
 atlas['meshes'].append({'id':name,'vertices':data,'indices':list(range(len(data)))})
 obj['unit_meters']=1.;obj['forward_axis']='+Z';obj['pivot']='rear_origin';obj.select_set(True)
(root/'vfx_mesh_atlas.json').write_text(json.dumps(atlas,indent=2),encoding='utf-8')
bpy.context.view_layer.objects.active=scene.objects[0]
# Export local origin geometry, then arrange a separate preview layout.
r=bpy.ops.export_scene.fbx.get_rna_type()
assert 'Z' in [e.identifier for e in r.properties['axis_forward'].enum_items]
assert 'Y' in [e.identifier for e in r.properties['axis_up'].enum_items]
bpy.ops.export_scene.fbx(filepath=str(root/'vfx_mesh_atlas.fbx'),use_selection=True,object_types={'MESH'},axis_forward='Z',axis_up='Y',bake_anim=False)
for idx,obj in enumerate(scene.objects): obj.location=(idx*.8,0,0)
bpy.ops.wm.save_as_mainfile(filepath=str(root/'vfx_mesh_atlas.blend'),copy=True)
print([(m['id'],len(m['indices'])//3) for m in atlas['meshes']])
