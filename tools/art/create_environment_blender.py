"""Run through Blender MCP. Authored metre-scale assets; original scene is preserved."""
import bpy, math, random, json
from pathlib import Path
from mathutils import Vector
ROOT=Path(r'C:/Users/crapa/Documents/GitHub/Project HS')
OUT=ROOT/'ContentSource/Models/Environment'
PACK=ROOT/'ContentSource/Textures/Environment'
SPEC=json.loads((PACK/'project_hs_environment_materials_v9_LEAN.json').read_text())
SCENE=bpy.data.scenes.new('HS_Environment_v9')
bpy.context.window.scene=SCENE
SCENE.unit_settings.system='METRIC'
SCENE.unit_settings.scale_length=1
ASSETS=[]

def image(path,noncolor=False):
    im=bpy.data.images.load(str(PACK/path),check_existing=True)
    if noncolor: im.colorspace_settings.name='Non-Color'
    return im

def material(name,paths,rect=None):
    m=bpy.data.materials.new('HSv9_'+name);m.use_nodes=True
    ns=m.node_tree.nodes;lk=m.node_tree.links;bs=ns.get('Principled BSDF')
    bs.inputs['Roughness'].default_value=.8
    uv=ns.new('ShaderNodeTexCoord').outputs['UV']
    if rect:
        scale=ns.new('ShaderNodeVectorMath');scale.operation='MULTIPLY';scale.inputs[1].default_value=(rect[3]/2048,-rect[4]/2048,1);lk.new(uv,scale.inputs[0])
        offset=ns.new('ShaderNodeVectorMath');offset.operation='ADD';offset.inputs[1].default_value=(rect[1]/2048,1-rect[2]/2048,0);lk.new(scale.outputs['Vector'],offset.inputs[0]);uv=offset.outputs['Vector']
    base=ns.new('ShaderNodeTexImage');base.image=image(paths['base_color']);lk.new(uv,base.inputs['Vector']);lk.new(base.outputs['Color'],bs.inputs['Base Color'])
    if rect:
        lk.new(base.outputs['Alpha'],bs.inputs['Alpha'])
        if hasattr(m,'surface_render_method'):m.surface_render_method='DITHERED'
        m.use_backface_culling=False
    # Source normal maps are provisional; retained exactly from the supplied pack.
    tex=ns.new('ShaderNodeTexImage');tex.image=image(paths['normal'],True);lk.new(uv,tex.inputs['Vector'])
    normal=ns.new('ShaderNodeNormalMap');lk.new(tex.outputs['Color'],normal.inputs['Color']);lk.new(normal.outputs['Normal'],bs.inputs['Normal'])
    rough=ns.new('ShaderNodeTexImage');rough.image=image(paths.get('surface_ra',paths.get('roughness')),True);lk.new(uv,rough.inputs['Vector'])
    sep=ns.new('ShaderNodeSeparateColor');lk.new(rough.outputs['Color'],sep.inputs['Color']);lk.new(sep.outputs['Red'],bs.inputs['Roughness'])
    return m
BARK=material('bark',SPEC['opaque_mesh']['materials'][1]['textures'])
ROCK=material('rock',SPEC['opaque_mesh']['materials'][0]['textures'])
LEAVES=[material('leaf_%02d'%i,SPEC['foliage']['leaf']['pages'][r[0]],r) for i,r in enumerate(SPEC['foliage']['leaf']['entries'])]
GRASSES=[material('grass_%02d'%i,SPEC['foliage']['grass']['pages'][r[0]],r) for i,r in enumerate(SPEC['foliage']['grass']['entries'])]

