"""PCF tap-table and CPU arithmetic check; compiled shader validation is separate."""
from collections import Counter
import math
from pathlib import Path
import random
import re
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
FILTER = (ROOT / "Content/Shaders/shadow_filter.hlsli").read_text(encoding="utf-8")
OFFSETS = [tuple(map(int, values)) for values in re.findall(r"int2\((-?\d+),(-?\d+)\)", FILTER)]
FOOTPRINTS = [tuple(map(int, values)) for values in re.findall(r"int4\((-?\d+),(-?\d+),(\d+),(\d+)\)", FILTER)]
LANES = ((0, 1), (1, 1), (1, 0), (0, 0))  # Documented GatherRed xyzw ordering.


def gather_offsets(samples):
    groups = FOOTPRINTS[4:] if samples == 16 else FOOTPRINTS[:4]
    return [(x + dx, y + dy) for x, y, mask8, mask12 in groups
            for lane, (dx, dy) in enumerate(LANES)
            if (mask12 if samples == 12 else mask8) & (1 << lane)]


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


class ShadowGatherContracts(unittest.TestCase):
    def test_exact_taps_banks_edges_and_coordinates(self):
        self.assertEqual((len(OFFSETS), len(FOOTPRINTS)), (16, 10))
        for samples in (8, 12, 16):
            self.assertEqual(Counter(gather_offsets(samples)), Counter(OFFSETS[:samples]))

        # Banks shadow different texels; combining their averaged visibilities fails.
        rng = random.Random(3127)
        extent, origin = 9, (3, 5)
        upper = tuple(value + extent - 1 for value in origin)
        static = {(x, y): rng.choice((0., .2, .5, .8, 1.))
                  for x in range(origin[0], upper[0] + 1)
                  for y in range(origin[1], upper[1] + 1)}
        dynamic = {point: 1. - value for point, value in static.items()}
        checked = 0
        for samples in (4, 8, 12, 16):
            for probe in (False, True):
                for x in range(origin[0], upper[0] + 1):
                    for y in range(origin[1], upper[1] + 1):
                        center = (x, y)
                        interior = all(origin[axis] <= center[axis] - 2 and
                                       center[axis] + 2 <= upper[axis] for axis in range(2))
                        optimized = gather_offsets(samples) if samples >= 8 and interior else OFFSETS[:samples]
                        for receiver, bias in ((0., 0.), (.2, 0.), (.5, 0.), (.8, 0.), (1., 0.), (.4999, .0001)):
                            reference = f32(f32(receiver) + f32(bias))
                            def visibility(offsets):
                                lit = 0
                                for dx, dy in offsets:
                                    p = (min(max(x + dx, origin[0]), upper[0]),
                                         min(max(y + dy, origin[1]), upper[1]))
                                    depth = static[p] if probe else max(static[p], dynamic[p])
                                    lit += reference >= depth
                                return lit
                            self.assertEqual(visibility(OFFSETS[:samples]), visibility(optimized))
                            checked += 1

        # Midway-between-centers UVs remain away from footprint selection boundaries.
        for extent in (260, 520, 1028, 2056, 4096, 6552, 8736, 16384):
            inverse = f32(1. / extent)
            for upper_left in range(extent - 1):
                uv = f32(f32(upper_left + 1.) * inverse)
                self.assertEqual(math.floor(f32(uv * extent) - .5), upper_left)
        print(f"PCF: {checked} mixed-bank/probe/edge cases exact; 8/12/16 depth instructions 16/24/32 -> 8/8/12.")


if __name__ == "__main__":
    unittest.main()
