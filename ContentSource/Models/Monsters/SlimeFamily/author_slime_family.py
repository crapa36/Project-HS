"""Production SlimeFamily authoring. Run build(), export(), then studio() via Blender MCP.

Meter-space, bottom-center origins, shared 14-control rig, UV-painted face and
closed sculpted surfaces. New scenes preserve existing Blender work on reruns.
"""
import bpy
import math
import json
from pathlib import Path
from mathutils import Vector, Matrix
import numpy as np

BASE = Path(__file__).resolve().parent
ANIMS = BASE.parents[2] / 'Animations' / 'Monsters' / 'SlimeFamily'
SEGMENTS, RINGS, SIZE = 80, 40, 1024
HEIGHT = .64
CONTROLS = {
    'root': ((0, 0, 0), None),
    'body_center': ((0, 0, .24), 'root'),
    'body_top': ((0, .01, .50), 'body_center'),
    'body_front': ((0, -.34, .27), 'body_center'),
    'body_back': ((0, .34, .27), 'body_center'),
    'body_left': ((-.35, 0, .27), 'body_center'),
    'body_right': ((.35, 0, .27), 'body_center'),
    'face_left': ((-.18, -.42, .37), 'body_front'),
    'face_right': ((.18, -.42, .37), 'body_front'),
    'mouth': ((0, -.48, .26), 'body_front'),
    'ornament_left': ((-.49, .015, .37), 'body_left'),
    'ornament_right': ((.49, .015, .37), 'body_right'),
    'flower_stem': ((0, .02, .60), 'body_top'),
    'flower_head': ((0, .02, .79), 'flower_stem'),
}
COLORS = {'Melee': (.015, .65, .82), 'Ranged': (.24, .68, .12),
          'Suicide': (.98, .37, .075), 'Projectile': (.25, .80, .12)}


def textures(variant, stem, folder):
    u, v = np.meshgrid((np.arange(SIZE) + .5) / SIZE,
                       (np.arange(SIZE) + .5) / SIZE)
    bu = (u - .015) / .72
    theta = (bu - .5) * 2 * math.pi
    base = np.array(COLORS[variant])
    # Broad authored color depth: saturated foot, warm luminous center, cool crown.
    front = np.exp(-((bu-.5)/.26)**2)
    lift = .12*np.sin(v*math.pi) + .045*np.cos(theta-.6)*np.sin(v*math.pi)
    rgb = base[None,None,:] * (.62 + .37*v[...,None])
    rgb = np.broadcast_to(rgb, (SIZE,SIZE,3)).copy()
    rgb += lift[...,None] * np.array((.65, .82, .85))
    glow = .12 * front * np.exp(-((v-.37)/.24)**2)
    rgb += glow[...,None] * np.array((.35, .80, .56))
    opacity = np.full((SIZE,SIZE), .68)
    roughness = .24 + .045*(1-v)
    def ellipse(x, y, rx, ry, softness=.012):
        d=((bu-x)/rx)**2+((v-y)/ry)**2
        return np.clip((1-d)/softness,0,1)
    def paint(mask, color, alpha=1.0, rough=.24):
        nonlocal rgb, opacity, roughness
        rgb=rgb*(1-mask[...,None])+np.array(color)*mask[...,None]
        opacity=opacity*(1-mask)+alpha*mask
        roughness=roughness*(1-mask)+rough*mask
    if variant != 'Projectile':
        for x in (.434,.566):
            # Soft colored eye rim, a deep navy oval, and asymmetric catchlights.
            rim=ellipse(x,.57,.0365,.081, .15)
            paint(rim,base*.72,1,.31)
            paint(ellipse(x,.573,.032,.073),(.012,.029,.052),1,.18)
            paint(ellipse(x-.009,.595,.0105,.021),(.93,.99,1),1,.17)
            paint(ellipse(x+.010,.548,.0055,.010),(.30,.71,.75),1,.24)
        if variant == 'Ranged':
            paint(ellipse(.5,.445,.015,.022),(.025,.09,.026),1,.25)
            paint(ellipse(.5,.438,.006,.005),(.60,.83,.37),1,.3)
        else:
            mouth=ellipse(.5,.437,.023,.025)
            mouth*=1-ellipse(.5,.455,.029,.021)
            paint(mouth,(.02,.055,.07),1,.25)
        # The restrained soft reflection complements runtime coat lighting.
        reflection=ellipse(.367,.773,.028,.034,.60)*.55
        paint(reflection,(.70,.94,.90),.76,.20)
        paint(ellipse(.402,.809,.008,.015,.12),(.82,.98,.94),
              .80,.18)
    # Reserved atlas strips for sculpted side droplets, stem, petals, flower center.
    for lo,hi,color,alpha,rough in [(.75,.8125,base, .72,.22),
                                   (.8125,.875,(.17,.39,.065),1,.42),
                                   (.875,.9375,(1,.85,.58),1,.34),
                                   (.9375,1,(1,.56,.055),1,.32)]:
        mask=((u>=lo)&(u<hi)).astype(float)
        paint(mask,color,alpha,rough)
    rgba=np.concatenate([np.clip(rgb,0,1),opacity[...,None]],axis=-1).astype(np.float32)
    normal=np.empty((SIZE,SIZE,4),dtype=np.float32)
    normal[:,:,0]=.5+.007*np.sin(theta*3)*np.sin(v*math.pi)**2
    normal[:,:,1]=.5+.006*np.sin(v*4*math.pi)
    normal[:,:,2]=np.sqrt(1-(normal[:,:,0]*2-1)**2-(normal[:,:,1]*2-1)**2)*.5+.5
    normal[:,:,3]=roughness
    normal[u>=.75,:3]=(.5,.5,1)
    folder.mkdir(parents=True,exist_ok=True)
    result=[]
    for suffix,pixels in [('BaseColor',rgba),('Normal',normal)]:
        im=bpy.data.images.new(stem+'_'+suffix,width=SIZE,height=SIZE,alpha=True)
        if suffix=='Normal': im.colorspace_settings.name='Non-Color'
        im.pixels.foreach_set(pixels.ravel())
        im.filepath_raw=str(folder/(stem+'_'+suffix+'.png')); im.file_format='PNG'; im.save()
        result.append(im)
    return result


