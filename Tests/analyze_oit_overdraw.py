"""Analyze OIT/camera raw captures; optionally run the existing native fixture."""
import argparse
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile


def read_surface(metadata, name, channels):
    document = json.loads(metadata.read_text(encoding="utf-8"))
    resource = next(row for row in document["resources"] if row["name"] == name)
    formats = {"DXGI_FORMAT_R16G16B16A16_FLOAT": (4, "e", 2),
               "DXGI_FORMAT_R32G32B32A32_FLOAT": (4, "f", 4), "DXGI_FORMAT_R16_FLOAT": (1, "e", 2)}
    layout = formats.get(resource["format"])
    if not layout or layout[0] != channels:
        raise ValueError(f"Unexpected {name} format")
    width, height, stride = (resource[key] for key in ("width", "height", "row_size"))
    if min(width, height) <= 0 or stride != width * channels * layout[2]:
        raise ValueError("Expected packed positive texture extent")
    path = metadata.parent / resource["file"]
    if path.resolve().parent != metadata.parent.resolve():
        raise ValueError("Surface raw path escapes its artifact directory")
    raw = path.read_bytes()
    if len(raw) != stride * height:
        raise ValueError("Surface raw size disagrees with metadata")
    return list(struct.iter_unpack("<" + layout[1] * channels, raw))


def statistics(pixels):
    flat = [value for pixel in pixels for value in pixel]
    finite = [value for value in flat if math.isfinite(value)]
    return {"nonfinite": len(flat) - len(finite),
            "finite_max": max(finite, default=0),
            "half_max_components": sum(abs(value) == 65504 for value in finite)}


def compare_components(baseline, actual, layers, hdr):
    if len(baseline) != len(actual):
        raise ValueError("Linearity inputs changed texture extent")
    maximum_relative_error = [0.0] * 4
    mismatched = compared = 0
    for reference, observed in zip(baseline, actual):
        if reference[3] <= 0:
            continue
        compared += 1
        for component in range(4):
            expected = reference[component] * layers * (hdr if component < 3 else 1)
            value = observed[component]
            if not math.isfinite(value):
                mismatched += 1
                continue
            relative_error = abs(value - expected) / max(abs(expected), 1e-6)
            maximum_relative_error[component] = max(maximum_relative_error[component], relative_error)
            # .2% accommodates capture/shader rounding, but rejects22-layer half
            # saturation (66000 ->65504). The same-camera fixture has no fog.
            if abs(value - expected) > 1e-6 + .002 * abs(expected):
                mismatched += 1
    return {"compared_pixels": compared, "relative_tolerance": .002, "absolute_tolerance": 1e-6,
            "max_relative_error": maximum_relative_error, "mismatched_components": mismatched}


def analyze(directory):
    cases = [(1, 1), (22, 1), (11, 2), (64, 5.5)]
    baseline = read_surface(directory / "oit-overdraw-1.json", "oit_accumulation", 4)
    core = [index for index, pixel in enumerate(baseline) if pixel[3] > 0]
    if not core or not any(any(value > 0 for value in baseline[index][:3]) for index in core):
        raise ValueError("The one-layer baseline has no actual positive RGB/alpha output")
    if statistics(baseline)["nonfinite"]:
        raise ValueError("The one-layer baseline is already nonfinite")
    rows = []
    for frame, (layers, hdr) in enumerate(cases, 1):
        metadata = directory / f"oit-overdraw-{frame}.json"
        accumulation = read_surface(metadata, "oit_accumulation", 4)
        revealage = read_surface(metadata, "oit_revealage", 1)
        composite = read_surface(metadata, "composite_hdr", 4)
        document = json.loads(metadata.read_text(encoding="utf-8"))
        accumulation_format = next(row["format"] for row in document["resources"]
                                   if row["name"] == "oit_accumulation")
        if len(accumulation) != len(baseline) or len(revealage) != len(baseline):
            raise ValueError("OIT cases changed texture extent")
        if not any(pixel[3] > 0 for pixel in accumulation):
            raise ValueError(f"Frame {frame} has empty accumulation")
        if not any(pixel[0] < 1 for pixel in revealage):
            raise ValueError(f"Frame {frame} has no transparent coverage")
        predicted_max = [max(baseline[i][c] * layers * (hdr if c < 3 else 1)
                             for i in core) for c in range(4)]
        rows.append({"frame": frame, "layers": layers, "input_hdr": hdr,
                     "accumulation_format": accumulation_format,
                     "predicted_unbounded_component_max": predicted_max,
                     "observed_component_max": [max((pixel[c] for pixel in accumulation
                         if math.isfinite(pixel[c])), default=0) for c in range(4)],
                     "linearity": compare_components(baseline, accumulation, layers, hdr),
                     "accumulation": statistics(accumulation), "revealage": statistics(revealage),
                     "composite": statistics(composite)})
    failed = any(row["accumulation"]["nonfinite"] or row["revealage"]["nonfinite"] or row["composite"]["nonfinite"] or
                 (row["accumulation_format"] == "DXGI_FORMAT_R16G16B16A16_FLOAT" and
                  row["accumulation"]["half_max_components"]) for row in rows)
    beyond_half = all(rows[i]["observed_component_max"][3] > 65504 for i in (1, 3))
    fp32 = all(row["accumulation_format"] == "DXGI_FORMAT_R32G32B32A32_FLOAT" for row in rows)
    proportional = all(not row["linearity"]["mismatched_components"] for row in rows)
    # A rescaled writer must not pass this regression accidentally: policy keeps
    # the existing3000 maximum weight and verifies actual 66000/192000 alpha.
    weight_preserved = abs(rows[0]["observed_component_max"][3] - 3000) < 1
    return {"scope": "Actual saved GPU surfaces; arithmetic prediction is separate from observed output",
            "baseline_nonempty_pixels": len(core), "range_failure_observed": failed,
            "alpha_22_and_64_exceed_half_max": beyond_half,
            "precision_regression_passed": fp32 and not failed and proportional and beyond_half and weight_preserved,
            "cases": rows}


