"""Test document-local preload reuse with WebKit's shared cache disabled.

Optional dependencies: GTK/WebKit GI bindings, python-h2, OpenSSL, D-Bus and
Xvfb. Select independently installed WebKit and Soup runtimes explicitly.
Neither installed libraries nor a running Lightview instance are modified.
"""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time

from libsoup_http2_recovery import Server, digest


SCRIPT_COUNT = 18
EXPECTED_DOCUMENT = {"ready": "complete", "tail": True,
                     "text": "healthy-after-timeout"}


class PreloadServer(Server):
    def handle_http2_request(self, path, stream, respond):
        if path == "/warm":
            respond(stream, b"<!doctype html><title>warm</title><p>warm</p>",
                    content_type="text/html")
        elif path.startswith("/page?"):
            page = (b"<!doctype html><title>Preload timeout regression</title>" +
                    b"".join(f'<script src="/blocked-{i}.js"></script>'.encode()
                             for i in range(SCRIPT_COUNT)) +
                    b'<body><p id="result">healthy-after-timeout</p>'
                    b'<script>window.preloadTail=true;</script></body>')
            respond(stream, page, content_type="text/html")
        elif not path.startswith("/blocked-"):
            raise RuntimeError("Unexpected browser fixture path")
        # Blocked scripts receive no headers on any HTTP/2 connection.
        return True

    def serve_http1(self, connection, identity):
        data = b""
        while b"\r\n\r\n" not in data and not self.stopped:
            try:
                chunk = connection.recv(8192)
            except socket.timeout:
                continue
            if not chunk:
                return
            data += chunk
            if len(data) > 65536:
                raise RuntimeError("Oversized fixture headers")
        if self.stopped:
            return
        method, path, _ = data.split(b"\r\n", 1)[0].decode("ascii").split(" ", 2)
        if method != "GET" or not path.startswith("/blocked-"):
            raise RuntimeError("Unexpected fallback request")
        with self.lock:
            self.requests.append({"connection": identity, "protocol": "http/1.1",
                                  "path": path, "at": time.monotonic()})
        # Keep the peer silent, but release a connection when its client closes.
        while not self.stopped:
            try:
                if not connection.recv(8192):
                    return
            except socket.timeout:
                continue