class Builder:
    def __init__(self):self.v=[];self.f=[];self.uv=[];self.mat=[]
    def face(self,pts,uvs,mat=0):
        start=len(self.v);self.v.extend(tuple(p) for p in pts);self.f.append(tuple(range(start,start+len(pts))));self.uv.append(uvs);self.mat.append(mat)
    def tube(self,points,radii,sides=12):
        rings=[];dist=0;lengths=[0]
        for a,b in zip(points,points[1:]):lengths.append(lengths[-1]+(b-a).length)
        for k,p in enumerate(points):
            tangent=(points[min(k+1,len(points)-1)]-points[max(0,k-1)]).normalized()
            ref=Vector((0,1,0)) if abs(tangent.y)<.9 else Vector((1,0,0))
            x=tangent.cross(ref).normalized();y=tangent.cross(x).normalized()
            ring=[]
            for j in range(sides+1):
                a=2*math.pi*j/sides
                radius=radii[k]*(1+.055*math.cos(a*5+k*.18))
                ring.append(p+(x*math.cos(a)+y*math.sin(a))*radius)
            rings.append(ring)
        tiles=max(1,round(2*math.pi*max(radii)))
        for k in range(len(points)-1):
            for j in range(sides):
                self.face([rings[k][j],rings[k][j+1],rings[k+1][j+1],rings[k+1][j]],[(j/sides*tiles,lengths[k]),((j+1)/sides*tiles,lengths[k]),((j+1)/sides*tiles,lengths[k+1]),(j/sides*tiles,lengths[k+1])])
        # Cap terminal branches; hidden embedded branch bases need no overlapping caps.
        end=points[-1]
        for j in range(sides):self.face([rings[-1][j],rings[-1][j+1],end],[(0,0),(1,0),(.5,1)])
    def card(self,center,right,up,width,height,entry,bend=.05):
        # Four quads form a gently curved, non-planar leaf cluster/grass blade card.
        for y in range(2):
            for x in range(2):
                pts=[];uv=[]
                for u,v in [(x/2,y/2),((x+1)/2,y/2),((x+1)/2,(y+1)/2),(x/2,(y+1)/2)]:
                    n=right.cross(up).normalized();pts.append(center+right*((u-.5)*width)+up*(v*height)+n*(math.sin(v*math.pi)*bend));uv.append((u,1-v))
                self.face(pts,uv,entry)
    def object(self,name,mats,smooth=True):
        mesh=bpy.data.meshes.new('HSv9_'+name);mesh.from_pydata(self.v,[],self.f);mesh.update()
        obj=bpy.data.objects.new(name,mesh);SCENE.collection.objects.link(obj)
        for mat in mats:mesh.materials.append(mat)
        layer=mesh.uv_layers.new(name='UVMap')
        for poly,uv,mi in zip(mesh.polygons,self.uv,self.mat):
            poly.material_index=mi;poly.use_smooth=smooth
            for li,p in zip(poly.loop_indices,uv):layer.data[li].uv=p
        # Weld matching positions while retaining UV seams in loop data.
        import bmesh
        bm=bmesh.new();bm.from_mesh(mesh)
        bmesh.ops.remove_doubles(bm,verts=list(bm.verts),dist=.00001)
        bmesh.ops.recalc_face_normals(bm,faces=list(bm.faces));bm.to_mesh(mesh);bm.free();mesh.update()
        ASSETS.append(obj);return obj

def build_tree(variant):
    rng=random.Random(841+variant*1009);wood=Builder();leaf=Builder()
    height=[5.5,5.0,6.0][variant]
    points=[Vector((.09*math.sin(k*.8+variant)*k/10,.1*math.sin(k*.55)*k/10,height*k/16)) for k in range(17)]
    wood.tube(points,[.27*(1-k/17)**1.1+.018 for k in range(17)],18)
    for root in range(7):
        angle=root*2*math.pi/7+.15*rng.random();direction=Vector((math.cos(angle),math.sin(angle),0))
        wood.tube([Vector((0,0,.38)),direction*.34+Vector((0,0,.12)),direction*.78+Vector((0,0,.045)),direction*1.04+Vector((0,0,.018))],[.19,.15,.065,.009],10)
    group=['oak_lobed','maple_lobed','serrated_oval'][variant]
    entries=[i for i,r in enumerate(SPEC['foliage']['leaf']['entries']) if r[5]==group]
    for branch in range(15):
        angle=branch*2.399963+variant;level=.32+branch*.038
        start=points[min(15,int(level*16))]
        reach=(1.3+math.sin(level*math.pi)*.8)*(1-level*.35)
        direction=Vector((math.cos(angle),math.sin(angle),.55+rng.random()*.3))
        end=start+direction*reach
        bp=[start.lerp(end,t/8)+Vector((0,0,.25*math.sin(t/8*math.pi))) for t in range(9)]
        wood.tube(bp,[.092*(1-t/9)**1.3+.006 for t in range(9)],10)
        tips=[]
        for twig in range(5):
            t=.3+twig*.14;p=bp[min(7,int(t*8))]
            a=angle+(-1 if twig%2 else 1)*(.5+rng.random()*.7)
            d=Vector((math.cos(a),math.sin(a),.3+rng.random()*.45))
            tip=p+d*(.65+rng.random()*.55)
            wood.tube([p,p.lerp(tip,.35)+Vector((0,0,.08)),p.lerp(tip,.7)+Vector((0,0,.12)),tip],[.025,.02,.012,.003],6)
            tips.extend([tip,p.lerp(tip,.65)])
        tips.append(end)
        for tip in tips:
            for cluster in range(7):
                entry=rng.choice(entries);r=SPEC['foliage']['leaf']['entries'][entry]
                a=rng.random()*2*math.pi;right=Vector((math.cos(a),math.sin(a),rng.uniform(-.3,.3))).normalized()
                up=Vector((-right.y,right.x,rng.uniform(.3,1))).normalized()
                center=tip+Vector((rng.uniform(-.32,.32),rng.uniform(-.32,.32),rng.uniform(-.18,.23)))
                h=rng.uniform(.22,.39);leaf.card(center,right,up,h*r[3]/r[4],h,entry,.025)
    a=wood.object('trunk_'+str(variant),[BARK]);b=leaf.object('leaf_'+str(variant),LEAVES)
    a.location=b.location=(variant*6,0,0)
    a['species']=b['species']=group
    return a,b

