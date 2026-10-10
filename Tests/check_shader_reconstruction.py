"""CPU arithmetic/source contracts only; compiled shader output needs native validation."""
import math
from pathlib import Path
import re
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


class ShaderContracts(unittest.TestCase):
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
        self.assertEqual(offset, 11728)


if __name__ == "__main__":
    unittest.main()
