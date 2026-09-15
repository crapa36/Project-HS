"""Blender MCP high-to-low authoring/bake and LOD/FBX export. Run functions per asset."""
import bpy, math, json, random
from pathlib import Path
from mathutils import Vector
ROOT=Path(r'C:/Users/crapa/Documents/GitHub/Project HS')
OUT=ROOT/'ContentSource/Models/Environment'
PACK=ROOT/'ContentSource/Textures/Environment'
BAKED=PACK/'baked';BAKED.mkdir(exist_ok=True)
SC=bpy.context.scene
SC.render.engine='CYCLES';SC.cycles.samples=16
SC.render.bake.use_selected_to_active=True
SC.render.bake.cage_extrusion=.06
SC.render.bake.max_ray_distance=.12
SC.render.bake.margin=24
SC.render.bake.use_clear=True
SC.cycles.bake_type='NORMAL'
JOBS={};ART=[]

def select_only(objects,active):
    bpy.ops.object.select_all(action='DESELECT')
    for ob in objects:ob.hide_set(False);ob.hide_render=False;ob.select_set(True)
    bpy.context.view_layer.objects.active=active

def apply_modifier(obj,name):
    select_only([obj],obj);bpy.ops.object.modifier_apply(modifier=name)

def prepare(name):
    low=bpy.data.objects[name]
    low.location=(0,0,0)
    high=low.copy();high.data=low.data.copy();high.name='HP_'+name;SC.collection.objects.link(high)
    # Reference UVs/materials remain on the high-poly source. Subdivision and real
    # surface displacement provide a geometrical source for tangent-space baking.
    mod=high.modifiers.new('HighPolySurface','SUBSURF');mod.subdivision_type='CATMULL_CLARK';mod.levels=2;apply_modifier(high,mod.name)
    texture=bpy.data.textures.new('MicroRelief_'+name,type='CLOUDS');texture.noise_scale=.045 if name.startswith('trunk') else .13;texture.noise_depth=2
    dis=high.modifiers.new('GeometryRelief','DISPLACE');dis.texture=texture;dis.strength=.008 if name.startswith('trunk') else .028;dis.mid_level=.5;dis.texture_coords='GLOBAL';apply_modifier(high,dis.name)
    # Preserve silhouettes in LOD0; bake UVs are uniquely packed, with no tiled overlap.
    select_only([low],low);bpy.ops.object.mode_set(mode='EDIT');bpy.ops.mesh.select_all(action='SELECT');bpy.ops.uv.smart_project(angle_limit=1.15192,island_margin=.012);bpy.ops.object.mode_set(mode='OBJECT')
    low.data.uv_layers.active.name='UV0'
    # Bake both supplied normal scales into the high-to-low result once.
    for mat in high.data.materials:
        ns=mat.node_tree.nodes;lk=mat.node_tree.links
        if any(n.name=='CombinedReferenceNormals' for n in ns):continue
        main=next(n for n in ns if n.type=='NORMAL_MAP')
        source=next(n for n in ns if n.type=='TEX_IMAGE' and n.image and '_normal.' in n.image.filepath)
        detail=ns.new('ShaderNodeTexImage');detail.image=bpy.data.images.load(bpy.path.abspath(source.image.filepath).replace('_normal.','_detail_normal.'),check_existing=True);detail.image.colorspace_settings.name='Non-Color'
        if source.inputs['Vector'].is_linked:lk.new(source.inputs['Vector'].links[0].from_socket,detail.inputs['Vector'])
        dn=ns.new('ShaderNodeNormalMap');lk.new(detail.outputs['Color'],dn.inputs['Color'])
        add=ns.new('ShaderNodeVectorMath');add.operation='ADD';lk.new(main.outputs['Normal'],add.inputs[0]);lk.new(dn.outputs['Normal'],add.inputs[1])
        geo=ns.new('ShaderNodeNewGeometry');sub=ns.new('ShaderNodeVectorMath');sub.operation='SUBTRACT';lk.new(add.outputs[0],sub.inputs[0]);lk.new(geo.outputs['Normal'],sub.inputs[1])
        norm=ns.new('ShaderNodeVectorMath');norm.operation='NORMALIZE';norm.name='CombinedReferenceNormals';lk.new(sub.outputs[0],norm.inputs[0]);lk.new(norm.outputs[0],ns.get('Principled BSDF').inputs['Normal'])
    original_materials=list(high.data.materials)
    low.data.materials.clear();target=bpy.data.materials.new('Baked_'+name);target.use_nodes=True;low.data.materials.append(target)
    for poly in low.data.polygons:poly.material_index=0
    JOBS[name]={'low':low,'high':high,'target':target,'source_materials':original_materials}
    ART.extend([low,high])
    print(name,'high_faces',len(high.data.polygons),'low_faces',len(low.data.polygons))