def validate_camera_bounds(rows):
    for row in rows:
        bounds = row["alpha_bounds"]
        width, height = row["texture_extent"]
        if row["foreground_depth_pixels"] != 0:
            raise ValueError(f"Camera frame {row['frame']} contains opaque fixture geometry")
        if bounds[0] <= 0 or bounds[1] <= 0 or bounds[2] >= width - 1 or bounds[3] >= height - 1:
            raise ValueError(f"Camera frame {row['frame']} clips the measured silhouette")
    # The asymmetric radial sprite stays horizontal in camera space, including
    # roll and a vertical view. Lighting of the six-way smoke may legitimately change.
    reference = rows[0]["alpha_bounds_extent"]
    for row in rows[:3]:
        extent = row["alpha_bounds_extent"]
        if extent[0] < 1.8 * extent[1] or any(abs(a - b) > max(4, .03 * b) for a, b in zip(extent, reference)):
            raise ValueError(f"Camera frame {row['frame']} changed camera-facing sprite orientation/extent")


def analyze_camera(directory):
    rows = []
    for frame in range(1, 7):
        metadata = directory / f"camera-basis-{frame}.json"
        document = json.loads(metadata.read_text(encoding="utf-8"))
        resource = next(row for row in document["resources"] if row["name"] == "oit_accumulation")
        width = resource["width"]
        accumulation = read_surface(metadata, "oit_accumulation", 4)
        revealage = read_surface(metadata, "oit_revealage", 1)
        stats = statistics(accumulation)
        if stats["nonfinite"] or statistics(revealage)["nonfinite"]:
            raise ValueError(f"Camera frame {frame} contains nonfinite output")
        covered = [(i % width, i // width) for i, pixel in enumerate(accumulation) if pixel[3] > .01]
        if not covered or not any(max(pixel[:3]) > 0 for pixel in accumulation):
            raise ValueError(f"Camera frame {frame} has empty actual RGB/alpha output")
        if not any(pixel[0] < 1 for pixel in revealage):
            raise ValueError(f"Camera frame {frame} has no transparent coverage")
        xs, ys = zip(*covered)
        extent = [max(xs) - min(xs) + 1, max(ys) - min(ys) + 1]
        rows.append({"frame": frame, "writer": "RadialEnergy" if frame <= 3 else "Smoke6Way",
                     "camera": ("front", "roll90", "vertical_down")[(frame - 1) % 3],
                     "covered_pixels": len(covered), "alpha_bounds_extent": extent,
                     "alpha_bounds": [min(xs), min(ys), max(xs), max(ys)],
                     "texture_extent": [width, resource["height"]],
                     "foreground_depth_pixels": document["foreground_depth_pixels"], "accumulation": stats})
    validate_camera_bounds(rows)
    return {"scope": "Actual camera-facing RadialEnergy and six-way smoke GPU output",
            "basis_coverage": ["front", "roll90", "vertical_down"], "cases": rows}


def selfcheck():
    assert statistics([(0, 65504, float("inf"), float("nan"))]) == {
        "nonfinite": 2, "finite_max": 65504, "half_max_components": 1}
    assert struct.unpack("<e", struct.pack("<e", 3000))[0] == 3000
    assert 22 * 3000 > 65504 and 11 * 2 * 3000 > 65504
    reference = [(2146, 293.25, 395.75, 3000)]
    assert compare_components(reference, [(47212, 6451.5, 8706.5, 66000)], 22, 1)["mismatched_components"] == 0
    assert compare_components(reference, [(47212, 6451.5, 8706.5, 65504)], 22, 1)["mismatched_components"] == 1
    assert compare_components(reference, [(755392, 103224, 139304, 192000)], 64, 5.5)["mismatched_components"] == 0
    assert struct.unpack("<f", struct.pack("<f", 192000))[0] == 192000
    camera_rows = [dict(frame=frame, alpha_bounds=[100, 100, 399, 199],
                        alpha_bounds_extent=[300, 100], texture_extent=[640, 360], foreground_depth_pixels=0)
                   for frame in range(1, 7)]
    validate_camera_bounds(camera_rows)
    for invalid in ({"alpha_bounds_extent": [100, 300]}, {"alpha_bounds_extent": [280, 100]},
                    {"alpha_bounds": [0, 100, 299, 199]}, {"foreground_depth_pixels": 1}):
        bad_camera_rows = [dict(row) for row in camera_rows]
        bad_camera_rows[1].update(invalid)
        try:
            validate_camera_bounds(bad_camera_rows)
        except ValueError:
            pass
        else:
            raise AssertionError("Camera oracle accepted a rotated/rescaled/clipped/occluded silhouette")
    # Synthetic CPU-only payloads exercise float32 decoding and the oracle;
    # these are not saved GPU results or renderer validation evidence.
    with tempfile.TemporaryDirectory(prefix="hs-oit-analyzer-", dir=Path.cwd()) as temporary:
        directory = Path(temporary)
        for frame, (layers, hdr) in enumerate(((1, 1), (22, 1), (11, 2), (64, 5.5)), 1):
            resources = []
            for name, fmt, code, values in (
                ("oit_accumulation", "DXGI_FORMAT_R32G32B32A32_FLOAT", "ffff",
                 tuple(reference[0][c] * layers * (hdr if c < 3 else 1) for c in range(4))),
                ("oit_revealage", "DXGI_FORMAT_R16_FLOAT", "e", (0,)),
                ("composite_hdr", "DXGI_FORMAT_R16G16B16A16_FLOAT", "eeee", (.7, .1, .2, 1))):
                filename = f"{frame}-{name}.raw"
                raw = struct.pack("<" + code, *values)
                (directory / filename).write_bytes(raw)
                resources.append(dict(name=name, format=fmt, file=filename, width=1, height=1, row_size=len(raw)))
            (directory / f"oit-overdraw-{frame}.json").write_text(json.dumps(dict(resources=resources)), encoding="utf-8")
        assert analyze(directory)["precision_regression_passed"]
        (directory / "2-oit_accumulation.raw").write_bytes(struct.pack("<ffff", 47212, 6451.5, 8706.5, 65504))
        assert not analyze(directory)["precision_regression_passed"]


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, nargs="?")
    parser.add_argument("--require-finite", action="store_true",
                        help="Require finite FP32 output, preserved weights and unsaturated layer/HDR proportionality")
    parser.add_argument("--camera-basis", action="store_true")
    parser.add_argument("--runner", type=Path)
    parser.add_argument("--generation", type=Path)
    args = parser.parse_args()
    selfcheck()
    if bool(args.runner) != bool(args.generation) or (args.runner and not args.directory):
        parser.error("--runner and --generation require each other and an artifact directory")
    if args.runner:
        if not args.runner.is_absolute() or not args.generation.is_absolute():
            parser.error("--runner and --generation require absolute paths")
        args.directory = args.directory.resolve()
        args.directory.mkdir(parents=True, exist_ok=True)
        native_log = args.directory / "native-run.log"
        with native_log.open("w", encoding="utf-8") as log:
            try:
                subprocess.run([str(args.runner), f"--generation={args.generation}", f"--artifacts={args.directory}",
                    "--resolution=640x360", "--validation", "--barriers=enhanced",
                    "--camera-basis-probe" if args.camera_basis else "--oit-overdraw-probe"],
                    stdout=log, stderr=subprocess.STDOUT, check=True)
            except subprocess.CalledProcessError as error:
                raise SystemExit(f"Native fixture failed ({error.returncode}); see {native_log}") from error
    if args.directory:
        result = analyze_camera(args.directory) if args.camera_basis else analyze(args.directory)
        print(json.dumps(result, indent=2, allow_nan=False))
        if args.require_finite and not args.camera_basis and not result["precision_regression_passed"]:
            raise SystemExit(1)
    else:
        print("OIT analyzer selfcheck PASS")
