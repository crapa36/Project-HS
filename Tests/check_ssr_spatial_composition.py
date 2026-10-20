"""SSR composition arithmetic checks; these do not execute production shaders."""
import unittest

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

if __name__ == '__main__':
    unittest.main()
