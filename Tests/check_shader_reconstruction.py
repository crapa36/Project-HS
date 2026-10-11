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
    for index in candidates[:2 * capacity]:
        words[index // 32] |= 1 << (index % 32)
    if world or len(candidates) > 2 * capacity:
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
        ray_hit = (SHADERS / "ray_hit.hlsli").read_text(encoding="utf-8-sig")
        self.assertIn("HsRayEvaluateSurface(HsRayHitContext hit)", ray_hit)
        self.assertIn("HsEntitySurface(h.Mesh,uint2(g.identity.x+1u,g.identity.y),h.WorldPosition)", ray_hit)
        ray_radiance = (SHADERS / "ray_radiance.hlsli").read_text(encoding="utf-8-sig")
        self.assertIn("HsRayEvaluateSurface(hit)", ray_radiance)

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
            # Independent old query: inline then spill, or authoritative world table.
            local = list(range(directional, total)) if world or len(candidates) > 2 * capacity else (
                candidates[:capacity] + candidates[capacity:2 * capacity])
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
        rays = (SHADERS / "ray_radiance.hlsli").read_text(encoding="utf-8-sig")
        self.assertRegex(rays, r"if\(\(light.metadata.z&1u\)!=0u\)\s*visibility=RtShadowVisibility")

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
        self.assertEqual(offset, 12288)


if __name__ == "__main__":
    unittest.main()
