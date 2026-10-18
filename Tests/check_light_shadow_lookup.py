"""CPU/source contracts for direct shadow references and complete 128-bit cell masks."""
import json
import math
import struct
import tempfile

from analyze_light_list import analyze_frame
from pathlib import Path
import random
import unittest

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / "Content/Shaders"


def direct_record(records, reference, identity, revision, enabled=True):
    # Match the raw-buffer address guard: invalid references cannot issue a load.
    if not enabled or reference == 0 or reference > len(records) or not 1 <= identity[3] <= 4:
        return None
    record = records[reference - 1]
    return record if (record[0] == identity and record[1] == revision and record[2]) else None


def cell_mask(bounds, lights, directional, inline_capacity, spill_capacity, invalid=False):
    words = [0] * 4
    total = 0
    for index in range(directional, len(lights)):
        center, radius = lights[index]
        separation = [max(bounds[0][axis] - center[axis], center[axis] - bounds[1][axis], 0)
                      for axis in range(3)]
        if sum(value * value for value in separation) > radius * radius:
            continue
        words[index // 32] |= 1 << (index % 32)
        total += 1
    inline = min(total, inline_capacity)
    spill = min(total - inline, spill_capacity)
    flags = int(total > inline_capacity + spill_capacity) | (2 if invalid else 0)
    return words, (inline, spill, flags, total)


def query(words, counts, directional, total, world=False):
    if world or counts[2] & 2:
        return list(range(directional, total))
    return [index for index in range(directional, total) if words[index // 32] & (1 << (index % 32))]


class LightShadowLookupContracts(unittest.TestCase):
    def test_direct_lookup_keeps_identity_revision_selection_guards(self):
        revision = (0xffffffff, 0x87654321)
        for slot in range(64):
            for kind in range(1, 5):
                identity = (0xffffffff, 0x12345678, 0xfedcba98, kind)
                records = [((0, 0, 0, 0), (0, 0), False)] * 64
                records[slot] = (identity, revision, True)
                reference = slot + 1
                packed = (reference << 1) | 1
                self.assertEqual(direct_record(records, packed >> 1, identity, revision), records[slot])
                for lane in range(4):
                    stale = list(identity)
                    stale[lane] ^= 1
                    self.assertIsNone(direct_record(records, reference, tuple(stale), revision))
                for lane in range(2):
                    stale = list(revision)
                    stale[lane] ^= 1
                    self.assertIsNone(direct_record(records, reference, identity, tuple(stale)))
                for invalid in (0, 65, 0x7fffffff):
                    self.assertIsNone(direct_record(records, invalid, identity, revision))
                self.assertIsNone(direct_record(records, reference, identity, revision, False))
                records[slot] = (identity, revision, False)
                self.assertIsNone(direct_record(records, reference, identity, revision))

    def test_over_capacity_mask_preserves_all_intersecting_lights_and_order(self):
        lights = [((0, 0, 0), 1)] * 128
        # Above the old 32+32 threshold, retain later candidates and do not scan unrelated lights.
        lights[67] = ((100, 100, 100), 1)
        words, counts = cell_mask(((-1, -1, -1), (1, 1, 1)), lights, 1, 32, 32)
        self.assertEqual(counts, (32, 32, 1, 126))
        self.assertEqual(query(words, counts, 1, 128), [index for index in range(1, 128) if index != 67])
        self.assertEqual(query(words, counts, 1, 128, world=True), list(range(1, 128)))
        sparse = [((0, 0, 0), 1)] * 66 + [((100, 100, 100), 1)] * 62
        sparse_words, sparse_counts = cell_mask(((-1, -1, -1), (1, 1, 1)), sparse, 1, 32, 32)
        self.assertEqual(sparse_counts, (32, 32, 1, 65))
        self.assertEqual(query(sparse_words, sparse_counts, 1, 128), list(range(1, 66)))
        _, invalid = cell_mask(((-1, -1, -1), (1, 1, 1)), lights, 1, 32, 32, invalid=True)
        self.assertEqual(invalid[2], 3)
        self.assertEqual(query(words, invalid, 1, 128), list(range(1, 128)))
        words, counts = cell_mask(((-1, -1, -1), (1, 1, 1)), [], 0, 32, 32)
        self.assertEqual(words, [0] * 4)
        self.assertEqual(query(words, counts, 0, 0), [])

    def test_complete_mask_agrees_with_exhaustive_point_lighting(self):
        rng = random.Random(9811)
        for _ in range(180):
            center = [rng.uniform(-10, 10) for _ in range(3)]
            bounds = ([value - 1 for value in center], [value + 1 for value in center])
            point = [rng.uniform(bounds[0][axis], bounds[1][axis]) for axis in range(3)]
            lights = [(tuple(rng.uniform(-14, 14) for _ in range(3)), rng.uniform(1, 24))
                      for _ in range(128)]
            directional = rng.randrange(5)
            words, counts = cell_mask(bounds, lights, directional, 4, 4)
            candidates = query(words, counts, directional, len(lights))
            affects = lambda index: math.dist(lights[index][0], point) < lights[index][1]
            self.assertEqual([index for index in candidates if affects(index)],
                             [index for index in range(directional, len(lights)) if affects(index)])

    def test_capture_analyzer_distinguishes_complete_mask_and_legacy_world_fallback(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            cells, total = 24, 128
            ranges = bytearray(256)
            struct.pack_into("<4f", ranges, 208, 16, 16, 100, 8)
            struct.pack_into("<4I", ranges, 224, 1, 1, 24, 0)
            struct.pack_into("<4I", ranges, 240, total, 32, 32, cells)
            ranges += struct.pack("<4I", 32, 32, 1, total) * cells
            records = bytearray(total * 128)
            for index in range(total):
                struct.pack_into("<4I", records, index * 128 + 64, index + 1, 0, 1, 1)
            data = {"light_records": records, "light_ranges": ranges,
                    "light_inline": struct.pack("<4I", *([0xffffffff] * 4)) * cells,
                    "light_spill": bytes(4), "lighting_hdr": bytes(16)}
            resources = []
            for name, content in data.items():
                (directory / name).write_bytes(content)
                resources.append({"name": name, "file": name, "row_size": len(content),
                                  "allocation_bytes": len(content)})
            metadata = {"width": 16, "height": 16, "resources": resources,
                        "light_list": {"frame_slot_count": 3, "frame_slot": 0,
                                       "capacity": {"width": 16, "height": 16},
                                       "index_encoding": "bitset_u32x4_complete",
                                       "upload_allocation_bytes": 0,
                                       "allocation_bytes": sum(len(data[name]) for name in
                                           ("light_records", "light_ranges", "light_inline", "light_spill")) * 3}}
            path = directory / "light-list-4.json"
            path.write_text(json.dumps(metadata), encoding="utf-8")
            result = analyze_frame(directory, 4)
            self.assertEqual((result["overflow_cells"], result["world_query_cells"]), (cells, 0))
            # New captures reject the old truncated mask even though it could scan on overflow.
            partial = struct.pack("<4I", 0xffffffff, 0xffffffff, 0, 0) * cells
            (directory / "light_inline").write_bytes(partial)
            with self.assertRaisesRegex(ValueError, "Bitset membership"):
                analyze_frame(directory, 4)
            metadata["light_list"]["index_encoding"] = "bitset_u32x4"
            path.write_text(json.dumps(metadata), encoding="utf-8")
            result = analyze_frame(directory, 4)
            self.assertEqual((result["overflow_cells"], result["world_query_cells"]), (cells, cells))

    def test_group_light_cache_covers_all_locals_before_partial_group_return(self):
        rng = random.Random(7438)
        for total, directional in ((0, 0), (1, 0), (1, 1), (65, 1), (128, 0), (128, 3), (128, 128)):
            world = [(tuple(rng.uniform(-30, 30) for _ in range(3)), rng.uniform(1, 25))
                     for _ in range(total)]
            origin = (3., -7., 11.)
            def prepare(light):
                center, radius = light
                relative = tuple(center[axis] - origin[axis] for axis in range(3))
                # A rigid camera rotation; each original cell used the same transform.
                return ((-relative[2], relative[1], relative[0]), radius)
            for valid_cells in (1, 63, 64, 65, 129):
                for first_cell in range(0, valid_cells, 64):
                    cache = [None] * total
                    writes = []
                    # Even inactive lanes in the final group prepare data before the barrier.
                    for lane in range(64):
                        for index in range(directional + lane, total, 64):
                            cache[index] = prepare(world[index])
                            writes.append(index)
                    self.assertEqual(sorted(writes), list(range(directional, total)))
                    for cell in range(first_cell, min(first_cell + 64, valid_cells)):
                        low = (cell * .1 - 4, -5, -3)
                        high = tuple(value + 8 for value in low)
                        original = cell_mask((low, high), list(map(prepare, world)), directional, 4, 4)
                        cached = cell_mask((low, high), cache, directional, 4, 4)
                        self.assertEqual(cached, original)
        source = (SHADERS / "light_list.hlsl").read_text(encoding="utf-8")
        body = source.split("void BuildLightLists(", 1)[1]
        barrier = body.index("GroupMemoryBarrierWithGroupSync()")
        self.assertNotIn("return", body[:barrier])
        self.assertLess(barrier, body.index("if (cell_index >= Limits.w) return"))
        self.assertIn("Grid.w + lane; index < Limits.x; index += 64u", body[:barrier])
        self.assertIn("float4(center, bound * bound)", body[:barrier])
        cell_loop = body.split("uint inline_count", 1)[1]
        self.assertNotIn("HsLightRecords[", cell_loop)
        self.assertNotIn("WorldToView", cell_loop)
        self.assertIn("dot(separation, separation) > bounds.w", cell_loop)

    def test_source_uses_one_validated_record_and_complete_mask(self):
        shadow = (SHADERS / "local_shadow.hlsli").read_text(encoding="utf-8")
        lookup = shadow.split("bool HsLocalShadowLookup(", 1)[1]
        self.assertEqual(shadow.count("bool HsLocalShadowLookup("), 1)
        self.assertNotIn("index<counts.x", lookup)
        self.assertNotIn("i<counts.x", lookup)
        self.assertEqual(lookup.count("HsLocalShadowLoadRecord("), 1)
        self.assertLess(lookup.index("record_reference>counts.x"), lookup.index("HsLocalShadowLoadRecord("))
        for check in ("record.identity!=identity", "record.revision.xy!=revision", "record.state.y==0u"):
            self.assertIn(check, lookup)
        # Preserve per-tap reversed-Z static/dynamic combination before filtering.
        self.assertIn("max(static_depth,HsLocalShadowDynamicBank.Load(int3(p,0)))", shadow)
        build = (SHADERS / "light_list.hlsl").read_text(encoding="utf-8")
        self.assertNotIn("if (total_count < Limits.y + Limits.z)", build)
        self.assertIn("membership[index / 32u] |= 1u << (index % 32u)", build)
        self.assertIn("else capacity_overflow = true", build)
        light = (SHADERS / "light_list.hlsli").read_text(encoding="utf-8")
        self.assertIn("HsLightWorldQueryFlag = 2u", light)
        self.assertIn("HsLightCapacityOverflowFlag = 1u", light)
        self.assertIn("HsLocalShadowLookup(light.metadata.z >> 1u", light)
        cpu = (ROOT / "Source/RendererD3D12/Private/renderer_light_list.cpp").read_text(encoding="utf-8")
        self.assertIn("reference.record_index >= 64u", cpu)
        for key in ("shadow.stable_id == light.stable_id", "shadow.generation == light.generation",
                    "shadow.revision == light.revision", "shadow.type == light.type"):
            self.assertIn(key, cpu)


if __name__ == "__main__":
    unittest.main()
