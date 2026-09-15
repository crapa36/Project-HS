import bpy, json, numpy as np
from pathlib import Path
ROOT=Path(r'C:/Users/crapa/Documents/GitHub/Project HS')
OUT=ROOT/'ContentSource/Models/Environment'
PACK=ROOT/'ContentSource/Textures/Environment'
SPEC=json.loads((PACK/'project_hs_environment_materials_v9_LEAN.json').read_text(encoding='utf-8'))
# Export copies use full-atlas UVs and simple FBX-compatible PBR nodes.
page_materials={}
for group,count in [('leaf',3),('grass',2)]:
    for page in range(count):
        source=SPEC['foliage'][group]['pages'][page]
        normal=bpy.data.images.load(str(PACK/'baked'/('foliage_'+group+'_atlas_'+str(page)+'_normal.png')),check_existing=False)
        normal.colorspace_settings.name='Non-Color'
        pixels=np.empty(len(normal.pixels),dtype=np.float32);normal.pixels.foreach_get(pixels);pixels[1::4]=1-pixels[1::4];normal.pixels.foreach_set(pixels)
        normal.filepath_raw=str(OUT/'FBXTextures'/(group+'_atlas_'+str(page)+'_normal.png'));normal.file_format='PNG';normal.save()
        mat=bpy.data.materials.new('FBX_'+group+'_page_'+str(page));mat.use_nodes=True
        ns=mat.node_tree.nodes;lk=mat.node_tree.links;bs=ns.get('Principled BSDF')
        for role,input_name in [('base_color','Base Color'),('roughness','Roughness'),('normal','Normal')]:
            tex=ns.new('ShaderNodeTexImage');tex.image=normal if role=='normal' else bpy.data.images.load(str(PACK/source[role]),check_existing=True)
            tex.image.colorspace_settings.name='sRGB' if role=='base_color' else 'Non-Color'
            if role=='normal':
                nm=ns.new('ShaderNodeNormalMap');lk.new(tex.outputs['Color'],nm.inputs['Color']);lk.new(nm.outputs['Normal'],bs.inputs[input_name])
            else:lk.new(tex.outputs['Color'],bs.inputs[input_name])
            if role=='base_color':lk.new(tex.outputs['Alpha'],bs.inputs['Alpha'])
        page_materials[group,page]=mat
for group,count in [('trunk',3),('leaf',3),('rock',4),('grass',4)]:
    for i in range(count):
        name=group+'_'+str(i);copies=[];renamed=[]
        names=[name,name+'_lod1',name+'_lod2']
        if bpy.data.objects.get('UCX_'+name+'_00'):names.append('UCX_'+name+'_00')
        for object_name in names:
            src=bpy.data.objects[object_name];src.name=object_name+'_source';renamed.append((src,object_name))
            ob=src.copy();ob.data=src.data.copy();ob.name=object_name;bpy.context.scene.collection.objects.link(ob);ob.hide_set(False);copies.append(ob)
            if group in ['leaf','grass']:
                entries=SPEC['foliage'][group]['entries'];uv=ob.data.uv_layers.active;uv.name='UV0'
                for face in ob.data.polygons:
                    page,x,y,w,h,_=entries[face.material_index]
                    for li in face.loop_indices:
                        u,v=uv.data[li].uv;uv.data[li].uv=((x+u*w)/2048,1-(y+v*h)/2048)
                    face.material_index=page
                ob.data.materials.clear()
                for page in range(3 if group=='leaf' else 2):ob.data.materials.append(page_materials[group,page])
            if object_name.startswith('UCX_'):
                import bmesh
                bm=bmesh.new();bm.from_mesh(ob.data);bmesh.ops.triangulate(bm,faces=list(bm.faces));bm.to_mesh(ob.data);bm.free()
        bpy.ops.object.select_all(action='DESELECT')
        for ob in copies:ob.select_set(True)
        bpy.context.view_layer.objects.active=copies[0]
        bpy.ops.export_scene.fbx(filepath=str(OUT/(name+'.fbx')),use_selection=True,object_types={'MESH'},apply_unit_scale=True,apply_scale_options='FBX_SCALE_UNITS',axis_forward='-Z',axis_up='Y',use_tspace=True,mesh_smooth_type='FACE',bake_anim=False,path_mode='RELATIVE',add_leaf_bones=False)
        for ob in copies:
            mesh=ob.data;bpy.data.objects.remove(ob,do_unlink=True);bpy.data.meshes.remove(mesh)
        for src,original in renamed:src.name=original
print('Exported 14 FBX files with full-atlas foliage UV0 and triangulated collision proxies')
