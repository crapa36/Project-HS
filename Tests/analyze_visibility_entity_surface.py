"""Native entity-surface readback oracle; no GPU execution or image display."""
import argparse
import json
from pathlib import Path
import unittest

import numpy as np


def unpack_emission(words):
    result = []
    for shift, bits in ((0, 6), (11, 6), (22, 5)):
        value = (words >> shift) & ((1 << (bits + 5)) - 1)
        exponent, fraction = value >> bits, value & ((1 << bits) - 1)
        if np.any(exponent == 31):
            raise ValueError("non-finite packed emission")
        result.append(np.where(exponent == 0, fraction * 2.0 ** (-14 - bits),
                               (1 + fraction / (1 << bits)) * np.exp2(exponent.astype(float) - 15)))
    return np.stack(result, axis=-1)


def load_frame(folder, frame):
    path = folder / f"entity-surface-{frame}.json"
    info = json.loads(path.read_text(encoding="utf-8"))
    resources = {r["name"]: r for r in info["resources"]}

    def raw(name, dtype):
        resource = resources[name]
        data = np.fromfile(folder / resource["file"], dtype=dtype)
        channels = resource["row_size"] // (resource["width"] * np.dtype(dtype).itemsize)
        if data.size != resource["height"] * resource["width"] * channels:
            raise ValueError(f"invalid raw extent: {name}")
        return data.reshape(resource["height"], resource["width"], channels)

    word = info["temporal_inputs"]["emission_word"]
    if not 0 <= word < 8:
        raise ValueError("capture has no emission sidecar")
    plane = raw(f"material_sidecar_{word // 4}", "<u4")
    emission = unpack_emission(plane[:, :, word % 4])
    depth = raw("depth", "<f4")[:, :, 0]
    history = raw("surface_history", "<f4")[:, :, 1]
    if not np.isfinite(depth).all() or not np.isfinite(history).all() or np.any((history < 0) | (history > 1)):
        raise ValueError("invalid depth/history capture")
    if np.any(np.asarray(info["temporal_inputs"]["jitter_uv"]) != 0):
        raise ValueError("entity-surface fixture must disable jitter")
    return {"depth": depth, "emission": emission, "history": history,
            "keys": raw("gbuffer_metadata", "<u4")}


def interior(mask):
    result = mask.copy()
    for axis in (0, 1):
        for shift in (-1, 1):
            result &= np.roll(mask, shift, axis=axis)
    result[[0, -1], :] = False
    result[:, [0, -1]] = False
    return result


def instance_key(folder, frame):
    info = json.loads((folder / f"entity-surface-geometry-{frame}.json").read_text(encoding="utf-8"))
    keys = {(ref["InstanceIndex"], ref["InstanceGeneration"])
            for record in info["records"] if record["mesh"] == 13 and record["refsource"] in (2, 5)
            for ref in record["visible_refs"]}
    if len(keys) != 1:
        raise ValueError(f"expected one actual boss slot at frame {frame}: {keys}")
    return next(iter(keys))