def bake(name,role):
    job=JOBS[name];low=job['low'];high=job['high'];target=job['target']
    # Temporarily use emission for data/color channels, keeping high-poly reference UVs.
    saved=[]
    for mat in job['source_materials']:
        ns=mat.node_tree.nodes;lk=mat.node_tree.links;output=next(n for n in ns if n.type=='OUTPUT_MATERIAL');bs=next(n for n in ns if n.type=='BSDF_PRINCIPLED')
        if role!='normal':
            previous=output.inputs['Surface'].links[0].from_socket
            emission=ns.new('ShaderNodeEmission')
            if role=='basecolor':lk.new(bs.inputs['Base Color'].links[0].from_socket,emission.inputs['Color'])
            else:
                # Bake supplied roughness and AO into R/G, without baking lighting.
                tex=next(n for n in ns if n.type=='TEX_IMAGE' and n.image and ('surface_ra' in n.image.filepath))
                lk.new(tex.outputs['Color'],emission.inputs['Color'])
            lk.new(emission.outputs[0],output.inputs['Surface']);saved.append((mat,output,previous,emission))
    im=bpy.data.images.new('Bake_'+name+'_'+role,width=2048,height=2048,alpha=False,float_buffer=True)
    im.colorspace_settings.name='sRGB' if role=='basecolor' else 'Non-Color'
    nodes=target.node_tree.nodes;node=nodes.new('ShaderNodeTexImage');node.image=im;nodes.active=node
    select_only([high,low],low)
    SC.render.bake.use_clear=True
    bpy.ops.object.bake(type='NORMAL' if role=='normal' else 'EMIT')
    for mat,out,previous,emission in saved:mat.node_tree.links.new(previous,out.inputs['Surface']);mat.node_tree.nodes.remove(emission)
    # Runtime UV0 flips Blender V for top-left PNG sampling. Export recomputes the
    # corresponding tangent basis, so +Y normal data must be converted here too.
    if role=='normal':
        # Standard +Y Blender/FBX bake, before engine UV-basis conversion.
        fbxdir=OUT/'FBXTextures';fbxdir.mkdir(exist_ok=True)
        im.filepath_raw=str(fbxdir/(name+'_normal.png'));im.file_format='PNG';im.save()
        import numpy as np
        pixels=np.empty(2048*2048*4,dtype=np.float32);im.pixels.foreach_get(pixels);pixels[1::4]=1-pixels[1::4]
        runtime=bpy.data.images.new('RuntimeNormal_'+name,width=2048,height=2048,alpha=False,float_buffer=False)
        runtime.colorspace_settings.name='Non-Color';runtime.pixels.foreach_set(pixels);im=runtime
    stem=('bark' if name.startswith('trunk') else 'rock')+'_'+name.rsplit('_',1)[1]
    im.filepath_raw=str(BAKED/(stem+'_'+role+'.png'));im.file_format='PNG';im.save()
    if role=='normal':
        # Main baked normal already includes high-poly and reference normal relief;
        # a neutral detail map avoids applying that relief twice.
        flat=bpy.data.images.new('Detail_'+name,width=2048,height=2048,alpha=False)
        flat.generated_color=(.5,.5,1,1);flat.colorspace_settings.name='Non-Color';flat.filepath_raw=str(BAKED/(stem+'_detail.png'));flat.file_format='PNG';flat.save()
    job.setdefault('images',{})[role]=im
    high.hide_render=True;high.hide_set(True)
    print(im.filepath_raw)

