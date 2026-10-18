"""Verify mutable material defaults do not recook structural shader/package products.

    The fixture copies authoritative material JSON and the terrain/rock_0 texture,
    shader, and geometry upstream ActionResults into a temporary build root.
    Product payloads are hard-linked where possible, so this check does not duplicate
    a full cooked tree.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def digest(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def copy_source_tree(repo: Path, destination: Path, material_names: tuple[str, ...]) -> None:
    for relative in ("Schemas/material_graph.schema.json",
                     "Source/RenderData/Public/hs/render_data/material_schema.hpp",
                     "Source/Apps/AssetPipeline/render_shader_bindings.hpp",
                     "Source/Apps/AssetPipeline/render_shader_binding_inputs.hpp"):
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(repo / relative, target)
    materials = repo / "ContentSource/Materials"
    material_destination = destination / "ContentSource/Materials"
    material_destination.mkdir(parents=True, exist_ok=True)
    for source in materials.glob("*.json"):
        shutil.copy2(source, material_destination / source.name)

    source_shaders = repo / "Content/Shaders"
    for source in source_shaders.rglob("*"):
        if not source.is_file():
            continue
        target = destination / source.relative_to(repo)
        target.parent.mkdir(parents=True, exist_ok=True)
        try:
            os.link(source, target)
        except OSError:
            shutil.copy2(source, target)


def copy_action(source_build: Path, fixture_build: Path, namespace: str) -> None:
    source_result = source_build / "Actions" / namespace / "result.json"
    result = json.loads(source_result.read_text(encoding="utf-8"))
    target_result = fixture_build / "Actions" / namespace / "result.json"
    target_result.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source_result, target_result)
    for product in result["products"]:
        locator = Path(product["locator"])
        if locator.is_absolute() or ".." in locator.parts:
            raise AssertionError(f"unsafe upstream locator: {locator}")
        source_product = (source_build / locator).resolve()
        if not source_product.is_file():
            raise AssertionError(f"missing upstream product: {source_product}")
        target_product = fixture_build / locator
        target_product.parent.mkdir(parents=True, exist_ok=True)
        try:
            os.link(source_product, target_product)
        except OSError:
            shutil.copyfile(source_product, target_product)
        if not target_product.is_file():
            raise AssertionError(f"upstream product was not copied: {target_product}")
        if digest(target_product) != product["digest"]:
            raise AssertionError(f"upstream product digest mismatch: {product['logical_id']}")


def read_result(build: Path, namespace: str) -> dict:
    return json.loads((build / "Actions" / namespace / "result.json").read_text(encoding="utf-8"))


def product_bytes(build: Path, result: dict, logical_id: str) -> bytes:
    product = next(item for item in result["products"] if item["logical_id"] == logical_id)
    return (build / product["locator"]).read_bytes()


def run(executable: Path, *arguments: str) -> None:
    subprocess.run([str(executable), *arguments], check=True)


def edit_defaults(path: Path) -> None:
    document = json.loads(path.read_text(encoding="utf-8"))
    parameters = document["parameters"]
    if isinstance(parameters, dict):
        parameters["metallic"] = 0.35
        parameters["roughness_scale"] = 1.35
        if "tint" in parameters:
            parameters["tint"] = [0.7, 0.8, 0.9]
    elif isinstance(parameters, list):
        changed = False
        for parameter in parameters:
            parameter_id = parameter["id"]
            if parameter_id == "metallic":
                parameter["default"] = 0.35
                changed = True
            elif parameter_id == "roughness_scale":
                parameter["default"] = 1.35
                changed = True
            elif parameter_id == "tint":
                parameter["default"] = [0.7, 0.8, 0.9]
                changed = True
        if not changed:
            raise AssertionError("fixture material has no mutable numeric defaults")
    else:
        raise AssertionError("unexpected material parameter encoding")
    path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")


def copy_case_upstreams(source_build: Path, fixture_build: Path, source: Path,
                        name: str, copied: set[str]) -> None:
    material = json.loads(
        (source / "ContentSource/Materials" / f"{name}.json").read_text(encoding="utf-8"))
    namespaces = {
        item["product_id"].removesuffix(".rpkg")
        for item in material["textures"]
    }
    namespaces.add(f"render/shader/{name}")
    if name == "rock_0":
        namespaces.add("environment/meshes")
    for namespace in sorted(namespaces):
        if namespace not in copied:
            copy_action(source_build, fixture_build, namespace)
            copied.add(namespace)


def run_case(shader_exe: Path, package_exe: Path, dxc: Path,
             source: Path, build: Path, name: str) -> dict[str, dict]:
    shader_args = ("--name", name, "--source-root", str(source),
                   "--build-root", str(build), "--dxc", str(dxc))
    parameter_args = ("--parameters", "--name", name, "--source-root",
                      str(source), "--build-root", str(build))
    package_args = ("--name", name, "--source-root", str(source),
                    "--build-root", str(build))
    run(shader_exe, *parameter_args)
    run(shader_exe, *shader_args)
    run(package_exe, *package_args)
    return {
        "parameter": read_result(build, f"render/material/{name}/parameters"),
        "shader": read_result(build, f"render/shader/{name}"),
        "package": read_result(build, "render/terrain" if name == "terrain"
                                else f"render/{name}"),
    }


def snapshot_products(build: Path, result: dict) -> dict[str, bytes]:
    return {
        product["logical_id"]: product_bytes(build, result, product["logical_id"])
        for product in result["products"]
    }


def assert_default_separation(build: Path, name: str, baseline: dict[str, dict],
                              changed: dict[str, dict]) -> None:
    parameter_before = baseline["parameter"]
    parameter_after = changed["parameter"]
    if parameter_after["products"][0]["digest"] == parameter_before["products"][0]["digest"]:
        raise AssertionError(f"{name}: mutable defaults did not change the parameter product")
    if snapshot_products(build, parameter_after) == snapshot_products(build, parameter_before):
        raise AssertionError(f"{name}: parameter payload stayed byte-identical")

    for kind in ("shader", "package"):
        before = baseline[kind]
        after = changed[kind]
        if after["action_key"] != before["action_key"]:
            raise AssertionError(f"{name}: {kind} ActionKey changed after default-only edit")
        if after["product_set_id"] != before["product_set_id"]:
            raise AssertionError(f"{name}: {kind} ProductSetId changed after default-only edit")
        before_products = snapshot_products(build, before)
        after_products = snapshot_products(build, after)
        if before_products.keys() != after_products.keys():
            raise AssertionError(f"{name}: {kind} product IDs changed after default-only edit")
        for logical_id, payload in before_products.items():
            if after_products[logical_id] != payload:
                raise AssertionError(f"{name}: {kind} product changed: {logical_id}")


def assert_hierarchy_cache_repairs_corrupt_page(package_exe: Path, source: Path,
                                              build: Path, expected: dict) -> None:
    before = snapshot_products(build, expected)
    page = next(product for product in expected["products"]
                if product["logical_id"].startswith("render/hierarchy/rock_0/")
                and product["logical_id"].endswith(".rpage"))
    path = build / page["locator"]
    damaged = bytearray(path.read_bytes())
    damaged[-1] ^= 1
    # Upstream fixture files may be hard-linked. Replace this path instead of
    # modifying a shared inode belonging to the authoritative cooked tree.
    replacement = path.with_name(path.name + ".invalid")
    replacement.write_bytes(damaged)
    os.replace(replacement, path)
    run(package_exe, "--name", "rock_0", "--source-root", str(source), "--build-root", str(build))
    restored = read_result(build, "render/rock_0")
    if restored["action_key"] != expected["action_key"] or restored["product_set_id"] != expected["product_set_id"]:
        raise AssertionError("hierarchy repair changed the unchanged recipe or product identity")
    if snapshot_products(build, restored) != before:
        raise AssertionError("hierarchy cache accepted corruption or changed healthy sibling products")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shader-exe", required=True, type=Path)
    parser.add_argument("--package-exe", required=True, type=Path)
    parser.add_argument("--dxc", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path,
                        help="authoritative Project HS source root")
    parser.add_argument("--build-root", required=True, type=Path,
                        help="authoritative cooked build root containing upstream products")
    parser.add_argument("--fixture-parent", type=Path, default=None)
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()

    parent = (args.fixture_parent or args.build_root.resolve().parent).resolve()
    parent.mkdir(parents=True, exist_ok=True)
    fixture = Path(tempfile.mkdtemp(prefix=".mp-", dir=parent))
    try:
        source = fixture / "source"
        build = fixture / "build"
        names = ("terrain", "rock_0")
        copy_source_tree(args.source_root.resolve(), source, names)
        copied: set[str] = set()
        for name in names:
            copy_case_upstreams(args.build_root.resolve(), build, source, name, copied)

        baseline = {
            name: run_case(args.shader_exe.resolve(), args.package_exe.resolve(),
                           args.dxc.resolve(), source, build, name)
            for name in names
        }
        source_before = {
            path.relative_to(source): digest(path)
            for path in source.rglob("*") if path.is_file()
        }
        for name in names:
            edit_defaults(source / "ContentSource/Materials" / f"{name}.json")
        changed = {
            name: run_case(args.shader_exe.resolve(), args.package_exe.resolve(),
                           args.dxc.resolve(), source, build, name)
            for name in names
        }
        for name in names:
            assert_default_separation(build, name, baseline[name], changed[name])
        source_after = {
            path.relative_to(source): digest(path)
            for path in source.rglob("*") if path.is_file()
        }
        material_relatives = {
            Path("ContentSource/Materials") / f"{name}.json" for name in names
        }
        if {key: value for key, value in source_before.items()
                if key not in material_relatives} != {
            key: value for key, value in source_after.items()
                if key not in material_relatives
        }:
            raise AssertionError("producer modified fixture source inputs")
        assert_hierarchy_cache_repairs_corrupt_page(args.package_exe.resolve(), source, build, changed["rock_0"]["package"])
        print("PASS: terrain/rock_0 default separation and corrupt hierarchy-page cache repair")
    finally:
        if args.keep:
            print(f"fixture retained at {fixture}")
        else:
            shutil.rmtree(fixture, ignore_errors=True)


if __name__ == "__main__":
    main()
