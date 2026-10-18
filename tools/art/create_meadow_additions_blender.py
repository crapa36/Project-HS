"""Meadow v004 source props. geometry -> reviewed blockout -> finish -> verify.
No registry mutation. New props use ground_detail array layer 1; optimized existing props keep layer 0.
"""
import argparse, hashlib, importlib.util, inspect, json, math, random, sys
from pathlib import Path
import bpy, bmesh
import numpy as np
from mathutils import Vector

parser=argparse.ArgumentParser()
parser.add_argument('--root',type=Path,required=True)
parser.add_argument('--stage',choices=('geometry','finish','verify'),required=True)
args=parser.parse_args(sys.argv[sys.argv.index('--')+1:]);ROOT=args.root.resolve()
OUT=ROOT/'ContentSource/Models/Environment/MeadowProps/v004'
TEX=ROOT/'ContentSource/Textures/Environment/textures'
for directory in ('','preview','verification','FBX','FBXTextures'):(OUT/directory).mkdir(parents=True,exist_ok=True)
NAMES=('fence_broken','fence_splitrail','sack_tied','fallen_log','rock_mossy')
EXISTING=('crate_intact','crate_broken','barrel_intact','barrel_broken','cart_broken')
RADII=(1.35,1.40,.50,1.75,1.05)
SCENE='HS_Meadow_Additions_v004';ASSETS='COL-meadow-additions-v004-assets';PREVIEWS='COL-meadow-additions-v004-preview'
ATLAS={'basecolor':'meadow_additions_v004_basecolor.png','surface':'meadow_additions_v004_surface_ra.png','normal':'meadow_additions_v004_normal.png'}
spec=importlib.util.spec_from_file_location('existing_props',ROOT/'tools/art/create_meadow_props_blender.py');p=importlib.util.module_from_spec(spec)
saved=sys.argv[:]
try:
    sys.argv=['blender','--','--root',str(ROOT),'--stage','verify'];spec.loader.exec_module(p)
finally:sys.argv=saved
g=p.g
# Raised grain sheets duplicate the normal map and account for 168 triangles on every old LOD0 board.
board_source=inspect.getsource(g.board)
assert 'segments=(7,4,2)[lod]' in board_source and '    if lod==0:\n' in board_source
board_source=board_source[:board_source.index('    if lod==0:\n')].replace('segments=(7,4,2)[lod]','segments=(4,3,2)[lod]')
exec(compile(board_source,'<reduced-board-recipe>','exec'),g.__dict__)


def read_pixels(path):
    image=bpy.data.images.load(str(path),check_existing=False);image.colorspace_settings.name='Non-Color'
    width,height=image.size
    data=np.rint(np.asarray(image.pixels[:],dtype=np.float32).reshape(height,width,4)[::-1]*255).astype(np.uint8)
    bpy.data.images.remove(image)
    return data


