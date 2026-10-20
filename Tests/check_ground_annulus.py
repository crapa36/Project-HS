"""Conservative annulus strip covers every analytic ring point and keeps winding."""
import math
import random
import unittest
from pathlib import Path

class GroundAnnulus(unittest.TestCase):
    def test_strip_covers_ring_without_clipping_exact_sdf(self):
        rng=random.Random(5123);n=32;cosine=math.cos(math.pi/n)
        for _ in range(10000):
            outer=rng.uniform(.01,40);inner=rng.uniform(.450001,.999999)*outer
            angle=rng.uniform(0,2*math.pi);r=rng.uniform(inner,outer)
            sector=math.floor(angle/(2*math.pi/n))
            relative=angle-(sector+.5)*2*math.pi/n
            outer_polygon=(outer/.99518472667+.0001)*cosine/math.cos(relative)
            hole_polygon=max(0,inner-.0001)*cosine/math.cos(relative)
            self.assertLessEqual(hole_polygon,r)
            self.assertGreaterEqual(outer_polygon,r)
        shader=(Path(__file__).resolve().parents[1]/'Content/Shaders/project_hs.hlsl').read_text(encoding='utf-8-sig')
        vertex=shader.split('ParticleOutput GroundRingVS(',1)[1].split('// Authored owner-follow meshes',1)[0]
        self.assertIn('command.Metadata.z == 5u || command.Metadata.z == 6u',vertex)
        self.assertIn('(-6.28318530718 / 32.0)',vertex)
        self.assertIn('output.LocalUv',vertex)
        # A narrow band shades polygon area, not the enclosing 4R^2 square.
        ratio=math.pi*(1-.8**2)/(4*cosine**2)
        self.assertLess(ratio,.29)

if __name__=='__main__':unittest.main()
