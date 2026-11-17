"""Build an isolated IPC lifetime regression against an existing WebKit 2.52.6 build.

The source/build trees and installed engine are never changed. A diagnostic-only
entry point is added to a copied translation unit and exposed by WebKit's symbol
map. Do not install the resulting library as a browser runtime.
"""
import argparse
import ctypes
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys


def command(build, target):
    lines = subprocess.check_output(
        ["ninja", "-t", "commands", target], cwd=build, text=True).splitlines()
    args = shlex.split(lines[-1])
    if args[:2] == [":", "&&"]:
        args = args[2:]
    if args[-2:] == ["&&", ":"]:
        args = args[:-2]
    if "&&" in args:
        raise RuntimeError("Unexpected compound Ninja command")
    return args


def execute(library, iterations, expect_retention):
    import time
    runtime = ctypes.CDLL(str(library))
    test = runtime.webkit_lightview_connection_invalidation_regression
    test.argtypes = [ctypes.c_uint]
    test.restype = ctypes.c_int
    rows = []
    started = time.monotonic()
    for iteration in range(iterations):
        for mode in (0, 1, 2):
            result = test(mode)
            row = {"iteration": iteration + 1, "mode": mode,
                   "result_bits": result,
                   "passed": result == (9 if expect_retention and mode != 2 else 0)}
            rows.append(row)
            print(json.dumps(row), flush=True)
            if not row["passed"]:
                return 1
    print(json.dumps({"passed": True, "cases": len(rows),
                      "expected_original_retention": expect_retention,
                      "seconds": round(time.monotonic() - started, 3)}), flush=True)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--connection-source", type=Path,
                        help="Optional patched Connection.cpp; other source/build files stay unchanged")
    parser.add_argument("--iterations", type=int, default=20)
    parser.add_argument("--expect-retention", action="store_true")
    parser.add_argument("--execute-library", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.iterations < 1:
        parser.error("iterations must be positive")
    if args.execute_library:
        return execute(args.execute_library.resolve(), args.iterations, args.expect_retention)
    if not all((args.source_dir, args.build_dir, args.output_dir)):
        parser.error("source-dir, build-dir, and output-dir are required")
    source, build, out = (p.resolve() for p in
                          (args.source_dir, args.build_dir, args.output_dir))
    out.mkdir(parents=True, exist_ok=False)
    ipc = source / "Source/WebKit/Platform/IPC"
    connection_source = (args.connection_source.resolve() if args.connection_source
                         else ipc / "Connection.cpp")
    original = connection_source.read_text()
    friend = "    friend int lightviewConnectionInvalidationRegression(unsigned);\n"
    class_name = ("class Connection::SyncMessageState final : public "
                  "ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<SyncMessageState> {")
    if original.count(class_name) != 1:
        raise RuntimeError("Unexpected SyncMessageState declaration")
    copied = original.replace(class_name, class_name + "\n" + friend)
    copied = copied.replace('#include "Connection.h"',
                            '#include "' + str(ipc / "Connection.h") + '"')
    fixture = Path(__file__).with_suffix(".cpp")
    copied += '\n#include "' + str(fixture) + '"\n'
    (out / "Connection.cpp").write_text(copied)
    header = (ipc / "Connection.h").read_text()
    declaration = ("class Connection : public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr"
                   "<Connection, WTF::DestructionThread::MainRunLoop> {")
    if header.count(declaration) != 1:
        raise RuntimeError("Unexpected Connection declaration")
    (out / "Connection.h").write_text(header.replace(declaration, declaration + "\n" + friend))
    overlay = {"version": 0, "roots": [{"type": "file", "name": str(ipc / "Connection.h"),
                                      "external-contents": str(out / "Connection.h")}]}
    (out / "overlay.json").write_text(json.dumps(overlay, indent=2) + "\n")
    includes = '#include "Platform/IPC/Connection.cpp"'
    units = [p for p in (build / "DerivedSources/WebKit/unified-sources").glob("*.cpp")
             if includes in p.read_text()]
    if len(units) != 1:
        raise RuntimeError("Expected exactly one unified Connection translation unit")
    unit = units[0]
    (out / "Unified.cpp").write_text(unit.read_text().replace(
        includes, '#include "' + str(out / "Connection.cpp") + '"'))
    object_target = ("Source/WebKit/CMakeFiles/WebKit.dir/__/__/"
                     "DerivedSources/WebKit/unified-sources/" + unit.name + ".o")
    compile_args = command(build, object_target)
    for flag, value in [("-o", out / "Connection.o"), ("-MF", out / "Connection.d"),
                        ("-MT", out / "Connection.o"), ("-c", out / "Unified.cpp")]:
        compile_args[compile_args.index(flag) + 1] = str(value)
    compile_args.extend(["-ivfsoverlay", str(out / "overlay.json")])
    target = "lib/libwebkit2gtk-4.1.so.0.21.10"
    edge = (build / "build.ninja").read_text().split("build " + target + ": ", 1)[1]
    edge = edge.split("\n\n", 1)[0]
    inputs = shlex.split(edge.splitlines()[0].split(" ", 1)[1].split(" | ", 1)[0])
    variables = dict(line.strip().split(" = ", 1)
                     for line in edge.splitlines()[1:] if " = " in line)
    response = inputs + shlex.split(variables.get("LINK_PATH", ""))
    response += shlex.split(variables["LINK_LIBRARIES"])
    if response.count(object_target) != 1:
        raise RuntimeError("Connection object not found exactly once in link inputs")
    response = [str(out / "Connection.o") if x == object_target else x for x in response]
    (out / "WebKit.rsp").write_text(shlex.join(response) + "\n")
    library = out / Path(target).name
    link_args = command(build, target)
    link_args[link_args.index("-o") + 1] = str(library)
    link_args = ["@" + str(out / "WebKit.rsp") if x == "@" + variables["RSP_FILE"] else x
                 for x in link_args]
    jobs = [("compile", compile_args), ("link", link_args)]
    (out / "commands.json").write_text(json.dumps(jobs, indent=2) + "\n")
    for name, job in jobs:
        with (out / (name + ".log")).open("x") as log:
            subprocess.run(job, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = str(build / "lib") + ":" + env.get("LD_LIBRARY_PATH", "")
    runner = [sys.executable, str(Path(__file__).resolve()), "--execute-library", str(library),
              "--iterations", str(args.iterations)]
    if args.expect_retention:
        runner.append("--expect-retention")
    with (out / "regression.jsonl").open("x") as stdout, (out / "regression-errors.log").open("x") as stderr:
        result = subprocess.run(runner, env=env, stdout=stdout, stderr=stderr)
    sha = lambda path: hashlib.file_digest(path.open("rb"), "sha256").hexdigest()
    manifest = {"created_at": datetime.now(timezone.utc).isoformat(),
                "diagnostic_only": True, "expected_retention": args.expect_retention,
                "connection_source_sha256": sha(connection_source),
                "fixture_sha256": sha(fixture), "library_sha256": sha(library),
                "exit_code": result.returncode}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print((out / "regression.jsonl").read_text(), end="")
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
