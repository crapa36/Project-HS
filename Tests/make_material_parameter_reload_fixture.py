#!/usr/bin/env python3
"""Create an isolated material-parameter Generation reload fixture.

The fixture owns a fresh sibling of the selected build root.  It hard-links
the baseline snapshot and asks the real material-parameter producer to encode
the valid and incompatible payloads, so ActionResult, action-key, and
ProductSetId validation stays in the production path.
"""

from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import uuid
from typing import Any


SCRIPT_ROOT = Path(__file__).resolve().parent
if str(SCRIPT_ROOT) not in sys.path:
    sys.path.insert(0, str(SCRIPT_ROOT))

from make_shader_reload_fixture import (  # noqa: E402
    active_pointer,
    atomic_publish,
    generation_id,
    hardlink,
    load_source,
    make_pointer,
    product_map,
    sha256_bytes,
    snapshot_locator,
    verify,
    write_generation,
    write_json,
    read_json,
)


FIXTURE_KIND = "material-parameter-reload"
FIXTURE_SCHEMA_VERSION = 1
MATERIAL_CHOICES = ("terrain", "leaf_0")


def material_spec(name: str) -> dict[str, str]:
    if name not in MATERIAL_CHOICES:
        fail(f"unsupported material fixture choice: {name}")
    return {
        "name": name,
        "source_relative": f"ContentSource/Materials/{name}.json",
        "logical_id": f"render/material/{name}/parameters.rpkg",
        "action_id": f"render.parameters.{name}",
        "namespace_path": f"render/material/{name}/parameters",
    }


def fail(message: str) -> "NoReturn":
    raise RuntimeError(message)


def absolute(path: str | Path) -> Path:
    return Path(path).expanduser().resolve()


def require_absolute_file(path: str | Path, label: str) -> Path:
    requested = Path(path).expanduser()
    if not requested.is_absolute():
        fail(f"{label} must be an absolute path: {path}")
    result = requested.resolve()
    if not result.is_file():
        fail(f"{label} is not a file: {result}")
    return result


def choose_root(source_build: Path) -> Path:
    parent = source_build.parent
    for _ in range(32):
        candidate = parent / f".params-{uuid.uuid4().hex[:8]}"
        if not candidate.exists():
            return candidate
    fail(f"could not allocate a unique fixture root beside {source_build}")


def ensure_safe_root(root: Path, source_build: Path, authoring_root: Path) -> None:
    root = root.resolve()
    source_build = source_build.resolve()
    authoring_root = authoring_root.resolve()
    if root == source_build or source_build in root.parents or root in source_build.parents:
        fail(f"fixture root must be isolated from selected source build: {root}")

    authoritative = (
        authoring_root / "ContentSource",
        authoring_root / "Schemas",
        authoring_root / "Content" / "Shaders",
    )
    selected_data = tuple(source_build / name for name in
                          ("Generations", "GenerationState", "Products", "Actions"))
    for protected in authoritative + selected_data:
        if root == protected or protected in root.parents or root in protected.parents:
            fail(f"fixture root overlaps protected data: {root}")

    if root.exists():
        fail(f"refusing to reuse an existing fixture root: {root}")
    if root.parent == root:
        fail(f"fixture root has no usable parent: {root}")
    if not root.parent.is_dir():
        fail(f"fixture root parent does not exist: {root.parent}")


def copy_material_source(authoring_root: Path, root: Path, material: dict[str, Any],
                         material_name: str, invalid: bool) -> Path:
    source_root = root / ("SourceInvalid" if invalid else "SourceValid")
    material_path = source_root / "ContentSource" / "Materials" / f"{material_name}.json"
    schema_path = source_root / "Schemas" / "material_graph.schema.json"
    material_path.parent.mkdir(parents=True)
    schema_path.parent.mkdir(parents=True)
    write_json(material_path, material)
    shutil.copyfile(authoring_root / "Schemas" / "material_graph.schema.json", schema_path)
    return source_root