def make_material(variant, stem, images):
    mat=bpy.data.materials.new(stem+'_Jelly'); mat.use_nodes=True
    nodes=mat.node_tree.nodes; links=mat.node_tree.links; p=nodes.get('Principled BSDF')
    p.inputs['Roughness'].default_value=.25
    p.inputs['IOR'].default_value=1.36
    p.inputs['Coat Weight'].default_value=.38
    p.inputs['Coat Roughness'].default_value=.19
    p.inputs['Subsurface Weight'].default_value=.14
    tex=nodes.new('ShaderNodeTexImage'); tex.image=images[0]
    links.new(tex.outputs['Color'],p.inputs['Base Color'])
    nt=nodes.new('ShaderNodeTexImage'); nt.image=images[1]
    nm=nodes.new('ShaderNodeNormalMap'); nm.inputs['Strength'].default_value=.65
    links.new(nt.outputs['Color'],nm.inputs['Color']); links.new(nm.outputs['Normal'],p.inputs['Normal'])
    links.new(nt.outputs['Alpha'],p.inputs['Roughness'])
    # Alpha is runtime coverage; export must retain it while Blender shows the same mass.
    links.new(tex.outputs['Alpha'],p.inputs['Alpha'])
    if variant!='Melee':
        p.inputs['Transmission Weight'].default_value=.18
        mat.surface_render_method='DITHERED'
    return mat


def new_scene(variant):
    scene=bpy.data.scenes.new('SlimeFamily_'+variant)
    bpy.context.window.scene=scene
    scene.unit_settings.system='METRIC'; scene.unit_settings.scale_length=1
    scene.render.fps=60
    collection=bpy.data.collections.new('SlimeFamily_'+variant+'_Asset')
    scene.collection.children.link(collection)
    scene['variant']=variant; scene['asset_collection']=collection.name
    return scene,collection


