#!/usr/bin/env python3
"""Repeatedly navigate Lightview and record survival, load state, and memory."""
import argparse
import csv
import importlib.machinery
import importlib.util
import json
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("lightviewctl", str(ROOT / "tools/lightviewctl"))
spec = importlib.util.spec_from_loader(loader.name, loader)
ctl = importlib.util.module_from_spec(spec)
loader.exec_module(ctl)

SITES = [
    ("Google", "https://www.google.com/"),
    ("YouTube", "https://www.youtube.com/"),
    ("YouTube Music", "https://music.youtube.com/"),
    ("Facebook", "https://www.facebook.com/"),
    ("Instagram", "https://www.instagram.com/"),
    ("X", "https://x.com/"),
    ("Reddit", "https://www.reddit.com/"),
    ("Wikipedia", "https://www.wikipedia.org/"),
    ("Amazon", "https://www.amazon.com/"),
    ("GitHub", "https://github.com/"),
    ("Microsoft", "https://www.microsoft.com/"),
    ("Apple", "https://www.apple.com/"),
    ("LinkedIn", "https://www.linkedin.com/"),
    ("Yahoo", "https://www.yahoo.com/"),
    ("Bing", "https://www.bing.com/"),
    ("Cloudflare", "https://www.cloudflare.com/"),
]


def memory(pid):
    output = subprocess.check_output(
        [str(ROOT / "tools/lightview-memory"), str(pid)], text=True, timeout=10)
    return json.loads(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", required=True)
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--load-timeout", type=float, default=12)
    parser.add_argument("--settle", type=float, default=2)
    parser.add_argument("--output", type=Path, default=Path("stability-results.csv"))
    args = parser.parse_args()
    if args.rounds < 1 or args.load_timeout <= 0 or args.settle < 0:
        parser.error("rounds and timeouts must be positive")

    initial = ctl.request(args.socket, "status", timeout=3)
    fields = ["round", "site", "requested_url", "final_url", "title", "ready_state",
              "load_error", "load_timed_out", "web_process_stopped", "pss_mib", "rss_mib", "processes"]
    rows = []
    crashed = False
    for round_number in range(1, args.rounds + 1):
        for name, url in SITES:
            row = {field: "" for field in fields}
            row.update(round=round_number, site=name, requested_url=url)
            try:
                ctl.request(args.socket, "open", timeout=3, uri=url)
                deadline = time.monotonic() + args.load_timeout
                state = None
                while time.monotonic() < deadline:
                    state = ctl.request(args.socket, "status", timeout=3)
                    if state["load_error"] or not state["loading"]:
                        break
                    time.sleep(0.25)
                row["load_timed_out"] = bool(state and state["loading"])
                time.sleep(args.settle)
                state = ctl.request(args.socket, "status", timeout=3)
                row["web_process_stopped"] = state["load_error"] == "Web process stopped. Reload to recover."
                try:
                    page = ctl.request(args.socket, "eval", timeout=5,
                        script="({url: String(location.href), title: String(document.title), ready: String(document.readyState)})")
                except Exception as error:
                    page = {"url": state["uri"], "title": state["title"], "ready": "unavailable"}
                    if not state["load_error"]:
                        state["load_error"] = f"PAGE_EVAL_FAILURE: {error}"
                usage = memory(initial["pid"])
                row.update(final_url=page["url"], title=page["title"],
                           ready_state=page["ready"], load_error=state["load_error"] or "",
                           pss_mib=usage["total_pss_mib"], rss_mib=usage["total_rss_mib"],
                           processes=len(usage["processes"]))
                print(f"round {round_number} {name}: ready={row['ready_state']} "
                      f"error={bool(row['load_error'])} PSS={row['pss_mib']} MiB", flush=True)
            except Exception as error:
                try:
                    ctl.request(args.socket, "status", timeout=2)
                except Exception:
                    row["load_error"] = f"CONTROL_FAILURE: {error}"
                    print(f"round {round_number} {name}: browser/control failure: {error}", flush=True)
                    crashed = True
                else:
                    row["load_error"] = f"PAGE_FAILURE: {error}"
                    print(f"round {round_number} {name}: page failure, browser alive: {error}", flush=True)
            rows.append(row)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            with args.output.open("w", encoding="utf-8-sig", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=fields)
                writer.writeheader()
                writer.writerows(rows)
            if crashed:
                break
        if crashed:
            break

    print(json.dumps({"output": str(args.output), "navigations": len(rows),
                      "survived": not crashed,
                      "max_pss_mib": max((float(r["pss_mib"]) for r in rows if r["pss_mib"] != ""), default=None),
                      "load_errors": sum(bool(r["load_error"]) for r in rows)}, indent=2))
    return 1 if crashed else 0


if __name__ == "__main__":
    raise SystemExit(main())
