"""Check an already published static-shadow Action without initializing a GPU."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


NAMESPACE = "render/lighting/arena-static-shadow"
ACTION = "render.lighting.arena-static-shadow"
INDEX = "render/lighting/arena_static_shadow.index"
PAGES = "render/lighting/arena_static_shadow.pages"
RECIPE = "project-hs.render.static-shadow/coarse-fallback-lod2"
QUALITY = "render/config/quality_profiles.rpkg"
SOURCES = {
    "layout/implementation": "Source/Presentation/Private/environment_layout.cpp",
    "lighting/environment-contract": "Source/Presentation/Public/hs/presentation/environment_layout.hpp",
}


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        value = hashlib.file_digest(stream, "sha256").hexdigest()
    return "sha256:" + value


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def located(root: Path, locator: str) -> Path:
    relative = Path(locator)
    if relative.is_absolute() or ".." in relative.parts:
        raise AssertionError(f"unsafe fixture locator: {locator}")
    path = (root / relative).resolve()
    path.relative_to(root.resolve())
    return path


def link_payload(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.link(source, destination)
    except OSError:
        shutil.copyfile(source, destination)


def reduced_generation(source_build: Path, fixture: Path, pointer: Path,
                       dependencies: dict[str, dict]) -> tuple[Path, dict[str, Path]]:
    selected = read_json(pointer)
    manifest = read_json(located(source_build, selected["manifest_locator"]))
    assert manifest["schema_version"] == 3, "fixture requires immutable schema-3 payloads"
    original = {item["logical_id"]: item for item in manifest["products"]}
    ids = sorted(dependencies)
    assert QUALITY not in ids, "fixture must omit native initialization prerequisites"
    assert not any(item.startswith("render/hlod/") for item in ids), "unexpected HLOD bake dependency"
    products = []
    for logical_id in ids:
        item = original[logical_id]
        expected = dependencies[logical_id]
        assert (item["digest"], item["size"]) == (expected["digest"], expected["size"])
        assert set(item.get("dependencies", ())) <= dependencies.keys(), "incomplete baked dependency closure"
        products.append({key: value for key, value in item.items() if key != "locator"})
    actions = {item["action_id"] for item in products}
    edges = [edge for edge in manifest["upstream_product_dependencies"]
             if edge["action_id"] in actions and edge["logical_id"] in dependencies]
    # Same schema-3 semantic identity encoding as AssetGeneration, without locators.
    semantics = dict(schema_version=3, scope="static-shadow-action-fixture", required_ids=ids,
                     products=products, upstream_product_dependencies=edges)
    generation_id = hashlib.sha256(json.dumps(semantics, separators=(",", ":"),
                                               ensure_ascii=False).encode()).hexdigest()
    payloads = {}
    for item in products:
        logical_id = item["logical_id"]
        name = hashlib.sha256(logical_id.encode()).hexdigest()
        item["locator"] = f"Generations/{generation_id}/products/{name}.bin"
        destination = located(fixture, item["locator"])
        link_payload(located(source_build, original[logical_id]["locator"]), destination)
        payloads[logical_id] = destination
    manifest_locator = f"Generations/{generation_id}/generation.json"
    target_manifest = located(fixture, manifest_locator)
    target_manifest.write_text(json.dumps(dict(semantics, generation_id=generation_id)), encoding="utf-8")
    target_pointer = fixture / "GenerationState/input.json"
    target_pointer.parent.mkdir(parents=True, exist_ok=True)
    target_pointer.write_text(json.dumps(dict(schema_version=1, scope=semantics["scope"],
        generation_id=generation_id, manifest_locator=manifest_locator, action_ids=sorted(actions))), encoding="utf-8")
    return target_pointer, payloads


def run_case(command: list[str], cwd: Path, log, label: str) -> subprocess.CompletedProcess:
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True,
                            encoding="utf-8", errors="replace", timeout=60)
    log.write(f"[{label}] exit={result.returncode}\n{result.stdout}{result.stderr}\n")
    log.flush()
    return result


def check(args, log) -> None:
    build = args.build_root.resolve()
    producer = args.producer.resolve()
    result_path = build / "Actions" / NAMESPACE / "result.json"
    baseline = read_json(result_path)
    assert baseline["action_id"] == ACTION and baseline["namespace_path"] == NAMESPACE
    assert digest(producer) == baseline["recipe_identity"], "stale published producer identity; do not rebake in this test"
    assert baseline["recipe_id"] == RECIPE, "fixture requires the coarse fallback-LOD2 archive Action"
    inputs = {item["logical_id"]: item["digest"] for item in baseline["inputs"]}
    assert inputs.keys() == SOURCES.keys()
    for name, relative in SOURCES.items():
        assert digest(args.source_root / relative) == inputs[name], "stale source input; do not rebake in this test"
    products = {item["logical_id"]: item for item in baseline["products"]}
    assert len(baseline["products"]) == 2 and products.keys() == {INDEX, PAGES}
    dependencies = {item["logical_id"]: item for item in baseline["upstream_product_dependencies"]}
    assert products[PAGES]["dependencies"] == sorted(dependencies)
    assert products[INDEX]["dependencies"] == sorted([*dependencies, PAGES])
    evidence = build.parent / "engine-lean-audit"
    with tempfile.TemporaryDirectory(prefix="static-shadow-action-", dir=evidence) as directory:
        root = Path(directory).resolve()
        root.relative_to(evidence.resolve())
        fixture = root / "build"
        source = root / "source"
        fixture.mkdir()
        for relative in SOURCES.values():
            destination = source / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(args.source_root / relative, destination)
        # Only explicit result metadata is indexed; unrelated product payloads are absent.
        fixture_results = []
        for entry in args.result_list.read_text(encoding="utf-8").splitlines():
            original = Path(entry).resolve()
            relative = original.relative_to(build)
            assert relative.parts[0] == "Actions" and original.name == "result.json"
            destination = fixture / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(original, destination)
            fixture_results.append(str(destination))
        result_list = fixture / "input-results.txt"
        result_list.write_text("\n".join(fixture_results) + "\n", encoding="utf-8")
        pointer, payloads = reduced_generation(build, fixture, args.generation, dependencies)
        cached_result = fixture / "Actions" / NAMESPACE / "result.json"
        cached_result.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(result_path, cached_result)
        shutil.copy2(result_path.with_name("result.d"), cached_result.with_name("result.d"))
        outputs = {}
        for logical_id, product in products.items():
            output = located(fixture, product["locator"])
            link_payload(located(build, product["locator"]), output)
            outputs[logical_id] = output
        original_result = cached_result.read_bytes()
        original_pointer = pointer.read_bytes()
        command = [str(producer), "--produce-static-shadow", "--source-root", str(source),
                   "--build-root", str(fixture), "--generation", str(pointer),
                   "--result-list", str(result_list)]
        hit = run_case(command, producer.parent, log, "cache-hit-without-native-prerequisites")
        assert hit.returncode == 0, (hit.stdout, hit.stderr)
        assert cached_result.read_bytes() == original_result, "cache hit changed Action content"
        for logical_id, output in outputs.items():
            assert digest(output) == products[logical_id]["digest"], f"cache hit changed {logical_id}"

        target_id = "render/material/leaf_0/parameters.rpkg"
        target = payloads[target_id]
        original_bytes = target.read_bytes()
        assert original_bytes, "missing actual alpha-material parameter payload"
        damaged = bytearray(original_bytes)
        damaged[-1] ^= 1  # Same size: selection's size checks alone must not accept it.
        target.unlink()  # Never mutate a hard link to the authoritative Generation.
        target.write_bytes(damaged)
        failed = run_case(command, producer.parent, log, "corrupt-selected-upstream")
        assert failed.returncode != 0
        assert f"Selected bake input differs from its Action product: {target_id}" in failed.stdout + failed.stderr
        assert cached_result.read_bytes() == original_result, "failure replaced the previous ActionResult"
        assert pointer.read_bytes() == original_pointer, "failure changed the selected Generation pointer"
        for logical_id, output in outputs.items():
            assert digest(output) == products[logical_id]["digest"], f"failure damaged previous product {logical_id}"
        original_manifest = read_json(located(build, read_json(args.generation)["manifest_locator"]))
        original_input = next(item for item in original_manifest["products"] if item["logical_id"] == target_id)
        assert digest(located(build, original_input["locator"])) == dependencies[target_id]["digest"]
        log.write(json.dumps(dict(result="passed", recipe=RECIPE, preserved_products=sorted(products),
            copied_upstream_payloads=len(payloads),
            indexed_action_results=len(fixture_results), gpu_initialized=False,
            corruption_target=target_id, previous_publication_preserved=True)) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("producer", "source-root", "build-root", "generation", "result-list"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    evidence = args.build_root.resolve().parent / "engine-lean-audit"
    evidence.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    log_path = evidence / f"static-shadow-action-{stamp}.log"
    print(f"Log: {log_path}", flush=True)
    with log_path.open("w", encoding="utf-8") as log:
        check(args, log)
    print("static shadow Action cache/corruption fixture passed (no GPU initialization)")


if __name__ == "__main__":
    main()