def make_mesh(variant, collection, mat):
    verts=[(0,0,0)]; faces=[]; uvfaces=[]; bindings=[{'root':1.0}]
    def weights(x,y,z):
        w={'body_center':.4,'body_top':max(0,(z-.20)/.44)*.65,
           'body_front':max(0,-y/.5)*.7,'body_back':max(0,y/.5)*.55,
           'body_left':max(0,-x/.5)*.55,'body_right':max(0,x/.5)*.55}
        for bone,sigma,gain in [('face_left',.13,1.7),('face_right',.13,1.7),
                                ('mouth',.105,1.8)]:
            p=CONTROLS[bone][0]
            w[bone]=gain*math.exp(-sum((a-b)**2 for a,b in zip((x,y,z),p))/(sigma*sigma))
        chosen=sorted(w.items(),key=lambda item:item[1],reverse=True)[:4]
        total=sum(v for _,v in chosen)
        return {n:v/total for n,v in chosen if v>0}
    for j in range(1,RINGS):
        a=math.pi*j/RINGS; z=HEIGHT*(1-math.cos(a))/2
        radius=.50*math.sin(a)**.68*(1.105-.21*z/HEIGHT)
        for i in range(SEGMENTS):
            t=2*math.pi*(i/SEGMENTS-.5)
            r=radius*(1+.014*math.cos(t*3)*math.exp(-z/.10))
            x=r*math.sin(t); y=-r*math.cos(t)+.018*(z/HEIGHT)**2
            verts.append((x,y,z)); bindings.append(weights(x,y,z))
    verts.append((0,.018,HEIGHT)); bindings.append(weights(0,.018,HEIGHT)); top=len(verts)-1
    for i in range(SEGMENTS):
        k=(i+1)%SEGMENTS
        faces.append((0,1+k,1+i)); uvfaces.append(((.015+(i+.5)/SEGMENTS*.72,0),(.015+(i+1)/SEGMENTS*.72,1/RINGS),(.015+i/SEGMENTS*.72,1/RINGS)))
    for j in range(RINGS-2):
        for i in range(SEGMENTS):
            k=(i+1)%SEGMENTS; a=1+j*SEGMENTS; b=a+SEGMENTS
            faces.append((a+i,a+k,b+k,b+i))
            uvfaces.append(((.015+i/SEGMENTS*.72,(j+1)/RINGS),(.015+(i+1)/SEGMENTS*.72,(j+1)/RINGS),(.015+(i+1)/SEGMENTS*.72,(j+2)/RINGS),(.015+i/SEGMENTS*.72,(j+2)/RINGS)))
    for i in range(SEGMENTS):
        k=(i+1)%SEGMENTS; a=1+(RINGS-2)*SEGMENTS
        faces.append((a+i,a+k,top)); uvfaces.append(((.015+i/SEGMENTS*.72,(RINGS-1)/RINGS),(.015+(i+1)/SEGMENTS*.72,(RINGS-1)/RINGS),(.015+(i+.5)/SEGMENTS*.72,1)))

    def ellipsoid(center,scale,bone,strip,rotation=0,teardrop=False,segments=24,rings=12):
        start=len(verts); cx,cy,cz=center; sx,sy,sz=scale
        texcoords={start:(.5,0)}
        def point(a,t):
            shape=1-.30*math.cos(a) if teardrop else 1
            x=sx*math.sin(a)*math.cos(t)*shape
            y=sy*math.sin(a)*math.sin(t)*shape
            z=sz*math.cos(a)
            return (cx+x*math.cos(rotation)+z*math.sin(rotation),cy+y,cz-x*math.sin(rotation)+z*math.cos(rotation))
        verts.append(point(math.pi,0)); bindings.append({bone:1})
        for j in range(1,rings):
            for i in range(segments):
                texcoords[len(verts)]=(i/segments,j/rings)
                verts.append(point(math.pi-j*math.pi/rings,2*math.pi*i/segments)); bindings.append({bone:1})
        end=len(verts); texcoords[end]=(.5,1); verts.append(point(0,0)); bindings.append({bone:1})
        fs=[]
        for i in range(segments):fs.append((start,start+1+(i+1)%segments,start+1+i))
        for j in range(rings-2):
            for i in range(segments):
                a=start+1+j*segments+i; b=start+1+j*segments+(i+1)%segments
                fs.append((a,b,b+segments,a+segments))
        for i in range(segments):fs.append((start+1+(rings-2)*segments+i,start+1+(rings-2)*segments+(i+1)%segments,end))
        for f in fs:
            faces.append(f)
            us=[texcoords[idx][0] for idx in f if idx not in (start,end)]
            seam=max(us)-min(us)>.5
            us=[q+1 if seam and q<.5 else q for q in us]
            coords=[]
            for idx in f:
                q,h=texcoords[idx]
                q=sum(us)/len(us) if idx in (start,end) else (q+1 if seam and q<.5 else q)
                coords.append((strip+.004+.054*q,.05+.90*h))
            uvfaces.append(tuple(coords))
    if variant=='Ranged':
        for sign in (-1,1):
            ellipsoid((sign*.485,.015,.375),(.065,.078,.145),'ornament_left' if sign<0 else 'ornament_right',.75,sign*.65,True)
    if variant=='Suicide':
        ellipsoid((0,.020,.695),(.020,.020,.10),'flower_stem',.8125,rotation=-.10,segments=16,rings=10)
        # Five softly cupped petals in a tilted flower disk, closed and overlapping.
        for i in range(5):
            t=2*math.pi*i/5
            ellipsoid((.065*math.sin(t),.018+.045*math.cos(t),.795+.012*math.cos(t)),(.043,.068,.025),'flower_head',.875,rotation=.2*math.sin(t),segments=20,rings=10)
        ellipsoid((0,-.008,.805),(.043,.038,.028),'flower_head',.9375,segments=24,rings=12)
    mesh=bpy.data.meshes.new('Slime'+variant+'_Surface'); mesh.from_pydata(verts,[],faces); mesh.update()
    obj=bpy.data.objects.new('Slime'+variant,mesh); collection.objects.link(obj); mesh.materials.append(mat)
    uv=mesh.uv_layers.new(name='UVMap')
    for p,coords in zip(mesh.polygons,uvfaces):
        p.use_smooth=True
        for li,co in zip(p.loop_indices,coords):uv.data[li].uv=co
    return obj,bindings


