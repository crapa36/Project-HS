"""CPU arithmetic parity only; compiled shader output requires native validation."""
import random
import unittest


def layout(width, height):
    levels, offset = [], 0
    while True:
        levels.append((width, height, offset))
        offset += width * height
        if width == height == 1:
            return levels, offset
        width, height = (width + 1) // 2, (height + 1) // 2


def grouped(depth, width, height, levels, total):
    output = [2.0] * total  # Poison padding above the valid reversed-Z range.
    for y in range(height):
        output[y * levels[0][0]:y * levels[0][0] + width] = depth[y * width:(y + 1) * width]
    for level in range(1, len(levels), 3):
        scale = 1 << level
        aw, ah = (width + scale - 1) // scale, (height + scale - 1) // scale
        pw, ph = (width + scale // 2 - 1) // (scale // 2), (height + scale // 2 - 1) // (scale // 2)
        stride, _, offset = levels[level - 1]
        for gy in range((ah + 7) // 8):
            for gx in range((aw + 7) // 8):
                tile = [0.0] * 64
                for y in range(8):
                    for x in range(8):
                        px, py = gx * 8 + x, gy * 8 + y
                        if px < aw and py < ah:
                            tile[y * 8 + x] = max(
                                output[offset + (py * 2 + dy) * stride + px * 2 + dx]
                                for dy in range(2) for dx in range(2)
                                if px * 2 + dx < pw and py * 2 + dy < ph)
                for step in range(min(3, len(levels) - level)):
                    side, scale = 8 >> step, 1 << (level + step)
                    active_w, active_h = (width + scale - 1) // scale, (height + scale - 1) // scale
                    stride_out, _, offset_out = levels[level + step]
                    for y in range(side):
                        for x in range(side):
                            px, py = gx * side + x, gy * side + y
                            if px < active_w and py < active_h:
                                output[offset_out + py * stride_out + px] = tile[y * side + x]
                    tile = [max(tile[(y * 2 + dy) * side + x * 2 + dx]
                                for dy in range(2) for dx in range(2))
                            for y in range(side // 2) for x in range(side // 2)]
    return output


class SsrDepthReduction(unittest.TestCase):
    def test_all_levels_match_direct_footprint_max(self):
        rng = random.Random(41703)
        sizes = [(1, 1), (1, 3), (3, 1), (3, 5), (7, 4), (15, 17),
                 (17, 15), (31, 33), (33, 31), (63, 65), (65, 63), (127, 73)]
        sizes += [(rng.randrange(1, 98), rng.randrange(1, 98)) for _ in range(64)]
        for width, height in sizes:
            for padded in (False, True):
                capacity = (width + rng.randrange(1, 20), height + rng.randrange(1, 20)) if padded else (width, height)
                levels, total = layout(*capacity)
                for background in (True, False):
                    with self.subTest(size=(width, height), capacity=capacity, background=background):
                        depth = [0.0 if background or rng.random() < .5 else rng.random()
                                 for _ in range(width * height)]
                        if not background:
                            depth[-1] = 1.0  # Odd trailing row/column extrema must survive.
                        expected = [2.0] * total
                        for level, (stride, _, offset) in enumerate(levels):
                            scale = 1 << level
                            for y in range((height + scale - 1) // scale):
                                for x in range((width + scale - 1) // scale):
                                    expected[offset + y * stride + x] = max(
                                        depth[sy * width + sx]
                                        for sy in range(y * scale, min((y + 1) * scale, height))
                                        for sx in range(x * scale, min((x + 1) * scale, width)))
                        self.assertEqual(grouped(depth, width, height, levels, total), expected)


if __name__ == '__main__':
    unittest.main()
