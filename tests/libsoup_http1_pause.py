"""Test libsoup's private pause API using its existing upstream static build.

Requires a completed Meson/Ninja build including tests/http2-test, a C compiler,
openssl and python-h2. Source/build trees and browser runtimes are unchanged.
"""
import argparse
import concurrent.futures
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shlex
import subprocess

from libsoup_http2_recovery import Server, digest


def compile_client(build, output):
    commands = subprocess.check_output(
        ["ninja", "-t", "commands", "tests/http2-test"], cwd=build, text=True).splitlines()
    compile_args = next(shlex.split(line) for line in commands
                        if "http2-test.c.o -c" in line)
    link_args = shlex.split(commands[-1])
    archive_index = next(i for i, arg in enumerate(link_args)
                         if arg.endswith("/libsoup-3.0-static.a"))
    if any(arg in ("&&", ";", "|") for arg in link_args):
        raise RuntimeError("Unexpected compound upstream linker command")
    archive = (build / link_args[archive_index]).resolve(strict=True)
    fixture = Path(__file__).with_suffix(".c").resolve()
    binary = output / "pause-client"
    command = [os.environ.get("CC", "cc"), "-DLIBSOUP_COMPILATION",
               *(arg for arg in compile_args if arg.startswith("-I")),
               str(fixture), "-o", str(binary), "-Wl,--start-group",
               *link_args[archive_index:]]
    (output / "build-command.json").write_text(
        json.dumps({"argv": command, "cwd": str(build)}, indent=2) + "\n")
    result = subprocess.run(command, cwd=build, text=True, capture_output=True)
    (output / "build.log").write_text(result.stdout + result.stderr)
    result.check_returncode()
    return binary, archive, fixture


def run_case(case, binary, output):
    output.mkdir()
    scenario = {"ordinary": "ordinary-http1", "fallback": "paused-headers",
                "cancel": "fallback-cancel"}[case]
    server = Server(output, scenario)
    try:
        result = subprocess.run([
            str(binary), f"https://127.0.0.1:{server.port}", str(server.certificate), case,
        ], capture_output=True, text=True, timeout=65)
        (output / "stdout.log").write_text(result.stdout)
        (output / "stderr.log").write_text(result.stderr)
        client = json.loads(result.stdout)
        requests = server.requests.copy()
        warm = [r for r in requests if r["path"] == "/warm"]
        target = [r for r in requests if r["path"] == "/first"]
        protocols = [r["protocol"] for r in target]
        expected = ["http/1.1"] if case == "ordinary" else ["h2", "http/1.1"]
        wire_ok = (len(warm) == 1 and protocols == expected and
                   len({r["connection"] for r in target}) == len(target))
        if case != "ordinary" and wire_ok:
            wire_ok &= (warm[0]["protocol"] == "h2" and
                        warm[0]["connection"] == target[0]["connection"])
        elif wire_ok:
            wire_ok &= (warm[0]["protocol"] == "http/1.1" and
                        warm[0]["connection"] != target[0]["connection"])
        passed = (result.returncode == 0 and client["passed"] and not client["timeout"] and
                  client["completions"] == 1 and wire_ok and not server.errors and
                  not any(marker in result.stderr for marker in ("WARNING", "CRITICAL")))
        report = {"case": case, "passed": bool(passed), "returncode": result.returncode,
                  "client": client, "server_requests": requests, "server_errors": server.errors}
    except Exception as error:
        report = {"case": case, "passed": False, "error": repr(error)}
    finally:
        server.close()
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report), flush=True)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--case", choices=("ordinary", "fallback", "cancel"))
    args = parser.parse_args()
    os.umask(0o077)
    build = args.build_dir.resolve(strict=True)
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    binary, archive, fixture = compile_client(build, output)
    inputs = {"started_at": datetime.now(timezone.utc).isoformat(),
              "linkage": "upstream static archive; private pause interface",
              "archive": str(archive), "archive_sha256": digest(archive),
              "fixture_sha256": digest(fixture), "script_sha256": digest(__file__),
              "server_sha256": digest(Path(__file__).with_name("libsoup_http2_recovery.py"))}
    (output / "inputs.json").write_text(json.dumps(inputs, indent=2) + "\n")
    cases = [args.case] if args.case else ["ordinary", "fallback", "cancel"]
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(cases)) as pool:
        results = list(pool.map(lambda case: run_case(case, binary, output / case), cases))
    passed = (all(result["passed"] for result in results) and
              digest(archive) == inputs["archive_sha256"] and
              digest(fixture) == inputs["fixture_sha256"] and
              digest(__file__) == inputs["script_sha256"] and
              digest(Path(__file__).with_name("libsoup_http2_recovery.py")) ==
              inputs["server_sha256"])
    report = {"passed": passed, "inputs": inputs, "results": results}
    (output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