def material_variant(authoring_root: Path, material_name: str, invalid: bool) -> dict[str, Any]:
    source_path = authoring_root / "ContentSource" / "Materials" / f"{material_name}.json"
    material = read_json(source_path)
    version = material.get("version")
    if material.get("id") != material_name or version not in (6, 7):
        fail(f"material-parameter fixture requires authoritative {material_name} JSON version 6 or 7")
    parameters = material.get("parameters")
    if not isinstance(parameters, list):
        fail("material-parameter fixture requires ordered version-6/7 parameters")

    if any(not isinstance(entry, dict) for entry in parameters):
        fail(f"{material_name} parameter declaration is malformed")
    ids = [entry.get("id") for entry in parameters]
    if any(not isinstance(parameter_id, str) for parameter_id in ids) or len(ids) != len(set(ids)):
        fail(f"{material_name} parameter declarations have duplicate or invalid IDs")
    expected_types = {"tint": "float3", "roughness_scale": "float", "metallic": "float"}
    if material_name == "leaf_0":
        expected_types["opacity_scale"] = "float"
    defaults: dict[str, Any] = {
        "tint": [0.25, 0.65, 1.0],
        "roughness_scale": 0.45,
        "metallic": 0.2,
    }
    if material_name == "leaf_0":
        defaults["opacity_scale"] = 0.0
    for parameter_id, parameter_type in expected_types.items():
        matches = [parameter for parameter in parameters if parameter.get("id") == parameter_id]
        if len(matches) != 1 or matches[0].get("type") != parameter_type:
            fail(f"{material_name} intended parameter declaration is malformed: {parameter_id}")
    for parameter in parameters:
        if parameter["id"] in defaults:
            parameter["default"] = copy.deepcopy(defaults[parameter["id"]])

    if invalid:
        for parameter in parameters:
            if parameter["id"] == "tint":
                parameter["id"] = "tint_incompatible_layout"
        for node in material.get("nodes", []):
            if not isinstance(node, dict):
                continue
            for field in ("name", "declaration"):
                if node.get(field) == "tint":
                    node[field] = "tint_incompatible_layout"
    return material


def produce_parameters(producer: Path, source_root: Path, build_root: Path,
                       spec: dict[str, str]) -> tuple[bytes, dict[str, Any]]:
    build_root.mkdir(parents=True)
    command = [str(producer), "--parameters", "--name", spec["name"],
               "--source-root", str(source_root), "--build-root", str(build_root)]
    try:
        subprocess.run(command, check=True)
    except OSError as error:
        fail(f"cannot execute material parameter producer {producer}: {error}")
    except subprocess.CalledProcessError as error:
        fail(f"material parameter producer failed: exit {error.returncode}")

    result_path = build_root / "Actions" / spec["namespace_path"] / "result.json"
    if not result_path.is_file():
        fail(f"material parameter producer did not publish {result_path}")
    result = read_json(result_path)
    if (result.get("schema_version") != 1 or result.get("action_id") != spec["action_id"] or
            result.get("namespace_path") != spec["namespace_path"]):
        fail("material parameter producer ActionResult identity is invalid")
    action_key = result.get("action_key")
    product_set_id = result.get("product_set_id")
    is_hex_sha256 = lambda value: isinstance(value, str) and len(value) == 64 and all(
        character in "0123456789abcdef" for character in value)
    is_prefixed_sha256 = lambda value: isinstance(value, str) and value.startswith("sha256:") \
        and is_hex_sha256(value[7:])
    if not is_prefixed_sha256(action_key):
        fail("material parameter producer ActionResult has an invalid action key")
    if not is_hex_sha256(product_set_id):
        fail("material parameter producer ActionResult has an invalid ProductSetId")
    products = result.get("products")
    if not isinstance(products, list) or len(products) != 1 or not isinstance(products[0], dict):
        fail("material parameter producer must publish exactly one product")
    product = products[0]
    if product.get("logical_id") != spec["logical_id"]:
        fail("material parameter producer published the wrong logical ID")
    locator = product.get("locator")
    expected_locator = f"Products/{spec['namespace_path']}/{product_set_id}/parameters.rpkg"
    if locator != expected_locator:
        fail("material parameter producer ProductSet locator is inconsistent")
    payload_path = build_root / Path(locator)
    if not payload_path.is_file():
        fail(f"material parameter producer payload is missing: {payload_path}")
    payload = payload_path.read_bytes()
    digest = sha256_bytes(payload)
    if product.get("digest") != digest or product.get("size") != len(payload):
        fail("material parameter producer payload metadata is inconsistent")
    return payload, {
        "action_id": spec["action_id"],
        "action_key": action_key,
        "product_set_id": product_set_id,
        "namespace_path": spec["namespace_path"],
        "logical_id": spec["logical_id"],
        "locator": locator,
        "digest": digest,
        "size": len(payload),
        "action_result": str(result_path),
    }