def build_grass(variant):
    rng=random.Random(1200+variant);g=Builder()
    entries=[i for i,r in enumerate(SPEC['foliage']['grass']['entries']) if (i+variant)%4==0]
    for card in range(11+variant*2):
        entry=entries[card%len(entries)];r=SPEC['foliage']['grass']['entries'][entry]
        angle=card*2.399963;right=Vector((math.cos(angle),math.sin(angle),0))
        up=Vector((math.cos(angle+.7)*.13,math.sin(angle+.7)*.13,1)).normalized()
        center=Vector((rng.uniform(-.23,.23),rng.uniform(-.23,.23),0));h=rng.uniform(.42,.92)
        g.card(center,right,up,h*r[3]/r[4],h,entry,.07)
    obj=g.object('grass_'+str(variant),GRASSES);obj.location=(variant*2,-5,0)

def build_rock(variant):
    rng=random.Random(370+variant)
    bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=4,radius=1)
    obj=bpy.context.object;obj.name='rock_'+str(variant)
    from mathutils import noise
    for v in obj.data.vertices:
        q=v.co.copy();n=noise.noise_vector(q*2.2+Vector((variant*3,0,0)))
        v.co.x=q.x*(1+.12*n.x)*[1.15,.88,1.3,.95][variant]
        v.co.y=q.y*(1+.12*n.y)*[.85,1.1,.9,1.2][variant]
        v.co.z=max(-.62,q.z*(.8+.15*n.z))+.62
    obj.data.materials.append(ROCK)
    # Rounded fracture edges preserve a deliberate weathered silhouette.
    for poly in obj.data.polygons:poly.use_smooth=True
    bpy.context.view_layer.objects.active=obj
    obj.select_set(True);bpy.ops.object.mode_set(mode='EDIT');bpy.ops.mesh.select_all(action='SELECT');bpy.ops.uv.smart_project(island_margin=.015);bpy.ops.object.mode_set(mode='OBJECT')
    ASSETS.append(obj);obj.location=(variant*3,-9,0);obj.select_set(False)

def export():
    meshes=[];stats=[]
    for obj in ASSETS:
        mesh=obj.data;mesh.calc_loop_triangles();mesh.calc_tangents(uvmap='UVMap')
        vertices=[]
        for tri in mesh.loop_triangles:
            for li in tri.loops:
                loop=mesh.loops[li];p=mesh.vertices[loop.vertex_index].co;n=loop.normal;t=loop.tangent;uv=mesh.uv_layers.active.data[li].uv
                # Blender Z-up to engine Y-up: proper rotation, no UV flip.
                vertices.append([p.x,p.z,-p.y,n.x,n.z,-n.y,uv.x,uv.y,t.x,t.z,-t.y,loop.bitangent_sign,tri.material_index])
        meshes.append({'name':obj.name,'vertices':vertices})
        stats.append({'name':obj.name,'triangles':len(vertices)//3,'bounds_blender_m':[list(min(v.co[i] for v in mesh.vertices) for i in range(3)),list(max(v.co[i] for v in mesh.vertices) for i in range(3))],'species':obj.get('species','')})
    OUT.mkdir(parents=True,exist_ok=True)
    (OUT/'environment_meshes.json').write_text(json.dumps({'version':1,'meshes':meshes},separators=(',',':')),encoding='utf-8')
    (OUT/'asset_manifest.json').write_text(json.dumps({'units':'metres','coordinate_export':'Y-up, right-handed rotation from Blender','assets':stats},indent=2),encoding='utf-8')
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT/'project_hs_environment_v9.blend'))
    print(json.dumps(stats))

if __name__ == '__main__':
    for variant in range(3): build_tree(variant)
    for variant in range(4):
        build_grass(variant)
        build_rock(variant)
    export()
