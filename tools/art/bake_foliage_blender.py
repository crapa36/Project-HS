import bpy, math, json
from pathlib import Path
ROOT=Path(r'C:/Users/crapa/Documents/GitHub/Project HS')
PACK=ROOT/'ContentSource/Textures/Environment'
SPEC=json.loads((PACK/'project_hs_environment_materials_v9_LEAN.json').read_text())
def bake_foliage_page(group,page):
    scene=bpy.context.scene
    col=bpy.data.collections.new('HP_Atlas_'+group+'_'+str(page));scene.collection.children.link(col)
    highverts=[];highfaces=[];highuv=[];lowverts=[];lowfaces=[];lowuv=[]
    for entry in SPEC['foliage'][group]['entries']:
        if entry[0]!=page:continue
        _,x,y,w,h,_=entry
        # A physical leaf fold/midrib or grass curvature, in metres, preserves the
        # photographed silhouette through source opacity while geometry supplies relief.
        nx=max(8,round(48*w/max(w,h)));ny=max(8,round(48*h/max(w,h)))
        start=len(highverts)
        for j in range(ny+1):
            for i in range(nx+1):
                u=i/nx;v=j/ny;px=(x+u*w)/2048;py=(y+v*h)/2048
                fold=.018*math.sin(math.pi*u)*math.sin(math.pi*v)
                vein=.003*math.exp(-((u-.5)/.05)**2)*math.sin(math.pi*v)
                highverts.append((px*2,py*2,fold+vein));highuv.append((px,1-py))
        for j in range(ny):
            for i in range(nx):
                a=start+j*(nx+1)+i;highfaces.append((a,a+1,a+nx+2,a+nx+1))
        a=len(lowverts)
        for u,v in [(0,0),(1,0),(1,1),(0,1)]:
            px=(x+u*w)/2048;py=(y+v*h)/2048;lowverts.append((px*2,py*2,0));lowuv.append((px,1-py))
        lowfaces.append((a,a+1,a+2,a+3))
    def obj(name,verts,faces,uv):
        m=bpy.data.meshes.new(name);m.from_pydata(verts,[],faces);m.update();o=bpy.data.objects.new(name,m);col.objects.link(o)
        layer=m.uv_layers.new(name='UV0')
        for poly in m.polygons:
            poly.use_smooth=True
            for li in poly.loop_indices:layer.data[li].uv=uv[m.loops[li].vertex_index]
        return o
    hp=obj('HP_AtlasSurface_'+group+'_'+str(page),highverts,highfaces,highuv)
    low=obj('BakeTarget_'+group+'_'+str(page),lowverts,lowfaces,lowuv)
    mat=bpy.data.materials.new('AtlasReference_'+group+'_'+str(page));mat.use_nodes=True;nodes=mat.node_tree.nodes;links=mat.node_tree.links;bs=nodes.get('Principled BSDF')
    source=SPEC['foliage'][group]['pages'][page]
    tex=nodes.new('ShaderNodeTexImage');tex.image=bpy.data.images.load(str(PACK/source['normal']),check_existing=True);tex.image.colorspace_settings.name='Non-Color'
    nm=nodes.new('ShaderNodeNormalMap');links.new(tex.outputs['Color'],nm.inputs['Color']);links.new(nm.outputs['Normal'],bs.inputs['Normal']);hp.data.materials.append(mat)
    destmat=bpy.data.materials.new('AtlasBakeTarget_'+group+'_'+str(page));destmat.use_nodes=True;low.data.materials.append(destmat)
    im=bpy.data.images.new('BakedAtlas_'+group+'_'+str(page),width=2048,height=2048,alpha=False,float_buffer=False);im.colorspace_settings.name='Non-Color';im.generated_color=(.5,.5,1,1)
    tex=destmat.node_tree.nodes.new('ShaderNodeTexImage');tex.image=im;destmat.node_tree.nodes.active=tex
    bpy.ops.object.select_all(action='DESELECT');hp.hide_set(False);low.hide_set(False);hp.select_set(True);low.select_set(True);bpy.context.view_layer.objects.active=low
    scene.render.bake.use_selected_to_active=True;scene.render.bake.use_clear=False;scene.render.bake.cage_extrusion=.05;scene.render.bake.max_ray_distance=.1;scene.render.bake.margin=8
    bpy.ops.object.bake(type='NORMAL')
    # Atlas bake UV.v points upwards; game card localUV.v points downwards.
    import numpy as np
    pixels=np.empty(2048*2048*4,dtype=np.float32);im.pixels.foreach_get(pixels);pixels[1::4]=1-pixels[1::4];im.pixels.foreach_set(pixels)
    name='foliage_'+group+'_atlas_'+str(page)+'_normal.png';im.filepath_raw=str(PACK/'baked'/name);im.file_format='PNG';im.save()
    hp.hide_render=True;low.hide_render=True;hp.hide_set(True);low.hide_set(True)
    print(name,len(highfaces),'high-poly surface quads baked')
