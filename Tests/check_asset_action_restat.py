"""Portable scheduler regression; does not execute the Win32 asset producer."""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def write_if_different(path, text):
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def producer(arguments):
    kind, source, output, depfile, log = arguments
    source, output = Path(source), Path(output)
    with Path(log).open("a") as stream:
        stream.write(kind + "\n")
    write_if_different(output, source.read_text())
    if kind == "A":
        def escape(path):
            return path.as_posix().replace("$", "$$").replace(" ", "\\ ").replace("#", "\\#").replace(":", "\\:")
        write_if_different(Path(depfile), escape(output) + ": " + escape(source) + "\n")
        if os.environ.get("HS_RESTAT_REPRODUCE_TOUCH") == "1":
            os.utime(output, None)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--ninja", default="ninja")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="hs asset restat ") as temporary:
        root = Path(temporary)
        build = root / "build"
        source = root / "input.txt"
        source.write_text("initial\n")
        quoted = lambda path: '"' + Path(path).as_posix() + '"'
        (root / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 4.4)
project(AssetRestat NONE)
include({quoted(repo / "cmake/AssetActions.cmake")})
add_executable(fixture_python IMPORTED GLOBAL)
set_target_properties(fixture_python PROPERTIES IMPORTED_LOCATION {quoted(sys.executable)})
hs_add_asset_action(NAME action_a BUILDER fixture_python
    RESULT "${{CMAKE_CURRENT_BINARY_DIR}}/result.json"
    DEPFILE "${{CMAKE_CURRENT_BINARY_DIR}}/result.d"
    STATIC_INPUTS "${{CMAKE_CURRENT_SOURCE_DIR}}/input.txt"
    ARGS {quoted(__file__)} --producer A
        "${{CMAKE_CURRENT_SOURCE_DIR}}/input.txt"
        "${{CMAKE_CURRENT_BINARY_DIR}}/result.json"
        "${{CMAKE_CURRENT_BINARY_DIR}}/result.d"
        "${{CMAKE_CURRENT_BINARY_DIR}}/runs.txt")
add_custom_command(OUTPUT "${{CMAKE_CURRENT_BINARY_DIR}}/consumer.json"
    COMMAND {quoted(sys.executable)} {quoted(__file__)} --producer B
        "${{CMAKE_CURRENT_BINARY_DIR}}/result.json"
        "${{CMAKE_CURRENT_BINARY_DIR}}/consumer.json" unused
        "${{CMAKE_CURRENT_BINARY_DIR}}/runs.txt"
    DEPENDS "${{CMAKE_CURRENT_BINARY_DIR}}/result.json" VERBATIM)
add_custom_target(consumer ALL DEPENDS "${{CMAKE_CURRENT_BINARY_DIR}}/consumer.json")
''')
        def run(*command):
            subprocess.run(command, check=True)
        run(args.cmake, "-S", str(root), "-B", str(build), "-G", "Ninja",
            "-DCMAKE_MAKE_PROGRAM=" + str(Path(args.ninja).resolve()) if Path(args.ninja).exists()
            else "-DCMAKE_MAKE_PROGRAM=" + args.ninja)
        def check_build(expected):
            run(args.cmake, "--build", str(build))
            actual = (build / "runs.txt").read_text().splitlines()
            assert actual == expected, (actual, expected)
        check_build(["A", "B"])
        result = build / "result.json"
        original_mtime = result.stat().st_mtime_ns
        time.sleep(1.1)
        os.utime(source, None)
        check_build(["A", "B", "A"])
        assert result.stat().st_mtime_ns == original_mtime
        check_build(["A", "B", "A"])
        time.sleep(1.1)
        source.write_text("changed\n")
        check_build(["A", "B", "A", "A", "B"])
        check_build(["A", "B", "A", "A", "B"])
    print("PASS: actual AssetActions.cmake preserves restat and next-build freshness")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--producer":
        producer(sys.argv[2:])
    else:
        main()