def analyze(compact, visibility):
    captures, report = {}, {}
    generation = None
    for name, folder in (("compact", compact), ("visibility", visibility)):
        receipt = json.loads((folder / "result.json").read_text(encoding="utf-8"))
        if (not receipt["execution_valid"] or receipt["surface_frontend"] != name or
                receipt["fixture"] != "entity-surface-identity-v1" or receipt["frames"] != 8 or
                receipt["validation_errors"] != 0):
            raise ValueError(f"invalid native receipt: {folder}")
        if generation is not None and receipt["generation_id"] != generation:
            raise ValueError("frontend runs use different cooked generations")
        generation = receipt["generation_id"]
        frames = {i: load_frame(folder, i) for i in range(1, 9)}
        captures[name] = frames
        mask = interior(frames[5]["depth"] > 0)
        if mask.sum() < 64:
            raise ValueError("fixture did not render enough actual boss interior pixels")
        if np.any(frames[6]["depth"] != 0):
            raise ValueError("removed boss still covers the empty frame")
        for i in (1, 4, 7, 8):
            np.testing.assert_array_equal(frames[i]["depth"], frames[5]["depth"])
            np.testing.assert_array_equal(frames[i]["emission"][mask], frames[5]["emission"][mask])
        # Frame 8 submits the retired owner's command against the reused slot.
        old, new = instance_key(folder, 5), instance_key(folder, 7)
        if old[0] != new[0] or new[1] <= old[1]:
            raise ValueError(f"fixture did not exercise generation-changing slot reuse: {old}, {new}")
        effects = []
        for i in (2, 3):
            np.testing.assert_array_equal(frames[i]["keys"], frames[5]["keys"])
            delta = frames[i]["emission"][mask] - frames[5]["emission"][mask]
            if np.any(delta < 0):
                raise ValueError(f"surface effect {i} reduced the baseline emission")
            if i == 3 and np.any((delta > 0).sum(axis=0) < 64):
                raise ValueError("actual midpoint did not add finite emission in all RGB channels")
            historical = mask & (frames[5]["history"] > .5)
            attenuated = historical & (frames[i]["history"] < frames[5]["history"] - .01)
            if i == 3 and attenuated.sum() < 64:
                raise ValueError("actual midpoint did not attenuate valid history through its alpha")
            effects.append({"frame": i, "emission_delta_mean_rgb": delta.mean(axis=0).tolist(),
                            "alpha_attenuated_pixels": int(attenuated.sum())})
        report[name] = {"boss_interior_pixels": int(mask.sum()), "old_key": old, "reused_key": new,
                        "effects": effects, "retired_owner_rejected": True}
    parity = []
    for i in (1, 2, 3, 4, 5, 7, 8):
        a, b = captures["compact"][i], captures["visibility"][i]
        mask = interior((a["depth"] > 0) & (b["depth"] > 0))
        if mask.sum() < 64:
            raise ValueError("frontend parity has insufficient common boss pixels")
        np.testing.assert_array_equal(a["keys"][mask], b["keys"][mask])
        np.testing.assert_allclose(a["depth"][mask], b["depth"][mask], rtol=0, atol=2e-5)
        # Packed emission uses 6/6/5 fraction bits; explicit gradients may differ slightly.
        np.testing.assert_allclose(a["emission"][mask], b["emission"][mask], rtol=.04, atol=.2)
        np.testing.assert_allclose(a["history"][mask], b["history"][mask], rtol=0, atol=.02)
        parity.append({"frame": i, "common_interior_pixels": int(mask.sum()),
                       "emission_max_abs_error_rgb": np.abs(a["emission"][mask] - b["emission"][mask]).max(axis=0).tolist()})
    return {"passed": True, "runs": report, "frontend_parity": parity,
            "scope": "actual GPU emission/history and slot reuse; stale generation guard is source-only"}


class DecoderTests(unittest.TestCase):
    def test_unsigned_float_zero_normal_subnormal_and_nonfinite(self):
        words = np.array([0, (15 << 6) | ((15 << 6) << 11) | ((15 << 5) << 22), 1], dtype=np.uint32)
        decoded = unpack_emission(words)
        np.testing.assert_array_equal(decoded[0], [0, 0, 0])
        np.testing.assert_array_equal(decoded[1], [1, 1, 1])
        self.assertEqual(decoded[2, 0], 2.0 ** -20)
        with self.assertRaises(ValueError):
            unpack_emission(np.array([31 << 6], dtype=np.uint32))

    def test_interior_removes_edge_and_does_not_wrap(self):
        mask = np.ones((5, 5), dtype=bool)
        self.assertEqual(int(interior(mask).sum()), 9)
        mask[2, 2] = False
        self.assertEqual(int(interior(mask).sum()), 4)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--compact", type=Path)
    parser.add_argument("--visibility", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.self_test:
        unittest.main(argv=[__file__])
    else:
        if not args.compact or not args.visibility:
            parser.error("--compact and --visibility are required")
        result = json.dumps(analyze(args.compact, args.visibility), indent=2)
        if args.output:
            args.output.write_text(result + "\n", encoding="utf-8")
        else:
            print(result)