def build_lods():
    # Game-ready LODs preserve UV0 and tangent-space bake correspondence.
    for base in [o for o in SC.objects if o.type=='MESH' and o.name.startswith(('trunk_','leaf_','rock_','grass_')) and '_lod' not in o.name]:
        base.location=(0,0,0)
        for level,ratio in [(1,.50),(2,.20)]:
            ob=base.copy();ob.data=base.data.copy();ob.name=base.name+'_lod'+str(level);SC.collection.objects.link(ob)
            if base.name.startswith(('leaf','grass','trunk')):
                # Remove complete independent curved cards, retaining atlas UVs. A
                # spatially distributed subset avoids intersections and giant holes.
                import bmesh
                bm=bmesh.new();bm.from_mesh(ob.data);visited=set();components=[]
                for v in bm.verts:
                    if v in visited:continue
                    todo=[v];visited.add(v);verts=[]
                    while todo:
                        item=todo.pop();verts.append(item)
                        for edge in item.link_edges:
                            other=edge.other_vert(item)
                            if other not in visited:visited.add(other);todo.append(other)
                    components.append(verts)
                remove=[]
                for i,verts in enumerate(components):
                    if base.name.startswith('trunk'):
                        drop=len(verts)<36 and (level==2 or i%2==0)
                    else:drop=((i*2654435761)&0xffff)/65536 >= ratio
                    if drop:remove.extend(verts)
                bmesh.ops.delete(bm,geom=remove,context='VERTS');bm.to_mesh(ob.data);bm.free()
            else:
                mod=ob.modifiers.new('LODDecimation','DECIMATE');mod.ratio=ratio;mod.use_collapse_triangulate=True;apply_modifier(ob,mod.name)
            ART.append(ob)
    # Conservative simple contact proxies, exported in metres at the same pivots.
    for name in ['trunk_'+str(i) for i in range(3)]+['rock_'+str(i) for i in range(4)]:
        if name.startswith('trunk'):
            bpy.ops.mesh.primitive_cylinder_add(vertices=12,radius=.30,depth=2.2,location=(0,0,1.1))
        else:
            bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=1,radius=1,location=(0,0,.6));bpy.context.object.scale=(1.1,.95,.72)
        ob=bpy.context.object;ob.name='UCX_'+name+'_00';select_only([ob],ob);bpy.ops.object.transform_apply(location=True,rotation=True,scale=True);ART.append(ob);ob.hide_render=True;ob.hide_set(True)

