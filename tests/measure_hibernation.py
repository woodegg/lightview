"""Measure an isolated blank Lightview before, during, and after hibernation."""
import argparse
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("lightview_memory",
    str(ROOT / "tools/lightview-memory"))
spec = importlib.util.spec_from_loader(loader.name, loader)
memory = importlib.util.module_from_spec(spec)
loader.exec_module(memory)


def command(socket_path, *args):
    result = subprocess.run([str(ROOT / "tools/lightviewctl"), "--socket", socket_path,
                             *args], capture_output=True, text=True, timeout=20, check=True)
    return json.loads(result.stdout)


def wait_state(socket_path, target, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            state = command(socket_path, "status")
            if state["engine_state"] == target:
                return state
        except (OSError, subprocess.CalledProcessError):
            pass
        time.sleep(0.2)
    raise TimeoutError(f"Lightview did not reach {target}")


def snapshot(pid):
    data = memory.measure(pid)
    return {
        "total_pss_mib": data["total_pss_mib"],
        "main_pss_mib": next(row["pss_mib"] for row in data["processes"]
                             if row["pid"] == pid),
        "processes": [{"name": row["name"], "pss_mib": row.get("pss_mib")}
                      for row in data["processes"]],
        "complete": data["complete"],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--settle-seconds", type=float, default=3,
                        help="time to wait after hibernation before measuring")
    parser.add_argument("--persistent", action="store_true",
                        help="measure an isolated persistent profile instead of private mode")
    args = parser.parse_args()
    if args.settle_seconds < 0:
        parser.error("--settle-seconds must not be negative")
    with tempfile.TemporaryDirectory(prefix="lightview-hibernate-") as directory:
        socket_path = str(Path(directory) / "control.sock")
        profile_options = (["--profile", str(Path(directory) / "profile")]
                           if args.persistent else ["--private"])
        with tempfile.TemporaryFile(mode="w+") as log:
            process = subprocess.Popen([str(ROOT / "build/lightview"), *profile_options,
                "--low-memory", "--idle-hibernate", "7", "--lease-ttl", "60",
                "--socket", socket_path, "about:blank"], stdout=log, stderr=log)
            token = None
            try:
                active = wait_state(socket_path, "ready")
                token = command(socket_path, "lease", "acquire")["token"]
                time.sleep(2)
                active_memory = snapshot(process.pid)
                wait_state(socket_path, "suspended")
                time.sleep(args.settle_seconds)
                sleeping_memory = snapshot(process.pid)
                command(socket_path, "--lease", token, "lease", "renew")
                assert command(socket_path, "status")["engine_state"] == "suspended"
                command(socket_path, "--lease", token, "wake")
                wait_state(socket_path, "ready")
                time.sleep(1)
                resumed_memory = snapshot(process.pid)
                print(json.dumps({
                    "version": active["version"],
                    "display": os.environ.get("DISPLAY"),
                    "profile_mode": "persistent" if args.persistent else "private",
                    "idle_hibernate_seconds": 7,
                    "settle_seconds": args.settle_seconds,
                    "lease_survived_hibernation": True,
                    "active": active_memory,
                    "suspended": sleeping_memory,
                    "resumed": resumed_memory,
                    "saved_pss_mib": round(active_memory["total_pss_mib"] -
                                           sleeping_memory["total_pss_mib"], 2),
                }, indent=2))
            finally:
                if process.poll() is None:
                    if token:
                        try:
                            command(socket_path, "--lease", token, "lease", "release")
                        except (OSError, subprocess.CalledProcessError):
                            pass
                    try:
                        command(socket_path, "quit")
                        process.wait(timeout=5)
                    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired):
                        process.terminate()
                        process.wait(timeout=5)
                if process.returncode:
                    log.seek(0)
                    raise RuntimeError(log.read())


if __name__ == "__main__":
    main()
