"""Read authored FBX Model scale properties without evaluating or modifying assets.

Binary record layout reference:
https://github.com/blender/blender-addons/blob/main/io_scene_fbx/parse_fbx.py
This bounded reader skips geometry/array payloads. It does not evaluate animation,
parent transforms, axis conversion, pivots, skinning or baked mesh handedness.
"""
import hashlib
import json
import math
from pathlib import Path
import struct
import sys


def inspect(path):
    result = {"file": path.as_posix(), "models": 0, "scale_properties": 0,
              "flagged": [], "archive": "Archive" in path.parts}
    with path.open("rb") as stream:
        def read(size):
            value = stream.read(size)
            if len(value) != size:
                raise ValueError("truncated FBX record")
            return value

        if read(23) != b"Kaydara FBX Binary  \x00\x1a\x00":
            raise ValueError("unsupported FBX header")
        version, = struct.unpack("<I", read(4))
        result["version"] = version
        if version < 7000:
            raise ValueError("pre-7000 model properties are outside this audit")
        header = struct.Struct("<IIIB" if version < 7500 else "<QQQB")
        file_size = path.stat().st_size

        def property_value():
            kind = read(1)
            if kind in (b"S", b"R"):
                size, = struct.unpack("<I", read(4))
                if size > file_size - stream.tell():
                    raise ValueError("property length exceeds file")
                return read(size)
            formats = {b"Y": "h", b"C": "?", b"I": "i", b"F": "f", b"D": "d", b"L": "q"}
            if kind in formats:
                fmt = struct.Struct("<" + formats[kind])
                return fmt.unpack(read(fmt.size))[0]
            if kind in (b"f", b"d", b"l", b"i", b"b", b"c"):
                _, _, size = struct.unpack("<III", read(12))
                if size > file_size - stream.tell():
                    raise ValueError("array length exceeds file")
                stream.seek(size, 1)
                return None
            raise ValueError(f"unsupported property type {kind!r}")

        def nodes(limit, parent=b"", model=None):
            while stream.tell() + header.size <= limit:
                start = stream.tell()
                end, count, length, name_size = header.unpack(read(header.size))
                if end == 0:
                    if count or length or name_size:
                        raise ValueError("invalid null record")
                    return
                if not start < end <= limit or end > file_size:
                    raise ValueError("invalid element extent")
                name = read(name_size)
                properties_end = stream.tell() + length
                if properties_end > end:
                    raise ValueError("invalid property extent")
                relevant_model = parent == b"Objects" and name == b"Model"
                relevant_property = parent == b"Properties70" and model is not None and name == b"P"
                if relevant_model or relevant_property:
                    values = [property_value() for _ in range(count)]
                    if stream.tell() != properties_end:
                        raise ValueError("property count/length mismatch")
                    if relevant_model:
                        result["models"] += 1
                        model = str(values[1], "utf-8", errors="replace")
                    elif values and values[0] in (b"Lcl Scaling", b"GeometricScaling"):
                        scale = values[-3:]
                        if len(scale) != 3 or not all(isinstance(v, (float, int)) for v in scale):
                            raise ValueError("unexpected scale property")
                        result["scale_properties"] += 1
                        if any(not math.isfinite(v) or v <= 0.0001 for v in scale):
                            result["flagged"].append({"model": model, "property": values[0].decode(), "scale": scale})
                stream.seek(properties_end)
                if (parent == b"" and name == b"Objects") or relevant_model or (
                        parent == b"Model" and name == b"Properties70"):
                    nodes(end, name, model)
                stream.seek(end)

        nodes(file_size)
        stream.seek(0)
        result["sha256"] = hashlib.file_digest(stream, "sha256").hexdigest()
    return result


if __name__ == "__main__":
    results, errors = [], []
    for path in sorted(Path("ContentSource").rglob("*.fbx")):
        try:
            results.append(inspect(path))
        except (ValueError, IndexError, struct.error) as error:
            errors.append({"file": path.as_posix(), "error": str(error)})
    report = {"scope": __doc__, "files": results, "errors": errors}
    Path(sys.argv[1]).write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"files": len(results), "errors": errors,
        "models": sum(r["models"] for r in results),
        "scale_properties": sum(r["scale_properties"] for r in results),
        "flagged_files": [r for r in results if r["flagged"]]}, ensure_ascii=False))
