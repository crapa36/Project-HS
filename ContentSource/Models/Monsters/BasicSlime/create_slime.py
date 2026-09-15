"""Run in Blender via MCP. Authored in meters, front -Y, Z up."""
import bpy
import math
import json
from pathlib import Path
import numpy as np

BASE = Path(__file__).resolve().parent
ANIMS = BASE.parents[2] / 'Animations' / 'Monsters' / 'BasicSlime'
ANIMS.mkdir(parents=True, exist_ok=True)
(BASE/'BasicSlime.fbm').mkdir(exist_ok=True)
# Preserve the user's scene, including unsaved objects, in the blend file.
scene = bpy.data.scenes.new('BasicSlime_Authoring')
bpy.context.window.scene = scene
scene.unit_settings.system = 'METRIC'
scene.unit_settings.scale_length = 1.0
scene.render.fps = 60

# Continuous spherical UVs; broad painted lighting remains readable without transparency.
N = 1024
u, v = np.meshgrid((np.arange(N)+.5)/N, (np.arange(N)+.5)/N)
theta = (u-.5)*2*math.pi
light = .14*np.cos(theta-.65)*np.sin(v*math.pi) + .22*v
rgb = np.stack([.035+light*.35, .57+light, .72+light*.85], axis=-1)
def paint(mask, color):
    global rgb
    rgb = rgb*(1-mask[...,None]) + np.array(color)*mask[...,None]
def ellipse(x,y,rx,ry):
    return np.clip((1-((u-x)/rx)**2-((v-y)/ry)**2)*70,0,1)
# Two oversized fixed eyes, visible from above, and one small smile.
for x in (.435,.565):
    paint(ellipse(x,.55,.032,.079),(.012,.037,.073))
    paint(ellipse(x-.008,.575,.010,.021),(.93,1,1))
    paint(ellipse(x+.010,.529,.0045,.009),(.20,.67,.76))
paint(ellipse(.5,.443,.022,.023),(.018,.07,.10))
paint(ellipse(.5,.458,.026,.020),(.12,.77,.86))
paint(ellipse(.383,.75,.037,.045),(.72,.98,1))
paint(ellipse(.427,.797,.012,.020),(.89,1,1))
rgba=np.concatenate([np.clip(rgb,0,1),np.ones((N,N,1))],axis=-1).astype(np.float32)
def save_image(name,pixels,noncolor=False):
    im=bpy.data.images.new(name,width=N,height=N,alpha=True)
    if noncolor: im.colorspace_settings.name='Non-Color'
    im.pixels.foreach_set(pixels.ravel())
    im.filepath_raw=str(BASE/'BasicSlime.fbm'/(name+'.png'))
    im.file_format='PNG'
    im.save()
    return im
albedo=save_image('BasicSlime_BaseColor',rgba)
normal=np.ones((N,N,4),dtype=np.float32)
normal[:,:,0]=.5+.012*np.sin(theta*3)*np.sin(v*math.pi)**2
normal[:,:,1]=.5+.009*np.sin(v*4*math.pi)
normal[:,:,2]=np.sqrt(1-(normal[:,:,0]*2-1)**2-(normal[:,:,1]*2-1)**2)*.5+.5
norm=save_image('BasicSlime_Normal',normal,True)
mat=bpy.data.materials.new('BasicSlime_Jelly')
mat.use_nodes=True
nodes=mat.node_tree.nodes
p=nodes.get('Principled BSDF')
p.inputs['Roughness'].default_value=.28
p.inputs['Metallic'].default_value=0
tex=nodes.new('ShaderNodeTexImage'); tex.image=albedo
mat.node_tree.links.new(tex.outputs['Color'],p.inputs['Base Color'])
nt=nodes.new('ShaderNodeTexImage'); nt.image=norm
nm=nodes.new('ShaderNodeNormalMap')
mat.node_tree.links.new(nt.outputs['Color'],nm.inputs['Color'])
mat.node_tree.links.new(nm.outputs['Normal'],p.inputs['Normal'])

