"""Run after create_slime.py in Blender via MCP."""
import bpy
import json
from pathlib import Path
from mathutils import Vector
BASE=Path(__file__).resolve().parent
ANIMS=BASE.parents[2]/'Animations'/'Monsters'/'BasicSlime'
scene=bpy.context.scene
rig=scene.objects[scene['slime_rig']]; obj=scene.objects[scene['slime_mesh']]
for other in bpy.context.view_layer.objects: other.select_set(False)
rig.select_set(True); obj.select_set(True); bpy.context.view_layer.objects.active=rig
rig.animation_data_clear()
for bone in rig.pose.bones:
    bone.location=(0,0,0); bone.rotation_euler=(0,0,0); bone.scale=(1,1,1)
scene.frame_set(1)
def export(path,animated=False):
    bpy.ops.export_scene.fbx(filepath=str(path),use_selection=True,object_types={'ARMATURE','MESH'},
        axis_forward='-Z',axis_up='Y',apply_unit_scale=True,apply_scale_options='FBX_SCALE_UNITS',
        global_scale=1.0,add_leaf_bones=False,use_armature_deform_only=False,
        bake_anim=animated,bake_anim_use_all_actions=False,bake_anim_use_nla_strips=False,
        bake_anim_force_startend_keying=True,bake_anim_step=1.0,bake_anim_simplify_factor=0.0,
        path_mode='AUTO',mesh_smooth_type='FACE')
export(BASE/'BasicSlime.fbx')
# frame, horizontal width, depth, height, hop, forward body stretch
clips={
 'Idle':[(1,1,1,1,0,0),(31,1.025,1.025,.95,0,0),(61,1,1,1,0,0),(91,.985,.985,1.03,0,0),(121,1,1,1,0,0)],
 'Run':[(1,1.14,1.12,.75,0,0),(10,.9,.93,1.22,.035,0),(23,.96,.98,1.10,.09,0),(37,1,1,1,.025,0),(46,1.14,1.12,.75,0,0)],
 'Draw':[(1,1,1,1,0,0),(8,1.14,1.09,.72,0,-.018),(15,1.16,1.10,.67,0,-.026),(18,.94,1.09,1.03,0,.17),(20,.98,1.06,.98,0,.10),(22,1,1,1,0,0)],
 'Recoil':[(1,1,1.02,.92,0,.04),(5,1.10,.91,.8,0,-.05),(12,.96,1.02,1.06,0,0),(19,1,1,1,0,0)],
 'Death':[(1,1,1,1,0,0),(9,1.13,1.10,.68,0,0),(20,1.33,1.30,.28,0,0),(37,1.47,1.44,.09,0,0),(49,1.47,1.44,.09,0,0)]}
authored_actions={}
for name,keys in clips.items():
    action=bpy.data.actions.new(name); action.use_fake_user=True; authored_actions[name]=action
    rig.animation_data_create(); rig.animation_data.action=action
    for frame,sx,sy,sz,hop,front in keys:
        for bone in rig.pose.bones:
            bone.location=(0,0,0); bone.rotation_euler=(0,0,0); bone.scale=(1,1,1)
        center=rig.pose.bones['body_center']
        center.scale=(sx,sz,sy)
        center.location=(0,.16*(sz-1)+hop,0)
        rig.pose.bones['body_front'].location=(0,0,front)
        rig.pose.bones['body_top'].location=(0,0,front*.30)
        for bone in rig.pose.bones:
            bone.keyframe_insert('location',frame=frame,group=bone.name)
            bone.keyframe_insert('rotation_euler',frame=frame,group=bone.name)
            bone.keyframe_insert('scale',frame=frame,group=bone.name)
    scene.frame_start=1; scene.frame_end=keys[-1][0]; scene.frame_set(1)
    export(ANIMS/(name+'.fbx'),True)
rig.animation_data.action=authored_actions['Idle']; scene.frame_start=1; scene.frame_end=121; scene.frame_set(1)
# A separate camera/lights leave existing scene content intact.
world=bpy.data.worlds.new('Slime_Studio'); world.use_nodes=True
world.node_tree.nodes['Background'].inputs[0].default_value=(.045,.075,.105,1)
world.node_tree.nodes['Background'].inputs[1].default_value=.4; scene.world=world
def aim(o,target): o.rotation_euler=(Vector(target)-o.location).to_track_quat('-Z','Y').to_euler()
for name,loc,power,size in [('Key',(-1,-1,2),110,1.4),('Fill',(1,-.6,.7),45,1),('Rim',(.3,1,1.4),140,1)]:
    data=bpy.data.lights.new('Slime_'+name,'AREA'); data.energy=power; data.shape='DISK'; data.size=size
    o=bpy.data.objects.new('Slime_'+name,data); scene.collection.objects.link(o); o.location=loc; aim(o,(0,0,.2))
data=bpy.data.cameras.new('Slime_Camera'); cam=bpy.data.objects.new('Slime_Camera',data); scene.collection.objects.link(cam)
cam.location=(.75,-1.8,1.05); aim(cam,(0,0,.20)); data.type='ORTHO'; data.ortho_scale=1.05; scene.camera=cam
scene.render.engine='CYCLES'; scene.cycles.samples=32
scene.render.resolution_x=800; scene.render.resolution_y=800; scene.render.resolution_percentage=100
scene.view_settings.view_transform='Standard'
scene.render.image_settings.file_format='PNG'; scene.render.filepath=str(BASE/'preview.png')
bpy.ops.wm.save_as_mainfile(filepath=str(BASE/'BasicSlime.blend'))
print(json.dumps({'exports':[str(BASE/'BasicSlime.fbx')]+[str(ANIMS/(n+'.fbx')) for n in clips],'blend':bpy.data.filepath}))
