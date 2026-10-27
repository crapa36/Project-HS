import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import tempfile
import zlib
from pathlib import Path


def run(producer: Path, source: Path, build: Path) -> str:
    result = subprocess.run(
        [str(producer), "--texture-batch", "--source-root", str(source), "--build-root", str(build)],
        check=False, capture_output=True, text=True,
    )
    if result.returncode:
        raise RuntimeError(f"batch failed ({result.returncode}):\n{result.stdout}\n{result.stderr}")
    return result.stdout + result.stderr


def add_text_chunk(path: Path, value: bytes) -> None:
    data = path.read_bytes()
    chunk_type = b"tEXt"
    chunk_data = b"batch-test\0" + value
    chunk = struct.pack(">I", len(chunk_data)) + chunk_type + chunk_data + struct.pack(">I", zlib.crc32(chunk_type + chunk_data))
    temporary = path.with_suffix(path.suffix + ".new")
    temporary.write_bytes(data[:-12] + chunk + data[-12:])
    os.replace(temporary, path)


def action(build: Path, namespace: str) -> dict:
    return json.loads((build / "Actions" / namespace / "result.json").read_text(encoding="utf-8"))


def assert_hashed_source(result: dict, source_root: Path, path: Path) -> None:
    logical_id = path.relative_to(source_root).as_posix()
    inputs = {item["logical_id"]: item["digest"] for item in result["inputs"]}
    expected = "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()
    if inputs.get(logical_id) != expected:
        raise RuntimeError(f"Action input digest does not match the consumed source snapshot: {logical_id}")


def depfile_path(path: Path) -> str:
    return "".join("\\" + ch if ch in " #:\\" else "$$" if ch == "$" else ch
                   for ch in path.as_posix())


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--producer", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    args = parser.parse_args()
    args.producer = args.producer.resolve()
    args.source_root = args.source_root.resolve()
    args.build_root = args.build_root.resolve()
    args.build_root.mkdir(parents=True, exist_ok=True)

    fixture = Path(tempfile.mkdtemp(prefix="environment-family-source-", dir=args.build_root))
    build = Path(tempfile.mkdtemp(prefix="environment-family-build-", dir=args.build_root))
    try:
        source_env = args.source_root / "ContentSource/Textures/Environment"
        fixture_env = fixture / "ContentSource/Textures/Environment"
        shutil.copytree(source_env, fixture_env)

        manifest = json.loads((fixture_env / "project_hs_environment_materials_v9_LEAN.json").read_text(encoding="utf-8"))
        base_rel = manifest["terrain"]["layers"][0]["textures"]["base_color"]
        normal_rel = manifest["terrain"]["layers"][0]["textures"]["normal"]
        base_path, normal_path = fixture_env / base_rel, fixture_env / normal_rel

        first_log = run(args.producer, fixture, build)
        stamp_path = build / "Actions/environment/textures/family.stamp"
        depfile = (build / "Actions/environment/textures/family.d").read_text(encoding="utf-8")
        if not stamp_path.is_file() or depfile_path(base_path) not in depfile:
            raise RuntimeError("successful batch did not publish its union depfile and completion stamp")
        role_before = action(build, "environment/textures/terrain/base_color")
        histogram_before = action(build, "environment/histogram")
        assert_hashed_source(role_before, fixture, fixture_env / "project_hs_environment_materials_v9_LEAN.json")
        assert_hashed_source(role_before, fixture, fixture_env / "baked_materials.json")
        assert_hashed_source(role_before, fixture, base_path)
        assert_hashed_source(histogram_before, fixture, fixture_env / "project_hs_environment_materials_v9_LEAN.json")
        assert_hashed_source(histogram_before, fixture, base_path)

        second_log = run(args.producer, fixture, build)
        if "D3D11 hardware unavailable" in second_log or "D3D11 GPU compressor" in second_log:
            raise RuntimeError("all-current family batch unnecessarily created a D3D11 compressor")
        if "environment.texture.terrain.base_color current" not in second_log:
            raise RuntimeError("all-current batch did not preserve current Action state")

        add_text_chunk(base_path, b"changed-source-snapshot")
        run(args.producer, fixture, build)
        role_after = action(build, "environment/textures/terrain/base_color")
        histogram_after = action(build, "environment/histogram")
        assert_hashed_source(role_after, fixture, base_path)
        assert_hashed_source(histogram_after, fixture, base_path)
        if role_after["action_key"] == role_before["action_key"] or histogram_after["action_key"] == histogram_before["action_key"]:
            raise RuntimeError("changed source snapshot did not invalidate each consuming Action")
        if role_after["product_set_id"] != role_before["product_set_id"]:
            raise RuntimeError("byte-identical role recook changed ProductSet identity")

        previous_stamp = stamp_path.read_bytes()
        previous_role = role_after["action_key"]
        normal_path.write_bytes(b"corrupt PNG; batch must fail before completion")
        failed = subprocess.run(
            [str(args.producer), "--texture-batch", "--source-root", str(fixture), "--build-root", str(build)],
            check=False, capture_output=True, text=True,
        )
        if failed.returncode == 0:
            raise RuntimeError("corrupt source unexpectedly produced a successful family stamp")
        if stamp_path.read_bytes() != previous_stamp or action(build, "environment/textures/terrain/base_color")["action_key"] != previous_role:
            raise RuntimeError("failed family batch changed its completion stamp or committed role")
    finally:
        shutil.rmtree(fixture, ignore_errors=True)
        shutil.rmtree(build, ignore_errors=True)


if __name__ == "__main__":
    main()