def candidate_manifest(source_manifest: dict[str, Any], digest: str, size: int,
                       target_id: str) -> dict[str, Any]:
    manifest = copy.deepcopy(source_manifest)
    target = product_map(manifest).get(target_id)
    if target is None:
        fail(f"selected Generation lacks {target_id}")
    target["digest"] = digest
    target["size"] = size
    for edge in manifest.get("upstream_product_dependencies", []):
        if isinstance(edge, dict) and edge.get("logical_id") == target_id:
            edge["digest"] = digest
            edge["size"] = size
    candidate = generation_id(manifest)
    manifest["generation_id"] = candidate
    for product in manifest["products"]:
        product["locator"] = snapshot_locator(candidate, product["logical_id"])
    return manifest


def changed_product_ids(source_manifest: dict[str, Any], candidate: dict[str, Any]) -> list[str]:
    before = product_map(source_manifest)
    after = product_map(candidate)
    if set(before) != set(after):
        fail("material parameter candidate changed the Generation product set")
    changed = [logical_id for logical_id in before
               if before[logical_id].get("digest") != after[logical_id].get("digest") or
               before[logical_id].get("size") != after[logical_id].get("size")]
    return changed


def run_runner(runner: Path, root: Path, artifacts: Path, geometry: str,
               surface: str, barriers: str) -> dict[str, Any]:
    artifacts.mkdir(parents=True, exist_ok=True)
    baseline_pointer = root / "Candidates" / "baseline.json"
    valid_pointer = root / "Candidates" / "valid.json"
    invalid_pointer = root / "Candidates" / "invalid.json"
    command = [
        str(runner),
        f"--generation={baseline_pointer}",
        f"--material-reload={valid_pointer}",
        f"--material-reload-invalid={invalid_pointer}",
        "--frames=6",
        "--gpu-validation",
        f"--geometry={geometry}",
        f"--surface={surface}",
        f"--barriers={barriers}",
        f"--artifacts={artifacts}",
    ]
    stdout_path = artifacts / "runner.stdout.json"
    stderr_path = artifacts / "stderr.log"
    try:
        with stdout_path.open("w", encoding="utf-8", newline="") as stdout, \
                stderr_path.open("w", encoding="utf-8", newline="") as stderr:
            completed = subprocess.run(command, stdout=stdout, stderr=stderr, check=False)
    except OSError as error:
        fail(f"cannot execute RenderTestRunner {runner}: {error}")

    result_path = artifacts / "result.json"
    if not result_path.is_file():
        fail(f"RenderTestRunner did not write {result_path}; see {stderr_path}")
    result = read_json(result_path)
    if completed.returncode != 0 or result.get("execution_valid") is not True:
        fail(f"RenderTestRunner failed with exit {completed.returncode}; see {stderr_path}")
    return {
        "runner": str(runner),
        "artifacts": str(artifacts),
        "stdout": str(stdout_path),
        "stderr": str(stderr_path),
        "returncode": completed.returncode,
        "execution_valid": result["execution_valid"],
        "result": str(result_path),
    }


