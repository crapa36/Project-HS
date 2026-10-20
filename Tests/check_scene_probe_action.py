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
        copied = set()
        for path in (args.build_root / "Actions").rglob("result.json"):
            result = json.loads(path.read_text(encoding="utf-8"))
            ids = {item["logical_id"] for item in result["products"]}
            if needed & ids:
                copy_action(args.build_root, build, result["namespace_path"])
                copied.update(ids)
        assert needed <= copied, "fixture must include every upstream product"
        command = [str(args.producer.resolve()), "--produce", "--source-root", str(source),
                   "--build-root", str(build)]
        subprocess.run(command, check=True)
        first = read_result(build, namespace)
        before = product_bytes(build, first, product)
        # Renderer implementation is not an input to an authored layout package.
        unrelated = source / "Source/RendererD3D12/Private/renderer_frame.cpp"
        unrelated.parent.mkdir(parents=True)
        unrelated.write_text("unrelated renderer edit\n", encoding="utf-8")
        subprocess.run(command, check=True)
        assert read_result(build, namespace)["action_key"] == first["action_key"]
        assert product_bytes(build, first, product) == before
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
    print("scene probe action: source independence, restat, authored invalidation and bounds passed")


if __name__ == "__main__":
    main()