def make_rig(variant,collection,obj,bindings):
    data=bpy.data.armatures.new('SlimeFamilySkeleton'); rig=bpy.data.objects.new('Slime'+variant+'_Rig',data)
    collection.objects.link(rig); bpy.context.view_layer.objects.active=rig; rig.select_set(True)
    bpy.ops.object.mode_set(mode='EDIT')
    controls={"root":CONTROLS["root"]} if variant=="Projectile" else CONTROLS
    for name,(pos,parent) in controls.items():
        bone=data.edit_bones.new(name); bone.head=pos; bone.tail=(pos[0],pos[1],pos[2]+.06)
        if parent:bone.parent=data.edit_bones[parent]
    bpy.ops.object.mode_set(mode='OBJECT')
    groups={n:obj.vertex_groups.new(name=n) for n in controls}
    for i,weights in enumerate(bindings):
        for name,value in weights.items():groups[name].add([i],value,'REPLACE')
    modifier=obj.modifiers.new('Shared jelly skin','ARMATURE'); modifier.object=rig; obj.parent=rig
    return rig


def build():
    scenes=[]
    for variant in ('Melee','Ranged','Suicide'):
        scene,collection=new_scene(variant); stem='Slime'+variant; folder=BASE/variant
        mat=make_material(variant,stem,textures(variant,stem,folder/(stem+'.fbm')))
        obj,bindings=make_mesh(variant,collection,mat); rig=make_rig(variant,collection,obj,bindings)
        scene['mesh']=obj.name; scene['rig']=rig.name; scenes.append(scene.name)
    # Authored closed sphere, prewarped for the fixed projectile presentation scale.
    scene,collection=new_scene('Projectile'); stem='SlimeGelProjectile'
    mat=make_material('Projectile',stem,textures('Projectile',stem,BASE/'Projectile'/(stem+'.fbm')))
    bpy.ops.mesh.primitive_uv_sphere_add(segments=32,ring_count=16,radius=.5)
    obj=bpy.context.object; obj.name=stem
    for c in list(obj.users_collection):c.objects.unlink(obj)
    collection.objects.link(obj)
    for vertex in obj.data.vertices:vertex.co.y*=.30
    obj.data.materials.append(mat)
    for p in obj.data.polygons:p.use_smooth=True
    rig=make_rig('Projectile',collection,obj,[{'root':1} for _ in obj.data.vertices])
    scene['mesh']=obj.name; scene['rig']=rig.name; scenes.append(scene.name)
    bpy.context.window_manager['slime_family_scenes']=scenes
    print(json.dumps({'scenes':scenes,'controls':list(CONTROLS)}))


