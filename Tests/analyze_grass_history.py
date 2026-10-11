"""Numerical native readback analysis; tap acceptance is CPU-derived, not GPU telemetry."""
import argparse
import json
from pathlib import Path
import unittest

import numpy as np


def decode_normal(octant):
    normal = np.column_stack((octant, 1 - np.abs(octant).sum(axis=1)))
    folded = normal[:, 2] < 0
    normal[folded, :2] = (1 - np.abs(octant[folded, ::-1])) * np.where(octant[folded] < 0, -1, 1)
    return normal / np.maximum(np.linalg.norm(normal, axis=1, keepdims=True), 1e-12)


def catmull(f):
    return np.column_stack((-.5*f + f*f - .5*f*f*f, 1-2.5*f*f+1.5*f*f*f,
                            .5*f+2*f*f-1.5*f*f*f, -.5*f*f+.5*f*f*f))


def analyze(path):
    metadata = json.loads(path.read_text(encoding="utf-8"))
    resources = {r["name"]: r for r in metadata["resources"]}
    def load(name, dtype, channels):
        r = resources[name]
        return np.fromfile(path.parent / r["file"], dtype=dtype).reshape(r["height"], -1, channels)
    keys = load("gbuffer_metadata", "<u4", 2)
    history = load("surface_history", "<f4", 2)
    motion = load("gbuffer_motion", "<f2", 2).astype(float)
    depth = load("depth", "<f4", 1)[:, :, 0]
    h, w = depth.shape
    inputs = metadata["temporal_inputs"]
    jitter = np.array(inputs["jitter_uv"][:2])
    ry, rx = np.nonzero((keys[:, :, 1] & (3 << 8)) == (2 << 8))
    if not len(rx):
        raise ValueError(f"no grass pixels in {path}")
    # Map output pixel centers to the same jittered nearest surface used by TemporalPS.
    ox = np.clip(np.rint(rx - jitter[0] * w).astype(int), 0, w - 1)
    oy = np.clip(np.rint(ry - jitter[1] * h).astype(int), 0, h - 1)
    uv = np.column_stack(((ox + .5) / w, (oy + .5) / h))
    conf = history[ry, rx, 1]
    active = conf > 0
    pixel_motion = motion[ry, rx] * [w, h]
    report = {"frame": inputs["frame"], "grass_pixels": len(rx),
              "history_fraction": float(active.mean()),
              "motion_pixels_mean": float(np.linalg.norm(pixel_motion, axis=1).mean()),
              "motion_pixels_max": float(np.linalg.norm(pixel_motion, axis=1).max()),
              "previous_grass_time": inputs["previous_grass_state"][0],
              "current_grass_time": inputs["camera_time"][3]}
    # Camera-only exact correspondence oracle; wind/contact cases also contain deformation.
    clip = np.column_stack(((rx + .5) / w * 2 - 1 - jitter[0]*2,
                            1 - (ry + .5) / h * 2 + jitter[1]*2,
                            depth[ry, rx], np.ones(len(rx))))
    current_vp = np.array(inputs["current_vp"]).reshape(4, 4)
    previous_vp = np.array(inputs["previous_vp"]).reshape(4, 4)
    previous_clip = clip @ np.linalg.inv(current_vp) @ previous_vp
    previous_uv = previous_clip[:, :2] / previous_clip[:, 3:4] * [.5, -.5] + .5
    current_uv = np.column_stack(((rx + .5) / w, (ry + .5) / h)) - jitter
    camera_error = ((previous_uv - current_uv) - motion[ry, rx]) * [w, h]
    report["camera_only_motion_error_p95_pixels"] = float(np.percentile(np.linalg.norm(camera_error[active], axis=1), 95)) if active.any() else None
    previous = load("temporal_previous", "<f2", 4).astype(float)
    previous_keys = load("temporal_material_previous", "<u4", 2)
    previous_normals = load("temporal_normal_previous", "<i2", 2).astype(float) / 32767
    normal = decode_normal(load("gbuffer_normal", "<i2", 2)[ry, rx].astype(float) / 32767)
    position = (uv + motion[ry, rx]) * [w, h] - .5
    base = np.floor(position).astype(int)
    fraction = position - base
    valid_uv = ((position >= 0) & (position <= [w-1, h-1])).all(axis=1)
    weights = np.zeros(len(rx))
    radiance = np.zeros((len(rx), 3))
    rejected = dict(depth=0, material=0, normal=0)
    for y in range(2):
        for x in range(2):
            sx = np.clip(base[:, 0]+x, 0, w-1)
            sy = np.clip(base[:, 1]+y, 0, h-1)
            weight = (fraction[:, 0] if x else 1-fraction[:, 0]) * (fraction[:, 1] if y else 1-fraction[:, 1])
            sample = previous[sy, sx]
            valid = active & valid_uv & (weight > 0) & np.isfinite(sample).all(axis=1)
            checks = {"depth": (sample[:, 3] > 0) & (np.abs(sample[:, 3]-history[ry, rx, 0]) <= .002),
                      "material": (previous_keys[sy, sx] == keys[ry, rx]).all(axis=1),
                      "normal": (decode_normal(previous_normals[sy, sx]) * normal).sum(axis=1) >= .85}
            for reason, check in checks.items():
                rejected[reason] += int((valid & ~check).sum())
                valid &= check
            weights += weight * valid
            radiance += sample[:, :3] * (weight * valid)[:, None]
    if inputs["options"][1] < .5:
        weights[:] = 0
    report["derived_accepted_footprint_mean"] = float(weights.mean())
    report["derived_rejected_taps"] = rejected
    scene = load("scene_work", "<f2", 4).astype(float)[:, :, :3]
    scene_position = (uv + jitter) * [w, h] - .5
    sb = np.floor(scene_position).astype(int)
    sf = scene_position - sb
    wx, wy = catmull(sf[:, 0]), catmull(sf[:, 1])
    current = np.zeros((len(rx), 3))
    low = np.full_like(current, np.inf)
    high = np.full_like(current, -np.inf)
    for y in range(4):
        for x in range(4):
            sample = scene[np.clip(sb[:, 1]+y-1, 0, h-1), np.clip(sb[:, 0]+x-1, 0, w-1)]
            current += sample * (wx[:, x] * wy[:, y])[:, None]
            low, high = np.minimum(low, sample), np.maximum(high, sample)
    current = np.maximum(np.clip(current, low, high), 0)
    low, high = current.copy(), current.copy()
    for y in range(-1, 2):
        for x in range(-1, 2):
            sample = scene[np.clip(ry+y, 0, h-1), np.clip(rx+x, 0, w-1)]
            low, high = np.minimum(low, sample), np.maximum(high, sample)
    exposure = load("exposure_previous", "<f4", 1).ravel()
    radiance /= np.maximum(weights[:, None], 1e-12)
    radiance *= exposure[0] / max(float(exposure[1]), 1e-12)
    radiance = np.clip(radiance, low, high)
    change = np.clip(np.linalg.norm(current-radiance, axis=1) / np.maximum(1, np.linalg.norm(current, axis=1)), 0, 1)
    weight = .88 * np.clip(conf, 0, 1) * weights * (1-change)
    # This fixture has no transparent VFX; a production reactive guide needs its own readback.
    expected = current + (radiance-current) * weight[:, None]
    actual = load("temporal_current", "<f2", 4)[oy, ox, :3].astype(float)
    report["derived_history_weight_mean"] = float(weight.mean())
    report["gpu_resolve_oracle_error_p99"] = float(np.percentile(np.abs(expected-actual), 99))
    report["temporal_rgb_mean"] = actual.mean(axis=0).tolist()
    report["temporal_rgb_std"] = actual.std(axis=0).tolist()
    # A/C have no deformation: this correspondence is independent of the motion under test.
    camera_position = (uv + previous_uv-current_uv) * [w, h] - .5
    cb = np.floor(camera_position).astype(int)
    cf = camera_position-cb
    camera_previous = np.zeros_like(actual)
    camera_weights = np.zeros(len(rx))
    expected_depth = previous_clip[:, 2] / previous_clip[:, 3]
    for y in range(2):
        for x in range(2):
            sx, sy = np.clip(cb[:, 0]+x, 0, w-1), np.clip(cb[:, 1]+y, 0, h-1)
            sample = previous[sy, sx]
            valid = (sample[:, 3] > 0) & (np.abs(sample[:, 3]-expected_depth) < .002)
            valid &= (previous_keys[sy, sx] == keys[ry, rx]).all(axis=1)
            valid &= (decode_normal(previous_normals[sy, sx])*normal).sum(axis=1) >= .85
            tap = (cf[:, 0] if x else 1-cf[:, 0]) * (cf[:, 1] if y else 1-cf[:, 1]) * valid
            camera_previous += sample[:, :3] * tap[:, None]
            camera_weights += tap
    interior = camera_weights > .99
    delta = np.abs(actual / max(float(exposure[0]), 1e-12) -
                   camera_previous / np.maximum(camera_weights[:, None], 1e-12) / max(float(exposure[1]), 1e-12))
    report["camera_only_correspondence_pixels"] = int(interior.sum())
    report["camera_only_reprojected_rgb_delta_mean"] = float(delta[interior].mean()) if interior.any() else None
    return report


class ArithmeticTests(unittest.TestCase):
    def test_catmull_partition_and_center(self):
        self.assertTrue(np.allclose(catmull(np.linspace(0, 1, 21)).sum(axis=1), 1))
        self.assertTrue(np.allclose(catmull(np.array([0]))[0], [0, 1, 0, 0]))

    def test_folded_normals(self):
        n = decode_normal(np.array([[0., 0.], [1., 1.], [.25, -.3]]))
        self.assertTrue(np.allclose(n[0], [0, 0, 1]))
        self.assertTrue(np.allclose(n[1], [0, 0, -1]))
        self.assertTrue(np.allclose(np.linalg.norm(n, axis=1), 1))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, nargs="?")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        unittest.main(argv=[__file__])
    else:
        result = {"measurement": "native raw readback; acceptance/weight are CPU-derived",
                  "frames": [analyze(p) for p in sorted(args.directory.glob("grass-[0-9][0-9][0-9].json"))]}
        if not result["frames"]:
            raise ValueError("no continuous grass captures")
        (args.directory / "grass-analysis.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
        print(json.dumps(result, indent=2))
