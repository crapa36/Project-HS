"""CPU arithmetic/source contracts only; compiled shader output needs native validation."""
import math
import itertools
from pathlib import Path
import re
import random
import unittest

SHADERS = Path(__file__).resolve().parents[1] / "Content" / "Shaders"


def reconstruct(taps, fraction, material=7, sky=False, exposure=1):
    """Taps are (red, depth, material, normal agreement)."""
    color = weight_sum = 0
    for index, (red, depth, key, normal) in enumerate(taps):
        x, y = index % 2, index // 2
        weight = (fraction[0] if x else 1 - fraction[0]) * (fraction[1] if y else 1 - fraction[1])
        if weight <= 0 or not all(map(math.isfinite, (red, depth, normal))):
            continue
        if (depth > 0) if sky else (depth <= 0 or abs(depth - .5) > .002 or key != material or normal < .85):
            continue
        color += red * weight
        weight_sum += weight
    return (color / weight_sum * exposure, weight_sum) if weight_sum > .00001 else (None, 0)


def cross(a, b):
    return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])


def bitset_light_query(candidates, directional, kinds, capacity, type_mask, transient=(), world=False):
    words = [0] * 4
    for index in candidates:
        words[index // 32] |= 1 << (index % 32)
    if world:
        selected = list(range(directional, len(kinds)))
    else:
        selected = []
        for word_index, word in enumerate(words):
            while word:
                lowest = word & -word
                selected.append(word_index * 32 + lowest.bit_length() - 1)
                word ^= lowest
    return [index for index in list(range(directional)) + selected
            if type_mask & (1 << kinds[index]) and index not in transient]


class ShaderContracts(unittest.TestCase):
    def test_ao_direct_view_reconstruction_matches_camera_relative_path(self):
        shader = (SHADERS / "ambient_occlusion.hlsl").read_text()
        body = shader.split("float3 AoViewPosition(", 1)[1].split("struct AoGuide", 1)[0]
        self.assertIn("AoInverseProjection", body)
        self.assertNotIn("AoWorldToView", body)
        self.assertIn("return view.xyz / view.w", body)
        frame = (SHADERS.parents[1] / "Source/RendererD3D12/Private/renderer_frame.cpp").read_text()
        setup = frame.split("AmbientOcclusionConstants ambient_constants;", 1)[1].split(
            "ambient_constants.previous_depth_parameters", 1)[0]
        self.assertIn("projection * DirectX::XMMatrixTranslation(jitter_x, jitter_y, 0.0f)", setup)
        self.assertIn("&ambient_constants.inverse_projection", setup)
        self.assertIn("XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&ambient_constants.projection))", setup)
        self.assertIn("XMMatrixTranslationFromVector(eye) * view", setup)

        def mul(a, b):
            return [[sum(x * y for x, y in zip(row, col)) for col in zip(*b)] for row in a]
        def transform(v, m):
            return mul([v], m)[0]
        def identity():
            return [[float(i == j) for j in range(4)] for i in range(4)]
        def translation(v):
            m = identity()
            m[3][:3] = v
            return m
        def inverse(m):
            rows = [list(row) + ident for row, ident in zip(m, identity())]
            for column in range(4):
                pivot = max(range(column, 4), key=lambda row: abs(rows[row][column]))
                rows[column], rows[pivot] = rows[pivot], rows[column]
                scale = rows[column][column]
                self.assertGreater(abs(scale), 1e-12)
                rows[column] = [x / scale for x in rows[column]]
                for row in range(4):
                    if row != column:
                        scale = rows[row][column]
                        rows[row] = [x - scale * y for x, y in zip(rows[row], rows[column])]
            return [row[4:] for row in rows]
        def divide(v):
            return [x / v[3] for x in v[:3]]

        # Row-vector DirectX LH projections, including the renderer's reversed depth.
        # Nonzero eye translations must cancel before the old world-to-view rotation.
        cases = itertools.product((False, True), (False, True),
            ((0., 0.), (.75 / 1920, -.4 / 1080)), ((0., 0.), (.73, -.48)),
            ((0., 0., 0.), (82., 17., -43.)))
        for ortho, reversed_depth, jitter, (yaw, pitch), eye in cases:
            near, far = (800., .1) if reversed_depth else (.1, 800.)
            projection = ([[.4, 0., 0., 0.], [0., .6, 0., 0.],
                           [0., 0., 1 / (far - near), 0.], [0., 0., -near / (far - near), 1.]]
                if ortho else [[.8, 0., 0., 0.], [0., 1.4, 0., 0.],
                               [0., 0., far / (far - near), 1.], [0., 0., -near * far / (far - near), 0.]])
            projection = mul(projection, translation((*jitter, 0.)))
            cy, sy, cp, sp = math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch)
            rotation = mul([[cy, 0., -sy, 0.], [0., 1., 0., 0.], [sy, 0., cy, 0.], [0., 0., 0., 1.]],
                           [[1., 0., 0., 0.], [0., cp, sp, 0.], [0., -sp, cp, 0.], [0., 0., 0., 1.]])
            view = mul(translation(tuple(-x for x in eye)), rotation)
            relative_view = mul(translation(eye), view)
            old_inverse = inverse(mul(relative_view, projection))
            new_inverse = inverse(projection)
            for z, xy in itertools.product((.11, 2., 45., 790.), ((0., 0.), (.3, -.2), (-.4, .25))):
                expected = [xy[0], xy[1], z]
                clip = transform([*expected, 1.], projection)
                ndc = [*divide(clip), 1.]
                old_world = divide(transform(ndc, old_inverse))
                old = transform([*old_world, 0.], relative_view)[:3]
                direct = divide(transform(ndc, new_inverse))
                for actual, previous, target in zip(direct, old, expected):
                    self.assertAlmostEqual(actual, target, places=7)
                    self.assertAlmostEqual(actual, previous, places=7)

    def test_sdr_scene_middle_grey_is_calibrated_without_scaling_ui(self):
        import re
        shader = (SHADERS / "display_transform.hlsli").read_text(encoding="utf-8-sig")
        tone = shader.split("float3 HsDisplayToneMapSdr(", 1)[1].split("float3 HsDisplayToneMapHdr(", 1)[0]
        scale = float(re.search(r"scene = max\(scene, 0.0\) \* ([0-9.]+)", tone).group(1))
        def fit(scene):
            x = max(scene, 0.) * scale
            return min(1., (x * (2.51 * x + .03)) / (x * (2.43 * x + .59) + .14))
        self.assertAlmostEqual(fit(.18), .18, places=7)
        values = [fit(i / 1000.) for i in range(10001)]
        self.assertEqual(values, sorted(values))
        self.assertEqual(fit(-1.), 0.)
        self.assertLessEqual(fit(1000.), 1.)
        encode = shader.split("float3 HsDisplayEncodeSdr(", 1)[1].split("float3 HsDisplayEncodeHdr10(", 1)[0]
        self.assertIn("return HsSrgbEncode(display_linear)", encode)

    def test_devtools_encoded_colors_are_decoded_before_display_composition(self):
        shader = (SHADERS / "project_hs.hlsl").read_text(encoding="utf-8-sig")
        ui = shader.split("float4 UiPS(", 1)[1].split("// Analytic silhouette", 1)[0]
        self.assertIn("HsDisplayDecodePremultipliedUi(PostA.SampleLevel", ui)
        self.assertIn("ui = ui * (1.0 - devtools.a) + devtools", ui)
        frame = (SHADERS.parents[1] / "Source/RendererD3D12/Private/renderer_frame.cpp").read_text(encoding="utf-8-sig")
        fxaa = frame.split("post_callbacks[7] =", 1)[1].split("post_callbacks[8] =", 1)[0]
        self.assertNotIn("ImGui_ImplDX12_RenderDrawData", fxaa)
        decode = lambda c: c / 12.92 if c <= .04045 else ((c + .055) / 1.055) ** 2.4
        encode = lambda c: 12.92 * c if c <= .0031308 else 1.055 * c ** (1 / 2.4) - .055
        # Opaque authored UI colors must round-trip; alpha stays coverage, not gamma.
        for c in (0., .06, .18, .5, 1.):
            self.assertAlmostEqual(encode(decode(c)), c, places=7)
            for alpha in (.1, .4, .94, 1.):
                stored = c * alpha
                decoded_premultiplied = decode(stored / alpha) * alpha
                self.assertAlmostEqual(decoded_premultiplied, decode(c) * alpha, places=7)
        self.assertGreater(encode(.5) - .5, .23)

    def test_meadow_and_outer_ground_preserve_the_same_seed(self):
        shader = (SHADERS / "project_hs.hlsl").read_text(encoding="utf-8-sig")
        seed = re.search(r"uint environment_seed = (.*?);", shader).group(1)
        self.assertRegex(seed, r"mesh == 9.*mesh == 20.*mesh == 22.*asuint\(instance.Padding\)")
        frame = (SHADERS.parents[1] / "Source/RendererD3D12/Private/renderer_frame.cpp").read_text(encoding="utf-8-sig")
        for name in ("seed", "animation_time"):
            producer = re.search(r"(?:const auto|float) " + name + r" = source.mesh == RenderMesh::Ground(.*?);", frame, re.S).group(1)
            self.assertIn("source.mesh == RenderMesh::MeadowSurface", producer)
            self.assertIn("std::bit_cast<float>(source.environment_seed)", producer)
        self.assertIn("environment && source.mesh != RenderMesh::MeadowSurface ? 0.0f : std::bit_cast<float>(source.environment_seed)", frame)
        visibility = (SHADERS / "visibility_buffer.hlsl").read_text(encoding="utf-8-sig")
        self.assertIn("output.EnvironmentSeed = a.EnvironmentSeed", visibility)
        self.assertIn("ctx.seed = input.EnvironmentSeed", shader)

    def test_meadow_boundary_blends_paths_and_traces_continuously(self):
        text = (SHADERS / "material_terrain.hlsli").read_text(encoding="utf-8-sig")
        body = text.split("float TerrainMeadowBlend(", 1)[1].split("}", 1)[0]
        match = re.search(r"smoothstep\(([0-9.]+),([0-9.]+),([0-9.]+)-max\(abs\(world.x\),abs\(world.y\)\)\)", body)
        self.assertIsNotNone(match)
        lo, hi, edge = map(float, match.groups())
        self.assertEqual(lo, 0.)
        self.assertGreater(hi, 0.)
        self.assertEqual(edge, 12.)
        def blend(x, z):
            t = min(1., max(0., (edge - max(abs(x), abs(z)) - lo) / (hi-lo)))
            return t*t*(3.-2.*t)
        for sign in (-1, 1):
            for along in (-12., -7., 0., 4., 12.):
                for swap in (False, True):
                    at = lambda d: blend(along, sign*(edge-d)) if swap else blend(sign*(edge-d), along)
                    self.assertEqual(at(0), 0.)
                    self.assertEqual(at(-.001), 0.)
                    self.assertLess(at(.001), 2e-6)
                    # Even maximally different path colors have no finite boundary jump.
                    self.assertLess(abs((1-at(.001)) - (1-at(-.001))), 2e-6)
        self.assertEqual(blend(0, 0), 1.)
        self.assertIn("const float path=TerrainMeadowRoad(world.xz,warp,meso);", text)
        self.assertNotIn("smoothstep(2.8,5.8", text)
        traces = text.split("void TerrainGroundTraces(",1)[1].split("HsMaterialTerrainResult EnvTerrain(",1)[0]
        self.assertIn("wetness*meadow", traces)
        self.assertIn("path*(1.0-grassCoverage)*(1.0-meadow)", traces)
        self.assertIn("float moss=edge*smoothstep(0.12,0.55,broad+fine*0.25)*(1.0-meadow)", traces)

    def test_meadow_road_contains_both_cart_ruts(self):
        text = (SHADERS / "material_terrain.hlsli").read_text(encoding="utf-8-sig")
        road = text.split("float TerrainMeadowRoad(", 1)[1].split("// Fixed metre-scale", 1)[0]
        self.assertIn("max(MeadowPressed(world)*TerrainMeadowBlend(world),MeadowRoadCoverage(world,edge)*.82)", road)
        shared = (SHADERS / "meadow_surface.hlsli").read_text(encoding="utf-8-sig")
        self.assertIn("MeadowRoadCoverage", shared)
        # Even maximum reused edge warp leaves both wheel centres inside the road.
        for warp_x, warp_y in itertools.product((-1., 0., 1.), repeat=2):
            edge = warp_x*.18 + warp_y*.10
            for side in (-1., 1.):
                self.assertLess(abs(side*.82)+edge, 1.15)
        self.assertEqual(text.count("float2(TerrainNoise(world.xz/7,seed+413),TerrainNoise(world.xz/7,seed+827))"), 1)

    def test_road_spur_joins_main_route_before_full_width(self):
        text = (SHADERS / "material_terrain.hlsli").read_text(encoding="utf-8-sig")
        road = text.split("float TerrainMeadowRoad(", 1)[1].split("// Fixed metre-scale", 1)[0]
        shared = (SHADERS / "meadow_surface.hlsli").read_text(encoding="utf-8-sig")
        match = re.search(r"smoothstep\(([0-9.]+)\+edge,([0-9.]+)\+edge,p.x\)", shared)
        start, full = map(float, match.groups())
        centre = math.sin(3*.085)*6.5
        # Full branch coverage begins before the narrowest main-road edge.
        self.assertLess(start, full)
        self.assertLess(full+.28, centre+1.15-.28)
        self.assertIn("MeadowPressed(world)*TerrainMeadowBlend(world)", road)
        self.assertIn("smoothstep(11-edge,11.7-edge,p.x)", shared)
        self.assertGreater(11.7-(-.5), 12.)  # A half-metre prop at the branch tip must be excluded.

    def test_soil_gain_preserves_grass_and_texture_contrast(self):
        text = (SHADERS / "material_terrain.hlsli").read_text(encoding="utf-8-sig")
        gain = tuple(map(float, re.search(r"m.base_color\*=lerp\(float3\(([^)]+)\),float3\(1,1,1\),grassCoverage\)", text).group(1).split(",")))
        self.assertTrue(0 < gain[2] < gain[1] < gain[0] < 1)
        for grass in (0., .5, 1.):
            multiplier = tuple(g*(1-grass)+grass for g in gain)
            if grass == 1: self.assertEqual(multiplier, (1., 1., 1.))
            for factor in (.2, .8, 2.):
                for g in multiplier:
                    self.assertAlmostEqual((factor*.6*g)/(.6*g), factor)

    def test_terrain_grass_palette_preserves_soil_and_brightness_detail(self):
        text = (SHADERS / "material_terrain.hlsli").read_text(encoding="utf-8-sig")
        body = text.split("float3 TerrainGrassPalette(", 1)[1].split("float3 DecodeTerrainHistogram(", 1)[0]
        self.assertIn("const float grassCoverage=mesh==20u?0.0:(1.0-path)*smoothstep(0.0,0.2,w.y)", text)
        self.assertIn("return color*lerp(float3(1,1,1),foliageAlbedo/groundAlbedo*canopyGain,coverage)", body)
        self.assertIn("m.ao*=lerp(1.0,0.1,grassCoverage)", text)
        self.assertIn("m.roughness=lerp(m.roughness,1.0,0.8*grassCoverage)", text)
        values = lambda name: tuple(map(float, re.search(name + r"=float3\(([^)]+)\)", body).group(1).split(",")))
        atlas, ground, gain = (values(name) for name in ("foliageAlbedo", "groundAlbedo", "canopyGain"))
        def coverage(weight, path, mesh):
            t = min(1.0, max(0.0, weight / .2))
            return 0.0 if mesh == 20 else (1.0 - path) * t * t * (3 - 2 * t)
        def palette(color, amount):
            return tuple(c * (1 - amount + a / g * k * amount) for c, a, g, k in zip(color, atlas, ground, gain))
        rng = random.Random(0x47524153)
        for _ in range(1000):
            color = tuple(rng.random() for _ in range(3))
            for args in ((1, 0, 20), (1, 1, 9), (0, 0, 9)):
                amount = coverage(*args)
                self.assertEqual(palette(color, amount), color)
                self.assertEqual(1 - amount + .1 * amount, 1.0)
                self.assertEqual(color[0] + (1 - color[0]) * .8 * amount, color[0])
            for amount in (0.0, .25, .5, 1.0):
                for scale in (.25, 2.0):
                    for actual, original in zip(palette(tuple(c * scale for c in color), amount), palette(color, amount)):
                        self.assertAlmostEqual(actual, original * scale, places=12)
        self.assertNotEqual(palette((.2, .1, .05), 1), palette((.1, .1297259508, .05), 1))
        self.assertEqual(palette((0, 0, 0), 1), (0, 0, 0))
        for actual, a, k in zip(palette(ground, 1), atlas, gain):
            self.assertAlmostEqual(actual, a * k, places=12)
        normalize = text.index("m.base_color/=total;m.roughness/=total;m.ao/=total;")
        apply = text.index("m.base_color=TerrainGrassPalette(m.base_color,grassCoverage)")
        self.assertLess(normalize, apply)
        self.assertLess(apply, text.index("m.base_color*=0.6*(1+macro*0.08+meso*0.04)"))
        for retained in ("for(uint layer=0;layer<4;++layer)", "if(mesh==20) w=float4(1,0,0,0)",
                         "float weight=w[layer]*exp2((n.height-0.5)*(2+3*path*(1-path)))",
                         "m.base_color+=n.base_color*weight;m.normal+=n.normal*weight;m.detail+=n.detail*weight",
                         "m.roughness+=n.roughness*weight;m.ao+=n.ao*weight;total+=weight"):
            self.assertIn(retained, text)

    def test_terrain_ground_traces_keep_texture_and_grass_contracts(self):
        text = (SHADERS / "material_terrain.hlsli").read_text(encoding="utf-8-sig")
        body = text.split("void TerrainGroundTraces(", 1)[1].split("HsMaterialTerrainResult EnvTerrain(", 1)[0]
        self.assertIn("if(mesh==20u||path<=0.015) return", body)
        self.assertIn("float soil=path*(1.0-grassCoverage)", body)
        self.assertIn("world.x-sin(world.z*0.085)*6.5", body)
        self.assertNotRegex(body, r"\b(for|while)\s*\(|\.Sample|HsTextureSample|m\.(height|detail)\s*=")
        self.assertNotIn("distanceToEye", body)
        self.assertEqual(body.count("TerrainNoise("), 2)
        self.assertIn("TerrainRandom(key+5)>0.48?soil:0.0", body)
        self.assertIn("TerrainRandom(key+10)>0.72?1.0:0.0", body)
        self.assertIn("floor(walkZ/0.62)", body)
        self.assertIn("abs(across)-0.82-fine*0.045", body)
        self.assertIn("float edge=smoothstep(0.03,0.16,path)*(1.0-smoothstep(0.45,0.8,path))", body)
        call = text.index("TerrainGroundTraces(m,world,path,grassCoverage,mesh,seed,wetness,dryness)")
        self.assertLess(text.index("m.normal=normalize(m.normal);m.detail=normalize(m.detail)"), call)
        self.assertLess(text.index("m.roughness=lerp(m.roughness,1.0,0.8*grassCoverage)"), call)

    def test_terrain_ground_trace_material_ranges_are_finite(self):
        text = (SHADERS / "material_terrain.hlsli").read_text(encoding="utf-8-sig")
        body = text.split("void TerrainGroundTraces(", 1)[1].split("HsMaterialTerrainResult EnvTerrain(", 1)[0]
        gains = [tuple(map(float, value.split(","))) for value in re.findall(
            r"m.base_color\*=lerp\(float3\(1,1,1\),float3\(([^)]+)\),\w+\)", body)]
        self.assertEqual(gains, [(0.72, 0.68, 0.62), (0.65, 0.60, 0.53), (0.54, 0.50, 0.43),
                                 (0.88, 0.82, 0.72), (0.72, 0.67, 0.58), (0.58, 0.92, 0.38)])
        self.assertTrue(all(0.0 < channel <= 1.0 for gain in gains for channel in gain))
        sat = lambda x: min(1.0, max(0.0, x))
        def smooth(a, b, x):
            t = sat((x - a) / (b - a))
            return t * t * (3 - 2 * t)
        rng = random.Random(0x54524143)
        for _ in range(2000):
            path, grass = rng.random(), rng.random()
            meadow = (0., 1., rng.random())[_ % 3]
            meadow_wet = rng.random() * meadow
            soil = path * (1 - grass) * (1 - meadow)
            wet, rim, rut, foot = (rng.random() * soil for _ in range(4))
            moss = smooth(.03, .16, path) * (1 - smooth(.45, .8, path)) * rng.random() * (1 - meadow)
            pressed = max(rut, foot)
            color = tuple(rng.random() for _ in range(3))
            for gain, amount in zip(gains, (rng.random()*meadow, meadow_wet, wet, rim, pressed, moss)):
                color = tuple(c * (1 - amount + g * amount) for c, g in zip(color, gain))
            roughness = rng.random() * (1 - meadow_wet) + .48 * meadow_wet
            film = smooth(.82, 1., meadow_wet) * .65
            roughness = roughness * (1 - film) + .24 * film
            roughness = roughness * (1 - wet) + .3 * wet
            roughness = sat(roughness + rim * .08)
            roughness += (max(roughness, .92) - roughness) * moss
            ao = rng.random() * (1 - pressed * .08 - moss * .05)
            normal = tuple(rng.uniform(-1, 1) for _ in range(3))
            length = math.sqrt(sum(c * c for c in normal))
            normal = tuple(c / length for c in normal)
            # Worst opposing basin/rut/shoe gradients cannot collapse a unit input normal.
            perturbed = (normal[0] + rng.uniform(-.075, .075),
                         normal[1] + rng.uniform(-.036, .036), normal[2])
            self.assertGreater(math.sqrt(sum(c * c for c in perturbed)), .91)
            self.assertTrue(all(math.isfinite(c) and 0 <= c <= 1 for c in (*color, roughness, ao)))
            self.assertTrue(all(0 <= amount <= path for amount in (wet, rim, rut, foot)))
        for path in (0.0, 1.0):
            self.assertEqual(smooth(.03, .16, path) * (1 - smooth(.45, .8, path)), 0.0)
        self.assertEqual(1.0 * (1 - 1.0), 0.0)  # Fully grassy coverage admits no soil traces.

    def test_terrain_tile_scale_matches_software_mip_demand(self):
        text = (SHADERS / "material_terrain.hlsli").read_text(encoding="utf-8-sig")
        sizes = tuple(map(float, re.search(r"TerrainTileSize\(uint layer\).*?layer==3 \? ([0-9.]+) : ([0-9.]+)", text).groups()))
        frame = (SHADERS.parents[1] / "Source/RendererD3D12/Private/renderer_frame.cpp").read_text(encoding="utf-8-sig")
        demand_tile = float(re.search(r"logical.width\) \* terrain_footprint / ([0-9.]+)f", frame).group(1))
        self.assertEqual(sizes, (9.0, 12.0))
        self.assertEqual(demand_tile, min(sizes))
        self.assertIn("uv=world.xz/tile,dx=worldDx.xz/tile,dy=worldDy.xz/tile", text)

    def test_terrain_pom_skips_only_bounded_subpixel_displacement(self):
        shader = (SHADERS / "material_terrain.hlsli").read_text(encoding="utf-8-sig")
        pom = shader.split("float2 TerrainParallaxUv(", 1)[1].split("HsMaterialTerrainResult TerrainSample(", 1)[0]
        self.assertIn("if (!pom || steps == 0u || distanceToEye >= 55.0) return uv", pom)
        self.assertIn("gradientEnergy=dot(dx,dx)+dot(dy,dy)", pom)
        self.assertIn("gradientArea=abs(dx.x*dy.y-dx.y*dy.x)", pom)
        self.assertIn("gradientEnergy>1e-20 && gradientArea>gradientEnergy*1e-6", pom)
        self.assertIn("length(ray)*(sqrt(gradientEnergy)/gradientArea)", pom)
        self.assertIn("if(isfinite(parallaxPixels))", pom)
        self.assertLess(pom.index("if(pomWeight<=0) return uv"), pom.index("HsTexturePrepareGrad(height_slot)"))
        self.assertIn("return start-ray*((previous+depth)*0.5*pomWeight)", pom)
        lo, hi = map(float, re.search(r"smoothstep\(([0-9.]+),([0-9.]+),parallaxPixels\)", pom).groups())
        self.assertEqual((lo, hi), (.25, .5))
        sample = shader.split("HsMaterialTerrainResult TerrainSample(", 1)[1].split("float3 TerrainGrassPalette(", 1)[0]
        self.assertIn("m.height=HsTextureSampleGrad(height,height_slot", sample)
        self.assertLess(sample.index("uv=TerrainParallaxUv("), sample.index("m.height=HsTextureSampleGrad"))

        def weight(dx, dy, ray):
            energy = sum(v*v for v in (*dx, *dy))
            area = abs(dx[0]*dy[1] - dx[1]*dy[0])
            if not (energy > 1e-20 and area > energy*1e-6):
                return 1., math.inf
            bound = math.hypot(*ray) * math.sqrt(energy) / area
            if not math.isfinite(bound):
                return 1., math.inf
            t = min(1., max(0., (bound-lo)/(hi-lo)))
            return t*t*(3.-2.*t), bound

        # Build UV rays from known screen displacement, independent of the bound.
        for scale, ratio, angle, shear in itertools.product(
                (1e-8, 1e-4, 1.), (1., 8., 1024., 100000.), (0., .37, 1.8), (0., .4, .999)):
            c, s = math.cos(angle), math.sin(angle)
            rotate = lambda v: (c*v[0]-s*v[1], s*v[0]+c*v[1])
            dx, dy = rotate((scale*ratio, 0.)), rotate((scale*shear, scale))
            for pixels in ((.02, .03), (.24, 0.), (0., .25), (.5, .5), (3., -8.)):
                ray = tuple(dx[i]*pixels[0] + dy[i]*pixels[1] for i in range(2))
                fade, bound = weight(dx, dy, ray)
                actual = math.hypot(*pixels)
                self.assertGreaterEqual(bound*(1.+1e-10), actual)
                if fade == 0.:
                    self.assertLessEqual(actual, lo*(1.+1e-10))

        thresholds = (0., .1, .25, .3, .4, .5, 8.)
        fades = [weight((1., 0.), (0., 1.), (p/math.sqrt(2.), 0.))[0] for p in thresholds]
        self.assertEqual(fades[:3], [0., 0., 0.])
        self.assertEqual(fades[-2:], [1., 1.])
        self.assertEqual(fades, sorted(fades))
        for dx, dy in (((0., 0.), (0., 0.)), ((1., 0.), (1., 1e-8)),
                       ((1e-20, 0.), (0., 1e-20)), ((math.inf, 0.), (0., 1.)),
                       ((math.nan, 0.), (0., 1.))):
            self.assertEqual(weight(dx, dy, (.1, .2))[0], 1.)

    def test_terrain_histogram_uses_available_gaussian_mip(self):
        text = (SHADERS / "material_terrain.hlsli").read_text(encoding="utf-8-sig")
        clamp = text.index("lod=max(lod,float(HsTextureMipMetadata[gaussian_slot].y))")
        self.assertLess(text.index("float lod=0.5*log2"), clamp)
        self.assertLess(clamp, text.index("x.base_color=DecodeTerrainHistogram"))
        streaming = (SHADERS / "texture_streaming.hlsli").read_text()
        self.assertIn("state.clamp_mip = float(mip.y - min(mip.x, mip.y))", streaming)
        for ideal, resident, expected in ((2.5, 0, 2.5), (2.5, 4, 4), (0, 1, 1)):
            self.assertEqual(max(ideal, resident), expected)

    def test_scene_exposure_art_target_survives_adaptation(self):
        frame = (SHADERS.parents[1] / "Source/RendererD3D12/Private/renderer_frame.cpp").read_text()
        target = float(re.search(r"exposure_constants.target_grey = ([0-9.]+)f", frame).group(1))
        self.assertEqual(target, .12)
        for luminance in (.03, .1, .18, .5, 2.0):
            exposure = target / luminance
            self.assertAlmostEqual(luminance * exposure, target)
            darker = luminance * .6
            self.assertAlmostEqual(darker * (target / darker), target)
        exposure_shader = (SHADERS / "exposure.hlsl").read_text()
        self.assertIn("log2(HsExposureTargetGrey) - average_log_luminance", exposure_shader)

    def test_renderer_bindings_derive_from_cpu_catalog(self):
        root = SHADERS.parents[1]
        helper = (root / "Source/Apps/AssetPipeline/render_shader_bindings.hpp").read_text()
        inputs = (root / "Source/Apps/AssetPipeline/render_shader_binding_inputs.hpp").read_text()
        definitions = dict(re.findall(r'\{"(HS_RENDER_\w+)", render_data::([^}]+)\}', helper))
        expected = {
            "HS_RENDER_GEOMETRY_PAGE_CAPACITY": "kGeometryPageCapacity",
            "HS_RENDER_GEOMETRY_BUFFERS_PER_PAGE": "kGeometryBuffersPerPage",
            "HS_RENDER_AUTHORED_MATERIAL_COUNT": "kAuthoredMaterialNames.size()",
            "HS_RENDER_HERO_MATERIAL_COUNT": "kHeroMaterialNames.size()",
            "HS_RENDER_ENVIRONMENT_GEOMETRY_COUNT": "kEnvironmentMaterialNames.size()",
            "HS_RENDER_ENVIRONMENT_GEOMETRY_LOD_COUNT": "kEnvironmentGeometryLodCount",
            "HS_RENDER_MATERIAL_TEXTURE_SLOT_COUNT": "kMaterialTextureSlots",
            "HS_RENDER_GROUND_DETAIL_GEOMETRY_BEGIN": "kGroundDetailGeometryBegin",
            "HS_RENDER_GROUND_DETAIL_MATERIAL_ORDINAL": "kGroundDetailMaterialOrdinal",
        }
        self.assertEqual(definitions, expected)
        contract = (SHADERS / "render_binding_contract.hlsli").read_text()
        for name in definitions:
            self.assertIn(f"!defined({name})", contract)
            self.assertNotRegex(contract, rf"#define\s+{name}\b")
        self.assertIn("#error Missing RenderData binding definitions", contract)
        self.assertIn("HsHeroVisibilityVariantBegin = 1u + HsAuthoredMaterialCount", contract)
        self.assertIn("HsVisibilityVariantCount = HsHeroVisibilityVariantBegin + HsHeroMaterialCount", contract)
        self.assertIn("HsEnvironmentGeometryPageCount = HS_RENDER_ENVIRONMENT_GEOMETRY_COUNT * HS_RENDER_ENVIRONMENT_GEOMETRY_LOD_COUNT", contract)
        self.assertIn('"shader/render-binding-defines-ir"', inputs)
        self.assertIn("const auto contract = RenderBindingContract()", inputs)
        self.assertIn("std::as_bytes(std::span(contract.data(), contract.size()))", inputs)
        for file in ("project_hs.hlsl", "material_parameters.hlsli", "visibility_records.hlsli", "texture_streaming.hlsli", "hero_material.hlsli"):
            self.assertIn('#include "render_binding_contract.hlsli"', (SHADERS / file).read_text())
        classify = (SHADERS / "visibility_buffer.hlsl").read_text()
        self.assertIn("return variant < HsHeroVisibilityVariantBegin;", classify)
        self.assertIn("variant = HsHeroVisibilityVariantBegin + ordinal;", classify)
        self.assertNotRegex(classify, r"\b17u\b")
        feedback = (SHADERS / "texture_streaming.hlsli").read_text()
        self.assertIn("HsTextureFeedback[HS_RENDER_AUTHORED_MATERIAL_COUNT * HS_RENDER_MATERIAL_TEXTURE_SLOT_COUNT]", feedback)

    def test_shader_producers_use_binding_contract(self):
        root = SHADERS.parents[1]
        runtime = (root / "Source/Apps/RenderRuntimeShader/main.cpp").read_text(encoding="utf-8-sig")
        material = (root / "Source/Apps/RenderMaterialShader/main.cpp").read_text(encoding="utf-8-sig")
        for source, function, following in ((runtime, "void RunDxc(", "void RunDependencyScan("),
                                            (runtime, "void RunDependencyScan(", "std::set<std::string> ParseDependencyFile("),
                                            (material, "void RunDxc(", "void ProducePreintegration(")):
            self.assertIn(following, source)
            body = source.split(function, 1)[1].split(following, 1)[0]
            self.assertIn("hs::shader_content::AppendRenderBindingArguments(args)", body)
        self.assertIn("hs::shader_content::AddRenderBindingInputs(inputs, source_root)", runtime)
        self.assertEqual(material.count("hs::shader_content::AddRenderBindingInputs(spec.inputs, source_root)"), 2)

    def test_shared_terrain_helper_is_an_authenticated_build_input(self):
        root = SHADERS.parents[1]
        producer = (root / "Source/Apps/RenderMaterialShader/main.cpp").read_text(encoding="utf-8-sig")
        common = producer.split("void ProducePreintegration(", 1)[1].split("void ProduceParameters(", 1)[0]
        self.assertIn('"Content/Shaders/material_terrain.hlsli"', common)
        cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8-sig")
        common = cmake.split("NAME hs_render_bsdf_preintegrate", 1)[1].split("ARGS", 1)[0]
        self.assertIn("STATIC_INPUTS", common)
        self.assertIn('"${CMAKE_CURRENT_SOURCE_DIR}/Content/Shaders/material_terrain.hlsli"', common)

    def test_scene_geometry_cpu_shader_layout_and_local_position_stride_match(self):
        root = SHADERS.parents[1]
        header = (root / "Source/RendererD3D12/Private/renderer_scene_geometry.hpp").read_text(encoding="utf-8-sig")
        runtime = (root / "Source/RendererD3D12/Private/renderer_scene_geometry.cpp").read_text(encoding="utf-8-sig")
        shader = (SHADERS / "scene_geometry.hlsli").read_text(encoding="utf-8-sig")
        cpu_record = re.search(r"struct SceneGeometryRecord\s*\{(.*?)\n\};", header, re.S).group(1)
        fields = re.findall(r"std::uint32_t\s+([^;]+);", cpu_record)
        fields = [re.sub(r"\{[^}]*\}", "", field).strip() for row in fields for field in row.split(",")]
        self.assertEqual(fields, ["vertex_descriptor", "index_descriptor", "first_index", "index_count",
                                 "vertex_byte_offset", "vertex_stride", "vertex_count", "position_byte_offset",
                                 "stable_gpu_scene_slot", "instance_generation", "skin_slot", "mesh_id",
                                 "environment_ordinal", "flags"])
        gpu_record = shader.split("struct SceneGeometryRecord", 1)[1].split("};", 1)[0]
        gpu_fields = re.findall(r"\b(uint[24])\s+(\w+)\s*;", gpu_record)
        self.assertEqual(gpu_fields, [("uint4", "source"), ("uint4", "range"),
                                     ("uint4", "identity"), ("uint2", "material")])
        self.assertEqual(len(fields) * 4, sum(int(kind[-1]) * 4 for kind, _ in gpu_fields))
        self.assertIn("static_assert(sizeof(SceneGeometryRecord)==56)", header)
        self.assertIn("positionBytes+=std::uint64_t(g.vertex_count)*12", runtime)
        stores = re.findall(r"ScenePositionOutput\.Store\d*\([^;]+;", shader)
        self.assertEqual(stores, ["ScenePositionOutput.Store3(g.range.w+vertex*12u,asuint(HsSceneGeometryWorldToLocal(instance,world)));"])
        self.assertIn("c.cachedSkinHistory=false", shader)
        self.assertIn("EvaluateSceneVertex(HsSceneGeometryCanonicalVertex(g,vertex),context,vertex,false)", shader)

    def test_scene_deformation_dispatch_axes_are_disjoint(self):
        shader = (SHADERS / "scene_geometry.hlsl").read_text(encoding="utf-8-sig")
        self.assertIn("uint job=PassValue+dispatch.y", shader)
        self.assertIn("dispatch.x+dispatch.z*(65535u*64u)", shader)
        self.assertIn("if(vertexId>=g.range.z||(g.material.y&1)==0) return", shader)
        # Dispatch rows must never change the vertex coordinate; Z continues large meshes.
        width = 65535 * 64
        for first in (0, 37, 65535):
            for rows in (1, 2, 65535):
                for vertices in (3, 63, 64, 65, width - 1, width, width + 1):
                    groups = (vertices + 63) // 64
                    x_width, layers = min(groups, 65535) * 64, (groups + 65534) // 65535
                    spans = [(z * width, min(z * width + x_width, vertices)) for z in range(layers)]
                    self.assertEqual(spans[0][0], 0)
                    self.assertEqual(spans[-1][1], vertices)
                    self.assertTrue(all(a[1] == b[0] for a, b in zip(spans, spans[1:])))
                    self.assertEqual(sum(end - start for start, end in spans), vertices)
                    self.assertEqual(len({(first + y, start) for y in (0, rows - 1) for start, _ in spans}),
                                     len(set((0, rows - 1))) * layers)

    def test_local_probe_explicit_source_base_contract(self):
        source = (SHADERS / "local_probe_capture.hlsl").read_text(encoding="utf-8-sig")
        vertex = source.split("SceneOutput LocalProbeCaptureSourceBaseVS(", 1)[1].split("struct LocalProbeCaptureOutput", 1)[0]
        self.assertIn("const uint source_slot=HsLocalProbeSourceSlot(PassValue,instance_id)", vertex)
        for use in ("uint2(source_slot+1u,SkinCacheOptions.w)",
                    "HsGpuSceneSlotValid(source_slot,h0,h1,h2,h3)",
                    "h1.x+source_slot*h2.z+20u", "HsLoadGpuSceneInstance(source_slot)",
                    "context.draw.InstanceIndex=source_slot"):
            self.assertIn(use, vertex)
        self.assertIn("output.MaterialId=0x10000000u+HsLocalProbeMaterialOrdinal(PassValue)", vertex)
        project = SHADERS.parents[1]
        product = "render/shader/local_probe_capture_source_base_vs.dxil"
        producer = (project / "Source/Apps/RenderMaterialShader/main.cpp").read_text(encoding="utf-8-sig")
        runtime = (project / "Source/RendererD3D12/Private/renderer_local_probe_runtime.cpp").read_text(encoding="utf-8-sig")
        self.assertIn(product, producer)
        self.assertIn('L"LocalProbeCaptureSourceBaseVS"', producer)
        self.assertIn(product, runtime)
        for text in (producer, runtime):
            self.assertNotIn("render/shader/local_probe_capture_vs.dxil", text)

    def test_meadow_controls_use_authored_uv_without_changing_world_mapping(self):
        terrain = (SHADERS / "material_terrain.hlsli").read_text(encoding="utf-8-sig")
        scene = (SHADERS / "project_hs.hlsl").read_text(encoding="utf-8-sig")
        self.assertIn("ctx.mesh = input.Mesh;", scene)
        controls = terrain.split("[branch] if(mesh==22u)", 1)[1].split("else", 1)[0]
        self.assertIn("wetness=saturate(authoredControls.x)", controls)
        self.assertIn("dryness=saturate(authoredControls.y)", controls)
        self.assertNotIn("MeadowWetness(", controls)
        self.assertNotIn("MeadowSoilDryness(", controls)
        self.assertIn("float2 uv=world.xz/tile", terrain)
        self.assertIn("w=lerp(w,float4(1-dryness,0,dryness,0),path)", terrain)
        for values in itertools.product((-1., 1.), repeat=3):
            edge = sum(a*b for a,b in zip(values, (.12,.07,.09)))
            self.assertLess(.82+edge, 1.15)

    def test_transmission_fallback_preserves_scene_and_fog_domains(self):
        scene = (SHADERS / "project_hs.hlsl").read_text(encoding="utf-8-sig")
        layer = scene.split("float3 HsHeroLayerBackground(", 1)[1].split("// Authored-thickness", 1)[0]
        # Offscreen/foreground rejection must keep the covered layer, not substitute sky.
        fallback = layer.split("const float3 exit_world", 1)[0]
        self.assertIn("HeroLayerBackground.Load", fallback)
        self.assertNotIn("EnvironmentSkyRadiance", fallback)
        lighting = scene.split("float3 HsHeroTranslucentLighting(HsSurfaceClosure surface, float3 world,", 1)[1]
        fallback = lighting.split("const float3 exit_world", 1)[0]
        self.assertIn("SurfaceDepth.Load(source_pixel)", fallback)
        self.assertIn("VmRawOpaqueHdr.Load(source_pixel)", fallback)
        self.assertIn("HdrColor.Load(source_pixel)", fallback)
        self.assertIn("background_view_depth=HsFogViewDepth(source_world)", fallback)
        # Foreground rejection and interval fog remain enforced for displaced samples.
        self.assertIn("if(depth>0.0 && depth<=exit_clip.z/exit_clip.w)", lighting)
        self.assertIn("HsSampleVolumetricInterval(background_uv", lighting)

    def test_virtual_shadow_depth_only_receivers_preserve_coverage_contract(self):
        shader = (SHADERS / "virtual_shadow.hlsl").read_text(encoding="utf-8")
        feedback = shader[shader.index("void FeedbackCS("):shader.index("bool KeyEqual(")]
        begin = feedback.index("BeginFeedbackRequests(lane);")
        gate = feedback.index("if (box.minimum.w == 0.0)")
        flush = feedback.index("FlushFeedbackRequests(lane);")
        self.assertLess(begin, gate)
        self.assertLess(gate, flush)
        # Depth-only lanes must still reach the group's final shared-memory barrier.
        after_begin = re.sub(r"//[^\n]*", "", feedback[begin:])
        self.assertNotRegex(after_begin, r"\breturn\b")
        renderer = SHADERS.parents[1] / "Source" / "RendererD3D12" / "Private"
        native = (renderer / "renderer_virtual_shadow.cpp").read_text(encoding="utf-8")
        self.assertIn("!Finite(receiver.minimum) || !Finite(receiver.maximum)", native)
        self.assertIn("receiver.minimum.w != 0.0f && receiver.minimum.w != 1.0f", native)
        self.assertIn("static_assert(sizeof(VirtualShadowReceiver) == 32)", native)
        # The existing depth-range walk includes depth-only probes as well as requesters.
        depth = native[native.index("for (const auto &receiver : in.receivers)"):native.index("minimum -= 1.0")]
        self.assertIn("const double z = Dot", depth)
        self.assertNotRegex(depth, r"\bcontinue\b")
        frame = (renderer / "renderer_frame.cpp").read_text(encoding="utf-8")
        probe = frame[frame.index("for (const auto &probe : impl_->local_probes.Probes())"):]
        probe = probe[:probe.index("if (impl_->volumetrics.AllocatedBytes()")]
        self.assertRegex(probe, r"probe\.position\.z - probe\.capture_distance\}\), 1\.0f\}")
        self.assertIn("if (water_receiver) receivers.push_back(*water_receiver)", frame)
        self.assertIn("if (gel_receiver) receivers.push_back(*gel_receiver)", frame)
        self.assertIn("environment_variant_radii[render_data::kMeadowWaterGeometry]", frame)
        # The origin-centered sphere includes offsets, signed/nonuniform scale, and yaw.
        rng = random.Random(0x56534D)
        for _ in range(128):
            offset = [rng.uniform(-3, 3) for _ in range(3)]
            radius = rng.uniform(.01, 3)
            scale = [rng.uniform(-4, 4) for _ in range(3)]
            yaw = rng.uniform(-math.pi, math.pi)
            world_radius = (math.sqrt(sum(v*v for v in offset)) + radius) * max(map(abs, scale))
            for axis in range(3):
                for sign in (-1, 1):
                    local = offset.copy()
                    local[axis] += sign * radius
                    x, y, z = [v*s for v, s in zip(local, scale)]
                    point = (x*math.cos(yaw)+z*math.sin(yaw), y, -x*math.sin(yaw)+z*math.cos(yaw))
                    self.assertLessEqual(max(map(abs, point)), world_radius + 1e-12)

    def test_shadow_frontends_skip_temporal_deformation(self):
        scene = (SHADERS / "project_hs.hlsl").read_text(encoding="utf-8-sig")
        shadow = scene.split("SceneOutput ShadowVS(", 1)[1].split("void EnvironmentShadowPS(", 1)[0]
        self.assertIn("(SceneDrawMaterial & 1u) != 0, false)", shadow)
        self.assertIn("cached_skin_history = evaluate_history && context.cachedSkinHistory", scene)
        self.assertIn("const bool grass_history = evaluate_history &&", scene)
        indexed = (SHADERS / "geometry.hlsl").read_text(encoding="utf-8-sig")
        self.assertIn("return EvaluateClusterVertex(vertex_id, instance_id, true)", indexed)
        self.assertIn("SceneOutput output = EvaluateClusterVertex(vertex_id, instance_id, false)", indexed)
        mesh = (SHADERS / "geometry.hlsli").read_text(encoding="utf-8-sig")
        self.assertIn("(record.Flags & 1u) != 0u, !shadow)", mesh)
        # Main surface resolve keeps the previous-frame deformation for motion.
        resolve = (SHADERS / "visibility_buffer.hlsl").read_text(encoding="utf-8-sig")
        self.assertIn("EvaluateSceneVertex(input0, context, indices.x)", resolve)

    def test_grass_outer_guard_skips_only_zero_contributions(self):
        scene = (SHADERS / "grass_deformation.hlsli").read_text(encoding="utf-8-sig")
        self.assertIn("if (max(abs(delta.x), abs(delta.y)) > radius) continue", scene)
        for radius in (0.1, 1.0, 1.25, 100.0):
            for x in (-100.0, -2.001, -2.0, -1.0, 0.0, 1.0, 2.0, 2.001, 100.0):
                for y in (-3.0, -1.0, 0.0, 1.0, 3.0):
                    dx, dy = x * radius, y * radius
                    if max(abs(dx), abs(dy)) > radius:
                        self.assertEqual(max(0.0, 1.0 - math.hypot(dx, dy) / radius), 0.0)
    def test_entity_surface_shares_primary_depth_world_across_frontends(self):
        surface = (SHADERS / "entity_surface.hlsli").read_text(encoding="utf-8-sig")
        self.assertIn("float4 HsEntitySurface(uint mesh, uint2 key, float3 world)", surface)
        self.assertIn("uint2(abs(noise_uv.xz) * float2(stbn_width, stbn_height))", surface)
        self.assertIn("VfxStbnScalar.Load(int4(noise_pixel % uint2(stbn_width, stbn_height)", surface)
        self.assertIn("const float3 noise_uv = local * shape.y + seed_offset", surface)
        self.assertIn("EntitySurfaceNoise.SampleLevel(MaterialSampler, noise_uv, 0)", surface)
        scene = (SHADERS / "project_hs.hlsl").read_text(encoding="utf-8-sig")
        output = scene.split("struct SceneOutput", 1)[1].split("};", 1)[0]
        self.assertIn("float4 Position : SV_Position", output)
        evaluate = scene.split("GBufferOutput HsHeroEvaluateSceneSurface(", 1)[1].split("return output;", 1)[0]
        self.assertRegex(evaluate, r"const float3 entity_world = HsReconstructSurfaceRelative\(\s*"
                                   r"HsSurfaceRasterUv\(int2\(input.Position.xy\)\), input.Position.z\) \+ SurfaceViewOrigin.xyz")
        self.assertIn("HsEntitySurface(input.Mesh, input.InstanceKey, entity_world)", evaluate)
        self.assertLess(scene.index('#include "surface_layout.hlsli"'),
                        scene.index("GBufferOutput HsHeroEvaluateSceneSurface("))
        shadow = scene.split("void HeroShadowPS(", 1)[1].split("#endif", 1)[0]
        self.assertNotIn("HsHeroEvaluateSceneSurface", shadow)
        visibility = (SHADERS / "visibility_buffer.hlsl").read_text(encoding="utf-8-sig")
        self.assertIn("output.Position = float4((float2)pixel + 0.5", visibility)
        interpolate = visibility.split("SceneOutput HsInterpolateSceneOutput(", 1)[1].split("return output;", 1)[0]
        self.assertRegex(interpolate, r"output.Position = float4\(\(float2\)pixel \+ 0.5,\s*"
                                     r"SurfaceDepth.Load\(int3\(pixel, 0\)\), rcp\(sample.clipW\)\)")
        frame = (SHADERS.parents[1] / "Source/RendererD3D12/Private/renderer_frame.cpp").read_text(encoding="utf-8-sig")
        resolve = frame.split('auto resolve = impl_->graph.AddPass(30, "SurfaceResolve"', 1)[1].split(
            "// Every successful feature registration", 1)[0]
        self.assertIn("resolve.Read(depth_version, Access::ComputeRead)", resolve)
        self.assertLess(frame.index("gbuffer_retest_pass.ReadWrite(depth_version, Access::DepthWrite)"),
                        frame.index('auto resolve = impl_->graph.AddPass(30, "SurfaceResolve"'))
    def test_visibility_entity_surface_identity_and_binding(self):
        source = (SHADERS / "visibility_buffer.hlsl").read_text(encoding="utf-8-sig")
        context = source.split("HsSceneVertexContext HsResolveVertexContext(", 1)[1].split(
            "bool HsLoadResolveTriangle(", 1)[0]
        self.assertIn("context.instanceKey = uint2(draw.InstanceIndex + 1u, draw.InstanceGeneration)", context)
        interpolate = source.split("SceneOutput HsInterpolateSceneOutput(", 1)[1].split("return output;", 1)[0]
        self.assertIn("output.InstanceKey = a.InstanceKey", interpolate)
        frame = (SHADERS.parents[1] / "Source/RendererD3D12/Private/renderer_frame.cpp").read_text(encoding="utf-8-sig")
        resolve = frame.split('auto resolve = impl_->graph.AddPass(30, "SurfaceResolve"', 1)[1].split(
            "// Every successful feature registration", 1)[0]
        self.assertRegex(resolve, r"SetComputeRootShaderResourceView\(\s*34,\s*dynamic_instance_base\)")
        self.assertIn("resolve.Read(dynamic_instances_version, Access::ComputeRead)", resolve)
        self.assertIn("bind_surface_compute(list)", resolve)
        surface_compute = frame.split("const auto bind_surface_compute =", 1)[1].split(
            "const auto draw_fullscreen_with_table =", 1)[0]
        self.assertIn("bind_compute(list)", surface_compute)
        self.assertIn("SetComputeRootDescriptorTable(13, character_table)", surface_compute)
        self.assertIn("SetComputeRootDescriptorTable(26, distortion_table)", surface_compute)
        self.assertIn("read_material_sampling(resolve, has_authored_material_draw, Access::ComputeRead)", resolve)
        sampling = frame.split("const auto read_material_sampling =", 1)[1].split(
            "const auto authored_source =", 1)[0]
        for resource in ("entity_surface_noise_version", "entity_surface_gradient_version", "entity_surface_stbn_version"):
            self.assertIn(f"pass.Read({resource}, sampling)", sampling)
        surface = (SHADERS / "entity_surface.hlsli").read_text(encoding="utf-8-sig")
        self.assertIn("mesh < 13u || mesh > 15u || key.x == 0u", surface)
        self.assertIn("instance1.y != key.y || instance1.w == 0u", surface)
        self.assertIn("HsGpuSceneSlotValid(slot, h0, h1, h2, h3)", surface)
        self.assertNotIn("HsGpuSceneTables.GetDimensions", surface)
        self.assertIn("const uint scene_bytes = SceneTableBounds.x", surface)
        self.assertIn("header.z > scene_bytes - group", surface)
        self.assertRegex(frame, r"constants->scene_table_bounds\s*=\s*\{\s*"
                               r"static_cast<std::uint32_t>\(dynamic_scene_layout.total_bytes\),\s*0u,\s*0u,\s*0u\}")
        self.assertLess(frame.index("constants->scene_table_bounds ="),
                        frame.index("SetGraphicsRootConstantBufferView(15"))
        self.assertLess(frame.index("constants->scene_table_bounds ="),
                        frame.index("SetComputeRootConstantBufferView(15"))

    def test_cluster_demand_survives_incomplete_residency(self):
        # CPU protocol contract, not compiled-HLSL execution: demand the entire
        # valid prefix even after a missing or malformed resident page.
        for states in itertools.product(("ready", "missing", "bad_page", "bad_cluster"), repeat=4):
            stop = states.index("bad_cluster") if "bad_cluster" in states else len(states)
            for check_residency in (False, True):
                demand, complete, catalog_fault = {}, True, False
                inspected = 0
                for index, state in enumerate(states):
                    if state == "bad_cluster":
                        catalog_fault, complete = True, False
                        break
                    page, priority = index % 2, index + 1
                    demand[page] = max(demand.get(page, 0), priority)
                    if check_residency:
                        inspected += 1
                        catalog_fault |= state == "bad_page"
                        complete = (state == "ready") and complete
                expected = {page: max(index + 1 for index in range(stop) if index % 2 == page)
                            for page in range(2) if any(index % 2 == page for index in range(stop))}
                self.assertEqual(demand, expected)
                self.assertEqual(complete, stop == len(states) and
                                 (not check_residency or all(state == "ready" for state in states)))
                self.assertEqual(catalog_fault, stop != len(states) or
                                 (check_residency and "bad_page" in states[:stop]))
                self.assertEqual(inspected, stop if check_residency else 0)
        source = (SHADERS / "cluster_refinement.hlsl").read_text(encoding="utf-8-sig")
        count = source.split("void CountCuts(", 1)[1].split("groupshared", 1)[0]
        self.assertNotIn("CompleteRepresentation(", count)
        self.assertIn("DemandRepresentation(id, group, priority, true)", count)
        demand = source.split("bool DemandRepresentation(", 1)[1].split("float4 WorldSphere(", 1)[0]
        self.assertLess(demand.index("InterlockedMax(PageDemand"), demand.index("Resident(cluster.PageSlot)"))
        self.assertIn("complete = Resident(cluster.PageSlot) && complete", demand)

    def test_cluster_empty_emit_preserves_fault_guards(self):
        # Count is authoritative and inputs are immutable until Emit; malformed high words still fault.
        def emit(expected, actual, base_high, prior_fault, optimized):
            if prior_fault:
                return "prior-fault", 0
            if base_high:
                return "capacity-fault", 0
            if optimized and expected == (0, 0):
                return "ok", 0
            if expected[1] or actual != expected[0]:
                return "catalog-fault", actual
            return "ok", actual

        for counts in ((0,), (0, 3, 0, 7), (1, 0, 0)):
            for high, base_high, prior_fault in itertools.product((0, 1), repeat=3):
                expected = [(count, high) for count in counts]
                before = [emit(value, count, base_high, prior_fault, False)
                          for value, count in zip(expected, counts)]
                after = [emit(value, count, base_high, prior_fault, True)
                         for value, count in zip(expected, counts)]
                self.assertEqual(before, after)
        self.assertEqual(emit((0, 1), 0, 0, False, True)[0], "catalog-fault")
        self.assertEqual(emit((0, 0), 0, 1, False, True)[0], "capacity-fault")
        self.assertEqual(emit((3, 0), 2, 0, False, True)[0], "catalog-fault")
        source = (SHADERS / "cluster_refinement.hlsl").read_text(encoding="utf-8-sig")
        body = source.split("void EmitCuts(", 1)[1].split("bool SphereOccluded(", 1)[0]
        empty = body.index("if (all(expected == uint2(0u, 0u))) return;")
        for guard in ("CutStatus.Load(16u) != 0u", "if (!CandidateValid(", "if (base.y != 0u)"):
            self.assertLess(body.index(guard), empty)
        self.assertLess(empty, body.index("for (uint selected_index"))
        self.assertIn("if (expected.y != 0u || count != expected.x)", body)

    def test_cluster_main_suffix_keeps_full_retest_transaction(self):
        retest_beyond_main = False
        for states in itertools.product(("inactive", "visible", "occluded"), repeat=4):
            for fault in (False, True):
                live = [index for index, state in enumerate(states) if not fault and state == "visible"]
                retest = [index for index, state in enumerate(states) if not fault and state == "occluded"]
                published = max((index + 1 for index in live), default=0)
                self.assertEqual([index for index in range(published) if states[index] == "visible"], live)
                self.assertFalse(any(index >= published for index in live))
                # Retest still addresses the complete committed transaction.
                retest_beyond_main |= any(index >= published for index in retest)
        self.assertTrue(retest_beyond_main)
        source = (SHADERS / "cluster_refinement.hlsl").read_text(encoding="utf-8-sig")
        self.assertIn("CommandCounts.Store4(0u, uint4(0u, committed, committed, committed))", source)
        cull = source.split("void CullClusters(", 1)[1].split("void RetestClusters(", 1)[0]
        self.assertIn("CommandCounts.InterlockedMax(0u, selection + 1u, ignored)", cull)
        self.assertNotIn("CommandCounts.Store(0u", cull)
        retest = source.split("void RetestClusters(", 1)[1]
        self.assertIn("min(CutStatus.Load(20u), OutputCapacity)", retest)
        self.assertNotIn("CommandCounts.Load(0u)", retest)

    def test_light_mask_word_boundaries_and_zero_reuse(self):
        candidates = [3, 31, 32, 63, 64, 95, 96, 127]
        kinds = [0] * 3 + [1] * 125
        self.assertEqual(bitset_light_query(candidates, 3, kinds, 8, 31), [0, 1, 2] + candidates)
        self.assertEqual(bitset_light_query(candidates, 3, kinds, 8, 2, {32, 96}),
                         [index for index in candidates if index not in (32, 96)])
        self.assertEqual(bitset_light_query([], 0, kinds, 8, 31), [])
        self.assertEqual(bitset_light_query(list(range(128)), 0, [1] * 128, 64, 31), list(range(128)))

    def test_light_mask_matches_inline_spill_and_world_queries(self):
        rng = random.Random(2903)
        for _ in range(1200):
            total = rng.randrange(129)
            directional = rng.randrange(min(5, total) + 1)
            kinds = [0] * directional + [rng.randrange(1, 5) for _ in range(total - directional)]
            candidates = sorted(rng.sample(range(directional, total), rng.randrange(total - directional + 1)))
            capacity = rng.choice((4, 8, 16, 32, 64, 128))
            type_mask = rng.randrange(32)
            transient = set(rng.sample(range(total), rng.randrange(total + 1)))
            world = rng.choice((False, True))
            # Complete membership is independent of diagnostic inline/spill limits.
            local = list(range(directional, total)) if world else candidates
            expected = [index for index in list(range(directional)) + local
                        if type_mask & (1 << kinds[index]) and index not in transient]
            self.assertEqual(bitset_light_query(candidates, directional, kinds, capacity,
                                               type_mask, transient, world), expected)

    def test_rejected_footprint_does_not_contribute(self):
        taps = [(.2, .5, 7, 1), (.4, .6, 8, 1), (.4, .6, 8, 1), (.4, .6, 8, 1)]
        color, confidence = reconstruct(taps, (.45, .45))
        self.assertAlmostEqual(color, .2)
        self.assertAlmostEqual(confidence, .3025)
        self.assertAlmostEqual(.88 * .1395 * (1 - .1395) + .2, .30563498)

    def test_all_valid_preserves_bilinear(self):
        taps = [(r, .5, 7, 1) for r in (.1, .2, .3, .4)]
        color, confidence = reconstruct(taps, (.25, .75), exposure=2)
        self.assertAlmostEqual(color, .55)
        self.assertAlmostEqual(confidence, 1)

    def test_independent_guide_and_nonfinite_rejection(self):
        for rejected in ((.8, .6, 7, 1), (.8, .5, 8, 1), (.8, .5, 7, .84),
                         (float("nan"), .5, 7, 1), (.8, float("nan"), 7, 1)):
            self.assertEqual(reconstruct([rejected] * 4, (.5, .5)), (None, 0))
        self.assertEqual(reconstruct([(.8, 0, 0, 1)] * 4, (.5, .5), sky=True), (.8, 1))
        self.assertEqual(reconstruct([(.8, .5, 0, 1)] * 4, (.5, .5), sky=True), (None, 0))

    def test_zero_weight_invalid_neighbor(self):
        taps = [(.2, .5, 7, 1)] + [(float("nan"), .5, 7, 1)] * 3
        self.assertEqual(reconstruct(taps, (0, 0)), (.2, 1))

    def test_fxaa_center_contrast(self):
        for center, neighbors in ((1, [0]*4), (0, [1]*4)):
            self.assertEqual(max(center, *neighbors) - min(center, *neighbors), 1)
            self.assertEqual(max(neighbors) - min(neighbors), 0)
        self.assertEqual(max([.4]*5) - min([.4]*5), 0)

    def test_actual_camera_basis(self):
        for forward, up, right in (((0, 0, 1), (0, 1, 0), (1, 0, 0)),
                                   ((0, 0, 1), (1, 0, 0), (0, -1, 0)),
                                   ((0, -1, 0), (0, 0, 1), (1, 0, 0)),
                                   ((0, 1, 0), (0, 0, 1), (-1, 0, 0))):
            self.assertEqual(cross(up, forward), right)
            self.assertEqual(cross(forward, right), up)

    def test_source_uses_validated_history_and_actual_basis(self):
        temporal = (SHADERS / "temporal_reconstruction.hlsli").read_text(encoding="utf-8-sig")
        self.assertNotIn("history_color_depth.SampleLevel", temporal)
        for source in ("history_color_depth.Load(coordinate)", "history_material.Load(coordinate)",
                       "history_normal_oct.Load(coordinate)", "valid_history_weight"):
            self.assertIn(source, temporal)
        project = (SHADERS / "project_hs.hlsl").read_text(encoding="utf-8-sig")
        for entry in ("ParticleVS", "VfxFlashVS", "VfxDistortionVS"):
            body = project[project.index(entry + "("):]
            body = body[:body.index("\n}\n")]
            self.assertIn("LodViewRight.xyz", body, entry)
            self.assertIn("LodViewUp.xyz", body, entry)
            self.assertNotIn("cross(float3(0, 1, 0)", body, entry)
        self.assertIn("max(luma_center, luma_max) - min(luma_center, luma_min)", project)

    def test_frame_constant_copies_are_exact_prefixes(self):
        def fields(name):
            source = (SHADERS / name).read_text(encoding="utf-8-sig")
            body = re.search(r"cbuffer FrameConstants[^\{]*\{(.*?)\};", source, re.S).group(1)
            body = re.sub(r"//[^\n]*", "", body)
            return [" ".join(field.split()) for field in body.split(";") if field.strip()]
        canonical = fields("project_hs.hlsl")
        for name in ("screen_reflections.hlsl", "skin_cache.hlsl", "static_visibility.hlsl", "depth_pyramid.hlsl"):
            copy = fields(name)
            self.assertEqual(copy, canonical[:len(copy)], name)
        offset = 0
        offsets = {}
        for field in canonical:
            matched = re.fullmatch(r"(?:row_major )?(float4x4|float4|uint4) (\w+)((?:\[\d+\])*)", field)
            self.assertIsNotNone(matched, field)
            kind, name, dimensions = matched.groups()
            offsets[name] = offset
            count = math.prod(int(n) for n in re.findall(r"\d+", dimensions))
            offset += (64 if kind == "float4x4" else 16) * count
        self.assertEqual({name: offsets[name] for name in ("ScreenSize", "LodClock", "OutputSize", "ProbeOptions")},
                         {"ScreenSize": 192, "LodClock": 11200, "OutputSize": 11392, "ProbeOptions": 11712})
        self.assertEqual(offsets["PreviousGrassBenders"], 11728)
        self.assertEqual(offsets["PreviousGrassState"], 12240)
        self.assertEqual(offsets["PreviousGrassOriginDelta"], 12256)
        self.assertEqual(offsets["SceneTableBounds"], 12272)
        self.assertEqual(offsets["GrassBenderGrid"], 12288)
        self.assertEqual(offsets["GrassBenderMasks"], 12304)
        self.assertEqual(offsets["PreviousGrassBenderGrid"], 12560)
        self.assertEqual(offsets["PreviousGrassBenderMasks"], 12576)
        self.assertEqual(offsets["ParticleTerrain"], 12832)
        self.assertEqual(offset, 12848)


if __name__ == "__main__":
    unittest.main()