# Closed lathed jelly body: broad stable bottom and gently domed upper body.
verts=[]; faces=[]; uvfaces=[]
RINGS=32; SEG=64
verts.append((0,0,0))
for j in range(1,RINGS):
    a=math.pi*j/RINGS
    z=.42*(1-math.cos(a))/2
    radius=.30*math.sin(a)**.75*(1.12-.26*z/.42)
    for i in range(SEG):
        t=2*math.pi*(i/SEG-.5)
        verts.append((radius*math.sin(t),-radius*math.cos(t),z))
verts.append((0,0,.42)); top=len(verts)-1
for i in range(SEG):
    k=(i+1)%SEG
    faces.append((0,1+k,1+i)); uvfaces.append(((i+.5)/SEG,0,(i+1)/SEG,1/RINGS,i/SEG,1/RINGS))
for j in range(RINGS-2):
    for i in range(SEG):
        k=(i+1)%SEG; a=1+j*SEG; b=a+SEG
        faces.append((a+i,a+k,b+k,b+i))
        uvfaces.append((i/SEG,(j+1)/RINGS,(i+1)/SEG,(j+1)/RINGS,(i+1)/SEG,(j+2)/RINGS,i/SEG,(j+2)/RINGS))
for i in range(SEG):
    k=(i+1)%SEG; a=1+(RINGS-2)*SEG
    faces.append((a+i,a+k,top)); uvfaces.append((i/SEG,(RINGS-1)/RINGS,(i+1)/SEG,(RINGS-1)/RINGS,(i+.5)/SEG,1))
mesh=bpy.data.meshes.new('BasicSlime_Surface'); mesh.from_pydata(verts,[],faces); mesh.update()
obj=bpy.data.objects.new('BasicSlime',mesh); scene.collection.objects.link(obj)
uv=mesh.uv_layers.new(name='UVMap')
for poly,coords in zip(mesh.polygons,uvfaces):
    poly.use_smooth=True
    for n,li in enumerate(poly.loop_indices): uv.data[li].uv=coords[n*2:n*2+2]
obj.data.materials.append(mat)
arm=bpy.data.armatures.new('BasicSlime_Rig'); rig=bpy.data.objects.new('BasicSlime_Rig',arm)
scene.collection.objects.link(rig); bpy.context.view_layer.objects.active=rig; rig.select_set(True)
bpy.ops.object.mode_set(mode='EDIT')
positions={'root':(0,0,0),'body_center':(0,0,.16),'body_top':(0,0,.34),'body_front':(0,-.22,.17),'body_back':(0,.22,.17),'body_left':(-.22,0,.17),'body_right':(.22,0,.17)}
for name,pos in positions.items():
    b=arm.edit_bones.new(name); b.head=pos; b.tail=(pos[0],pos[1],pos[2]+.06)
    if name!='root': b.parent=arm.edit_bones['root' if name=='body_center' else 'body_center']
bpy.ops.object.mode_set(mode='OBJECT')
groups={n:obj.vertex_groups.new(name=n) for n in positions}
for i,(x,y,z) in enumerate(verts):
    if i == 0:
        groups['root'].add([i],1.0,'REPLACE')
        continue
    weights={'body_center':.55,'body_top':max(0,(z-.18)/.24)*.6,'body_front':max(0,-y/.30)*.45,'body_back':max(0,y/.30)*.45,'body_left':max(0,-x/.30)*.45,'body_right':max(0,x/.30)*.45}
    weights=sorted(weights.items(),key=lambda p:p[1],reverse=True)[:4]; total=sum(w for _,w in weights)
    for n,w in weights:
        if w>0: groups[n].add([i],w/total,'REPLACE')
mod=obj.modifiers.new('Skin','ARMATURE'); mod.object=rig; obj.parent=rig
scene['slime_mesh']=obj.name
scene['slime_rig']=rig.name
print(json.dumps({'mesh_vertices':len(verts),'triangles':sum(len(p.vertices)-2 for p in mesh.polygons),'bones':len(arm.bones),'source_dimensions':list(obj.dimensions)}))
