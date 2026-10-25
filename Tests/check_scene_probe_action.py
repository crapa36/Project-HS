"""Exercise probe layout cooking without renderer source-file dependencies."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

from check_material_parameter_actions import copy_action, read_result, product_bytes


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--producer", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--result-list", type=Path)
    args = parser.parse_args()
    namespace = "render/lighting/local-probes"
    product = "render/lighting/local_probes.spkg"
    original = read_result(args.build_root, namespace)
    needed = {item["logical_id"] for item in original["upstream_product_dependencies"]}
    with tempfile.TemporaryDirectory(prefix="hs_probe_action_") as temporary:
        root = Path(temporary)
        source, build = root / "source", root / "build"
        for relative in ("ContentSource/Lighting/local_probes.json", "Schemas/scene_probe.schema.json"):
            target = source / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(args.source_root / relative, target)
        copied, selected_namespaces = set(), set()
        native_results = args.result_list or args.build_root / "GenerationState/scene-probe-input-results.txt"
        for line in native_results.read_text(encoding="utf-8").splitlines():
            if not line:
                continue
            path = Path(line)
            result = json.loads(path.read_text(encoding="utf-8"))
            ids = {item["logical_id"] for item in result["products"]}
            if needed & ids:
                copy_action(args.build_root, build, result["namespace_path"])
                selected_namespaces.add(result["namespace_path"])
                copied.update(ids)
        assert needed <= copied, "fixture must include every upstream product"
        result_list = root / "scene-probe-input-results.txt"
        result_list.write_text("".join(str(build / "Actions" / name / "result.json") + "\n"
                                      for name in sorted(selected_namespaces)), encoding="utf-8")
        command = [str(args.producer.resolve()), "--produce", "--source-root", str(source),
                   "--build-root", str(build), "--result-list", str(result_list)]
        subprocess.run(command, check=True)
        first = read_result(build, namespace)
        before = product_bytes(build, first, product)
        # A retired producer can retain the same logical ID after ownership moves
        # (N03's old render/character namespace did). It is not part of this DAG.
        upstream_path = build / "Actions" / sorted(selected_namespaces)[0] / "result.json"
        retired = json.loads(upstream_path.read_text(encoding="utf-8"))
        retired_namespace = "retired/scene-probe-fixture"
        retired["namespace_path"] = retired_namespace
        retired_path = build / "Actions" / retired_namespace / "result.json"
        retired_path.parent.mkdir(parents=True)
        retired_path.write_text(json.dumps(retired), encoding="utf-8")
        subprocess.run(command, check=True)
        assert read_result(build, namespace)["action_key"] == first["action_key"]
        assert product_bytes(build, first, product) == before
        # Explicitly admitting both owners is still an error. Scope restriction
        # must not become first-match selection or modify the committed product.
        selected_list = result_list.read_bytes()
        with result_list.open("a", encoding="utf-8") as output:
            output.write(str(retired_path) + "\n")
        duplicate = subprocess.run(command, capture_output=True, text=True)
        assert duplicate.returncode != 0 and "multiple producer namespaces" in duplicate.stderr
        assert read_result(build, namespace)["action_key"] == first["action_key"]
        assert product_bytes(build, first, product) == before
        result_list.write_bytes(selected_list)
        # Renderer implementation is not an input to an authored layout package.
        unrelated = source / "Source/RendererD3D12/Private/renderer_frame.cpp"
        unrelated.parent.mkdir(parents=True)
        unrelated.write_text("unrelated renderer edit\n", encoding="utf-8")
        subprocess.run(command, check=True)
        assert read_result(build, namespace)["action_key"] == first["action_key"]
        assert product_bytes(build, first, product) == before
        # Snapshot reuse must not turn metadata caching into an integrity bypass.
        upstream = json.loads(upstream_path.read_text(encoding="utf-8"))
        selected = next(item for item in upstream["products"] if item["logical_id"] in needed)
        payload = build / selected["locator"]
        saved = payload.read_bytes()
        assert saved, "fixture upstream must have bytes to corrupt"
        payload.unlink()  # copy_action may have hard-linked the production payload.
        payload.write_bytes(bytes([saved[0] ^ 1]) + saved[1:])
        assert subprocess.run(command).returncode != 0, "cached metadata must still reject corrupt bytes"
        payload.write_bytes(saved)
        authored = source / "ContentSource/Lighting/local_probes.json"
        data = json.loads(authored.read_text(encoding="utf-8"))
        data["probes"][0]["weight"] *= 0.5
        authored.write_text(json.dumps(data), encoding="utf-8")
        subprocess.run(command, check=True)
        changed = read_result(build, namespace)
        assert changed["action_key"] != first["action_key"]
        assert product_bytes(build, changed, product) != before
        data["probes"][0]["weight"] = 0
        authored.write_text(json.dumps(data), encoding="utf-8")
        assert subprocess.run(command).returncode != 0, "encoder must reject zero weight"
    print("scene probe action: scoped DAG ownership, source independence, restat, integrity and bounds passed")


if __name__ == "__main__":
    main()
