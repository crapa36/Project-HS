"""Deterministic meadow props; run in Blender with --root REPO --stage geometry|finish|verify|register.
Inspect geometry preview before finish. Reuses the existing ground-detail atlas and vertex ABI.
Only the explicit register stage updates the environment registry after round-trip verification.
"""
import argparse
import importlib.util
import hashlib
import json
import math
import random
import sys
from pathlib import Path
import bpy
from mathutils import Vector

parser = argparse.ArgumentParser()
parser.add_argument('--root', type=Path, required=True)
parser.add_argument('--stage', choices=('geometry', 'finish', 'verify', 'register'), required=True)
args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
ROOT = args.root.resolve()
spec = importlib.util.spec_from_file_location('hs_ground_detail_helpers', ROOT / 'tools/art/create_ground_detail_blender.py')
g = importlib.util.module_from_spec(spec)
saved_argv = sys.argv[:]
try:
    sys.argv = [sys.argv[0], '--', '--root', str(ROOT), '--stage', 'verify']
    spec.loader.exec_module(g)  # Import helpers without executing their main stage.
finally:
    sys.argv = saved_argv
OUT = ROOT / 'ContentSource/Models/Environment/MeadowProps'
for directory in ('', 'preview_v003', 'verification', 'FBX'):
    (OUT / directory).mkdir(parents=True, exist_ok=True)
NAMES = ('shrub_low', 'stump_roots', 'crate_intact', 'crate_broken', 'barrel_intact', 'barrel_broken', 'cart_broken')
RADII = (1.0, 1.3, .75, .75, .65, .65, 2.0)
SCENE = 'HS_Meadow_Props_v003'
ASSETS = 'COL-meadow-prop-assets'
PREVIEWS = 'COL-meadow-prop-preview'


def vary_wood_uvs(builder, first_face, seed):
    # Crop and 180-degree rotation preserve longitudinal grain and stay inside the existing atlas tile.
    rng=random.Random(seed)
    scale=(rng.uniform(.73,.94),rng.uniform(.70,.93))
    offset=(rng.uniform(.015,1-scale[0]-.015),rng.uniform(.015,1-scale[1]-.015))
    reverse=rng.random()<.5
    lo=g.atlas_uv('fence_wood',(0,0));hi=g.atlas_uv('fence_wood',(1,1))
    for index in range(first_face,len(builder.uvs)):
        source=builder.uvs[index]
        normalized=[((u-lo[0])/(hi[0]-lo[0]),(v-lo[1])/(hi[1]-lo[1])) for u,v in source]
        if not all(-.001<=u<=1.001 and -.001<=v<=1.001 for u,v in normalized):
            continue  # End-grain lives in a different atlas cell and retains its mapping.
        result=[]
        for u,v in normalized:
            if reverse:u,v=1-u,1-v
            u,v=offset[0]+u*scale[0],offset[1]+v*scale[1]
            assert 0<=u<=1 and 0<=v<=1
            result.append(g.atlas_uv('fence_wood',(u,v)))
        builder.uvs[index]=result


def plank(b, a, z, width, depth, lod, seed, flat=False):
    a, z = Vector(a), Vector(z)
    axis = (z-a).normalized()
    reference = Vector((1,0,0)) if abs(axis.z)>.8 else Vector((0,0,1))
    side = (reference-axis*reference.dot(axis)).normalized()
    if flat: side = axis.cross(side).normalized()
    other = axis.cross(side)
    part = g.Builder()
    g.board(part, (0,0,0), (0,0,1), width, depth, (z-a).length, lod, seed)
    vary_wood_uvs(part,0,seed)
    offset = len(b.vertices)
    b.vertices.extend(tuple(a+side*p[0]+other*p[1]+axis*p[2]) for p in part.vertices)
    b.faces.extend([offset+i for i in face] for face in part.faces)
    b.uvs.extend(part.uvs);b.smooth.extend(part.smooth)