def atlas(destination=TEX):
    destination.mkdir(parents=True,exist_ok=True)
    arrays={role:read_pixels(TEX/g.ATLAS_NAMES[role]) for role in ATLAS}
    n=g.CELL-2*g.GUTTER;y,x=np.mgrid[0:n,0:n]/(n-1);rng=np.random.default_rng(4004)
    noise=rng.normal(0,.009,(n,n));sources=[]
    rock={role:read_pixels(TEX/('mesh_rock_'+{'basecolor':'basecolor','surface':'surface_ra','normal':'normal'}[role]+'.png')) for role in ATLAS}
    for name,tile in (('jute','daisy'),('rope','stem'),('stone','leaf_ochre'),('moss','leaf_brown')):
        i=g.TILES.index(tile);by,bx=(i//4)*g.CELL,(i%4)*g.CELL
        if name=='stone':
            for role in ATLAS:
                data=rock[role];sample=data[np.linspace(0,data.shape[0]-1,n).astype(int)[:,None],np.linspace(0,data.shape[1]-1,n).astype(int)[None,:]].copy()
                arrays[role][by:by+g.CELL,bx:bx+g.CELL]=np.pad(sample,((g.GUTTER,g.GUTTER),(g.GUTTER,g.GUTTER),(0,0)),mode='edge')
            continue
        if name=='jute':
            weave=np.sin(x*math.tau*77)*np.sin(y*math.tau*63)
            fibres=np.sin(x*math.tau*155+y*4)*.012+np.sin(y*math.tau*127)*.012
            dirt=np.exp(-((x-.25)/.26)**2-((y-.78)/.24)**2)*.08
            rgb=np.array([.54,.425,.285])+(weave*.024+fibres+noise-dirt)[...,None]
            height=weave*.015+fibres*.25;rough=.92;ao=.97
        elif name=='rope':
            yarn=np.sin((x*18+y*34)*math.tau)
            rgb=np.array([.39,.285,.16])+(yarn*.035+noise)[...,None]
            height=yarn*.02;rough=.88;ao=.96
        else:
            def grain(resolution):
                grid=rng.random((resolution,resolution))
                coord=np.linspace(0,resolution-1.001,n);index=coord.astype(int);weight=coord-index
                weight=weight*weight*(3-2*weight)
                top=grid[index[:,None],index[None,:]]*(1-weight[None,:])+grid[index[:,None],index[None,:]+1]*weight[None,:]
                bottom=grid[index[:,None]+1,index[None,:]]*(1-weight[None,:])+grid[index[:,None]+1,index[None,:]+1]*weight[None,:]
                return top*(1-weight[:,None])+bottom*weight[:,None]
            mottle=grain(13)-.5;clusters=grain(47)-.5;particles=grain(173)-.5
            rgb=np.array([.205,.275,.065])+mottle[...,None]*[.19,.21,.075]+clusters[...,None]*[.075,.085,.02]+particles[...,None]*.025
            height=(grain(79)-.5)*.07+(grain(198)-.5)*.045+noise*.004;rough=.98;ao=.90
        dy,dx=np.gradient(height);strength=27 if name=='moss' else 15;normal=np.stack((-dx*strength,-dy*strength,np.ones_like(x)),axis=2);normal/=np.linalg.norm(normal,axis=2)[...,None]
        data={'basecolor':np.dstack((np.clip(rgb,0,1),np.ones_like(x))),
              'surface':np.dstack((np.full_like(x,rough),np.full_like(x,ao),np.zeros_like(x),np.ones_like(x))),
              'normal':np.dstack((normal*.5+.5,np.ones_like(x)))}
        for role in ATLAS:
            sample=np.rint(np.clip(data[role],0,1)*255).astype(np.uint8)
            arrays[role][by:by+g.CELL,bx:bx+g.CELL]=np.pad(sample,((g.GUTTER,g.GUTTER),(g.GUTTER,g.GUTTER),(0,0)),mode='edge')
    for role,data in arrays.items():g.write_png(destination/ATLAS[role],data[...,:3] if role=='normal' else data)
    normal=arrays['normal'].copy();normal[...,1]=255-normal[...,1];g.write_png(OUT/'FBXTextures/meadow_additions_v004_normal_opengl.png',normal)
    rough=np.repeat(arrays['surface'][...,:1],4,axis=2);rough[...,3]=255;g.write_png(OUT/'FBXTextures/meadow_additions_v004_roughness.png',rough)
    for name in list(g.ATLAS_NAMES.values())+['mesh_rock_'+role+'.png' for role in ('basecolor','surface_ra','normal')]:
        sources.append({'path':str((TEX/name).relative_to(ROOT)),'sha256':hashlib.sha256((TEX/name).read_bytes()).hexdigest()})
    manifest={'size':[2048,2048],'gutter_px':24,'opaque':True,'array_layer':1,'normal_convention':'Runtime UV V-down; Blender preview flips green','surface_channels':{'R':'roughness','G':'AO','B':'unused zero','A':'opaque'},'source_reuse':sources,'overrides':{'daisy':'jute','stem':'rope','leaf_ochre':'stone','leaf_brown':'moss'},'textures':{role:str((TEX/name).relative_to(ROOT)) for role,name in ATLAS.items()}}
    (OUT/'atlas_manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')


def material(texture_root=TEX):
    mat=bpy.data.materials.new('MAT-meadow-additions-v004');mat.use_nodes=True;mat.use_backface_culling=True
    nodes=mat.node_tree.nodes;links=mat.node_tree.links;bs=nodes.get('Principled BSDF');bs.inputs['Metallic'].default_value=0
    for role,path in [('basecolor',texture_root/ATLAS['basecolor']),('roughness',OUT/'FBXTextures/meadow_additions_v004_roughness.png'),('normal',OUT/'FBXTextures/meadow_additions_v004_normal_opengl.png')]:
        node=nodes.new('ShaderNodeTexImage');node.image=bpy.data.images.load(str(path),check_existing=False);node.image.colorspace_settings.name='sRGB' if role=='basecolor' else 'Non-Color'
        if role=='normal':
            normal=nodes.new('ShaderNodeNormalMap');links.new(node.outputs['Color'],normal.inputs['Color']);links.new(normal.outputs['Normal'],bs.inputs['Normal'])
        else:links.new(node.outputs['Color'],bs.inputs['Base Color' if role=='basecolor' else 'Roughness'])
    return mat


def blob(b,center,scale,lod,tile,seed,moss=False):
    mesh=bmesh.new();bmesh.ops.create_icosphere(mesh,subdivisions=(2,1,1)[lod] if moss else (3,2,1)[lod],radius=1)
    mesh.verts.ensure_lookup_table();center=Vector(center)
    if moss:scale=(scale[0]*.55,scale[1]*.55,scale[2]*1.4)
    for vertex in mesh.verts:
        v=vertex.co.copy()
        warp=1+(.22 if moss else .065)*math.sin(v.x*(10 if moss else 8)+seed)+(.12 if moss else .045)*math.sin(v.y*(14 if moss else 11)-v.z*7)
        rise=v.z*scale[2]*(1+(.40 if moss else .06)*math.sin(v.x*9+v.y*7+seed))
        if moss:rise+=.004*math.sin(v.x*17+v.y*13+seed)
        vertex.co=center+Vector((v.x*scale[0]*warp,v.y*scale[1]*warp,rise))
    for face in mesh.faces:
        points=[v.co.copy() for v in face.verts];normal=(points[1]-points[0]).cross(points[2]-points[0]);axis=max(range(3),key=lambda i:abs(normal[i]));uvaxes=[i for i in range(3) if i!=axis]
        uv=[(.5+(point[uvaxes[0]]-center[uvaxes[0]])/(scale[uvaxes[0]]*2.4),.5+(point[uvaxes[1]]-center[uvaxes[1]])/(scale[uvaxes[1]]*2.4)) for point in points]
        chosen=tile
        if not moss and tile=='leaf_ochre':
            middle=sum(points,Vector())/3
            if middle.z>.52+.025*math.sin(middle.x*13+middle.y*11):chosen='leaf_brown'
        b.face(points,uv,chosen,moss)
    points=[v.co.copy() for v in mesh.verts];mesh.free();return points


def broken_plank(b,start,end,width,depth,lod,seed,broken=False):
    part=g.Builder();p.plank(part,start,end,width,depth,lod,seed)
    if broken:
        direction=(Vector(end)-Vector(start)).normalized();length=(Vector(end)-Vector(start)).length
        part.vertices=[tuple(Vector(v)+direction*(.065*math.sin(i*2.1+seed)-.012) if (Vector(v)-Vector(start)).dot(direction)>length-.025 else Vector(v)) for i,v in enumerate(part.vertices)]
    offset=len(b.vertices);b.vertices.extend(part.vertices);b.faces.extend([[i+offset for i in face] for face in part.faces]);b.uvs.extend(part.uvs);b.smooth.extend(part.smooth)


def fence(lod,split=False):
    b=g.Builder()
    if split:
        for i,x in enumerate((-1.05,1.05)):
            for side in (-1,1):
                b.tube([(x,side*.10,0),(x+.02,side*.10,.48),(x+side*.045,side*.13,1.02)],[.066,.057,.042],'twig_bark',sides=(10,7,5)[lod])
        for j,z in enumerate((.32,.61,.86)):
            broken_plank(b,(-1.21,-.02,z+.02),(1.20,.01,z-.025),.095,.075,lod,440+j)
    else:
        for i,x in enumerate((-.98,1.03)):
            broken_plank(b,(x,0,0),(x+(.07 if i else -.03),.025,.93 if i else 1.0),.13,.13,lod,420+i)
        broken_plank(b,(-1.12,-.08,.72),(-.18,-.08,.68),.13,.055,lod,424,True)
        broken_plank(b,(.38,-.08,.70),(1.13,-.08,.72),.13,.055,lod,425,True)
        broken_plank(b,(-1.12,-.075,.37),(.14,-.075,.26),.12,.055,lod,426,True)
        broken_plank(b,(.13,-.11,.04),(1.08,-.075,.39),.12,.055,lod,427,True)
        broken_plank(b,(-.33,-.22,.025),(.50,-.29,.05),.09,.045,lod,428,True)
        if lod<2:
            for j in range(3):
                broken_plank(b,(-.19,-.081,.67+j*.025),(-.04+j*.018,-.083,.65+j*.03),.018,.014,lod,430+j,True)
    return b


def sack(lod):
    b=g.Builder();sides=(32,22,14)[lod];levels=(18,12,8)[lod];rings=[]
    controls=[(0,.18),(.035,.245),(.15,.29),(.31,.30),(.45,.27),(.55,.22),(.62,.115),(.665,.064),(.70,.068),(.77,.105)]
    for j in range(levels):
        t=j/(levels-1);z=.01+.76*t;r=np.interp(z,[x[0] for x in controls],[x[1] for x in controls]);ring=[]
        for k in range(sides):
            a=k*math.tau/sides;fold=(.009+.018*math.exp(-((z-.60)/.14)**2))*math.sin(a*9+z*6)+.008*math.sin(a*3-z*8)
            radius=r+fold
            ring.append(Vector((radius*math.cos(a)+.015*math.sin(z*5),radius*.85*math.sin(a),z+.005*math.sin(a*5)*t)))
        rings.append(ring)
    for j in range(levels-1):
        for k in range(sides):
            q=(k+1)%sides;b.face([rings[j][k],rings[j][q],rings[j+1][q],rings[j+1][k]],[(k/sides,j/(levels-1)),((k+1)/sides,j/(levels-1)),((k+1)/sides,(j+1)/(levels-1)),(k/sides,(j+1)/(levels-1))],'daisy')
    for ring,z,reverse in ((rings[0],.01,True),(rings[-1],.754,False)):
        center=Vector((.015*math.sin(z*5),0,z))
        for k in range(sides):
            q=(k+1)%sides;points=[center,ring[k],ring[q]];uv=[(.5,.5),(.5+.45*math.cos(k*math.tau/sides),.5+.45*math.sin(k*math.tau/sides)),(.5+.45*math.cos(q*math.tau/sides),.5+.45*math.sin(q*math.tau/sides))]
            if reverse:points.reverse();uv.reverse()
            b.face(points,uv,'daisy')
    count=(24,16,10)[lod];minor=(6,5,4)[lod];center=Vector((.015,0,.67));radius=.078
    for i in range(count):
        for j in range(minor):
            points=[]
            for a,c in ((i,j),(i+1,j),(i+1,j+1),(i,j+1)):
                theta=a/count*math.tau;phi=c/minor*math.tau
                points.append(center+Vector(((radius+.011*math.cos(phi))*math.cos(theta),(radius+.011*math.cos(phi))*math.sin(theta),.011*math.sin(phi))))
            b.face(points,[(i/count,j/minor),((i+1)/count,j/minor),((i+1)/count,(j+1)/minor),(i/count,(j+1)/minor)],'stem')
    for side in (-1,1):
        loop=[(.015+side*(.018+.06*math.sin(math.pi*t)), -.085-.014*math.sin(math.tau*t),.678+.045*math.sin(math.tau*t)) for t in np.linspace(0,1,(11,8,6)[lod])]
        b.tube(loop,[.008]*len(loop),'stem',sides=(6,5,4)[lod])
        b.tube([(.015,-.083,.673),(.015+side*.045,-.097,.63),(.015+side*.075,-.13,.58)],[.008,.007,.005],'stem',sides=(6,5,4)[lod])
    return b


def log(lod):
    b=g.Builder();sides=(32,22,14)[lod];levels=(10,7,4)[lod];rings=[]
    for j in range(levels):
        t=j/(levels-1);x=-1.02+2.23*t;ring=[]
        for k in range(sides):
            a=k/sides*math.tau;r=.26*(1-.22*t)+.018*math.sin(a*7+t*5)+.010*math.sin(a*13-t*8)
            ring.append(Vector((x+.014*math.sin(a*5)*(1 if j in (0,levels-1) else 0),r*math.cos(a),.30+.025*math.sin(t*5)+r*math.sin(a))))
        rings.append(ring)
    for j in range(levels-1):
        for k in range(sides):
            q=(k+1)%sides;points=[rings[j][k],rings[j][q],rings[j+1][q],rings[j+1][k]];middle=sum(points,Vector())/4
            tile='leaf_brown' if -.8<middle.x<.5 and middle.z>.49+.015*math.sin(middle.x*9) else 'twig_bark'
            b.face(points,[(k/sides,j/(levels-1)),((k+1)/sides,j/(levels-1)),((k+1)/sides,(j+1)/(levels-1)),(k/sides,(j+1)/(levels-1))],tile)
    for ring,reverse in ((rings[0],True),(rings[-1],False)):
        center=sum(ring,Vector())/sides
        for k in range(sides):
            q=(k+1)%sides;points=[center,ring[k],ring[q]];uv=[(.5,.5)]+[(.5+(point.y-center.y)/.64,.5+(point.z-center.z)/.64) for point in (ring[k],ring[q])]
            if reverse:points.reverse();uv.reverse()
            b.face(points,uv,'fence_end',False)
    for j,a in enumerate((.18,1.5,2.8,3.9,5.35)):
        start=Vector((-.91,.17*math.cos(a),.30+.17*math.sin(a)))
        tip=Vector((-1.42-.07*math.sin(j),.47*math.cos(a),max(.025,.22+.27*math.sin(a))))
        b.tube([start,start.lerp(tip,.45)+Vector((-.07,0,.015)),tip],[.085,.050,.012],'twig_bark',sides=(9,7,5)[lod])
    b.tube([(.2,.10,.47),(.27,.21,.62),(.39,.23,.67)],[.075,.046,.025],'twig_bark',sides=(9,7,5)[lod])
    for i,(x,y) in enumerate(((-.55,.06),(-.26,.11),(.04,.05),(.24,-.03))):
        blob(b,(x,y,.55-.025*(x+.6)),(.22,.115,.035),lod,'leaf_brown',500+i,True)
    return b


def rock(lod):
    b=g.Builder();points=blob(b,(0,0,.36),(.70,.51,.34),lod,'leaf_ochre',615)
    for i,(x,y,sx,sy) in enumerate(((-.28,-.10,.20,.15),(.05,.07,.24,.18),(.30,.02,.19,.13),(-.08,-.23,.18,.12))):
        near=min(points,key=lambda p:(p.x-x)**2+(p.y-y)**2+(0 if p.z>.36 else 10))
        blob(b,(x,y,near.z-.018),(sx,sy,.035),lod,'leaf_brown',620+i,True)
    return b


def clean(ob):
    mesh=ob.data;bm=bmesh.new();bm.from_mesh(mesh);before=len(bm.verts)
    bmesh.ops.remove_doubles(bm,verts=list(bm.verts),dist=1e-7)
    bad=[face for face in bm.faces if face.calc_area()<=1e-13]
    if bad:bmesh.ops.delete(bm,geom=bad,context='FACES_ONLY')
    loose=[v for v in bm.verts if not v.link_faces]
    if loose:bmesh.ops.delete(bm,geom=loose,context='VERTS')
    bmesh.ops.recalc_face_normals(bm,faces=list(bm.faces));bm.to_mesh(mesh);bm.free();mesh.update();mesh.calc_loop_triangles()
    keys=[tuple(round(c,7) for c in v.co) for v in mesh.vertices];assert len(keys)==len(set(keys)),(ob.name,'duplicate position vertices')
    assert all(tri.area>1e-13 for tri in mesh.loop_triangles),(ob.name,'zero triangle')
    assert mesh.uv_layers.active
    return {'merged_vertices':before-len(mesh.vertices),'removed_zero_faces':len(bad),'triangles':len(mesh.loop_triangles)}


def objects(names=NAMES):return [bpy.data.objects[n+s] for n in names for s in ('','_lod1','_lod2')]


def preview_group(names):
    for ob in bpy.data.collections[PREVIEWS].objects:ob.hide_render=ob['hs_base'] not in names


def geometry():
    assert not bpy.data.scenes.get(SCENE),'Inspect existing owned scene before regeneration'
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT/'prebuild_checkpoint_v004.blend'))
    scene=bpy.data.scenes.new(SCENE);bpy.context.window.scene=scene;scene.unit_settings.system='METRIC'
    assets=bpy.data.collections.new(ASSETS);scene.collection.children.link(assets);previews=bpy.data.collections.new(PREVIEWS);scene.collection.children.link(previews)
    clay=bpy.data.materials.new('MAT-additions-clay');clay.diffuse_color=(.35,.32,.24,1)
    builders=[lambda l:fence(l),lambda l:fence(l,True),sack,log,rock]+[lambda l:p.crate(l),lambda l:p.crate(l,True),lambda l:p.barrel(l),lambda l:p.barrel(l,True),p.cart]
    manifest=[]
    for index,(name,build) in enumerate(zip(NAMES+EXISTING,builders)):
        counts=[]
        for lod in range(3):
            b=build(lod);low=min(v[2] for v in b.vertices);b.vertices=[(v[0],v[1],v[2]-low) for v in b.vertices]
            ob=b.object(name+('' if lod==0 else '_lod'+str(lod)),assets);result=clean(ob);counts.append(result['triangles'])
            radius=max(math.hypot(v.co.x,v.co.y) for v in ob.data.vertices)
            if index<5:assert radius<RADII[index],(name,radius,RADII[index])
            ob.data.materials.append(clay);ob.hide_render=True;ob.hide_set(True)
            bounds=[[min(v.co[i] for v in ob.data.vertices) for i in range(3)],[max(v.co[i] for v in ob.data.vertices) for i in range(3)]]
            manifest.append({'name':ob.name,'vertices':len(ob.data.vertices),'bounds_blender_m':bounds,'planar_radius_m':radius,**result})
        assert counts[0]>counts[1]>counts[2],(name,counts)
        src=bpy.data.objects[name];ob=src.copy();ob.data=src.data;ob.name='PREVIEW-'+name;previews.objects.link(ob);ob.hide_render=False;ob.hide_set(False);ob['hs_base']=name
        local=index%5;ob.location=((local%3-1)*3.2,(local//3-.5)*2.7,0)
    g.setup_render(scene);scene.cycles.samples=16;preview_group(NAMES);g.camera(scene,(7,-10,8),(0,0,.3),10.5)
    (OUT/'verification/geometry.json').write_text(json.dumps(manifest,indent=2))
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT/'meadow_props_blockout_v004.blend'));g.render(scene,OUT/'preview/blockout.png',1200,900)
    print('V004_GEOMETRY_READY',json.dumps({r['name']:r['triangles'] for r in manifest}))


