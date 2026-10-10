#!/usr/bin/env python3
"""Create an isolated immutable Generation set for the Debug reload test.

The fixture is deliberately made from hard links to the selected build output.
Changed payloads are copied, so the test never writes a source ProductSet or a
payload that another test may be using. A catalog edit also exercises full
resident adoption, and a changed arena layout must be rejected before it.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import struct
import sys
from typing import Any


SCHEMA_VERSION = 3
SCOPE = "runtime-full"
TARGET_LOGICAL_ID = "render/shader/bloom_ps.dxil"
REPLACEMENT_LOGICAL_ID = "render/shader/bloom_upsample_ps.dxil"
ZERO_DIGEST = "sha256:" + ("0" * 64)
CATALOG_LOGICAL_ID = "vfx/particle_effects.hsbin"
LAYOUT_LOGICAL_ID = "game-data/arena-layout"


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
        fail(f"cannot write fixture payload {destination}: {error}")


def fnv1a64(data: bytes) -> int:
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


def changed_particle_catalog(data: bytes) -> bytes:
    # CookedParticleEffects v4 is a little-endian raw struct stream. Change a
    # real line width, preserving all handles, sprite indices and program links.
    if len(data) < 40:
        fail("particle catalog fixture has no header")
    magic, version, effects, emitters, sprites, schema, payload_hash = struct.unpack_from("<8sIIIIQQ", data)
    if (magic != b"HSPFX\0\0\0" or version != 4 or schema != 0x4853504658563034 or
            len(data) != 40 + effects * 56 + emitters * 144 + sprites * 16 or
            fnv1a64(data[40:]) != payload_hash):
        fail("particle catalog fixture has an unsupported cooked ABI or hash")
    edited = bytearray(data)
    for index in range(effects):
        offset = 40 + index * 56
        if data[offset + 14] != 1:  # VfxDefinitionKind::Line
            continue
        width = struct.unpack_from("<f", data, offset + 32)[0]
        if not math.isfinite(width) or width <= 0:
            fail("particle catalog fixture has an invalid line width")
        struct.pack_into("<f", edited, offset + 32, width * 1.125)
        struct.pack_into("<Q", edited, 32, fnv1a64(edited[40:]))
        return bytes(edited)
    fail("particle catalog fixture has no line effect to edit")


def changed_arena_layout(data: bytes) -> bytes:
    layout = json.loads(data)
    if layout.get("abi") != 1 or not isinstance(layout.get("half_extent_bits"), int):
        fail("arena layout fixture has an unsupported ABI")
    extent = struct.unpack("<f", struct.pack("<I", layout["half_extent_bits"]))[0]
    if not math.isfinite(extent) or extent <= 0:
        fail("arena layout fixture has an invalid extent")
    layout["half_extent_bits"] = struct.unpack("<I", struct.pack("<f", extent + 1.0))[0]
    return canonical_json(layout)


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

    def candidate_manifest(digest: str, base: dict[str, Any] = source_manifest,
                           logical_id: str = target_id, size: int = replacement_size) -> dict[str, Any]:
        manifest = copy.deepcopy(base)
        target = product_map(manifest)[logical_id]
        target["digest"] = digest
        target["size"] = size
        for edge in manifest["upstream_product_dependencies"]:
            if edge["logical_id"] == logical_id:
                edge["digest"] = digest
                edge["size"] = size
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

    for logical_id in (CATALOG_LOGICAL_ID, LAYOUT_LOGICAL_ID):
        if logical_id not in source_products:
            fail(f"selected Generation lacks {logical_id}")
    catalog = changed_particle_catalog(
        (source_root / Path(source_products[CATALOG_LOGICAL_ID]["locator"])).read_bytes())
    resident_manifest = candidate_manifest(sha256_bytes(catalog), valid_manifest, CATALOG_LOGICAL_ID, len(catalog))
    resident_id = write_generation(root, resident_manifest, root, valid_manifest,
                                   catalog, CATALOG_LOGICAL_ID)
    write_json(candidate_pointers / "resident.json", make_pointer(source_pointer, resident_id))
    candidates["resident"] = "Candidates/resident.json"

    layout = changed_arena_layout(
        (source_root / Path(source_products[LAYOUT_LOGICAL_ID]["locator"])).read_bytes())
    incompatible_manifest = candidate_manifest(sha256_bytes(layout), resident_manifest, LAYOUT_LOGICAL_ID, len(layout))
    incompatible_id = write_generation(root, incompatible_manifest, root, resident_manifest,
                                       layout, LAYOUT_LOGICAL_ID)
    write_json(candidate_pointers / "incompatible.json", make_pointer(source_pointer, incompatible_id))
    candidates["incompatible"] = "Candidates/incompatible.json"

    metadata = {
        "fixture_kind": "shader-reload",
        "schema_version": 1,
        "scope": SCOPE,
        "baseline_id": baseline_id,
        "valid_id": valid_id,
        "invalid_id": invalid_id,
        "resident_id": resident_id,
        "incompatible_id": incompatible_id,
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
    atomic_publish(root, "resident")
    verify(root, verifier, SCOPE)
    atomic_publish(root, "incompatible")
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
    select_parser.add_argument("--candidate", choices=("baseline", "invalid", "valid", "resident", "incompatible"), required=True)
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
