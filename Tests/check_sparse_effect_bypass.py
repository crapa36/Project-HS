"""Focused CPU/source contracts; native sparse-effect shader checks remain separate."""
import math
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SparseEffectBypass(unittest.TestCase):
    def test_zero_scattering_preserves_absorption_and_emission(self):
        for extinction in (0.0, .01, 4.0):
            for emission in ((0.0, 0.0, 0.0), (.2, 3.0, 50.0)):
                for lighting in ((0.0, 1.0, 1000.0), (20.0, 3.0, .5)):
                    reference = tuple(0.0 * light + emit for light, emit in zip(lighting, emission)) + (extinction,)
                    self.assertEqual(emission + (extinction,), reference)
                    distance = 2.0
                    self.assertEqual(math.exp(-reference[3] * distance), math.exp(-extinction * distance))
        shader = (ROOT/'Content/Shaders/volumetrics.hlsl').read_text()
        lighting = shader.split('void VolumetricLightingCS', 1)[1].split('float4 VmPreviousSample', 1)[0]
        self.assertLess(lighting.index('if(all(medium.xyz==0.0))'), lighting.index('VmFroxelWorld(p)'))
        self.assertLess(lighting.index('VmRawOutput[p]=float4(emission,medium.w);'), lighting.index('HsBeginLightQuery'))
        self.assertEqual(lighting.count('VmRawOutput[p].xyz'), 1)
        self.assertIn('VmRawOutput[p]=float4(medium.xyz*lighting+emission,medium.w);', lighting)
        self.assertIn('VmLuminanceOutput[uint3(p.xy,0)]=0.0; VmTransmittanceOutput[uint3(p.xy,0)]=1.0;', shader)

    def test_center_guide_reuse_keeps_actual_normal_weight(self):
        def filtered(reuse_center, active_channels):
            reads = 0
            center = (.2, .3, .9)
            def load(offset):
                nonlocal reads
                reads += 1
                return center if offset == 0 else (0.0, 0.0, 1.0)
            result = []
            for channel in range(active_channels):
                total = normalization = 0.0
                for offset in range(-2, 3):
                    guide = center if reuse_center and offset == 0 else load(offset)
                    normal_weight = min(max(sum(a*b for a, b in zip(center, guide)), 0.0), 1.0)
                    distance, radius = abs(offset)*.001, (.0001, .002, .1)[channel]
                    weight = (math.exp(-distance/radius) + math.exp(-distance/(3*radius))) * normal_weight
                    total += (offset + 3 + channel) * weight
                    normalization += weight
                result.append(total / normalization)
            return result, reads
        for channels in (1, 2, 3):
            reference, old_reads = filtered(False, channels)
            reused, new_reads = filtered(True, channels)
            self.assertEqual(reused, reference)
            self.assertEqual(old_reads - new_reads, channels)
        shader = (ROOT/'Content/Shaders/subsurface_diffusion.hlsl').read_text()
        self.assertIn('SssGuide neighbor=center;', shader)
        self.assertIn('if(offset!=0) neighbor=SssLoadGuide(neighbor_pixel);', shader)
        self.assertIn('const float normal_weight=saturate(dot(center.normal,neighbor.normal));', shader)
        self.assertIn('SssHorizontalOutput[pixel]=float4(diffuse,1.0);', shader)
        self.assertIn('SssCorrectedOutput[pixel]=hdr;', shader)
        self.assertIn('hdr.rgb+=saturate(profile.sss_weight)*(blurred-original);', shader)


if __name__ == '__main__':
    unittest.main()