def export_set(names,filename):
    for ob in objects(names):ob.hide_set(False)
    for name in names:
        bpy.ops.object.select_all(action='DESELECT');group=[bpy.data.objects[name+s] for s in ('','_lod1','_lod2')]
        for ob in group:ob.select_set(True)
        bpy.context.view_layer.objects.active=group[0]
        bpy.ops.export_scene.fbx(filepath=str(OUT/'FBX'/(name+'.fbx')),use_selection=True,object_types={'MESH'},apply_unit_scale=True,apply_scale_options='FBX_SCALE_UNITS',axis_forward='-Z',axis_up='Y',use_tspace=True,mesh_smooth_type='FACE',bake_anim=False,path_mode='RELATIVE',add_leaf_bones=False)
    bpy.ops.object.select_all(action='DESELECT')
    for name in names:bpy.data.objects[name].select_set(True)
    bpy.context.view_layer.objects.active=bpy.data.objects[names[0]]
    bpy.ops.export_scene.gltf(filepath=str(OUT/filename),export_format='GLB',use_selection=True,use_active_scene=True,export_animations=False,export_yup=True,export_normals=True,export_tangents=True)
    assert (OUT/filename).stat().st_size<100*1024*1024
    for ob in objects(names):ob.hide_set(True)
    bpy.ops.object.select_all(action='DESELECT')


