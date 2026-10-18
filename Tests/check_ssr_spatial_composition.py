"""SSR fusion CPU/source contracts; native GPU equivalence is a separate gate."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


def compose(opaque, environment, resolved):
    confidence = min(max(resolved[3], 0.0), 1.0)
    return tuple(min(max(opaque[i] - environment[i] + environment[i] * (1-confidence)
                         + resolved[i] * confidence, 0.0), 65000.0) for i in range(3)) + (opaque[3],)


class SpatialComposition(unittest.TestCase):
    def test_miss_keeps_environment_and_alpha(self):
        self.assertEqual(compose((3, 4, 5, .25), (1, 2, 3), (0, 0, 0, 0)), (3, 4, 5, .25))

    def test_hit_replaces_environment_instead_of_adding_it(self):
        self.assertEqual(compose((3, 4, 5, .25), (1, 2, 3), (7, 8, 9, 1)), (9, 10, 11, .25))
        self.assertEqual(compose((3, 4, 5, .25), (1, 2, 3), (7, 8, 9, .5)), (6, 7, 8, .25))

    def test_confidence_and_hdr_bounds(self):
        self.assertEqual(compose((0, 2, 3, 1), (10, 1, 1), (0, 70000, 5, 2)), (0, 65000, 7, 1))
        self.assertEqual(compose((1, 2, 3, 1), (1, 1, 1), (70000, 0, 0, -1)), (1, 2, 3, 1))

    def test_graph_and_shader_fuse_only_final_spatial_output(self):
        shader = (ROOT/'Content/Shaders/screen_reflections.hlsl').read_text()
        graph = (ROOT/'Source/RendererD3D12/Private/renderer_reflections.cpp').read_text()
        spatial = shader.split('void ScreenReflectionSpatialCS', 1)[1]
        self.assertNotIn('SsrRawOutput[', spatial)
        self.assertIn('SsrComposed[dispatch.xy]', spatial)
        self.assertIn('opaque.rgb - environment + lerp(environment, reflection.rgb, saturate(reflection.a))', spatial)
        self.assertNotIn('ScreenReflectionComposeCS', shader)
        self.assertIn('temporal.Read(raw, Access::UnorderedRead)', graph)
        self.assertIn('raw = trace.Write(raw, Access::UnorderedWrite)', graph)
        self.assertIn('spatial.Read(inputs.opaque_hdr.version, Access::ComputeRead)', graph)
        self.assertIn('spatial.Read(inputs.environment_specular.version, Access::ComputeRead)', graph)
        self.assertIn('composed = spatial.Write(composed, Access::UnorderedWrite)', graph)
        self.assertNotIn('AddPass(73,', graph)
        self.assertNotIn('ssr_compose_cs', (ROOT/'Source/Apps/RenderMaterialShader/main.cpp').read_text())


if __name__ == '__main__':
    unittest.main()
