#!/usr/bin/env python3
"""Create an isolated immutable Generation set for the Debug reload test.

The fixture is deliberately made from hard links to the selected build output.
Only the one replacement shader is copied, so the test never writes a source
ProductSet or a payload that another test may be using.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from typing import Any


SCHEMA_VERSION = 3
SCOPE = "runtime-full"
TARGET_LOGICAL_ID = "render/shader/bloom_ps.dxil"
REPLACEMENT_LOGICAL_ID = "render/shader/bloom_upsample_ps.dxil"
ZERO_DIGEST = "sha256:" + ("0" * 64)


def fail(message: str) -> "NoReturn":
    raise RuntimeError(message)


def sha256_bytes(data: bytes) -> str:
    return "sha256:" + hashlib.sha256(data).hexdigest()


def canonical_json(value: Any) -> bytes:
    # nlohmann::ordered_json::dump() uses compact separators and emits UTF-8.
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def pretty_json(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        fail(f"cannot read JSON {path}: {error}")
    if not isinstance(value, dict):
        fail(f"expected JSON object: {path}")
    return value


def write_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(pretty_json(value))


def absolute(path: str | Path) -> Path:
    return Path(path).expanduser().resolve()


def hardlink(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.link(source, destination)
    except OSError as error:
        fail(f"cannot hard-link {source} to {destination}: {error}")


def load_source(pointer_path: Path, expected_scope: str = SCOPE) -> tuple[Path, dict[str, Any], dict[str, Any]]:
    pointer = read_json(pointer_path)
    if pointer.get("schema_version") != 1:
        fail("selected Generation pointer must use schema_version=1")
    generation_id = pointer.get("generation_id")
    locator = pointer.get("manifest_locator")
    scope = pointer.get("scope")
    if not isinstance(generation_id, str) or not isinstance(locator, str):
        fail("selected Generation pointer has no identity")
    if scope != expected_scope or locator != f"Generations/{generation_id}/generation.json":
        fail(f"selected Generation pointer does not target {expected_scope}")
    source_root = pointer_path.parent.parent
    manifest_path = source_root / Path(locator)
    manifest = read_json(manifest_path)
    if manifest.get("schema_version") != SCHEMA_VERSION:
        fail("shader reload fixture requires a schema_version=3 manifest")
    if manifest.get("generation_id") != generation_id or manifest.get("scope") != expected_scope:
        fail("selected Generation manifest does not match its pointer")
    products = manifest.get("products")
    if not isinstance(products, list) or not products:
        fail("selected Generation manifest has no products")
    return source_root, pointer, manifest


def product_map(manifest: dict[str, Any]) -> dict[str, dict[str, Any]]:
    products = manifest.get("products")
    if not isinstance(products, list):
        fail("Generation products are not an array")
    result: dict[str, dict[str, Any]] = {}
    for product in products:
        if not isinstance(product, dict):
            fail("Generation product is not an object")
        logical_id = product.get("logical_id")
        if not isinstance(logical_id, str) or logical_id in result:
            fail("Generation products have an invalid or duplicate logical ID")
        result[logical_id] = product
    return result


def generation_id(manifest: dict[str, Any]) -> str:
    semantics = {
        "schema_version": manifest["schema_version"],
        "scope": manifest["scope"],
        "required_ids": manifest["required_ids"],
        "products": [],
        "upstream_product_dependencies": manifest["upstream_product_dependencies"],
    }
    for product in manifest["products"]:
        item = {
            "action_id": product["action_id"],
            "namespace_path": product["namespace_path"],
            "logical_id": product["logical_id"],
            "digest": product["digest"],
            "size": product["size"],
        }
        if "dependencies" in product and product["dependencies"]:
            item["dependencies"] = product["dependencies"]
        semantics["products"].append(item)
    return hashlib.sha256(canonical_json(semantics)).hexdigest()


def snapshot_locator(generation: str, logical_id: str) -> str:
    digest = hashlib.sha256(logical_id.encode("utf-8")).hexdigest()
    return f"Generations/{generation}/products/{digest}.bin"


def make_pointer(source: dict[str, Any], generation: str) -> dict[str, Any]:
    pointer = copy.deepcopy(source)
    pointer["generation_id"] = generation
    pointer["manifest_locator"] = f"Generations/{generation}/generation.json"
    return pointer


def active_pointer(root: Path, scope: str = SCOPE) -> Path:
    return root / "GenerationState" / f"{scope}.json"


def atomic_publish(root: Path, candidate: str) -> None:
    metadata = read_json(root / "fixture.json")
    pointers = metadata.get("pointers")
    if not isinstance(pointers, dict) or candidate not in pointers:
        fail(f"unknown fixture candidate: {candidate}")
    pointer_source = root / Path(pointers[candidate])
    if not pointer_source.is_file():
        fail(f"fixture pointer is missing: {pointer_source}")
    scope = metadata.get("scope")
    if scope not in (SCOPE, "test-render"):
        fail("fixture has an unsupported Generation scope")
    destination = active_pointer(root, scope)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + f".tmp-{os.getpid()}")
    try:
        shutil.copyfile(pointer_source, temporary)
        os.replace(temporary, destination)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def verify(root: Path, verifier: Path, scope: str) -> None:
    command = [str(verifier), "--verify", "--build-root", str(root), "--scope", scope]
    try:
        subprocess.run(command, check=True)
    except OSError as error:
        fail(f"cannot execute Generation verifier {verifier}: {error}")
    except subprocess.CalledProcessError as error:
        fail(f"Generation verification failed for {scope}: exit {error.returncode}")


def write_generation(root: Path, manifest: dict[str, Any], source_root: Path,
                     source_manifest: dict[str, Any], replacement: bytes,
                     target_logical_id: str) -> str:
    generation = generation_id(manifest)
    if manifest.get("generation_id") != generation:
        fail("internal Generation ID calculation mismatch")
    generation_root = root / "Generations" / generation
    if generation_root.exists():
        fail(f"Generation ID collision in fixture: {generation}")
    generation_root.mkdir(parents=True)
    source_products = product_map(source_manifest)
    for product in manifest["products"]:
        logical_id = product["logical_id"]
        destination = root / Path(product["locator"])
        if logical_id == target_logical_id:
            copy_payload_from_bytes(replacement, destination)
        else:
            source_product = source_products.get(logical_id)
            if source_product is None:
                fail(f"candidate product is not in source Generation: {logical_id}")
            hardlink(source_root / Path(source_product["locator"]), destination)
    write_json(generation_root / "generation.json", manifest)
    return generation


def copy_payload_from_bytes(data: bytes, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    try:
        destination.write_bytes(data)
    except OSError as error:
        fail(f"cannot write fixture shader payload {destination}: {error}")


def prepare(args: argparse.Namespace) -> None:
    pointer_path = absolute(args.source_pointer)
    root = absolute(args.root)
    source_root, source_pointer, source_manifest = load_source(pointer_path)
    protected_roots = [source_root / name for name in
                       ("Generations", "GenerationState", "Products", "Actions")]
    if root == source_root or root in source_root.parents:
        fail("fixture root must not be the selected build root or its ancestor")
    if any(root == protected or protected in root.parents for protected in protected_roots):
        fail("fixture root must not be under a selected build data directory")
    if root.exists():
        marker = root / "fixture.json"
        if not root.is_dir() or not marker.is_file():
            fail(f"refusing to delete unowned or partial fixture root: {root}")
        existing = read_json(marker)
        if (existing.get("fixture_kind") != "shader-reload" or
                existing.get("schema_version") != 1 or
                existing.get("source_pointer") != str(pointer_path) or
                existing.get("scope") != SCOPE):
            fail(f"refusing to delete foreign fixture root: {root}")
    source_products = product_map(source_manifest)
    target_id = args.target_logical_id
    replacement_id = args.replacement_logical_id
    if target_id not in source_products or replacement_id not in source_products:
        fail(f"selected Generation lacks {target_id} or {replacement_id}")
    if target_id == replacement_id:
        fail("target and replacement products must differ")
    for edge in source_manifest.get("upstream_product_dependencies", []):
        if isinstance(edge, dict) and edge.get("logical_id") == target_id:
            fail(f"target product unexpectedly has an upstream identity: {target_id}")

    replacement_path = source_root / Path(source_products[replacement_id]["locator"])
    try:
        replacement = replacement_path.read_bytes()
    except OSError as error:
        fail(f"cannot read replacement shader {replacement_path}: {error}")
    replacement_digest = sha256_bytes(replacement)
    replacement_size = len(replacement)
    if replacement_digest == source_products[target_id]["digest"] and replacement_size == source_products[target_id]["size"]:
        fail("replacement shader is byte-identical to the target")

    if root.exists():
        shutil.rmtree(root)
    (root / "Generations").mkdir(parents=True)
    (root / "GenerationState").mkdir(parents=True)
    baseline_id = source_pointer["generation_id"]
    baseline_generation = root / "Generations" / baseline_id
    baseline_generation.mkdir(parents=True)
    for product in source_manifest["products"]:
        hardlink(source_root / Path(product["locator"]), root / Path(product["locator"]))
    try:
        shutil.copyfile(source_root / Path(source_pointer["manifest_locator"]),
                        baseline_generation / "generation.json")
    except OSError as error:
        fail(f"cannot copy baseline Generation manifest: {error}")

    candidates: dict[str, str] = {}
    candidate_pointers = root / "Candidates"
    candidate_pointers.mkdir()
    baseline_pointer = make_pointer(source_pointer, baseline_id)
    write_json(candidate_pointers / "baseline.json", baseline_pointer)
    candidates["baseline"] = "Candidates/baseline.json"

    def candidate_manifest(digest: str) -> dict[str, Any]:
        manifest = copy.deepcopy(source_manifest)
        target = product_map(manifest)[target_id]
        target["digest"] = digest
        target["size"] = replacement_size
        # Snapshot locators are part of the immutable manifest identity.
        candidate = generation_id(manifest)
        manifest["generation_id"] = candidate
        for product in manifest["products"]:
            product["locator"] = snapshot_locator(candidate, product["logical_id"])
        return manifest

    valid_manifest = candidate_manifest(replacement_digest)
    valid_id = write_generation(root, valid_manifest, source_root, source_manifest,
                                replacement, target_id)
    valid_pointer = make_pointer(source_pointer, valid_id)
    write_json(candidate_pointers / "valid.json", valid_pointer)
    candidates["valid"] = "Candidates/valid.json"

    invalid_manifest = candidate_manifest(ZERO_DIGEST)
    invalid_id = write_generation(root, invalid_manifest, source_root, source_manifest,
                                  replacement, target_id)
    invalid_pointer = make_pointer(source_pointer, invalid_id)
    write_json(candidate_pointers / "invalid.json", invalid_pointer)
    candidates["invalid"] = "Candidates/invalid.json"

    metadata = {
        "fixture_kind": "shader-reload",
        "schema_version": 1,
        "scope": SCOPE,
        "baseline_id": baseline_id,
        "valid_id": valid_id,
        "invalid_id": invalid_id,
        "target_logical_id": target_id,
        "replacement_logical_id": replacement_id,
        "source_pointer": str(pointer_path),
        "active_pointer": str(active_pointer(root)),
        "pointers": candidates,
    }
    write_json(root / "fixture.json", metadata)
    atomic_publish(root, "baseline")
    verifier = absolute(args.generation_verifier)
    verify(root, verifier, SCOPE)
    atomic_publish(root, "valid")
    verify(root, verifier, SCOPE)
    atomic_publish(root, "baseline")
    print(json.dumps(metadata, ensure_ascii=False, separators=(",", ":")))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    prepare_parser = subparsers.add_parser("prepare")
    prepare_parser.add_argument("--source-pointer", required=True)
    prepare_parser.add_argument("--root", required=True)
    prepare_parser.add_argument("--generation-verifier", required=True)
    prepare_parser.add_argument("--target-logical-id", default=TARGET_LOGICAL_ID)
    prepare_parser.add_argument("--replacement-logical-id", default=REPLACEMENT_LOGICAL_ID)
    prepare_parser.set_defaults(handler=prepare)

    select_parser = subparsers.add_parser("select")
    select_parser.add_argument("--root", required=True)
    select_parser.add_argument("--candidate", choices=("baseline", "invalid", "valid"), required=True)
    select_parser.set_defaults(handler=lambda args: atomic_publish(absolute(args.root), args.candidate))

    args = parser.parse_args()
    try:
        args.handler(args)
        return 0
    except RuntimeError as error:
        print(f"shader-reload-fixture.error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
