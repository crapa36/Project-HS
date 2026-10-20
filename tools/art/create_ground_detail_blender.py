"""Author Project HS ground detail with Blender 5.2, without network assets.

Run: blender --background --factory-startup --python this_file -- --root REPO --stage geometry
Then inspect preview/blockout.png before running --stage finish (loads the checkpoint).
--stage verify reopens the final .blend and round-trips all seven FBX exports.
Use --revision v002 for foliage refinement and --stage register after visual/export review.
Runtime JSON uses indexed triangle lists in (x,z,-y), with UV V flipped before tangent calculation.
"""
import argparse, hashlib, json, math, random, struct, sys, zlib
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from mesh_indexing import index_mesh
import bpy
import numpy as np
from mathutils import Vector

ARGS = argparse.ArgumentParser()
ARGS.add_argument("--root", type=Path, required=True)
ARGS.add_argument("--stage", choices=("geometry", "finish", "verify", "register"), required=True)
ARGS.add_argument("--revision", choices=("v001", "v002"), default="v001")
OPT = ARGS.parse_args(sys.argv[sys.argv.index("--") + 1:])
ROOT = OPT.root.resolve()
SOURCE = ROOT / "ContentSource/Models/Environment/GroundDetail"
OUT = SOURCE if OPT.revision == "v001" else SOURCE / OPT.revision
PACK = ROOT / "ContentSource/Textures/Environment"
TEX = PACK / "textures"
OUT.mkdir(parents=True, exist_ok=True)
(OUT / "preview").mkdir(exist_ok=True)
(OUT / "verification").mkdir(exist_ok=True)
(OUT / "FBX").mkdir(exist_ok=True)
(OUT / "FBXTextures").mkdir(exist_ok=True)
NAMES = ("flower_daisy", "flower_buttercup", "flower_violet", "clover_patch",
         "ground_leaves", "twig_cluster", "fence_short")
TILES = ("leaf_green", "clover", "daisy", "buttercup", "violet_upper", "violet_lower",
         "pollen", "stem", "leaf_ochre", "leaf_brown", "leaf_red", "twig_bark",
         "fence_wood", "fence_end", "lichen_bark", "leaf_under")
SIZE, CELL, GUTTER = 2048, 512, 24
ATLAS_NAMES = {"basecolor": "ground_detail_basecolor.png",
               "surface": "ground_detail_surface_ra.png", "normal": "ground_detail_normal.png"}


def write_png(path, array):
    array = np.asarray(array, dtype=np.uint8)
    h, w, channels = array.shape
    assert channels in (3, 4)
    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff)
    raw = b"".join(b"\0" + row.tobytes() for row in array)
    encoded=b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6 if channels == 4 else 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    temporary=path.with_suffix(path.suffix+".tmp");temporary.write_bytes(encoded);temporary.replace(path)


def read_png_rgb(path):
    """Read the existing 8-bit RGB/RGBA PNG source without an extra dependency."""
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    w, h, bits, color, comp, filt, interlace = struct.unpack(">IIBBBBB", data[16:29])
    assert bits == 8 and color in (2, 6) and interlace == 0
    c = 3 if color == 2 else 4
    cursor, packed = 8, bytearray()
    while cursor < len(data):
        n = struct.unpack(">I", data[cursor:cursor + 4])[0]
        kind = data[cursor + 4:cursor + 8]
        if kind == b"IDAT": packed.extend(data[cursor + 8:cursor + 8 + n])
        cursor += n + 12
    raw = zlib.decompress(packed)
    result = np.empty((h, w, c), np.uint8)
    stride = w * c
    previous = np.zeros(stride, np.uint8)
    for y in range(h):
        k = y * (stride + 1)
        mode = raw[k]
        row = np.frombuffer(raw[k + 1:k + stride + 1], np.uint8).copy()
        for x in range(stride):
            a = int(row[x-c]) if x >= c else 0
            b = int(previous[x])
            cc = int(previous[x-c]) if x >= c else 0
            if mode == 1: predict = a
            elif mode == 2: predict = b
            elif mode == 3: predict = (a+b)//2
            elif mode == 4:
                p = a + b - cc
                pa, pb, pc = abs(p-a), abs(p-b), abs(p-cc)
                predict = a if pa <= pb and pa <= pc else b if pb <= pc else cc
            elif mode == 0: predict = 0
            else: raise ValueError("Unsupported PNG filter")
            row[x] = (int(row[x]) + predict) & 255
        result[y] = row.reshape(w, c)
        previous = row
    return result[..., :3] / 255.0