def client(args):
    import gi
    gi.require_version("Gtk", "3.0")
    gi.require_version("WebKit2", "4.1")
    from gi.repository import Gio, GLib, Gtk, WebKit2

    loop = GLib.MainLoop()
    context = WebKit2.WebContext(website_data_manager=
                                WebKit2.WebsiteDataManager.new_ephemeral())
    context.set_sandbox_enabled(True)
    context.set_cache_model(WebKit2.CacheModel.DOCUMENT_VIEWER)
    context.allow_tls_certificate_for_host(
        Gio.TlsCertificate.new_from_file(str(args.certificate)), "127.0.0.1")
    view = WebKit2.WebView(web_context=context)
    view.get_settings().set_enable_page_cache(False)
    window = Gtk.Window()
    window.set_default_size(800, 600)
    window.add(view)
    report = {"phases": [], "resource_failures": [], "terminations": 0,
              "timeout": False, "sandbox_enabled": context.get_sandbox_enabled()}
    started = None
    warming = True

    def resource_started(view, resource, request):
        if "/blocked-" in request.get_uri():
            resource.connect("failed", lambda resource, error:
                             report["resource_failures"].append(
                                 {"domain": error.domain, "code": error.code}))

    def verify_mappings():
        tool = Path(__file__).resolve().parents[1] / "tools/lightview-memory"
        usage = json.loads(subprocess.check_output([str(tool), str(os.getpid())],
                                                   text=True, timeout=10))
        mappings = {}
        for process in usage["processes"]:
            try:
                lines = Path(f'/proc/{process["pid"]}/maps').read_text().splitlines()
                engines = sorted({line.split()[-1] for line in lines
                                  if "libwebkit2gtk-4.1.so." in line})
                if not engines:
                    continue
                soups = sorted({line.split()[-1] for line in lines
                                if "libsoup-3.0.so." in line})
                mappings[str(process["pid"])] = {
                    "name": process["name"], "engines": engines, "soups": soups}
            except OSError:
                continue
        report["mappings"] = mappings
        expected = str(args.engine_prefix / "lib") + "/"
        return (len(mappings) >= 3 and
                all(row["engines"] and all(path.startswith(expected)
                                           for path in row["engines"]) and
                    row["soups"] == [str(args.library)]
                    for row in mappings.values()))

    def next_page():
        nonlocal started
        started = time.monotonic()
        view.load_uri(args.base + f'/page?round={len(report["phases"])}')
        return False

    def evaluated(view, result, data):
        try:
            document = json.loads(view.evaluate_javascript_finish(result).to_string())
            seconds = time.monotonic() - started
            passed = document == EXPECTED_DOCUMENT and seconds < 50
            report["phases"].append({"document": document, "seconds": seconds,
                                     "passed": passed})
            if passed and len(report["phases"]) < args.pages:
                GLib.idle_add(next_page)
                return
        except Exception as error:
            report["error"] = repr(error)
        loop.quit()

    def load_changed(view, event):
        nonlocal warming
        if event != WebKit2.LoadEvent.FINISHED:
            return
        if warming:
            warming = False
            try:
                report["mappings_verified"] = verify_mappings()
            except Exception as error:
                report["mapping_error"] = repr(error)
            if not report.get("mappings_verified"):
                loop.quit()
                return
            GLib.idle_add(next_page)
            return
        view.evaluate_javascript(
            'JSON.stringify({ready:document.readyState,tail:window.preloadTail===true,'
            'text:document.getElementById("result")?.textContent})',
            -1, None, None, None, evaluated, None)

    def terminated(*unused):
        report["terminations"] += 1
        loop.quit()

    def deadline():
        report["timeout"] = True
        loop.quit()
        return False

    view.connect("resource-load-started", resource_started)
    view.connect("load-changed", load_changed)
    view.connect("web-process-terminated", terminated)
    window.show_all()
    view.load_uri(args.base + "/warm")
    GLib.timeout_add_seconds(args.pages * 50 + 5, deadline)
    loop.run()
    report["passed"] = bool(
        report.get("mappings_verified") and report["sandbox_enabled"] and
        not report["timeout"] and not report["terminations"] and
        len(report["phases"]) == args.pages and
        all(phase["passed"] for phase in report["phases"]) and
        len(report["resource_failures"]) == SCRIPT_COUNT * args.pages and
        all(error == {"domain": "g-io-error-quark", "code": 24}
            for error in report["resource_failures"]))
    view.stop_loading()
    window.destroy()
    print(json.dumps(report), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine-prefix", required=True, type=Path)
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--pages", type=int, choices=(1, 2), default=2)
    parser.add_argument("--client", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--base", help=argparse.SUPPRESS)
    parser.add_argument("--certificate", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    os.umask(0o077)
    args.engine_prefix = args.engine_prefix.resolve(strict=True)
    args.library = args.library.resolve(strict=True)
    if args.client:
        client(args)
        return 0
    if not args.output_dir:
        parser.error("--output-dir is required")
    args.output_dir.mkdir(parents=True, exist_ok=False)
    engine = args.engine_prefix / "lib/libwebkit2gtk-4.1.so.0"
    inputs = {"test_sha256": digest(__file__), "engine_sha256": digest(engine),
              "library_sha256": digest(args.library),
              "server_sha256": digest(Path(__file__).with_name("libsoup_http2_recovery.py"))}
    server = PreloadServer(args.output_dir, "preload-document")
    try:
        env = dict(os.environ)
        env.pop("SOUP_FORCE_HTTP1", None)
        env["LD_LIBRARY_PATH"] = ":".join(filter(None, (
            str(args.engine_prefix / "lib"), str(args.library.parent),
            env.get("LD_LIBRARY_PATH", ""))))
        process = subprocess.run([
            "dbus-run-session", "--", "xvfb-run", "-a", sys.executable,
            str(Path(__file__).resolve()), "--client", "--engine-prefix",
            str(args.engine_prefix), "--library", str(args.library), "--pages",
            str(args.pages), "--base", f"https://127.0.0.1:{server.port}",
            "--certificate", str(server.certificate),
        ], env=env, capture_output=True, text=True, timeout=args.pages * 50 + 25)
        (args.output_dir / "stdout.log").write_text(process.stdout)
        (args.output_dir / "stderr.log").write_text(process.stderr)
        lines = [line for line in process.stdout.splitlines() if line.startswith("{")]
        result = json.loads(lines[-1]) if lines else {"passed": False}
        requests = server.requests.copy()
        initial = Counter(row["path"] for row in requests
                          if row["protocol"] == "h2" and row["path"].startswith("/blocked-"))
        fallback = [row for row in requests if row["protocol"] == "http/1.1"]
        unchanged = (digest(__file__) == inputs["test_sha256"] and
                     digest(engine) == inputs["engine_sha256"] and
                     digest(args.library) == inputs["library_sha256"] and
                     digest(Path(__file__).with_name("libsoup_http2_recovery.py")) ==
                     inputs["server_sha256"])
        passed = (result["passed"] and process.returncode == 0 and unchanged and
                  not server.errors and len(fallback) >= 2 * args.pages and
                  initial == Counter({f"/blocked-{i}.js": args.pages
                                      for i in range(SCRIPT_COUNT)}))
        report = {"passed": bool(passed), "inputs": inputs, "client": result,
                  "server_requests": requests, "server_errors": server.errors,
                  "inputs_unchanged": unchanged, "returncode": process.returncode,
                  "formal_endurance_eligible": False}
        (args.output_dir / "results.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps({"passed": bool(passed), "output": str(args.output_dir)}))
        return 0 if passed else 1
    finally:
        server.close()


if __name__ == "__main__":
    raise SystemExit(main())