def pose(rig,frame,width=1,depth=1,height=1,hop=0,front=0,cheeks=0,mouth=1,strain=0,flower=0):
    for bone in rig.pose.bones:
        bone.location=(0,0,0); bone.rotation_mode='XYZ'; bone.rotation_euler=(0,0,0); bone.scale=(1,1,1)
    center=rig.pose.bones['body_center']; center.scale=(width,height,depth)
    center.location=(0,.24*(height-1)+hop,0)
    rig.pose.bones['body_front'].location=(0,0,front)
    rig.pose.bones['body_top'].location=(0,0,front*.25)
    for name,sign in [('face_left',-1),('face_right',1)]:
        b=rig.pose.bones[name]; b.location=(sign*cheeks,0,cheeks*.25); b.scale=(1+cheeks*2,1-strain*.30,1+cheeks)
        b.rotation_euler.z=sign*strain*.15
    b=rig.pose.bones['mouth'];b.scale=(mouth,mouth,1);b.location=(0,0,cheeks*.6)
    rig.pose.bones['ornament_left'].rotation_euler.z=flower*.3
    rig.pose.bones['ornament_right'].rotation_euler.z=-flower*.3
    rig.pose.bones['flower_stem'].rotation_euler.x=flower*.35
    rig.pose.bones['flower_head'].rotation_euler.z=flower*.25
    for bone in rig.pose.bones:
        for channel in ('location','rotation_euler','scale'):bone.keyframe_insert(channel,frame=frame,group=bone.name)


def author_actions(scene,rig,variant):
    rig.animation_data_clear(); actions={}
    def action(name,keys):
        a=bpy.data.actions.new('Slime'+variant+'_'+name); a.use_fake_user=True
        rig.animation_data_create(); rig.animation_data.action=a
        for frame,kwargs in keys:pose(rig,frame,**kwargs)
        actions[name]=(a,keys[-1][0])
    action('Idle',[(1,{}),(31,dict(width=1.018,depth=1.018,height=.965,flower=.08)),(61,{}),(91,dict(width=.985,depth=.985,height=1.03,flower=-.05)),(121,{})])
    action('Run',[(1,dict(width=1.13,depth=1.1,height=.79)),(10,dict(width=.92,depth=.94,height=1.16,hop=.025,flower=-.4)),(23,dict(width=.965,depth=.98,height=1.06,hop=.085,flower=.25)),(37,dict(hop=.02,flower=.12)),(46,dict(width=1.13,depth=1.1,height=.79))])
    if variant=='Melee':
        draw=[(1,{}),(8,dict(width=1.13,depth=1.06,height=.73,front=-.025)),(14,dict(width=1.16,depth=1.08,height=.68,front=-.035)),(18,dict(width=.97,depth=1.03,height=1.02,front=.15)),(20,dict(front=.07)),(22,{})]
    elif variant=='Ranged':
        draw=[(1,{}),(10,dict(width=1.035,height=.92,cheeks=.025,mouth=.85)),(23,dict(width=1.07,height=.88,cheeks=.075,mouth=.55,strain=.3)),(28,dict(width=1.05,height=.92,cheeks=.085,mouth=.46,strain=.35)),(31,dict(width=.98,height=1.02,front=.035,cheeks=.035,mouth=.62))]
    else:
        draw=[(1,{}),(10,dict(width=1.035,depth=1.03,height=.96,flower=.1)),(25,dict(width=1.12,depth=1.10,height=1.05,cheeks=.045,strain=.45,flower=-.16)),(40,dict(width=1.22,depth=1.20,height=1.16,cheeks=.07,strain=.8,flower=.2)),(49,dict(width=1.29,depth=1.27,height=1.22,cheeks=.09,strain=1,flower=-.2))]
    action('Draw',draw)
    action('Recoil',[(1,dict(height=.98,cheeks=.03 if variant=='Ranged' else 0)),(5,dict(width=1.09,depth=.92,height=.83,front=-.065,flower=-.3)),(12,dict(width=.98,depth=1.01,height=1.04,flower=.12)),(19,{})])
    action('Death',[(1,{}),(9,dict(width=1.10,depth=1.08,height=.70,flower=-.3)),(22,dict(width=1.31,depth=1.27,height=.27,flower=-1)),(39,dict(width=1.43,depth=1.40,height=.07,flower=-1.5)),(49,dict(width=1.43,depth=1.40,height=.07,flower=-1.5))])
    return actions