def atlas():
    n = CELL - GUTTER * 2
    yy, xx = np.mgrid[0:n, 0:n] / (n-1)
    x, t = xx * 2 - 1, 1 - yy
    rng = np.random.default_rng(813762)
    base = np.zeros((SIZE, SIZE, 4), np.uint8); base[..., 3] = 255
    surf = base.copy(); normal = base.copy()
    # Authored project bark is reused only inside two new atlas cells.
    bark = read_png_rgb(TEX / "mesh_bark_basecolor.png")
    sampled_bark = bark[np.linspace(0, bark.shape[0]-1, n).astype(int)[:, None],
                        np.linspace(0, bark.shape[1]-1, n).astype(int)[None, :]]
    provenance = {"source": "textures/mesh_bark_basecolor.png", "sha256": hashlib.sha256((TEX/"mesh_bark_basecolor.png").read_bytes()).hexdigest()}
    for i, name in enumerate(TILES):
        noise = rng.normal(0, .009, (n,n))
        broad = .025*np.sin(xx*19 + np.sin(yy*9)) + .013*np.cos(yy*31 + xx*8)
        center = np.exp(-x*x*55)
        side_veins = np.exp(-((np.mod(t*6 + np.abs(x)*1.3, 1)-.08)/.025)**2)
        veins = np.maximum(center, side_veins * np.exp(-np.abs(x)*.7))
        height = .25*veins + broad*.7 + noise*.5
        ao = .97 - .07*veins
        rough = np.full((n,n), .76)
        if name == "leaf_green":
            rgb = np.array([.21,.34,.105]) + (broad + noise)[...,None]
            rgb += (veins*.035)[...,None] * np.array([.5,1,.25])
            rgb -= ((np.abs(x)**2)*.035)[...,None]
        elif name == "clover":
            rgb = np.array([.17,.31,.075]) + (broad + noise)[...,None]
            chevron = np.exp(-((t-(.64-.27*np.abs(x)))/.045)**4)
            rgb += chevron[...,None] * np.array([.15,.16,.095])
            rgb += (veins*.025)[...,None]
            height += chevron*.025
        elif name in ("daisy", "buttercup", "violet_upper", "violet_lower"):
            if name == "daisy":
                rgb = np.zeros((n,n,3)) + [.79,.765,.68]
                rgb -= ((1-t)**3)[...,None] * [.14,.17,.18]
                rough[:] = .61
            elif name == "buttercup":
                rgb = np.zeros((n,n,3)) + [.91,.65,.075]
                rgb -= ((1-t)**2)[...,None] * [.13,.21,.03]
                rough[:] = .39
            else:
                rgb = np.zeros((n,n,3)) + ([.39,.19,.53] if name=="violet_upper" else [.43,.25,.58])
                throat = np.exp(-x*x*7) * np.exp(-((t-.1)/.26)**2)
                rgb += throat[...,None] * [.24,.25,.10]
                striae = (np.sin((x/(.23+t))*42)>.80) * np.exp(-t*4)
                rgb -= striae[...,None] * [.16,.10,.18]
                rough[:] = .58
            rib = np.sin(x*48 + t*4) * .5 + .5
            rgb += (rib*.025 + noise*.5)[...,None]
            height = rib*.075 + .14*center + noise*.25
            ao = np.full((n,n),.98)
        elif name == "pollen":
            cells = np.sin(xx*95)*np.cos(yy*97)
            rgb = np.zeros((n,n,3)) + [.70,.43,.045] + (cells*.055 + noise)[...,None]
            height = cells*.17 + noise
            rough[:] = .88
        elif name == "stem":
            grain = np.sin(xx*80 + yy*6)
            rgb = np.zeros((n,n,3)) + [.18,.29,.075] + (grain*.018+noise)[...,None]
            height = grain*.05
        elif name in ("leaf_ochre","leaf_brown","leaf_red","leaf_under"):
            color = {"leaf_ochre":[.43,.265,.105],"leaf_brown":[.30,.18,.08],
                     "leaf_red":[.37,.17,.075],"leaf_under":[.33,.275,.13]}[name]
            rgb = np.zeros((n,n,3)) + color + (broad+noise)[...,None]
            rgb += veins[...,None] * [.065,.05,.023]
            freckles = np.maximum(0,np.sin(xx*45+np.sin(yy*39)*2)*np.cos(yy*56+xx*17)-.5)
            rgb -= freckles[...,None]*.16
            rgb -= (np.abs(x)**5)[...,None]*.055
            rough[:] = .91
            height += freckles*.05
        elif name in ("twig_bark","lichen_bark"):
            rgb = sampled_bark * [.68,.67,.60]
            luma = np.mean(sampled_bark,axis=2)
            height = luma*.65 + noise
            ao = np.clip(.70+luma*.4,.7,1)
            rough[:] = .91
            if name=="lichen_bark":
                patch = np.clip((np.sin(xx*31+np.cos(yy*25))*np.cos(yy*32)-.05)*2,0,1)
                rgb = rgb*(1-patch[...,None]*.55) + patch[...,None]*[.21,.25,.10]
        elif name=="fence_wood":
            grain = np.sin(xx*130 + np.sin(yy*13)*7 + np.sin(xx*17+yy*6)*3)
            fine = np.sin(xx*370+yy*8)
            knot = np.sqrt(((xx-.62)/.23)**2+((yy-.39)/.12)**2)
            rings = np.sin(knot*32)*np.exp(-knot*.65)
            cracks = np.maximum(0,grain-.94)*np.clip(np.sin(yy*21)*.5+.3,0,1)*.9
            photo_luma=np.mean(sampled_bark,axis=2)
            rgb = np.zeros((n,n,3)) + [.39,.35,.29] + (grain*.012+fine*.005+rings*.018+broad*.7+noise*.65+(photo_luma-.4)*.035)[...,None]
            rgb -= cracks[...,None]
            height = grain*.018+fine*.004+rings*.022-cracks*.10
            ao = .97-cracks*.6
            rough[:] = .92
        else:
            radius = np.sqrt((x*.8)**2+((t-.5)*2)**2)
            rings = np.sin(radius*55+np.sin(xx*13)*.5)
            split = np.exp(-((xx-.42-yy*.12)/.009)**2)
            rgb = np.zeros((n,n,3)) + [.41,.32,.22] + (rings*.035+noise)[...,None] - split[...,None]*.18
            height = rings*.06-split*.13
            rough[:] = .94
        dy, dx = np.gradient(height)
        v = np.stack((-dx*22, -dy*22, np.ones_like(dx)),axis=2)
        v /= np.linalg.norm(v,axis=2)[...,None]
        data = {"basecolor":np.dstack((np.clip(rgb,0,1), np.ones_like(xx))),
                "surface":np.dstack((np.clip(rough+noise*.7,0,1), np.clip(ao,0,1),np.zeros_like(xx),np.ones_like(xx))),
                "normal":np.dstack((v*.5+.5,np.ones_like(xx)))}
        by, bx = (i//4)*CELL, (i%4)*CELL
        for target, role in ((base,"basecolor"),(surf,"surface"),(normal,"normal")):
            rgba = np.rint(np.clip(data[role],0,1)*255).astype(np.uint8)
            target[by:by+CELL,bx:bx+CELL] = np.pad(rgba,((GUTTER,GUTTER),(GUTTER,GUTTER),(0,0)),mode="edge")
    for role,data in (("basecolor",base),("surface",surf),("normal",normal)):
        write_png(TEX/ATLAS_NAMES[role],data[...,:3] if role=="normal" else data)
    preview_normal=normal.copy();preview_normal[...,1]=255-preview_normal[...,1]
    write_png(OUT/"FBXTextures"/"ground_detail_normal_opengl.png",preview_normal)
    rough_rgba=np.repeat(surf[...,:1],4,axis=2);rough_rgba[...,3]=255
    write_png(OUT/"FBXTextures"/"ground_detail_roughness.png",rough_rgba)
    report={"size":[SIZE,SIZE],"gutter_px":GUTTER,"opaque_alpha":255,"normal_convention":"Runtime UV V-down; XY=-height image gradients. Blender/FBX +Y copy flips green.",
            "surface_channels":{"R":"roughness","G":"ambient occlusion","B":"unused zero","A":"opaque 255"},
            "source_reuse":[provenance],"tiles":[{"name":name,"rect_px":[(i%4)*CELL+GUTTER,(i//4)*CELL+GUTTER,CELL-2*GUTTER,CELL-2*GUTTER]} for i,name in enumerate(TILES)]}
    (OUT/"atlas_regions.json").write_text(json.dumps(report,indent=2),encoding="utf-8")


def atlas_uv(tile, uv):
    i = TILES.index(tile)
    u, v = uv
    return (((i%4)*CELL+GUTTER+u*(CELL-2*GUTTER))/SIZE,
            1-((i//4)*CELL+GUTTER+(1-v)*(CELL-2*GUTTER))/SIZE)


class Builder:
    def __init__(self):
        self.vertices=[]; self.faces=[]; self.uvs=[]; self.smooth=[]; self.index={}; self.face_keys=set()
    def face(self, points, uvs, tile, smooth=True):
        ids=[]
        for p in points:
            key=tuple(round(float(v),9) for v in p)
            if key not in self.index:
                self.index[key]=len(self.vertices);self.vertices.append(tuple(p))
            ids.append(self.index[key])
        assert len(set(ids))==len(ids), "Degenerate input face"
        key=tuple(sorted(ids))
        if key in self.face_keys:return
        self.face_keys.add(key)
        self.faces.append(ids);self.uvs.append([atlas_uv(tile,uv) for uv in uvs]);self.smooth.append(smooth)
    def tube(self, points, radii, tile="stem", sides=8, cap=True):
        points=[Vector(p) for p in points];rings=[]
        for k,p in enumerate(points):
            tangent=(points[min(k+1,len(points)-1)]-points[max(k-1,0)]).normalized()
            ref=Vector((0,1,0)) if abs(tangent.y)<.9 else Vector((1,0,0))
            a=tangent.cross(ref).normalized();b=tangent.cross(a)
            ring=[p+radii[k]*(a*math.cos(2*math.pi*j/sides)+b*math.sin(2*math.pi*j/sides)) for j in range(sides)]
            rings.append(ring)
        for k in range(len(points)-1):
            for j in range(sides):
                jj=(j+1)%sides
                self.face([rings[k][j],rings[k][jj],rings[k+1][jj],rings[k+1][j]],
                          [(j/sides,k/(len(points)-1)),((j+1)/sides,k/(len(points)-1)),((j+1)/sides,(k+1)/(len(points)-1)),(j/sides,(k+1)/(len(points)-1))],tile)
        if cap:
            for k,reverse in ((0,True),(len(points)-1,False)):
                for j in range(sides):
                    jj=(j+1)%sides
                    pts=[points[k],rings[k][j],rings[k][jj]]
                    uv=[(.5,.5),(.5+.48*math.cos(j*math.tau/sides),.5+.48*math.sin(j*math.tau/sides)),
                        (.5+.48*math.cos((j+1)*math.tau/sides),.5+.48*math.sin((j+1)*math.tau/sides))]
                    if reverse:pts.reverse();uv.reverse()
                    self.face(pts,uv,"fence_end" if tile in ("twig_bark","lichen_bark","fence_wood") else tile,False)
    def sheet(self, center, right, forward, up, length, width, tile, rows=6, cols=3,
              curl=.008, cup=.002, thickness=.0007, serration=0, phase=0, dead=False):
        center,right,forward,up=map(Vector,(center,right,forward,up))
        grids=[]
        for sign in (1,-1):
            grid=[]
            for j in range(rows):
                t=j/(rows-1)
                profile=max(.035,math.sin(math.pi*(.035+.93*t))**.65)
                profile*=1+serration*math.sin(t*math.pi*10+phase)
                line=[]
                for k in range(cols):
                    x=k/(cols-1)*2-1
                    z=curl*t*t+cup*x*x+(.0018 if dead else .0007)*math.sin(t*8+phase)*x
                    p=center+forward*(length*t)+right*(width*.5*profile*x)+up*(z+sign*thickness*.5)
                    line.append(p)
                grid.append(line)
            grids.append(grid)
        for side,grid in enumerate(grids):
            for j in range(rows-1):
                for k in range(cols-1):
                    pts=[grid[j][k],grid[j][k+1],grid[j+1][k+1],grid[j+1][k]]
                    uv=[(k/(cols-1),j/(rows-1)),((k+1)/(cols-1),j/(rows-1)),((k+1)/(cols-1),(j+1)/(rows-1)),(k/(cols-1),(j+1)/(rows-1))]
                    if side:pts.reverse();uv.reverse()
                    self.face(pts,uv,("leaf_under" if dead and side else tile))
        edges=[]
        for k in range(cols-1):edges.append(((0,k),(0,k+1)));edges.append(((rows-1,k+1),(rows-1,k)))
        for j in range(rows-1):edges.append(((j+1,0),(j,0)));edges.append(((j,cols-1),(j+1,cols-1)))
        for (a,b) in edges:
            pts=[grids[0][a[0]][a[1]],grids[0][b[0]][b[1]],grids[1][b[0]][b[1]],grids[1][a[0]][a[1]]]
            uvs=[(a[1]/(cols-1),a[0]/(rows-1)),(b[1]/(cols-1),b[0]/(rows-1)),(b[1]/(cols-1),b[0]/(rows-1)),(a[1]/(cols-1),a[0]/(rows-1))]
            # Give side faces finite UV area as well as physical thickness.
            uvs[2]=(uvs[2][0]+.0005,uvs[2][1]+.0005);uvs[3]=(uvs[3][0]+.0005,uvs[3][1]+.0005)
            self.face(pts,uvs,tile)
    def heart(self, base, direction, length, width, segments, phase, tile="clover", pitch=0):
        base=Vector(base);r=Vector((-math.sin(direction),math.cos(direction),0))
        f=Vector((math.cos(direction)*math.cos(pitch),math.sin(direction)*math.cos(pitch),math.sin(pitch)));up=f.cross(r)
        outline=[]
        for j in range(segments):
            a=math.tau*j/segments
            x=math.sin(a)**3
            y=(13*math.cos(a)-5*math.cos(2*a)-2*math.cos(3*a)-math.cos(4*a)+17)/29
            outline.append((x,y))
        centers=[base+f*(length*.46)+up*.002,base+f*(length*.46)+up*.0012]
        rings=[]
        for sign in (1,-1):
            rings.append([base+r*(width*.5*x)+f*(length*y)+up*(.002*y+.004*x*x+.001*math.sin(y*8+phase)+sign*.0004) for x,y in outline])
        for j in range(segments):
            k=(j+1)%segments
            uv=[(.5,.46),(.5+outline[j][0]*.5,outline[j][1]),(.5+outline[k][0]*.5,outline[k][1])]
            # The heart parametrization runs clockwise.
            self.face([centers[0],rings[0][k],rings[0][j]], [uv[0],uv[2],uv[1]],tile)
            self.face([centers[1],rings[1][j],rings[1][k]],uv,tile)
            self.face([rings[0][j],rings[0][k],rings[1][k],rings[1][j]],
                      [uv[1],uv[2],(uv[2][0]+.0005,uv[2][1]+.0005),(uv[1][0]+.0005,uv[1][1]+.0005)],tile)
    def dome(self, center, radius, height, rings=3, sides=12, tile="pollen"):
        center=Vector(center)
        for j in range(rings):
            a0=j/rings*math.pi*.5;a1=(j+1)/rings*math.pi*.5
            # Last ring ends below the exact pole; cap separately without collapsed quads.
            if j==rings-1:a1=math.pi*.5-.08
            for k in range(sides):
                aa=k/sides*math.tau;bb=(k+1)/sides*math.tau
                p=lambda a,t:center+Vector((math.cos(a)*math.cos(t)*radius,math.cos(a)*math.sin(t)*radius,math.sin(a)*height))
                self.face([p(a0,aa),p(a0,bb),p(a1,bb),p(a1,aa)],[(k/sides,j/rings),((k+1)/sides,j/rings),((k+1)/sides,(j+1)/rings),(k/sides,(j+1)/rings)],tile)
        for k in range(sides):
            a=k/sides*math.tau;b=(k+1)/sides*math.tau
            self.face([center+Vector((radius*math.sin(.08)*math.cos(a),radius*math.sin(.08)*math.sin(a),height*math.cos(.08))),
                       center+Vector((radius*math.sin(.08)*math.cos(b),radius*math.sin(.08)*math.sin(b),height*math.cos(.08))),
                       center+Vector((0,0,height))],[(.1,.1),(.9,.1),(.5,.9)],tile)
        # Hidden underside is still opaque and has a valid downward silhouette.
        for k in range(sides):
            a=k/sides*math.tau;b=(k+1)/sides*math.tau
            self.face([center,center+Vector((radius*math.cos(b),radius*math.sin(b),0)),
                       center+Vector((radius*math.cos(a),radius*math.sin(a),0))],[(.5,.5),(.1,.9),(.9,.9)],tile)
    def object(self,name,collection):
        mesh=bpy.data.meshes.new("GEO-"+name);mesh.from_pydata(self.vertices,[],self.faces);mesh.update()
        uv=mesh.uv_layers.new(name="UV0")
        for polygon,uvs,smooth in zip(mesh.polygons,self.uvs,self.smooth):
            polygon.use_smooth=smooth
            polygon.material_index=0
            for li,value in zip(polygon.loop_indices,uvs):uv.data[li].uv=value
        import bmesh
        bm=bmesh.new();bm.from_mesh(mesh);bmesh.ops.recalc_face_normals(bm,faces=list(bm.faces));bm.to_mesh(mesh);bm.free()
        ob=bpy.data.objects.new(name,mesh);collection.objects.link(ob);ob["hs_asset_name"]=name
        return ob


def flowers(name, lod):
    b=Builder();rng=random.Random(3100+NAMES.index(name)*71)
    count=(7,5,3)[lod];rows=(5,4,3)[lod];sides=(8,6,5)[lod]
    centers=[(-.21,-.16),(.25,.20),(-.27,.23),(.03,-.21),(.22,-.02),(-.15,.10),(.07,.16)]
    for i,(cx,cy) in enumerate(centers[:count]):
        rng=random.Random(3100+NAMES.index(name)*71+i*983)
        cx+=rng.uniform(-.018,.018);cy+=rng.uniform(-.018,.018)
        h=(.43 if i==0 else rng.uniform(.27,.40)) if name!="flower_violet" else (.31 if i==0 else rng.uniform(.22,.29))
        bend=Vector((rng.uniform(-.025,.025),rng.uniform(-.035,.035),0))
        p=Vector((cx,cy,0))
        path=[p+Vector((0,0,h*t))+bend*(t*t) for t in np.linspace(0,1,(5,4,3)[lod])]
        b.tube(path,[.0024*(1-.40*j/(len(path)-1)) for j in range(len(path))],sides=sides)
        for k in range((3,2,1)[lod] if name!="flower_violet" else 0):
            t=.24+k*.18;angle=rng.uniform(0,math.tau)
            f=Vector((math.cos(angle),math.sin(angle),.35)).normalized();r=Vector((-math.sin(angle),math.cos(angle),0))
            b.sheet(p+Vector((0,0,h*t))+bend*t*t,r,f,(0,0,1),rng.uniform(.050,.075),.017,"leaf_green",rows=rows,curl=.006,cup=.003,serration=.12,phase=i)
        head=path[-1]
        rng=random.Random(6200+NAMES.index(name)*71+i*983)
        tilt=Vector((rng.uniform(-.18,.18),rng.uniform(-.18,.18),1)).normalized()
        if name=="flower_violet":
            tilt=Vector((rng.uniform(-.22,.22),-.40,.92)).normalized()
            for k in range((3,2,1)[lod]):
                a=rng.uniform(0,math.tau)
                basal=Vector((cx+.018*math.cos(a),cy+.018*math.sin(a),rng.uniform(.025,.05)))
                b.tube([(cx,cy,0),basal],[.0014,.0008],sides=sides)
                b.heart(basal,a,.047,.036,(20,16,10)[lod],i,"leaf_green",pitch=rng.uniform(.15,.50))
        rx=Vector((1,0,-tilt.x/max(tilt.z,.1))).normalized();ry=tilt.cross(rx).normalized()
        if name=="flower_daisy":
            petals=(12,10,8)[lod];radius=rng.uniform(.045,.058)
            for j in range(petals):
                a=j/petals*math.tau+rng.uniform(-.045,.045)
                f=rx*math.cos(a)+ry*math.sin(a);r=tilt.cross(f)
                b.sheet(head+f*.006,r,f,tilt,radius*rng.uniform(.88,1.12),.017*rng.uniform(.8,1.1),"daisy",rows=rows,curl=rng.uniform(-.006,.002),cup=.0018,phase=j)
            b.dome(head,.0135,.0075,rings=(3,2,1)[lod],sides=(16,12,8)[lod])
        elif name=="flower_buttercup":
            for j in range(5):
                a=j/5*math.tau+rng.uniform(-.08,.08);f=rx*math.cos(a)+ry*math.sin(a);r=tilt.cross(f)
                b.sheet(head+f*.002,r,f,tilt,rng.uniform(.035,.042),rng.uniform(.030,.037),"buttercup",rows=rows,curl=.014,cup=.005,phase=j)
            b.dome(head+tilt*.001,.009,.005,rings=(3,2,1)[lod],sides=(12,10,6)[lod])
            if lod==0:
                for j in range(9):
                    a=j/9*math.tau;b.tube([head+Vector((.005*math.cos(a),.005*math.sin(a),.002)),head+Vector((.006*math.cos(a),.006*math.sin(a),.008))],[.0008,.0011],"pollen",sides=4)
        else:
            for j in range(5):
                a=j/5*math.tau+math.pi*.5;f=rx*math.cos(a)+ry*math.sin(a);r=tilt.cross(f)
                bottom=j==2
                b.sheet(head+f*.003,r,f,tilt,.050 if bottom else .040,.037 if bottom else .030,
                        "violet_lower" if bottom else "violet_upper",rows=rows,curl=(-.009 if bottom else .004),cup=.0035,phase=j)
            # Violet rear spur and small yellow throat are geometry, not a painted sphere.
            b.tube([head,head-ry*.014+Vector((0,0,-.005)),head-ry*.022+Vector((0,0,-.008))],[.0035,.0025,.0009],"violet_upper",sides=sides)
            b.sheet(head+Vector((0,0,.001)),rx,ry,tilt,.011,.009,"pollen",rows=3,curl=.001)
        # Green sepal geometry peeks below the flower head.
        if lod<2:
            for j in range(5):
                a=j/5*math.tau;f=rx*math.cos(a)+ry*math.sin(a);r=tilt.cross(f)
                b.sheet(head-tilt*.001,r,f,-tilt,.013,.007,"stem",rows=3,curl=.003)
    return b


def clover(lod):
    b=Builder();rng=random.Random(493)
    points=[(-.22,-.18),(.25,-.02),(-.23,.19),(.24,.23),(-.07,-.20),(.14,-.18),(-.18,.02),(0,.02),(.15,.09),(-.08,.20),(.08,.24),(-.3,-.05)]
    for i,(x,y) in enumerate(points[:(12,8,4)[lod]]):
        h=.15 if i==0 else rng.uniform(.060,.135);a=rng.uniform(0,math.tau);head=Vector((x+.008*math.cos(a),y+.008*math.sin(a),h))
        b.tube([(x,y,0),(x,y,h*.5),head],[.0014,.0012,.0009],sides=(7,5,4)[lod])
        for j in range(3):
            angle=a+j*math.tau/3
            b.heart(head,angle,rng.uniform(.033,.045),rng.uniform(.029,.040),(20,16,10)[lod],i*.8,pitch=rng.uniform(-.20,.55))
        if i < (2,1,1)[lod]:
            bloom=head+Vector((-.014,.008,.060))
            b.tube([head,bloom],[.0011,.0008],sides=(6,5,4)[lod])
            florets=(8,6,4)[lod]
            for j in range(florets):
                angle=j*math.tau/florets
                center=bloom+Vector((.008*math.cos(angle),.008*math.sin(angle),.003*(j%2)))
                b.dome(center,.0045,.009,rings=(2,1,1)[lod],sides=(6,5,4)[lod],tile="violet_lower")
    return b


def leaves(lod):
    b=Builder();rng=random.Random(816)
    for i in range((17,11,6)[lod]):
        x=rng.uniform(-.32,.30);y=rng.uniform(-.27,.28);a=rng.uniform(0,math.tau)
        f=Vector((math.cos(a),math.sin(a),0));r=Vector((-f.y,f.x,0))
        b.sheet((x,y,.001+i*.0003),r,f,(0,0,1),rng.uniform(.105,.17),rng.uniform(.058,.096),
                ("leaf_ochre","leaf_brown","leaf_red")[i%3],rows=(9,6,4)[lod],cols=3,curl=rng.uniform(-.003,.010),
                cup=rng.uniform(.002,.006),thickness=.0008,serration=.20 if i%3==0 else .09,phase=i*.7,dead=True)
        if lod==0:
            b.tube([(x,y,.002),(x-f.x*.022,y-f.y*.022,.002)], [.0008,.0004],"twig_bark",sides=4)
    # Keep the dried patch within two centimetres of the soil without floating tips.
    minz=min(p[2] for p in b.vertices);maxz=max(p[2] for p in b.vertices)
    b.vertices=[(p[0],p[1],(p[2]-minz)/max(maxz-minz,1e-6)*.020) for p in b.vertices]
    return b


def twigs(lod):
    b=Builder();rng=random.Random(974)
    angles=(.18,.39,.61,.22,1.05)
    lengths=(.62,.47,.54,.33,.25)
    for i in range((5,4,3)[lod]):
        a=angles[i]+rng.uniform(-.10,.10);d=Vector((math.cos(a),math.sin(a),0));r=Vector((-d.y,d.x,0))
        p=Vector((rng.uniform(-.06,.06),rng.uniform(-.09,.09),.011+i*.005))
        length=lengths[i];steps=(8,5,3)[lod];radius=rng.uniform(.007,.012)
        def curve(t):
            return p+d*(length*(t-.5))+r*(math.sin(t*4+i)*.017)+Vector((0,0,math.sin(t*math.pi)*.008))
        points=[curve(t) for t in np.linspace(0,1,steps)]
        b.tube(points,[radius*(1-.52*j/(steps-1)) for j in range(steps)],"lichen_bark" if i==2 else "twig_bark",sides=(10,7,5)[lod])
        for k in range((3,2,1)[lod]):
            t=.22+k*.21;start=curve(t)
            side=(1 if (k+i)%2 else -1)
            tip=start+d*rng.uniform(.035,.08)+r*side*rng.uniform(.065,.13)+Vector((0,0,rng.uniform(.005,.018)))
            b.tube([start,(start+tip)*.5+Vector((0,0,.006)),tip],[radius*.52,radius*.32,.0018],"twig_bark",sides=(7,5,4)[lod])
            if lod==0 and k==1:
                # Small unequal fork fibres break the manufactured pointed-end silhouette.
                split=start*.15+tip*.85
                b.tube([split,tip+r*side*.014+Vector((0,0,.004))],[.0017,.0006],"twig_bark",sides=4)
    return b


def board(b, origin, axis, width, depth, length, lod, seed):
    rng=random.Random(seed);origin=Vector(origin);axis=Vector(axis).normalized()
    side=Vector((1,0,0)) if abs(axis.z)>.8 else Vector((0,0,1))
    other=axis.cross(side).normalized()
    bevel=.12
    cross=[(-1+bevel,-1), (1-bevel,-1),(1,-1+bevel),(1,1-bevel),
           (1-bevel,1),(-1+bevel,1),(-1,1-bevel),(-1,-1+bevel)]
    rings=[]
    segments=(7,4,2)[lod]
    for j in range(segments):
        t=j/(segments-1);ring=[]
        for k,(a,c) in enumerate(cross):
            endwarp=(rng.uniform(-.012,.012) if j in (0,segments-1) and lod<2 else 0)
            # Uneven endpoints and a central split remain part of the closed board silhouette.
            edge=a
            if lod<2 and j in (1,segments-2) and k in (2,3) and seed%2:edge*=.65
            p=origin+axis*(length*t+endwarp)+side*(width*.5*edge*(.97+.035*math.sin(t*5+seed+k*.3)))+other*(depth*.5*c)
            p+=side*(math.sin(t*4+seed)*.004+(t-.5)*(.025 if abs(axis.z)>.8 else .012))
            p+=other*(math.sin(t*7+seed)*.003)
            if abs(axis.x)>.8:p+=side*(-.012*math.sin(t*math.pi))
            ring.append(p)
        rings.append(ring)
    for j in range(segments-1):
        for k in range(8):
            kk=(k+1)%8
            b.face([rings[j][k],rings[j][kk],rings[j+1][kk],rings[j+1][k]],
                   [(k/8,j/(segments-1)),((k+1)/8,j/(segments-1)),((k+1)/8,(j+1)/(segments-1)),(k/8,(j+1)/(segments-1))],"fence_wood",False)
    for j,reverse in ((0,True),(segments-1,False)):
        center=sum(rings[j],Vector())/8
        if lod<2 and j==segments-1:center-=axis*(.025 if abs(axis.z)>.8 else .012)
        for k in range(8):
            kk=(k+1)%8;pts=[center,rings[j][k],rings[j][kk]]
            uv=[(.5,.5),(.5+cross[k][0]*.48,.5+cross[k][1]*.48),(.5+cross[kk][0]*.48,.5+cross[kk][1]*.48)]
            if reverse:pts.reverse();uv.reverse()
            b.face(pts,uv,"fence_end",False)
    if lod==0:
        for k in range(3):
            t=.20+k*.23;start=origin+axis*(length*t)+other*(depth*.51)
            b.sheet(start,side,axis,other,.10,width*.035,"fence_wood",rows=5,curl=.002,thickness=.001,phase=k)


def fence(lod):
    b=Builder()
    for i,x in enumerate((-.77,.77)):
        board(b,(x,0,0),(0,0,1),.105,.115,.75,lod,142+i)
    for i,z in enumerate((.27,.55)):
        board(b,(-.9,-.072,z),(1,0,0),.10,.045,1.8,lod,251+i)
    if lod<2:
        for x in (-.77,.77):
            for z in (.27,.55):
                b.tube([(x,-.105,z),(x,-.096,z)],[.007,.007],"fence_end",sides=(8,5)[lod])
    lo=[min(p[i] for p in b.vertices) for i in range(3)]
    hi=[max(p[i] for p in b.vertices) for i in range(3)]
    b.vertices=[((p[0]-(lo[0]+hi[0])*.5)*1.8/(hi[0]-lo[0]),p[1],(p[2]-lo[2])*.75/(hi[2]-lo[2])) for p in b.vertices]
    return b


def new_scene():
    scene=bpy.data.scenes.new("HS_Ground_Detail_"+OPT.revision)
    bpy.context.window.scene=scene
    scene.unit_settings.system="METRIC";scene.unit_settings.scale_length=1
    assets=bpy.data.collections.new("COL-ground-detail-assets");scene.collection.children.link(assets)
    previews=bpy.data.collections.new("COL-ground-detail-preview");scene.collection.children.link(previews)
    return scene,assets,previews


def material():
    m=bpy.data.materials.new("MAT-ground_detail");m.use_nodes=True
    ns=m.node_tree.nodes;lk=m.node_tree.links;bs=ns.get("Principled BSDF")
    bs.inputs["Metallic"].default_value=0
    for role,path in (("basecolor",TEX/ATLAS_NAMES["basecolor"]),("roughness",OUT/"FBXTextures"/"ground_detail_roughness.png"),("normal",OUT/"FBXTextures"/"ground_detail_normal_opengl.png")):
        tex=ns.new("ShaderNodeTexImage");tex.name="ATLAS-"+role;tex.image=bpy.data.images.load(str(path),check_existing=True)
        tex.image.colorspace_settings.name="sRGB" if role=="basecolor" else "Non-Color"
        if role=="normal":
            nm=ns.new("ShaderNodeNormalMap");lk.new(tex.outputs["Color"],nm.inputs["Color"]);lk.new(nm.outputs["Normal"],bs.inputs["Normal"])
        else:lk.new(tex.outputs["Color"],bs.inputs["Base Color" if role=="basecolor" else "Roughness"])
    m.use_backface_culling=True
    return m


def camera(scene,location,target,scale):
    ob=scene.objects.get("CAM-ground-detail")
    if not ob:
        ob=bpy.data.objects.new("CAM-ground-detail",bpy.data.cameras.new("CAM-ground-detail"));scene.collection.objects.link(ob)
    ob.location=location;ob.rotation_euler=(Vector(target)-ob.location).to_track_quat("-Z","Y").to_euler()
    ob.data.type="ORTHO";ob.data.ortho_scale=scale;scene.camera=ob
    return ob


def setup_render(scene):
    scene.render.engine="CYCLES";scene.cycles.device="CPU";scene.cycles.samples=24;scene.cycles.use_denoising=True
    scene.view_settings.view_transform="AgX";scene.view_settings.exposure=0;scene.view_settings.gamma=1
    scene.world=bpy.data.worlds.new("LGT-ground-detail-world");scene.world.use_nodes=True
    scene.world.node_tree.nodes["Background"].inputs[0].default_value=(.4,.47,.54,1)
    scene.world.node_tree.nodes["Background"].inputs[1].default_value=.45
    for name,loc,energy,size in (("key",(-3,-4,6),480,5),("fill",(3,1,4),180,4)):
        data=bpy.data.lights.new("LGT-ground-"+name,"AREA");data.energy=energy;data.shape="DISK";data.size=size
        ob=bpy.data.objects.new(data.name,data);scene.collection.objects.link(ob);ob.location=loc
        ob.rotation_euler=(-ob.location).to_track_quat("-Z","Y").to_euler()
    mesh=bpy.data.meshes.new("GEO-preview-floor");mesh.from_pydata([(-10,-10,-.007),(10,-10,-.007),(10,10,-.007),(-10,10,-.007)],[],[(0,1,2,3)])
    floor=bpy.data.objects.new("PREVIEW-floor",mesh);scene.collection.objects.link(floor)
    m=bpy.data.materials.new("MAT-preview-floor");m.diffuse_color=(.12,.105,.078,1);m.use_nodes=True
    m.node_tree.nodes.get("Principled BSDF").inputs["Base Color"].default_value=(.12,.105,.078,1)
    m.node_tree.nodes.get("Principled BSDF").inputs["Roughness"].default_value=.96;mesh.materials.append(m)
    scene.render.image_settings.file_format="PNG";scene.render.film_transparent=False
    scene.render.resolution_percentage=100


def render(scene,path,w,h):
    assert scene.camera
    scene.render.resolution_x=w;scene.render.resolution_y=h;scene.render.filepath=str(path)
    bpy.ops.render.render(write_still=True,scene=scene.name)


def set_preview_lod(lod):
    coll=bpy.data.collections["COL-ground-detail-preview"]
    for ob in coll.objects:
        name=ob["hs_base"]
        source=bpy.data.objects[name+("" if lod==0 else "_lod"+str(lod))]
        ob.data=source.data


def geometry():
    scene,assets,previews=new_scene()
    clay=bpy.data.materials.new("MAT-ground-detail-blockout");clay.diffuse_color=(.28,.33,.25,1)
    builders={"clover_patch":clover,"ground_leaves":leaves,"twig_cluster":twigs,"fence_short":fence}
    for name in NAMES:
        for lod in range(3):
            b=flowers(name,lod) if name.startswith("flower") else builders[name](lod)
            ob=b.object(name+("" if lod==0 else "_lod"+str(lod)),assets)
            ob.data.materials.append(clay);ob.hide_render=True;ob.hide_set(True)
    layout=[(-.9,.70,0),(0,.70,0),(.9,.70,0),(-.9,-.15,0),(0,-.15,0),(.9,-.15,0),(0,-1.0,0)]
    for name,loc in zip(NAMES,layout):
        src=bpy.data.objects[name];ob=src.copy();ob.data=src.data;ob.name="PREVIEW-"+name;previews.objects.link(ob)
        ob.hide_render=False;ob.hide_set(False);ob.location=loc;ob["hs_base"]=name
    setup_render(scene);camera(scene,(3,-4.5,4),(0,0,.13),3.5)
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT/("ground_detail_blockout_"+OPT.revision+".blend")))
    render(scene,OUT/"preview"/"blockout.png",1200,900)
    print("GROUND_DETAIL_GEOMETRY_READY",json.dumps({n:len(bpy.data.objects[n].data.polygons) for n in NAMES}))


def runtime_records():
    records=[];stats=[]
    for name in NAMES:
        counts=[]
        for lod in range(3):
            key=name+("" if lod==0 else "_lod"+str(lod));ob=bpy.data.objects[key]
            mesh=ob.data.copy()
            for item in mesh.uv_layers.active.data:item.uv.y=1-item.uv.y
            mesh.calc_loop_triangles();mesh.calc_tangents(uvmap="UV0")
            polygon_keys=[tuple(sorted(tuple(mesh.vertices[i].co) for i in poly.vertices)) for poly in mesh.polygons]
            assert len(set(polygon_keys))==len(polygon_keys),(key,"duplicate polygons")
            triangle_keys=[tuple(sorted(tuple(mesh.vertices[i].co) for i in tri.vertices)) for tri in mesh.loop_triangles]
            assert len(set(triangle_keys))==len(triangle_keys),(key,"duplicate triangles")
            vertices=[];bad=0;minimum_area=float("inf");length_errors=0
            for tri in mesh.loop_triangles:
                points=[mesh.vertices[mesh.loops[li].vertex_index].co for li in tri.loops]
                area=(points[1]-points[0]).cross(points[2]-points[0]).length*.5
                minimum_area=min(minimum_area,area);bad+=area<=1e-14
                for li in tri.loops:
                    loop=mesh.loops[li];p=mesh.vertices[loop.vertex_index].co;n=loop.normal;t=loop.tangent;uv=mesh.uv_layers.active.data[li].uv
                    row=[p.x,p.z,-p.y,n.x,n.z,-n.y,uv.x,uv.y,t.x,t.z,-t.y,loop.bitangent_sign,0]
                    assert all(math.isfinite(v) for v in row)
                    assert -.000001<=uv.x<=1.000001 and -.000001<=uv.y<=1.000001
                    assert abs(n.length-1)<.001 and abs(t.length-1)<.001 and abs(n.dot(t))<.002
                    assert abs(loop.bitangent_sign)==1 and tri.material_index==0
                    vertices.append(row)
            assert bad==0,(key,bad,minimum_area)
            bounds=[[min(v[i] for v in vertices) for i in range(3)],[max(v[i] for v in vertices) for i in range(3)]]
            records.append(index_mesh({"name":key,"vertices":vertices}));counts.append(len(vertices)//3)
            stats.append({"name":key,"triangles":len(vertices)//3,"bounds_engine_m":bounds,"minimum_triangle_area_m2":minimum_area,
                          "uv0_range":[[min(v[i] for v in vertices) for i in (6,7)],[max(v[i] for v in vertices) for i in (6,7)]],
                          "material_index":0,"renderer_material_ordinal":15,"texture_layer":0,"finite_unit_normal_tangent":True,"opaque_closed_leaf_shells":True})
            bpy.data.meshes.remove(mesh)
        assert counts[0]>counts[1]>counts[2],(name,counts)
    return records,stats


def register(records,stats):
    records=[index_mesh(record) for record in records]
    path=ROOT/"ContentSource/Models/Environment/environment_meshes.json"
    document=json.loads(path.read_text(encoding="utf-8"))
    owned={v["name"] for v in records}
    assert len(owned)==len(records), "Duplicate authored mesh names"
    base=[x for x in document["meshes"] if x["name"] not in owned]
    assert len(base)>=42 and len(document)==2
    before=hashlib.sha256(json.dumps(base,separators=(",",":"),ensure_ascii=False).encode()).hexdigest()
    replacements={v["name"]:v for v in records}
    merged=[replacements.pop(x["name"],x) for x in document["meshes"]]
    merged.extend(replacements.values())
    assert [x for x in merged if x["name"] not in owned]==base
    document["meshes"]=merged
    temporary=path.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(document,separators=(",",":"),ensure_ascii=False),encoding="utf-8")
    temporary.replace(path)
    if OPT.revision == "v001":
        material_path=ROOT/"ContentSource/Materials/ground_detail.json"
        source=json.loads((ROOT/"ContentSource/Materials/trunk_0.json").read_text(encoding="utf-8"))
        source["id"]="ground_detail"
        source["textures"]=[t for t in source["textures"] if t["id"]!="detail"]
        for texture in source["textures"]:
            texture["product_id"]="render/texture/environment_ground_detail_"+texture["id"].replace("base_color","basecolor")+".rpkg"
        remove={"material_distance","material_faded_detail","material_composed_normal"}
        source["nodes"]=[n for n in source["nodes"] if "detail" not in n["id"] and n["id"] not in remove]
        next(n for n in source["nodes"] if n["id"]=="material_closure")["normal"]["node"]="material_decoded_normal"
        assert len(source["textures"])==3
        material_path.write_text(json.dumps(source,indent=2)+"\n",encoding="utf-8")
        p=PACK/"project_hs_environment_materials_v9_LEAN.json";doc=json.loads(p.read_text(encoding="utf-8"))
        addition={"id":"mesh_ground_detail","mapping":{"mode":"uv0_atlas","atlas_size":[2048,2048],"gutter_px":GUTTER,"layers":1,"material_index":0,"opaque_geometry_silhouette":True},
                  "textures":{"base_color":"textures/"+ATLAS_NAMES["basecolor"],"normal":"textures/"+ATLAS_NAMES["normal"],"surface_ra":"textures/"+ATLAS_NAMES["surface"]}}
        previous=[m for m in doc["opaque_mesh"]["materials"] if m["id"]!="mesh_ground_detail"]
        doc["opaque_mesh"]["materials"]=previous+[addition];p.write_text(json.dumps(doc,indent=2)+"\n",encoding="utf-8")
        p=PACK/"baked_materials.json";doc=json.loads(p.read_text(encoding="utf-8"))
        doc["ground_detail"]=[addition["textures"]];p.write_text(json.dumps(doc,indent=2)+"\n",encoding="utf-8")
    (OUT/"verification"/"runtime_mesh_validation.json").write_text(json.dumps({"complete":True,"baseline_mesh_count":len(base),"baseline_mesh_values_sha256":before,
        "unowned_mesh_values_preserved":True,"total_meshes":len(merged),"new_meshes":stats,"coordinate_transform":"Blender Z-up -> engine Y-up: (x,z,-y), proper rotation determinant +1",
        "uv_tangent_contract":"Flip V into PNG coordinates before calc_tangents; rotate tangent with position/normal and preserve recalculated bitangent sign"},indent=2),encoding="utf-8")


def exports():
    assets=bpy.data.collections["COL-ground-detail-assets"]
    for ob in assets.objects:ob.hide_set(False)
    for name in NAMES:
        bpy.ops.object.select_all(action="DESELECT")
        objects=[bpy.data.objects[name+("" if i==0 else "_lod"+str(i))] for i in range(3)]
        for ob in objects:ob.select_set(True)
        bpy.context.view_layer.objects.active=objects[0]
        bpy.ops.export_scene.fbx(filepath=str(OUT/"FBX"/(name+".fbx")),use_selection=True,object_types={"MESH"},
                                apply_unit_scale=True,apply_scale_options="FBX_SCALE_UNITS",axis_forward="-Z",axis_up="Y",
                                use_tspace=True,mesh_smooth_type="FACE",bake_anim=False,path_mode="RELATIVE",add_leaf_bones=False)
    bpy.ops.object.select_all(action="DESELECT")
    for name in NAMES:bpy.data.objects[name].select_set(True)
    bpy.context.view_layer.objects.active=bpy.data.objects[NAMES[0]]
    bpy.ops.export_scene.gltf(filepath=str(OUT/("ground_detail_"+OPT.revision+".glb")),export_format="GLB",use_selection=True,
                             export_animations=False,export_yup=True,use_active_scene=True,export_normals=True,export_tangents=True,export_materials="EXPORT")
    for ob in assets.objects:ob.hide_set(True)
    bpy.ops.object.select_all(action="DESELECT")


def finish():
    bpy.ops.wm.open_mainfile(filepath=str(OUT/("ground_detail_blockout_"+OPT.revision+".blend")))
    with bpy.context.temp_override(window=bpy.context.window_manager.windows[0]):
        scene=bpy.data.scenes["HS_Ground_Detail_"+OPT.revision];bpy.context.window_manager.windows[0].scene=scene
        if OPT.revision == "v001":
            atlas()
        else:
            import shutil
            for name in ("ground_detail_normal_opengl.png", "ground_detail_roughness.png"):
                shutil.copy2(SOURCE/"FBXTextures"/name, OUT/"FBXTextures"/name)
            shutil.copy2(SOURCE/"atlas_regions.json", OUT/"atlas_regions.json")
        m=material()
        for ob in bpy.data.collections["COL-ground-detail-assets"].objects:
            ob.data.materials.clear();ob.data.materials.append(m)
        records,stats=runtime_records()
        (OUT/"runtime_records.json").write_text(json.dumps({"meshes":records,"stats":stats},separators=(",",":")),encoding="utf-8")
        exports()
        # Packed engineering source keeps all map inputs portable; FBX uses relative external PNGs.
        for im in bpy.data.images:
            if im.source=="FILE" and "ground_detail" in im.name:im.pack()
        camera(scene,(3,-4.5,4),(0,0,.13),3.5)
        bpy.ops.wm.save_as_mainfile(filepath=str(OUT/("ground_detail_"+OPT.revision+".blend")))
        render(scene,OUT/"preview"/"all_assets.png",1600,1200)
        previews=bpy.data.collections["COL-ground-detail-preview"]
        for ob in previews.objects:ob.hide_render=True
        for name in NAMES:
            ob=bpy.data.objects["PREVIEW-"+name];old=ob.location.copy();ob.location=(0,0,0);ob.hide_render=False
            scale=2.15 if name=="fence_short" else .95 if name.startswith("flower") else .82
            camera(scene,(1.2,-1.6,1.3 if name!="fence_short" else 1.0),(0,0,.32 if name=="fence_short" else .20 if name.startswith("flower") else .10),scale)
            render(scene,OUT/"preview"/(name+"_close.png"),800,800)
            camera(scene,(1.2,-1.6,1.7),(0,0,.18),2.7 if name=="fence_short" else 1.9)
            render(scene,OUT/"preview"/(name+"_game_scale.png"),400,400)
            ob.hide_render=True;ob.location=old
        for ob in previews.objects:ob.hide_render=False
        for lod in (1,2):
            set_preview_lod(lod);camera(scene,(3,-4.5,4),(0,0,.13),3.5)
            render(scene,OUT/"preview"/("all_assets_lod"+str(lod)+".png"),1200,900)
        set_preview_lod(0);camera(scene,(3,-4.5,4),(0,0,.13),3.5)
        bpy.ops.wm.save_as_mainfile(filepath=str(OUT/("ground_detail_"+OPT.revision+".blend")))
        print("GROUND_DETAIL_FINISH_READY",json.dumps({x["name"]:x["triangles"] for x in stats}))


def verify():
    bpy.ops.wm.open_mainfile(filepath=str(OUT/("ground_detail_"+OPT.revision+".blend")))
    with bpy.context.temp_override(window=bpy.context.window_manager.windows[0]):
        source={name:sum(len(p.vertices)-2 for p in bpy.data.objects[name].data.polygons) for name in [n+suffix for n in NAMES for suffix in ("","_lod1","_lod2")]}
        report=[]
        for name in NAMES:
            scene=bpy.data.scenes.new("VERIFY-"+name);bpy.context.window_manager.windows[0].scene=scene
            bpy.ops.import_scene.fbx(filepath=str(OUT/"FBX"/(name+".fbx")))
            meshes=[o for o in scene.objects if o.type=="MESH"]
            assert len(meshes)==3,(name,len(meshes))
            entries=[]
            for ob in meshes:
                key=next(k for k in source if ob.name==k or ob.name.startswith(k+".")) if ob.name not in source else ob.name
                ob.data.calc_loop_triangles();actual=len(ob.data.loop_triangles)
                assert actual==source[key],(key,actual,source[key])
                assert ob.data.uv_layers.active and len(ob.data.materials)==1
                assert all(p.material_index==0 for p in ob.data.polygons)
                assert all(math.isfinite(v) for p in ob.data.vertices for v in p.co)
                expected=bpy.data.objects[key]
                diff=max(abs(ob.dimensions[i]-expected.dimensions[i]) for i in range(3))
                assert diff<.00005,(key,diff,list(ob.dimensions),list(expected.dimensions))
                entries.append({"name":key,"triangles":actual,"dimension_max_error_m":diff,"uv0":True,"one_opaque_material":True})
            report.append({"file":"FBX/"+name+".fbx","meshes":entries,"bytes":(OUT/"FBX"/(name+".fbx")).stat().st_size})
        scene=bpy.data.scenes.new("VERIFY-glb");bpy.context.window_manager.windows[0].scene=scene
        bpy.ops.import_scene.gltf(filepath=str(OUT/("ground_detail_"+OPT.revision+".glb")))
        meshes=[o for o in scene.objects if o.type=="MESH"];assert len(meshes)==7
        for ob in meshes:
            ob.data.calc_loop_triangles()
            key=next(k for k in NAMES if ob.name==k or ob.name.startswith(k+"."))
            assert len(ob.data.loop_triangles)==source[key]
            assert ob.data.uv_layers.active and len(ob.data.materials)==1
        (OUT/"verification"/"export_roundtrip.json").write_text(json.dumps({"complete":True,"blender_version":bpy.app.version_string,
            "runtime_records_sha256":hashlib.sha256((OUT/"runtime_records.json").read_bytes()).hexdigest(),
            "fbx_roundtrip":report,"glb_roundtrip":{"meshes":7,"embedded_maps":True,"triangles_match":True},"actual_visual_review":"See preview images; human/agent image review is separate from structural checks."},indent=2),encoding="utf-8")
        print("GROUND_DETAIL_ROUNDTRIP_PASS",len(report),"FBX files + GLB; 21 LOD meshes.")


def register_verified():
    report=json.loads((OUT/"verification/export_roundtrip.json").read_text(encoding="utf-8"))
    encoded=(OUT/"runtime_records.json").read_bytes()
    assert report["complete"] and hashlib.sha256(encoded).hexdigest()==report["runtime_records_sha256"], "Runtime records changed after export verification"
    data=json.loads(encoded)
    owned={n+s for n in NAMES[:4] for s in ("","_lod1","_lod2")} if OPT.revision == "v002" else {m["name"] for m in data["meshes"]}
    register([m for m in data["meshes"] if m["name"] in owned], [m for m in data["stats"] if m["name"] in owned])
    print("GROUND_DETAIL_REGISTERED",len(owned),"verified meshes")


if __name__=="__main__":
    if OPT.stage=="geometry":geometry()
    elif OPT.stage=="finish":finish()
    elif OPT.stage=="verify":verify()
    else:register_verified()
