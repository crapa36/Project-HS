"""Repair verified duplicate root caps; preserve the already-reviewed atlas and previews."""
import runpy,sys,bpy
from pathlib import Path
root=Path(r"C:/Users/crapa/.codex/worktrees/cpu-gpu-utilization/Project-HS")
sys.argv=["create_ground_detail_blender.py","--","--root",str(root),"--stage","verify"]
d=runpy.run_path(str(root/"tools/art/create_ground_detail_blender.py"))
d["geometry"]()
scene=bpy.data.scenes["HS_Ground_Detail_v001"];bpy.context.window.scene=scene
material=d["material"]()
for ob in bpy.data.collections["COL-ground-detail-assets"].objects:
    ob.data.materials.clear();ob.data.materials.append(material)
records,stats=d["runtime_records"]()
d["register"](records,stats);d["exports"]()
for im in bpy.data.images:
    if im.source=="FILE" and "ground_detail" in im.name:im.pack()
d["camera"](scene,(3,-4.5,4),(0,0,.13),3.5)
bpy.ops.wm.save_as_mainfile(filepath=str(d["OUT"]/"ground_detail_v001.blend"))
preview=bpy.data.objects["PREVIEW-flower_violet"]
for ob in bpy.data.collections["COL-ground-detail-preview"].objects:ob.hide_render=True
preview.hide_render=False;preview.location=(0,0,0)
d["camera"](scene,(1.2,-1.6,1.3),(0,0,.10),.82)
d["render"](scene,d["OUT"]/"preview/flower_violet_close.png",800,800)
d["verify"]()
print("DUPLICATE_REPAIR_VERIFIED", {x["name"]:x["triangles"] for x in stats})
