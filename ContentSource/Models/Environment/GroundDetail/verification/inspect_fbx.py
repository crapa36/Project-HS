import bpy,json,collections
from pathlib import Path
root=Path(r"C:/Users/crapa/.codex/worktrees/cpu-gpu-utilization/Project-HS")
folder=root/"ContentSource/Models/Environment/GroundDetail"
bpy.ops.wm.open_mainfile(filepath=str(folder/"ground_detail_v001.blend"))
def signatures(mesh):
 return collections.Counter(tuple(sorted(tuple(round(float(v),7) for v in mesh.vertices[i].co) for i in p.vertices)) for p in mesh.polygons)
src=bpy.data.objects["flower_violet_lod1"]
source=signatures(src.data)
print("source",len(src.data.vertices),len(src.data.polygons),collections.Counter(len(p.vertices) for p in src.data.polygons))
sc=bpy.data.scenes.new("VERIFY-debug");bpy.context.window.scene=sc
bpy.ops.import_scene.fbx(filepath=str(folder/"FBX/flower_violet.fbx"))
ob=next(o for o in sc.objects if o.name.startswith("flower_violet_lod1"))
imp=signatures(ob.data);missing=source-imp;extra=imp-source
print("import",len(ob.data.vertices),len(ob.data.polygons),collections.Counter(len(p.vertices) for p in ob.data.polygons))
print("missing polygons",sum(missing.values()),"extra",sum(extra.values()),"first",list(missing.items())[:8])
print("FBX use_triangles", "use_triangles" in bpy.ops.export_scene.fbx.get_rna_type().properties)