def prepare(args: argparse.Namespace) -> None:
    pointer_path = require_absolute_file(args.source_pointer, "source pointer")
    spec = material_spec(args.material)
    scope = read_json(pointer_path).get("scope")
    if scope not in ("runtime-full", "test-render"):
        fail("material reload requires a runtime-full or test-render Generation")
    source_root, source_pointer, source_manifest = load_source(pointer_path, scope)
    source_root = source_root.resolve()
    authoring_root = absolute(args.authoring_root) if args.authoring_root else SCRIPT_ROOT.parent
    authoring_root = authoring_root.resolve()
    if not (authoring_root / spec["source_relative"]).is_file():
        fail(f"authoritative {args.material} source is missing under {authoring_root}")

    target_id = spec["logical_id"]
    source_products = product_map(source_manifest)
    if target_id not in source_products:
        fail(f"selected Generation lacks {target_id}")
    source_target = source_products[target_id]
    if (source_target.get("action_id") != spec["action_id"] or
            source_target.get("namespace_path") != spec["namespace_path"]):
        fail(f"selected Generation target has unexpected material parameter identity: {target_id}")
    for edge in source_manifest.get("upstream_product_dependencies", []):
        if isinstance(edge, dict) and edge.get("logical_id") == target_id:
            if (edge.get("digest") != source_target.get("digest") or
                    edge.get("size") != source_target.get("size")):
                fail(f"target product upstream identity disagrees with baseline: {target_id}")

    root = absolute(args.root) if args.root else choose_root(source_root)
    ensure_safe_root(root, source_root, authoring_root)
    root.mkdir()
    (root / "Generations").mkdir()
    (root / "GenerationState").mkdir()

    baseline_id = source_pointer["generation_id"]
    baseline_generation = root / "Generations" / baseline_id
    baseline_generation.mkdir()
    for product in source_manifest["products"]:
        hardlink(source_root / Path(product["locator"]), root / Path(product["locator"]))
    shutil.copyfile(source_root / Path(source_pointer["manifest_locator"]),
                    baseline_generation / "generation.json")

    candidates_dir = root / "Candidates"
    candidates_dir.mkdir()
    baseline_pointer = make_pointer(source_pointer, baseline_id)
    write_json(candidates_dir / "baseline.json", baseline_pointer)

    valid_source = copy_material_source(
        authoring_root, root, material_variant(authoring_root, args.material, False),
        args.material, False)
    invalid_source = copy_material_source(
        authoring_root, root, material_variant(authoring_root, args.material, True),
        args.material, True)
    valid_payload, valid_producer = produce_parameters(
        require_absolute_file(args.material_producer, "material producer"), valid_source,
        root / "FixtureProductBuild" / "valid", spec)
    invalid_payload, invalid_producer = produce_parameters(
        require_absolute_file(args.material_producer, "material producer"), invalid_source,
        root / "FixtureProductBuild" / "invalid", spec)

    valid_manifest = candidate_manifest(source_manifest, sha256_bytes(valid_payload),
                                        len(valid_payload), target_id)
    invalid_manifest = candidate_manifest(source_manifest, sha256_bytes(invalid_payload),
                                          len(invalid_payload), target_id)
    if changed_product_ids(source_manifest, valid_manifest) != [target_id] or \
            changed_product_ids(source_manifest, invalid_manifest) != [target_id]:
        fail("material parameter fixture changed more than the parameter product")
    if valid_payload == invalid_payload:
        fail("valid and incompatible material parameter payloads are identical")
    valid_id = write_generation(root, valid_manifest, source_root, source_manifest,
                                valid_payload, target_id)
    invalid_id = write_generation(root, invalid_manifest, source_root, source_manifest,
                                  invalid_payload, target_id)
    write_json(candidates_dir / "valid.json", make_pointer(source_pointer, valid_id))
    write_json(candidates_dir / "invalid.json", make_pointer(source_pointer, invalid_id))

    metadata = {
        "fixture_kind": FIXTURE_KIND,
        "schema_version": FIXTURE_SCHEMA_VERSION,
        "material": args.material,
        "material_source": spec["source_relative"],
        "material_action_id": spec["action_id"],
        "material_namespace_path": spec["namespace_path"],
        "material_logical_id": spec["logical_id"],
        "scope": scope,
        "baseline_id": baseline_id,
        "valid_id": valid_id,
        "invalid_id": invalid_id,
        "target_logical_id": target_id,
        "source_pointer": str(pointer_path),
        "active_pointer": str(active_pointer(root, scope)),
        "authoring_root": str(authoring_root),
        "pointers": {
            "baseline": "Candidates/baseline.json",
            "valid": "Candidates/valid.json",
            "invalid": "Candidates/invalid.json",
        },
        "producer": {"valid": valid_producer, "invalid": invalid_producer},
    }
    write_json(root / "fixture.json", metadata)

    verifier = require_absolute_file(args.generation_verifier, "Generation verifier")
    for candidate in ("baseline", "valid", "invalid"):
        atomic_publish(root, candidate)
        verify(root, verifier, scope)
    atomic_publish(root, "baseline")
    if args.runner:
        runner = require_absolute_file(args.runner, "RenderTestRunner")
        if not args.artifacts:
            fail("--artifacts is required with --runner")
        if not Path(args.artifacts).expanduser().is_absolute():
            fail("--artifacts must be an absolute path")
        artifacts = absolute(args.artifacts)
        metadata["runner"] = run_runner(
            runner, root, artifacts, args.geometry, args.surface, args.barriers)
        write_json(root / "fixture.json", metadata)
    print(json.dumps(metadata, ensure_ascii=False, separators=(",", ":")))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-pointer", required=True)
    parser.add_argument("--material", choices=MATERIAL_CHOICES, default="terrain")
    parser.add_argument("--root")
    parser.add_argument("--authoring-root")
    parser.add_argument("--material-producer", required=True)
    parser.add_argument("--generation-verifier", required=True)
    parser.add_argument("--runner")
    parser.add_argument("--artifacts")
    parser.add_argument("--geometry", choices=("indexed", "mesh"), default="indexed")
    parser.add_argument("--surface", choices=("visibility", "compact"), default="compact")
    parser.add_argument("--barriers", choices=("enhanced", "legacy"), default="enhanced")
    args = parser.parse_args()
    try:
        prepare(args)
        return 0
    except RuntimeError as error:
        print(f"material-parameter-reload-fixture.error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