def finish():
    bpy.ops.wm.open_mainfile(filepath=str(OUT/'meadow_props_blockout_v004.blend'))
    with bpy.context.temp_override(window=bpy.context.window_manager.windows[0]):
        scene=bpy.data.scenes[SCENE];bpy.context.window_manager.windows[0].scene=scene;atlas();new=material();old=g.material()
        for names,mat,layer,file in [(NAMES,new,1,'runtime_records.json'),(EXISTING,old,0,'optimized_existing_runtime_records.json')]:
            for ob in objects(names):ob.data.materials.clear();ob.data.materials.append(mat)
            g.NAMES=names;records,stats=g.runtime_records()
            for record in records:
                for vertex in record['vertices']:vertex[12]=layer
            for item in stats:item['texture_layer']=layer;item['renderer_material_ordinal']=15
            path=OUT/file;temporary=path.with_suffix('.json.tmp');temporary.write_text(json.dumps({'meshes':records},separators=(',',':')));temporary.replace(path)
            (OUT/'verification'/('new_runtime_validation.json' if layer else 'optimized_existing_runtime_validation.json')).write_text(json.dumps({'complete':True,'meshes':stats,'array_layer':layer,'source_sha256':hashlib.sha256((OUT/file).read_bytes()).hexdigest()},indent=2))
        export_set(NAMES,'meadow_additions_v004.glb');export_set(EXISTING,'optimized_existing_v004.glb')
        for image in bpy.data.images:
            if image.source=='FILE' and ('ground_detail' in image.name or 'meadow_additions' in image.name):image.pack()
        preview_group(NAMES);g.camera(scene,(7,-10,8),(0,0,.3),10.5);g.render(scene,OUT/'preview/all_assets.png',1600,1100)
        for name in NAMES:
            preview_group(())
            ob=bpy.data.objects['PREVIEW-'+name];oldpos=ob.location.copy();ob.location=(0,0,0);ob.hide_render=False
            bounds=ob.dimensions;scale=max(bounds.x,bounds.y,bounds.z)*1.6
            target=(0,0,bounds.z*.45)
            for label,location in [('front',(3,-4,2.8)),('rear',(-3,4,2.5))]:
                g.camera(scene,location,target,scale);g.render(scene,OUT/'preview'/(name+'_'+label+'.png'),640,640)
            ob.location=oldpos
        preview_group(EXISTING);g.camera(scene,(7,-10,8),(0,0,.3),9.0);g.render(scene,OUT/'preview/optimized_existing.png',1400,1000)
        preview_group(NAMES);g.camera(scene,(7,-10,8),(0,0,.3),10.5)
        bpy.ops.wm.save_as_mainfile(filepath=str(OUT/'meadow_additions_v004.blend'))
        print('V004_FINISH_READY')