def finish():
    assets=[o for o in SC.objects if o.type=='MESH' and o.name.startswith(('trunk_','leaf_','rock_','grass_'))]
    meshes=[];stats=[]
    for ob in assets:
        mesh=ob.data;mesh.calc_loop_triangles();is_baked=ob.name.startswith(('trunk','rock'))
        # Keep working Blender UVs intact; temporary mesh uses runtime UV orientation.
        copy=mesh.copy()
        if is_baked:
            for loop in copy.uv_layers.active.data:loop.uv.y=1-loop.uv.y
        copy.calc_loop_triangles();copy.calc_tangents(uvmap=copy.uv_layers.active.name);vertices=[]
        for tri in copy.loop_triangles:
            for li in tri.loops:
                loop=copy.loops[li];p=copy.vertices[loop.vertex_index].co;n=loop.normal;t=loop.tangent;uv=copy.uv_layers.active.data[li].uv
                material=int(ob.name.split('_')[1]) if is_baked else tri.material_index
                vertices.append([p.x,p.z,-p.y,n.x,n.z,-n.y,uv.x,uv.y,t.x,t.z,-t.y,loop.bitangent_sign,material])
        meshes.append({'name':ob.name,'vertices':vertices});stats.append({'name':ob.name,'triangles':len(vertices)//3,'units':'metres','pivot':'ground origin','uv0':True,'normal':True,'tangent':True})
        bpy.data.meshes.remove(copy)
    (OUT/'environment_meshes.json').write_text(json.dumps({'version':1,'meshes':meshes},separators=(',',':')),encoding='utf-8')
    (OUT/'asset_manifest.json').write_text(json.dumps({'units':'metres','assets':stats,'highpoly':[o.name for o in SC.objects if o.name.startswith('HP_')],'collision':[o.name for o in SC.objects if o.name.startswith('UCX_')]},indent=2),encoding='utf-8')
    material={"version":1}
    for group,count in [('bark',3),('rock',4)]:
        material[group]=[{role:'baked/'+group+'_'+str(i)+'_'+suffix+'.png' for role,suffix in [('base_color','basecolor'),('normal','normal'),('surface_ra','surface'),('detail_normal','detail')]} for i in range(count)]
    material['foliage_normals']={g:['baked/foliage_'+g+'_atlas_'+str(i)+'_normal.png' for i in range(n)] for g,n in [('grass',2),('leaf',3)]}
    (PACK/'baked_materials.json').write_text(json.dumps(material,indent=2),encoding='utf-8')
    for name,job in JOBS.items():
        mat=job['target'];ns=mat.node_tree.nodes;lk=mat.node_tree.links;bs=ns.get('Principled BSDF')
        for role in ['basecolor','surface','normal']:
            tex=ns.new('ShaderNodeTexImage')
            if role=='normal':
                tex.image=bpy.data.images.load(str(OUT/'FBXTextures'/(name+'_normal.png')),check_existing=True);tex.image.colorspace_settings.name='Non-Color'
                nm=ns.new('ShaderNodeNormalMap');lk.new(tex.outputs['Color'],nm.inputs['Color']);lk.new(nm.outputs['Normal'],bs.inputs['Normal'])
            else:
                tex.image=job['images'][role]
                if role=='basecolor':lk.new(tex.outputs['Color'],bs.inputs['Base Color'])
                else:
                    sep=ns.new('ShaderNodeSeparateColor');lk.new(tex.outputs['Color'],sep.inputs['Color']);lk.new(sep.outputs['Red'],bs.inputs['Roughness'])
    # One FBX per archetype with all three LOD objects and collision proxy.
    for group,count in [('trunk',3),('leaf',3),('rock',4),('grass',4)]:
        for i in range(count):
            name=group+'_'+str(i);objects=[bpy.data.objects[name],bpy.data.objects[name+'_lod1'],bpy.data.objects[name+'_lod2']]
            collision=bpy.data.objects.get('UCX_'+name+'_00')
            if collision:objects.append(collision)
            select_only(objects,objects[0]);bpy.ops.export_scene.fbx(filepath=str(OUT/(name+'.fbx')),use_selection=True,object_types={'MESH'},apply_unit_scale=True,apply_scale_options='FBX_SCALE_UNITS',axis_forward='-Z',axis_up='Y',use_tspace=True,mesh_smooth_type='FACE',bake_anim=False,path_mode='RELATIVE',add_leaf_bones=False)
            if collision:collision.hide_render=True;collision.hide_set(True)
    # Dedicated FBX export converts local foliage UVs to full atlas coordinates.
    exec(compile((ROOT/'tools/art/export_environment_fbx.py').read_text(encoding='utf-8'),'export_environment_fbx.py','exec'),{})
    for ob in SC.objects:
        if ob.name.startswith(('HP_','UCX_')) or '_lod' in ob.name:ob.hide_render=True;ob.hide_set(True)
    for im in bpy.data.images:
        if im.filepath and not im.filepath.startswith('//'):
            try:im.filepath=bpy.path.relpath(im.filepath,start=str(OUT))
            except:pass
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT/'project_hs_environment_v9_baked.blend'))
    print(json.dumps(stats))
