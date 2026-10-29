"""Static FrameConstants ABI shared by independently compiled shader entrypoints."""
import math
from pathlib import Path
import re
import unittest

SHADERS = Path(__file__).resolve().parents[1] / "Content" / "Shaders"


class ShaderContracts(unittest.TestCase):
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
                         {"ScreenSize": 192, "LodClock": 3008, "OutputSize": 3200, "ProbeOptions": 3520})
        self.assertEqual(offsets["PreviousGrassBenders"], 3536)
        self.assertEqual(offsets["PreviousGrassState"], 4048)
        self.assertEqual(offsets["PreviousGrassOriginDelta"], 4064)
        self.assertEqual(offsets["SceneTableBounds"], 4080)
        self.assertEqual(offsets["GrassBenderGrid"], 4096)
        self.assertEqual(offsets["GrassBenderMasks"], 4112)
        self.assertEqual(offsets["PreviousGrassBenderGrid"], 4368)
        self.assertEqual(offsets["PreviousGrassBenderMasks"], 4384)
        self.assertEqual(offsets["ParticleTerrain"], 4640)
        self.assertEqual(offsets["PreviousInstanceUnjitteredViewProjection"], 4656)
        self.assertEqual(offset, 4720)


if __name__ == "__main__":
    unittest.main()