def verify():
    bpy.ops.wm.open_mainfile(filepath=str(OUT/'meadow_additions_v004.blend'))
    with bpy.context.temp_override(window=bpy.context.window_manager.windows[0]):
        for ob in objects(NAMES+EXISTING):ob.data.calc_loop_triangles()
        source={ob.name:(len(ob.data.loop_triangles),tuple(ob.dimensions)) for ob in objects(NAMES+EXISTING)}
        report=[]
        for name in NAMES+EXISTING:
            scene=bpy.data.scenes.new('VERIFY-'+name);bpy.context.window_manager.windows[0].scene=scene;bpy.ops.import_scene.fbx(filepath=str(OUT/'FBX'/(name+'.fbx')))
            meshes=[o for o in scene.objects if o.type=='MESH'];assert len(meshes)==3
            for ob in meshes:
                key=next(n for n in source if ob.name==n or ob.name.startswith(n+'.'));ob.data.calc_loop_triangles();count,dimensions=source[key]
                assert len(ob.data.loop_triangles)==count,(key,count,len(ob.data.loop_triangles))
                error=max(abs(ob.dimensions[i]-dimensions[i]) for i in range(3));assert error<5e-5,(key,error)
                assert ob.data.uv_layers.active and len(ob.data.materials)==1
                assert all(math.isfinite(v) for p in ob.data.vertices for v in p.co)
                report.append({'name':key,'triangles':count,'dimension_max_error_m':error,'uv0':True,'opaque_materials':1})
        for filename,names in [('meadow_additions_v004.glb',NAMES),('optimized_existing_v004.glb',EXISTING)]:
            scene=bpy.data.scenes.new('VERIFY-'+filename);bpy.context.window_manager.windows[0].scene=scene;bpy.ops.import_scene.gltf(filepath=str(OUT/filename));meshes=[o for o in scene.objects if o.type=='MESH'];assert len(meshes)==5
            for ob in meshes:
                key=next(n for n in names if ob.name==n or ob.name.startswith(n+'.'));ob.data.calc_loop_triangles();assert len(ob.data.loop_triangles)==source[key][0]
                assert ob.data.uv_layers.active and len(ob.data.materials)==1
        for filename,layer in [('runtime_records.json',1),('optimized_existing_runtime_records.json',0)]:
            records=json.loads((OUT/filename).read_text())['meshes'];assert len(records)==15
            for m in records:
                assert len(m['indices'])%3==0 and max(m['indices'])<len(m['vertices']);assert all(len(v)==13 and v[12]==layer for v in m['vertices'])
        (OUT/'verification/export_roundtrip.json').write_text(json.dumps({'complete':True,'blender_version':bpy.app.version_string,'fbx_meshes':report,'glb_files':2,'glb_meshes':10,'runtime_records_per_set':15,'registry_modified':False},indent=2))
        print('V004_ROUNDTRIP_PASS',len(report),'FBX meshes + 2 GLBs')


if __name__=='__main__':globals()[args.stage]()