def export():
    reports=[]
    for scene_name in bpy.context.window_manager['slime_family_scenes']:
        scene=bpy.data.scenes[scene_name]; bpy.context.window.scene=scene
        variant=scene['variant']; obj=scene.objects[scene['mesh']]; rig=scene.objects[scene['rig']]
        folder=BASE/variant; stem='SlimeGelProjectile' if variant=='Projectile' else 'Slime'+variant
        rig.animation_data_clear()
        for b in rig.pose.bones:b.location=(0,0,0);b.rotation_euler=(0,0,0);b.scale=(1,1,1)
        scene.frame_set(1)
        for other in scene.objects:other.select_set(False)
        rig.select_set(True);obj.select_set(True);bpy.context.view_layer.objects.active=rig
        def fbx(path,animated):
            bpy.ops.export_scene.fbx(filepath=str(path),use_selection=True,object_types={'ARMATURE','MESH'},
                axis_forward='-Z',axis_up='Y',global_scale=1,apply_unit_scale=True,apply_scale_options='FBX_SCALE_UNITS',
                add_leaf_bones=False,use_armature_deform_only=False,bake_anim=animated,bake_anim_use_all_actions=False,
                bake_anim_use_nla_strips=False,bake_anim_force_startend_keying=True,bake_anim_step=1,
                bake_anim_simplify_factor=0,path_mode='AUTO',mesh_smooth_type='FACE')
        fbx(folder/(stem+'.fbx'),False)
        if variant!='Projectile':
            actions=author_actions(scene,rig,variant); (ANIMS/variant).mkdir(parents=True,exist_ok=True)
            for name,(action,end) in actions.items():
                rig.animation_data.action=action;scene.frame_start=1;scene.frame_end=end;scene.frame_set(1)
                fbx(ANIMS/variant/(name+'.fbx'),True)
            scene['actions']={name:action.name for name,(action,_) in actions.items()}
            rig.animation_data.action=actions['Idle'][0];scene.frame_start=1;scene.frame_end=121;scene.frame_set(1)
        reports.append({'variant':variant,'vertices':len(obj.data.vertices),'triangles':sum(len(p.vertices)-2 for p in obj.data.polygons),'bounds':list(obj.dimensions),'rig_controls':len(rig.data.bones)})
    (BASE/'asset_report.json').write_text(json.dumps(reports,indent=2)+'\n',encoding='utf-8')
    bpy.ops.wm.save_as_mainfile(filepath=str(BASE/'SlimeFamily.blend'))
    print(json.dumps(reports))


def studio():
    scene=bpy.data.scenes.new('SlimeFamily_Studio'); bpy.context.window.scene=scene
    for index,scene_name in enumerate(bpy.context.window_manager['slime_family_scenes'][:3]):
        source=bpy.data.scenes[scene_name]
        instance=bpy.data.objects.new('Review_'+source['variant'],None)
        instance.instance_type='COLLECTION';instance.instance_collection=bpy.data.collections[source['asset_collection']]
        instance.location=(1.3*(index-1),0,0);scene.collection.objects.link(instance)
    world=bpy.data.worlds.new('SlimeFamily_World');world.use_nodes=True
    world.node_tree.nodes['Background'].inputs[0].default_value=(.10,.16,.21,1)
    world.node_tree.nodes['Background'].inputs[1].default_value=.35;scene.world=world
    def aim(o,target):o.rotation_euler=(Vector(target)-o.location).to_track_quat('-Z','Y').to_euler()
    for name,loc,power,size in [('Key',(-2,-3,4),500,3.5),('Fill',(3,-1,2),260,3),('Rim',(0,3,3),650,2.5)]:
        data=bpy.data.lights.new('Family_'+name,'AREA');data.energy=power;data.shape='DISK';data.size=size
        o=bpy.data.objects.new('Family_'+name,data);scene.collection.objects.link(o);o.location=loc;aim(o,(0,0,.4))
    bpy.ops.mesh.primitive_plane_add(size=200);floor=bpy.context.object;floor.name='Studio_Floor'
    material=bpy.data.materials.new('Studio_Matte');material.diffuse_color=(.055,.085,.105,1);material.use_nodes=True
    material.node_tree.nodes['Principled BSDF'].inputs['Base Color'].default_value=(.055,.085,.105,1)
    material.node_tree.nodes['Principled BSDF'].inputs['Roughness'].default_value=.85;floor.data.materials.append(material)
    data=bpy.data.cameras.new('Family_Camera');cam=bpy.data.objects.new('Family_Camera',data);scene.collection.objects.link(cam)
    cam.location=(1.6,-6,3);aim(cam,(0,0,.30));data.type='ORTHO';data.ortho_scale=4.8;scene.camera=cam
    scene.render.engine='CYCLES';scene.cycles.samples=48
    scene.render.resolution_x=1500;scene.render.resolution_y=750;scene.render.resolution_percentage=100
    scene.view_settings.view_transform='AgX';scene.render.image_settings.file_format='PNG'
    scene.render.filepath=str(BASE/'family_preview.png')
    bpy.ops.wm.save_as_mainfile(filepath=str(BASE/'SlimeFamily.blend'))
    bpy.ops.render.render(write_still=True)