def ring(b, center, radius, width, thickness, lod, axis='Z', gap=False):
    # Closed annular wood bands; no cylinder stand-in for hoops or wheel rims.
    center = Vector(center)
    count = (32, 20, 12)[lod]
    def p(angle, radial, axial):
        local = Vector((radial*math.cos(angle), radial*math.sin(angle), axial))
        return center + (Vector((local.z, local.x, local.y)) if axis == 'X' else local)
    active = {i for i in range(count) if not (gap and i in range(count//8, count//4))}
    for i in sorted(active):
        a, c = math.tau*i/count, math.tau*(i+1)/count
        points = [p(t, r, h) for h in (-width/2, width/2) for r in (radius-thickness, radius) for t in (a,c)]
        faces = [(0,1,3,2),(4,6,7,5),(0,4,5,1),(2,3,7,6)]
        if (i-1)%count not in active: faces.append((0,2,6,4))
        if (i+1)%count not in active: faces.append((1,5,7,3))
        for ids in faces:
            b.face([points[k] for k in ids],[(0,0),(1,0),(1,1),(0,1)], 'fence_wood', False)


def shrub(lod):
    b = g.Builder()
    for branch in range(7):
        rng = random.Random(712+branch*97)
        angle = branch*2.39996 + rng.uniform(-.25,.25)
        direction = Vector((math.cos(angle),math.sin(angle),0))
        base = direction*.055 + Vector((0,0,.015))
        elbow = direction*rng.uniform(.12,.22) + Vector((0,0,rng.uniform(.22,.38)))
        tip = direction*rng.uniform(.40,.55) + Vector((0,0,rng.uniform(.47,.76)))
        b.tube([base,elbow,tip],[.025,.013,.003],'twig_bark',sides=(7,5,4)[lod])
        for fork in range(3):
            start = elbow.lerp(tip,.15+fork*.25)
            theta = angle + (-1 if fork%2 else 1)*rng.uniform(.65,1.7)
            end = start + Vector((math.cos(theta)*.20,math.sin(theta)*.20,rng.uniform(.04,.16)))
            bend = start.lerp(end,.5) + Vector((0,0,.025))
            b.tube([start,bend,end],[.007,.004,.0015],'twig_bark',sides=(6,5,4)[lod])
            # Seed every leaf independently so all LODs retain matching canopy landmarks.
            for k in range(6):
                if lod==1 and k in (1,4): continue
                if lod==2 and k not in (1,4): continue
                leaf_rng=random.Random(2000+branch*100+fork*10+k)
                t=.12+.85*k/5
                pos=start.lerp(end,t)+Vector((0,0,.015*math.sin(k)))
                leaf_angle=theta+(-1 if k%2 else 1)*leaf_rng.uniform(.65,1.8)
                forward=Vector((math.cos(leaf_angle),math.sin(leaf_angle),leaf_rng.uniform(-.3,.65))).normalized()
                right=Vector((-forward.y,forward.x,leaf_rng.uniform(-.35,.35))).normalized()
                right=(right-forward*right.dot(forward)).normalized()
                up=right.cross(forward)
                b.sheet(pos,right,forward,up,leaf_rng.uniform(.12,.20),leaf_rng.uniform(.07,.105),'leaf_green',
                        rows=(7,5,3)[lod],cols=3,curl=.018,cup=.009,thickness=.0012,serration=.06,phase=k+fork)
    return b


def stump(lod):
    b = g.Builder(); sides=(56,36,22)[lod]
    # One continuous trunk/buttress surface: irregular roots merge into the trunk, not separate cones.
    roots=((.18,.73,.18), (1.32,.53,.23), (2.63,.88,.16), (3.51,.61,.25), (4.75,.78,.18), (5.60,.47,.20))
    heights=(.004,.035,.10,.23,.43,.60,.69) if lod==0 else (.004,.07,.23,.47,.69) if lod==1 else (.004,.13,.39,.69)
    rings=[]
    for level,z in enumerate(heights):
        ring_points=[]
        for j in range(sides):
            angle=math.tau*j/sides
            bend=.12*math.sin(z*7+angle*2)*(1-z/.75)
            root=0
            for bearing,length,spread in roots:
                delta=math.atan2(math.sin(angle-bearing-bend),math.cos(angle-bearing-bend))
                root+=length*math.exp(-(delta/spread)**2)*max(0,1-z/.42)**2
            bark=(.014*math.sin(angle*13+z*3)+.009*math.sin(angle*23-z*7))
            radius=.255+.085*max(0,1-z/.5)+root+bark
            radius+=.022*math.sin(angle*3+z*4)
            cut=.018*math.sin(angle*3)+.010*math.sin(angle*7)
            height=z+cut*(z/.69)**4
            ring_points.append(Vector((radius*math.cos(angle)+.035*math.sin(z*5),radius*math.sin(angle),height)))
        rings.append(ring_points)
    for k in range(len(rings)-1):
        for j in range(sides):
            q=(j+1)%sides
            b.face([rings[k][j],rings[k][q],rings[k+1][q],rings[k+1][j]],
                   [(j/sides,k/(len(rings)-1)),((j+1)/sides,k/(len(rings)-1)),((j+1)/sides,(k+1)/(len(rings)-1)),(j/sides,(k+1)/(len(rings)-1))], 'twig_bark')
    center=Vector((.035*math.sin(.69*5),0,.677))
    for j in range(sides):
        q=(j+1)%sides
        uv=lambda p: (.5+(p.x-center.x)/.62,.5+p.y/.62)
        b.face([center,rings[-1][j],rings[-1][q]],[uv(center),uv(rings[-1][j]),uv(rings[-1][q])],'fence_end',False)
        bottom=Vector((0,0,.004))
        b.face([bottom,rings[0][q],rings[0][j]],[(.5,.5),(.5+.48*math.cos(math.tau*q/sides),.5+.48*math.sin(math.tau*q/sides)),(.5+.48*math.cos(math.tau*j/sides),.5+.48*math.sin(math.tau*j/sides))],'twig_bark',False)
    return b


def crate(lod, broken=False):
    b = g.Builder()
    for i in range(5):
        x = -.34+i*.17
        plank(b,(x,-.38,.08),(x,.38,.08),.16,.045,lod,100+i,flat=True)
    for side in (-1,1):
        for row in range(4):
            z = .16+row*.15
            if not (broken and side==-1 and row>=2):
                plank(b,(-.43,side*.40,z),(.43 if not(broken and side==-1 and row==1) else .07,side*.40,z),.14,.045,lod,210+row)
            if not (broken and side==1 and row==3):
                plank(b,(side*.43,-.38,z),(side*.43,.38,z),.14,.045,lod,240+row)
        for y in (-.36,.36):
            plank(b,(side*.39,y,.06),(side*.39,y,.70),.065,.07,lod,310+int(y>0))
    for y in (-.41,.41):
        if not (broken and y<0):
            plank(b,(-.35,y,.14),(.35,y,.61),.07,.035,lod,402)
    if broken:
        plank(b,(-.38,-.46,.06),(.27,-.43,.08),.13,.04,lod,513,flat=True)
        plank(b,(-.12,-.42,.1),(.30,-.28,.42),.12,.04,lod,515)
    return b


def barrel(lod, broken=False):
    b = g.Builder(); count = (18,14,10)[lod]; levels = (7,5,3)[lod]
    for stave in range(count):
        if broken and stave in range(count//4,count//2):
            continue
        first_face=len(b.faces)
        a = math.tau*(stave+.025)/count; c = math.tau*(stave+.975)/count
        rings = []
        for j in range(levels):
            t = j/(levels-1); h = .04+.91*t
            if broken and stave==count//2: h *= .64
            radius = .28+.095*math.sin(math.pi*t)
            rings.append([Vector((r*math.cos(angle),r*math.sin(angle),h)) for r in (radius-.035,radius) for angle in (a,c)])
        for j in range(levels-1):
            for x,y in ((0,1),(1,3),(3,2),(2,0)):
                b.face([rings[j][x],rings[j][y],rings[j+1][y],rings[j+1][x]],
                       [(0,j/(levels-1)),(1,j/(levels-1)),(1,(j+1)/(levels-1)),(0,(j+1)/(levels-1))],'fence_wood',False)
        for j, ids in ((0,(0,2,3,1)),(-1,(0,1,3,2))):
            b.face([rings[j][i] for i in ids],[(0,0),(1,0),(1,1),(0,1)],'fence_end',False)
        vary_wood_uvs(b,first_face,900+stave)
    for h in (.15,.35,.69,.85):
        ring(b,(0,0,h),.28+.095*math.sin(math.pi*(h-.04)/.91)+.012,.045,.018,lod,gap=broken and h>.6)
    for top in (False,True):
        if broken and top: continue
        for i in range(5):
            x=(i-2)*.10; half=math.sqrt(max(.001,.265**2-x*x))
            plank(b,(x,-half,.93 if top else .055),(x,half,.93 if top else .055),.095,.025,lod,620+i,flat=True)
    if broken:
        plank(b,(.20,-.32,.06),(.48,.27,.06),.12,.035,lod,699,flat=True)
    return b


def cart(lod):
    b = g.Builder()
    # Wheel centres at x +/- .82: 1.64 m track, shared with terrain rut placement.
    for x in (-.82,.82):
        broken = x > 0
        ring(b,(x,0,.53),.52,.105,.065,lod,axis='X',gap=broken)
        b.tube([(x-.13,0,.53),(x+.13,0,.53)],[.105,.105],'fence_wood',sides=(14,10,7)[lod])
        for i in range(10):
            if broken and i in (1,2): continue
            theta=math.tau*i/10
            plank(b,(x,.075*math.cos(theta),.53+.075*math.sin(theta)),
                  (x,.46*math.cos(theta),.53+.46*math.sin(theta)),.045,.04,lod,720+i)
    plank(b,(-.97,0,.53),(.97,0,.53),.09,.10,lod,740)
    for x in (-.53,.53):
        plank(b,(x,-.76,.68),(x,.80,.68),.12,.12,lod,750)
        plank(b,(x,-.62,.68),(x,-1.61,.39),.07,.08,lod,752)
    for i in range(7):
        y=-.64+i*.21
        if i==4: continue
        plank(b,(-.65,y,.77),(.65,y,.77),.19,.045,lod,760+i,flat=True)
    for x in (-.64,.64):
        for y in (-.69,.69):
            plank(b,(x,y,.72),(x,y,1.2),.07,.07,lod,780)
        for j in range(3):
            z=.88+j*.13
            if x>0 and j==2: continue
            plank(b,(x,-.70,z),(x,.70 if not(x>0 and j==1) else .22,z),.115,.04,lod,790+j)
    for z in (.88,1.01,1.14):
        plank(b,(-.64,.70,z),(.64,.70,z),.115,.04,lod,800)
    plank(b,(.30,-.26,.07),(.66,.58,.07),.16,.045,lod,820,flat=True)
    return b


def objects():
    return [bpy.data.objects[name+suffix] for name in NAMES for suffix in ('','_lod1','_lod2')]


def geometry():
    if bpy.data.scenes.get(SCENE):
        raise RuntimeError('Owned scene already exists; inspect or reopen original checkpoint before regeneration.')
    assert not any(bpy.data.objects.get(n+s) for n in NAMES for s in ('','_lod1','_lod2')), 'Asset names already exist'
    scene=bpy.data.scenes.new(SCENE);bpy.context.window.scene=scene
    scene.unit_settings.system='METRIC';scene.unit_settings.scale_length=1
    assets=bpy.data.collections.new(ASSETS);scene.collection.children.link(assets)
    previews=bpy.data.collections.new(PREVIEWS);scene.collection.children.link(previews)
    clay=bpy.data.materials.new('MAT-meadow-props-clay');clay.diffuse_color=(.32,.30,.23,1)
    builders=(shrub,stump,lambda l:crate(l),lambda l:crate(l,True),lambda l:barrel(l),lambda l:barrel(l,True),cart)
    manifest=[]
    for name,radius,build in zip(NAMES,RADII,builders):
        for lod in range(3):
            b=build(lod)
            low=min(p[2] for p in b.vertices)
            b.vertices=[(p[0],p[1],p[2]-low) for p in b.vertices]
            actual=max(math.hypot(p[0],p[1]) for p in b.vertices)
            assert actual<=radius,(name,actual,radius)
            ob=b.object(name+('' if lod==0 else '_lod'+str(lod)),assets)
            ob.data.materials.append(clay);ob.hide_render=True;ob.hide_set(True)
            ob['hs_planar_radius_m']=radius;ob['hs_seed']=712;ob['hs_static_prop']=True
            manifest.append({'name':ob.name,'planar_radius_m':actual,'triangles':sum(len(p.vertices)-2 for p in ob.data.polygons)})
    for i,name in enumerate(NAMES):
        src=bpy.data.objects[name];ob=src.copy();ob.data=src.data;ob.name='PREVIEW-'+name;previews.objects.link(ob)
        ob.hide_render=False;ob.hide_set(False);ob.location=((i%3-1)*2.5,(i//3-1)*2.4,0);ob['hs_base']=name
    g.setup_render(scene);g.camera(scene,(8,-12,10),(0,0,.3),10)
    (OUT/'verification/geometry.json').write_text(json.dumps(manifest,indent=2))
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT/'meadow_props_blockout_v003.blend'))
    g.render(scene,OUT/'preview_v003/blockout.png',1400,1100)
    print('MEADOW_PROPS_GEOMETRY_READY',len(manifest))


def finish():
    bpy.ops.wm.open_mainfile(filepath=str(OUT/'meadow_props_blockout_v003.blend'))
    scene=bpy.data.scenes[SCENE];bpy.context.window.scene=scene
    m=g.material()
    for ob in objects():ob.data.materials.clear();ob.data.materials.append(m)
    g.NAMES=NAMES
    records,stats=g.runtime_records()
    (OUT/'runtime_records.json').write_text(json.dumps({'meshes':records},separators=(',',':')))
    (OUT/'verification/runtime_mesh_validation.json').write_text(json.dumps({'complete':True,'meshes':stats,'cart_wheel_track_m':1.64,'material':'ground_detail','seed':712},indent=2))
    for ob in objects():ob.hide_set(False)
    for name in NAMES:
        bpy.ops.object.select_all(action='DESELECT')
        group=[bpy.data.objects[name+s] for s in ('','_lod1','_lod2')]
        for ob in group:ob.select_set(True)
        bpy.context.view_layer.objects.active=group[0]
        bpy.ops.export_scene.fbx(filepath=str(OUT/'FBX'/(name+'.fbx')),use_selection=True,object_types={'MESH'},apply_unit_scale=True,
            apply_scale_options='FBX_SCALE_UNITS',axis_forward='-Z',axis_up='Y',use_tspace=True,mesh_smooth_type='FACE',bake_anim=False,path_mode='RELATIVE',add_leaf_bones=False)
    bpy.ops.object.select_all(action='DESELECT')
    for name in NAMES:bpy.data.objects[name].select_set(True)
    bpy.context.view_layer.objects.active=bpy.data.objects[NAMES[0]]
    bpy.ops.export_scene.gltf(filepath=str(OUT/'meadow_props_v003.glb'),export_format='GLB',use_selection=True,export_animations=False,
        export_yup=True,use_active_scene=True,export_normals=True,export_tangents=True,export_materials='EXPORT')
    for ob in objects():ob.hide_set(True)
    previews=bpy.data.collections[PREVIEWS]
    g.camera(scene,(8,-12,10),(0,0,.3),10);g.render(scene,OUT/'preview_v003/all_assets.png',1600,1200)
    for ob in previews.objects:ob.hide_render=True
    for name in NAMES:
        ob=bpy.data.objects['PREVIEW-'+name];old=ob.location.copy();ob.location=(0,0,0);ob.hide_render=False
        size=4.4 if name=='cart_broken' else 2.7 if name=='stump_roots' else 2.0
        for label,location in (('front',(2,-3,1.7)),('rear',(-2,3,1.8)),('top',(0,-.01,5))):
            g.camera(scene,location,(0,0,.4),size);g.render(scene,OUT/'preview_v003'/(name+'_'+label+'.png'),800,800)
        ob.hide_render=True;ob.location=old
    for ob in previews.objects:ob.hide_render=False
    for lod in (1,2):
        for ob in previews.objects:ob.data=bpy.data.objects[ob['hs_base']+'_lod'+str(lod)].data
        g.camera(scene,(8,-12,10),(0,0,.3),10);g.render(scene,OUT/'preview_v003'/('all_assets_lod'+str(lod)+'.png'),1200,900)
    for ob in previews.objects:ob.data=bpy.data.objects[ob['hs_base']].data
    for image in bpy.data.images:
        if image.source=='FILE' and 'ground_detail' in image.name:image.pack()
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT/'meadow_props_v003.blend'))
    print('MEADOW_PROPS_FINISH_READY',len(stats))


def verify():
    bpy.ops.wm.open_mainfile(filepath=str(OUT/'meadow_props_v003.blend'))
    source={ob.name:(sum(len(p.vertices)-2 for p in ob.data.polygons),tuple(ob.dimensions)) for ob in objects()}
    report=[]
    for name in NAMES:
        scene=bpy.data.scenes.new('VERIFY-meadow-'+name);bpy.context.window.scene=scene
        bpy.ops.import_scene.fbx(filepath=str(OUT/'FBX'/(name+'.fbx')))
        imported=[o for o in scene.objects if o.type=='MESH'];assert len(imported)==3
        for ob in imported:
            key=ob.name.rsplit('.',1)[0] if ob.name.rsplit('.',1)[-1].isdigit() else ob.name
            count,dimensions=source[key];ob.data.calc_loop_triangles()
            error=max(abs(ob.dimensions[i]-dimensions[i]) for i in range(3))
            assert len(ob.data.loop_triangles)==count and error<.00005
            assert ob.data.uv_layers.active and len(ob.data.materials)==1
            report.append({'name':key,'triangles':count,'dimension_max_error_m':error})
    scene=bpy.data.scenes.new('VERIFY-meadow-glb');bpy.context.window.scene=scene
    bpy.ops.import_scene.gltf(filepath=str(OUT/'meadow_props_v003.glb'))
    imported=[o for o in scene.objects if o.type=='MESH'];assert len(imported)==7
    for ob in imported:
        key=ob.name.rsplit('.',1)[0] if ob.name.rsplit('.',1)[-1].isdigit() else ob.name
        ob.data.calc_loop_triangles();assert len(ob.data.loop_triangles)==source[key][0]
        assert ob.data.uv_layers.active and len(ob.data.materials)==1
    (OUT/'verification/export_roundtrip.json').write_text(json.dumps({'complete':True,'blender_version':bpy.app.version_string,
        'fbx':report,'glb_mesh_count':len(imported),'runtime_records_sha256':hashlib.sha256((OUT/'runtime_records.json').read_bytes()).hexdigest(),'visual_review':'Separate image inspection required'},indent=2))
    print('MEADOW_PROPS_ROUNDTRIP_PASS',len(report))


def register():
    record_path=OUT/'runtime_records.json'
    raw_records=record_path.read_bytes()
    records=json.loads(raw_records)['meshes']
    expected={n+suffix for n in NAMES for suffix in ('','_lod1','_lod2')}
    assert len(records)==21 and {r['name'] for r in records}==expected
    proof=json.loads((OUT/'verification/export_roundtrip.json').read_text())
    assert proof['complete'] and len(proof['fbx'])==21 and {r['name'] for r in proof['fbx']}==expected
    assert proof['runtime_records_sha256']==hashlib.sha256(raw_records).hexdigest(), 'Re-run verify for changed records'
    target=ROOT/'ContentSource/Models/Environment/environment_meshes.json'
    original=target.read_bytes();document=json.loads(original)
    existing=document['meshes']
    assert len({m['name'] for m in existing})==len(existing), 'Existing duplicate mesh names'
    replacement={m['name']:g.index_mesh(m) for m in records}
    untouched=[m for m in existing if m['name'] not in expected]
    result=[replacement.pop(m['name']) if m['name'] in replacement else m for m in existing]
    result.extend(replacement.values())
    document['meshes']=result
    assert [m for m in result if m['name'] not in expected]==untouched
    assert len({m['name'] for m in result})==len(result)
    encoded=json.dumps(document,separators=(',',':'),ensure_ascii=False).encode('utf-8')
    assert json.loads(encoded)['meshes']==result
    backup_root=ROOT/'Build/asset-authoring-backups/meadow-props'
    backup_root.mkdir(parents=True,exist_ok=True)
    backup=backup_root/(f"environment_meshes_before_{hashlib.sha256(original).hexdigest()[:16]}.json")
    if not backup.exists():backup.write_bytes(original)
    temporary=target.with_suffix('.json.meadow.tmp')
    temporary.write_bytes(encoded);temporary.replace(target)
    manifest={'complete':True,'source_script_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'runtime_records_sha256':hashlib.sha256(raw_records).hexdigest(),'environment_before_sha256':hashlib.sha256(original).hexdigest(),
        'environment_after_sha256':hashlib.sha256(encoded).hexdigest(),'preserved_record_count':len(untouched),
        'preserved_records_sha256':hashlib.sha256(json.dumps(untouched,separators=(',',':'),ensure_ascii=False).encode()).hexdigest(),
        'mesh_names':sorted(expected),'original_backup':str(backup.relative_to(ROOT))}
    (OUT/'verification/registration_manifest.json').write_text(json.dumps(manifest,indent=2))
    print('MEADOW_PROPS_REGISTERED',len(records),'meshes;',len(untouched),'unrelated records preserved')


if __name__=='__main__':
    {'geometry':geometry,'finish':finish,'verify':verify,'register':register}[args.stage]()
