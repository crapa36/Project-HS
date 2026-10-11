"""Validate --light-list-probe GPU buffers and allocation sizes, optionally running the fixture."""
import argparse
from collections import Counter
from contextlib import ExitStack
import hashlib
import json
import math
import mmap
from pathlib import Path
import struct
import subprocess


LOCAL_COUNTS = (4, 64, 65, 128, 127, 0)
DIRECTIONAL_COUNTS = (0, 0, 0, 0, 1, 1)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def analyze_frame(directory, frame):
    metadata = json.loads((directory / f"light-list-{frame}.json").read_text(encoding="utf-8"))
    resources = {item["name"]: item for item in metadata["resources"]}
    status = metadata["light_list"]
    names = ("light_records", "light_ranges", "light_inline", "light_spill")
    require(status["frame_slot_count"] == 3 and 0 <= status["frame_slot"] < 3, "Invalid frame slot")
    per_slot = sum(resources[name]["allocation_bytes"] for name in names)
    require(status["allocation_bytes"] == per_slot * 3 + status["upload_allocation_bytes"],
            "Allocation summary disagrees with actual resource allocations")
    for name in names:
        require(resources[name]["allocation_bytes"] >= resources[name]["row_size"],
                f"{name} allocation is smaller than the resource")

    with ExitStack() as stack:
        buffers = {}
        for name in names:
            source = stack.enter_context((directory / resources[name]["file"]).open("rb"))
            buffers[name] = stack.enter_context(mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ))
            require(len(buffers[name]) == resources[name]["row_size"], f"{name} raw byte count changed")
        ranges = buffers["light_ranges"]
        require(len(ranges) >= 256, "Light constants are missing")
        require(all(math.isfinite(value) for value in struct.unpack_from("<56f", ranges)),
                "Light camera constants contain nonfinite values")
        width, height, far, logarithm = struct.unpack_from("<4f", ranges, 208)
        grid_x, grid_y, slices, directional = struct.unpack_from("<4I", ranges, 224)
        total, inline_capacity, spill_capacity, cells = struct.unpack_from("<4I", ranges, 240)
        local = total - directional
        require(local == LOCAL_COUNTS[frame - 1] and directional == DIRECTIONAL_COUNTS[frame - 1],
                "Native fixture did not submit the required light counts")
        require((grid_x, grid_y, slices) == (math.ceil(width / 16), math.ceil(height / 16), 24)
                and cells == grid_x * grid_y * slices, "Active tile addressing changed")
        require((width, height) == (metadata["width"], metadata["height"])
                and far > 0 and logarithm > 0, "Active extent or depth interval changed")
        require((inline_capacity, spill_capacity) == (32, 32), "Fixture is not using Ultra light capacity")
        capacity = status["capacity"]
        capacity_cells = math.ceil(capacity["width"] / 16) * math.ceil(capacity["height"] / 16) * 24
        require(cells <= capacity_cells and len(ranges) == 256 + capacity_cells * 16,
                "Range allocation does not match capacity")

        records = buffers["light_records"]
        identities = [struct.unpack_from("<4I", records, index * 128 + 64) for index in range(total)]
        ids = [low | high << 32 for low, high, generation, kind in identities]
        require(all(generation and kind == 0 for low, high, generation, kind in identities[:directional])
                and all(generation and 1 <= kind <= 4 for low, high, generation, kind in identities[directional:]),
                "Directional/local partition changed")
        require(ids[:directional] == sorted(ids[:directional])
                and ids[directional:] == sorted(ids[directional:]) and len(set(ids)) == total,
                "Stable record order or identity uniqueness changed")

        inline_count = min(local, inline_capacity)
        spill_count = min(local - inline_count, spill_capacity)
        overflow = local > inline_capacity + spill_capacity
        expected = (inline_count, spill_count, int(overflow), local)
        occupancy = Counter(struct.iter_unpack("<4I", ranges[256:256 + cells * 16]))
        require(occupancy == Counter({expected: cells}),
                f"Unexpected occupancy/overflow or stale reused-slot ranges: {dict(occupancy)}")
        indices = list(range(directional, total))
        encoding = status["index_encoding"]
        if encoding == "inline_spill_u32":
            for name, stride, expected_indices in (
                    ("light_inline", inline_capacity, indices[:inline_count]),
                    ("light_spill", spill_capacity, indices[inline_count:inline_count + spill_count])):
                buffer = buffers[name]
                require(len(buffer) == max(4, capacity_cells * stride * 4), "Index capacity changed")
                if not expected_indices:
                    continue  # Unused array tails are not initialized or consumed.
                packed = struct.pack(f"<{len(expected_indices)}I", *expected_indices)
                require(all(buffer[cell * stride * 4:cell * stride * 4 + len(packed)] == packed
                            for cell in range(cells)), f"{name} lost or reordered a candidate")
        elif encoding == "bitset_u32x4":
            words = [0] * 4
            for index in indices[:inline_count + spill_count]:
                words[index // 32] |= 1 << (index % 32)
            require(len(buffers["light_inline"]) == capacity_cells * 16 and len(buffers["light_spill"]) == 4,
                    "Bitset allocation changed")
            require(buffers["light_inline"][:cells * 16] == struct.pack("<4I", *words) * cells,
                    "Bitset membership/order or zero-after-overflow clearing changed")
        else:
            raise ValueError(f"Unknown index encoding: {encoding}")
        # Overflow uses the complete authoritative table, never its partial stored set.
        selected = indices if overflow else indices[:inline_count + spill_count]
        require(selected == indices, "The shading query would discard or repeat a local light")
        result = {"frame": frame, "frame_slot": status["frame_slot"], "local_lights": local,
                  "directional_lights": directional, "active_extent": [int(width), int(height)],
                  "capacity": capacity, "active_cells": cells, "occupied_cells": cells if local else 0,
                  "overflow_cells": cells if overflow else 0, "occupancy": list(expected),
                  "index_encoding": encoding, "allocation_bytes": status["allocation_bytes"],
                  "upload_allocation_bytes": status["upload_allocation_bytes"],
                  "resource_allocation_bytes": {name: resources[name]["allocation_bytes"] for name in names},
                  "ordered_local_ids": ids[directional:], "consumer_order_mismatches": 0,
                  "auxiliary_targets": metadata.get("auxiliary_targets", [])}
    result["lighting_hdr_sha256"] = digest(directory / resources["lighting_hdr"]["file"])
    return result


def analyze(directory):
    receipt = json.loads((directory / "result.json").read_text(encoding="utf-8"))
    require(receipt["execution_valid"] and receipt["fixture"] == "light-list-capacity-v1"
            and receipt["validation_errors"] == 0, "Native execution or validation failed")
    require(len(receipt["light_list"]["frames"]) == 6, "Fixture did not capture all six frames")
    frames = [analyze_frame(directory, frame) for frame in range(1, 7)]
    require(len({frame["allocation_bytes"] for frame in frames}) == 1,
            "Allocation changed with light count in a fixed-capacity fixture")
    timeline = receipt.get("diagnostics", {}).get("timeline", {})
    return {"scope": "Actual saved GPU buffers and allocation sizes; not resident heap usage or FPS",
            "generation_id": receipt["generation_id"], "assertions_passed": True, "frames": frames,
            "last_completed_cluster_timings": [item for item in timeline.get("passes", [])
                if item["name"] == "LightListClusters" and item["measured"]]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--runner", type=Path)
    parser.add_argument("--generation", type=Path)
    args = parser.parse_args()
    if args.runner:
        if not args.generation:
            parser.error("--runner requires --generation")
        args.directory.mkdir(parents=True, exist_ok=True)
        with (args.directory / "native.log").open("w", encoding="utf-8") as log:
            subprocess.run([str(args.runner.resolve()), f"--generation={args.generation.resolve()}",
                f"--artifacts={args.directory.resolve()}", "--resolution=1920x1080", "--validation",
                "--barriers=enhanced", "--light-list-probe"], check=True, stdout=log,
                stderr=subprocess.STDOUT, timeout=120)
    result = analyze(args.directory)
    if args.baseline:
        baseline = analyze(args.baseline)
        for before, after in zip(baseline["frames"], result["frames"]):
            for key in ("local_lights", "directional_lights", "active_extent", "capacity", "active_cells",
                        "occupied_cells", "overflow_cells", "occupancy", "ordered_local_ids", "lighting_hdr_sha256"):
                require(before[key] == after[key], f"Baseline equivalence failed: frame{after['frame']} {key}")
        result["comparison"] = {"baseline": str(args.baseline), "equivalence_passed": True,
            "before_allocation_bytes": baseline["frames"][0]["allocation_bytes"],
            "after_allocation_bytes": result["frames"][0]["allocation_bytes"]}
    output = json.dumps(result, indent=2, allow_nan=False) + "\n"
    if args.output:
        args.output.write_text(output, encoding="utf-8")
    print(output, end="")
