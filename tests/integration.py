"""Exercise the real GTK/WebKit browser through its public control socket."""
import contextlib
import csv
import fcntl
import http.server
import importlib.machinery
import importlib.util
import io
import json
import os
import re
import signal
from pathlib import Path
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
BROWSER = Path(os.environ.get("LIGHTVIEW_TEST_BROWSER", ROOT / "build/lightview"))
loader = importlib.machinery.SourceFileLoader("lightviewctl", str(ROOT / "tools/lightviewctl"))
spec = importlib.util.spec_from_loader(loader.name, loader)
ctl = importlib.util.module_from_spec(spec)
loader.exec_module(ctl)
stability_spec = importlib.util.spec_from_file_location(
    "stability_test", ROOT / "tools/stability-test.py")
stability = importlib.util.module_from_spec(stability_spec)
stability_spec.loader.exec_module(stability)

PAGE = b"""<!doctype html><meta charset="utf-8"><title>Lightview test</title>
<input id="field"><textarea id="notes"></textarea>
<button id="button" onclick="this.textContent='clicked'">Click</button>
<a id="next" href="/next" target="_blank">Next</a>
<a id="download" href="/download">Download</a>
<img id="image" src="/image.svg" alt="fixture">
<script type="module">
const answer = await fetch('/api').then(response => response.json());
document.body.dataset.answer = answer.value?.toString() ?? 'missing';
document.querySelector('#field').addEventListener('input', event => {
  document.body.dataset.input = event.target.value;
});
</script>"""


class Handler(http.server.BaseHTTPRequestHandler):
    retry_lock = threading.Lock()
    retry_requests = 0
    rebuild_requests = 0

    def do_GET(self):
        if self.path == "/resource-trace-page":
            payload = (b'<!doctype html><title>Resource trace fixture</title>' +
                       b''.join(f'<img src="/trace-private-path/{i}.svg?trace-private-query=sentinel">'.encode()
                                for i in range(270)))
            content_type = "text/html; charset=utf-8"
        elif self.path.startswith("/trace-private-path/"):
            payload = b'<svg xmlns="http://www.w3.org/2000/svg" width="8" height="6"></svg>'
            content_type = "image/svg+xml"
        elif self.path == "/media.mp4":
            payload = (ROOT / "tests/fixtures/media.mp4").read_bytes()
            content_type = "video/mp4"
        elif self.path == "/api":
            payload, content_type = b'{"value":42}', "application/json"
        elif self.path == "/empty-page":
            payload = b'<!doctype html><title>Titled empty fixture</title><body></body>'
            content_type = "text/html; charset=utf-8"
        elif self.path == "/blocked-head":
            payload = (b'<!doctype html><html><head><title>Blocked head fixture</title>'
                       b'<link rel="stylesheet" href="/delayed-style.css">'
                       b'<script>window.headScriptRan=true;</script></head>'
                       b'<body><p id="content">Delayed but valid document</p></body></html>')
            content_type = "text/html; charset=utf-8"
        elif self.path == "/delayed-style.css":
            time.sleep(8)
            payload, content_type = b'body { color: green; }', "text/css"
        elif self.path == "/deferred-document":
            payload = (b'<!doctype html><title>Deferred script fixture</title>'
                       b'<script defer src="/delayed-script.js"></script>'
                       b'<p>Document awaiting its deferred script</p>')
            content_type = "text/html; charset=utf-8"
        elif self.path == "/delayed-script.js":
            time.sleep(8)
            payload, content_type = b'window.deferredReady=true;', "text/javascript"
        elif self.path == "/probe-wedge":
            payload = (b'<!doctype html><title>Load probe cancellation fixture</title>'
                       b'<img src="/slow-resource"><script>'
                       b'setTimeout(()=>{while(true){}},100);</script>')
            content_type = "text/html; charset=utf-8"
        elif self.path == "/slow-page":
            payload = (b'<!doctype html><meta charset="utf-8">'
                       b'<title>Slow resource fixture</title>'
                       b'<img src="/slow-resource">')
            content_type = "text/html; charset=utf-8"
        elif self.path == "/slow-main":
            time.sleep(3)
            payload = (b'<!doctype html><meta charset="utf-8">'
                       b'<title>Slow main fixture</title>')
            content_type = "text/html; charset=utf-8"
        elif self.path == "/slow-wake-main":
            time.sleep(12)
            payload = (b'<!doctype html><meta charset="utf-8">'
                       b'<title>Slow wake fixture</title>')
            content_type = "text/html; charset=utf-8"
        elif self.path == "/retry-main":
            with self.retry_lock:
                type(self).retry_requests += 1
                request_number = type(self).retry_requests
            if request_number == 1:
                time.sleep(10)
            payload = (b'<!doctype html><meta charset="utf-8">'
                       b'<title>Retry main fixture</title>')
            content_type = "text/html; charset=utf-8"
        elif self.path == "/rebuild-main":
            with self.retry_lock:
                type(self).rebuild_requests += 1
                request_number = type(self).rebuild_requests
            if request_number <= 3:
                time.sleep(10)
            payload = (b'<!doctype html><meta charset="utf-8">'
                       b'<title>Rebuild main fixture</title>')
            content_type = "text/html; charset=utf-8"
        elif self.path == "/always-slow-main":
            time.sleep(30)
            payload = (b'<!doctype html><meta charset="utf-8">'
                       b'<title>Always slow main fixture</title>')
            content_type = "text/html; charset=utf-8"
        elif self.path == "/slow-resource":
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(1024 * 1024))
            self.end_headers()
            self.wfile.write(b"x")
            self.wfile.flush()
            time.sleep(15)
            return
        elif self.path == "/download":
            payload, content_type = b"download fixture\n", "application/octet-stream"
        elif self.path == "/image.svg":
            payload = b'<svg xmlns="http://www.w3.org/2000/svg" width="8" height="6"><rect width="8" height="6" fill="green"/></svg>'
            content_type = "image/svg+xml"
        else:
            payload, content_type = PAGE, "text/html; charset=utf-8"
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        if self.path == "/download":
            self.send_header("Content-Disposition", 'attachment; filename="fixture.txt"')
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        try:
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *args):
        pass


@contextlib.contextmanager
def browser(directory, *options, allow_main_exit=False, capture_output=None):
    path = str(Path(directory) / "control.sock")
    with tempfile.TemporaryFile(mode="w+") as log:
        process = subprocess.Popen([str(BROWSER), "--socket", path, *options],
                                   stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 20
            while True:
                if process.poll() is not None:
                    log.seek(0)
                    raise RuntimeError(f"Browser exited ({process.returncode}):\n{log.read()}")
                try:
                    ctl.request(path, "status", timeout=1)
                    break
                except (OSError, RuntimeError):
                    if time.monotonic() > deadline:
                        log.seek(0)
                        raise RuntimeError(f"Browser did not start:\n{log.read()}")
                    time.sleep(0.1)
            if os.environ.get("LIGHTVIEW_TEST_BROWSER"):
                fields = {}
                for line in Path(f"/proc/{process.pid}/status").read_text().splitlines():
                    if ":" in line:
                        key, value = line.split(":", 1)
                        fields[key] = value.strip()
                expected_uid = str(os.getuid())
                if fields.get("Uid", "").split() != [expected_uid] * 4:
                    raise AssertionError(f"launcher did not restore caller UID: {fields.get('Uid')}")
                if fields.get("CapEff") != "0000000000000000":
                    raise AssertionError(f"launcher retained capabilities: {fields.get('CapEff')}")
                if fields.get("NoNewPrivs") != "1":
                    raise AssertionError("launcher did not enable no_new_privs")
            yield path, process
        finally:
            exited_before_cleanup = process.poll() is not None
            if process.poll() is None:
                try:
                    ctl.request(path, "quit", timeout=2)
                    process.wait(timeout=5)
                except (OSError, RuntimeError, subprocess.TimeoutExpired):
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
            log.seek(0)
            output = log.read()
            if capture_output is not None:
                capture_output.append(output)
            if (exited_before_cleanup or process.returncode) and not allow_main_exit:
                raise AssertionError(
                    f"Browser main process exited unexpectedly ({process.returncode}):\n{output}")
            fatal_markers = ("Assertion `", "ASSERTION FAILED:", "FATAL ERROR:",
                             "Segmentation fault", "core dumped")
            if any(marker in output for marker in fatal_markers):
                raise AssertionError(f"Browser or WebKit subprocess failed:\n{output}")
            if "CRITICAL" in output or "WARNING" in output:
                print(output)


class BrowserIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.url = f"http://127.0.0.1:{cls.server.server_port}"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()

    def cli(self, path, *arguments, success=True):
        result = subprocess.run([str(ROOT / "tools/lightviewctl"), "--socket", path,
                                 "--timeout", "10", *arguments],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode == 0, success, result.stderr)
        return json.loads(result.stdout) if success else result.stderr

    def test_resource_trace_exceeds_timing_buffer_without_url_secrets(self):
        logs = []
        with tempfile.TemporaryDirectory() as directory, mock.patch.dict(os.environ, {
                "LIGHTVIEW_TRACE_RESOURCES": "1", "LIGHTVIEW_TRACE_NAVIGATION": "0"}):
            with browser(directory, "--private", capture_output=logs) as (path, _):
                self.cli(path, "open", self.url + "/resource-trace-page", "--wait")
                deadline = time.monotonic() + 30
                while not ctl.request(path, "eval", script=(
                        "document.images.length===270 && [...document.images].every("
                        "image=>image.complete && image.naturalWidth===8)")):
                    self.assertLess(time.monotonic(), deadline, "Fixture images did not load")
                    time.sleep(0.1)
                self.assertEqual(ctl.request(path, "eval", script=(
                    "performance.getEntriesByType('resource').length")), 250)
        lines = [line for line in logs[0].splitlines() if "Resource trace:" in line]
        events = [dict(re.findall(r"([\w-]+)=([^ ]+)", line)) for line in lines]
        finished = {event["id"] for event in events
                    if event["phase"] == "finished" and event["failed"] == "0"}
        self.assertGreaterEqual(len(finished), 271)
        started = {event["id"] for event in events if event["phase"] == "started"}
        self.assertTrue(finished <= started)
        self.assertTrue(any(event["main"] == "0" and event["phase"] == "response"
                            and event["status"] == "200" for event in events))
        for secret in ("trace-private-path", "trace-private-query", "sentinel"):
            self.assertNotIn(secret, "\n".join(lines))
        self.assertNotIn("Navigation trace:", logs[0])

    def test_resource_trace_disabled_by_default(self):
        logs = []
        with tempfile.TemporaryDirectory() as directory, mock.patch.dict(os.environ, {
                "LIGHTVIEW_TRACE_RESOURCES": "0", "LIGHTVIEW_TRACE_NAVIGATION": "0"}):
            with browser(directory, "--private", capture_output=logs) as (path, _):
                self.cli(path, "open", self.url, "--wait")
        self.assertNotIn("Resource trace:", logs[0])

    def test_resource_trace_records_cancelled_subresource(self):
        logs = []
        with tempfile.TemporaryDirectory() as directory, mock.patch.dict(os.environ, {
                "LIGHTVIEW_TRACE_RESOURCES": "1", "LIGHTVIEW_TRACE_NAVIGATION": "0"}):
            with browser(directory, "--private", capture_output=logs) as (path, _):
                ctl.request(path, "open", uri=self.url + "/slow-page")
                deadline = time.monotonic() + 10
                while not ctl.request(path, "eval", script="document.images.length===1"):
                    self.assertLess(time.monotonic(), deadline, "Slow image was not requested")
                    time.sleep(0.1)
                self.cli(path, "open", "about:blank", "--wait")
        events = [dict(re.findall(r"([\w-]+)=([^ ]+)", line))
                  for line in logs[0].splitlines() if "Resource trace:" in line]
        failures = [event for event in events
                    if event["phase"] == "failed" and event["main"] == "0"]
        self.assertTrue(failures)
        for failed in failures:
            self.assertEqual(failed["failed"], "1")
            self.assertNotEqual(failed["error-domain"], "none")
            self.assertTrue(any(event["id"] == failed["id"] and
                                event["sequence"] == failed["sequence"] and
                                event["phase"] == "finished" and event["failed"] == "1"
                                for event in events))

    def descendant_processes(self, root_pid, name):
        parents = {}
        names = {}
        for entry in Path("/proc").iterdir():
            if not entry.name.isdecimal():
                continue
            try:
                fields = (entry / "stat").read_text().rsplit(")", 1)[1].split()
                pid = int(entry.name)
                parents[pid] = int(fields[1])
                names[pid] = (entry / "comm").read_text().strip()
            except (OSError, IndexError, ValueError):
                continue
        selected = {root_pid}
        while True:
            children = {pid for pid, parent in parents.items() if parent in selected}
            if children <= selected:
                break
            selected |= children
        return {pid for pid in selected if names.get(pid) == name}

    def web_processes(self, root_pid):
        return self.descendant_processes(root_pid, "WebKitWebProces")

    def network_processes(self, root_pid):
        return self.descendant_processes(root_pid, "WebKitNetworkPr")

    def run_stability_fixture(self, path, output, sites):
        arguments = ["stability-test.py", "--socket", path, "--rounds", "1",
                     "--load-timeout", "0.25", "--settle", "0.1",
                     "--output", str(output)]
        with (mock.patch.object(stability, "SITES", sites),
              mock.patch.object(sys, "argv", arguments),
              contextlib.redirect_stdout(io.StringIO())):
            result = stability.main()
        with output.open(encoding="utf-8-sig", newline="") as stream:
            return result, list(csv.DictReader(stream))

    def test_stability_runner_probes_a_continuously_loading_document(self):
        with tempfile.TemporaryDirectory() as directory:
            with browser(directory, "--private", "--control-timeout", "30") as (path, _):
                result, rows = self.run_stability_fixture(
                    path, Path(directory) / "results.csv",
                    [("Slow fixture", self.url + "/slow-page"),
                     ("Regular fixture", self.url)])
                self.assertEqual(result, 0, rows)
                self.assertEqual(len(rows), 2)
                self.assertEqual(rows[0]["load_timed_out"], "True")
                self.assertIn(rows[0]["ready_state"], ("interactive", "complete"))
                self.assertGreater(int(rows[0]["body_elements"]), 0)
                self.assertEqual(rows[0]["load_stop_count_after"], "0")
                self.assertTrue(all(row["reset_during_site"] == "False"
                                    for row in rows))

    def test_stability_runner_rejects_a_titled_empty_document(self):
        with tempfile.TemporaryDirectory() as directory:
            with browser(directory, "--private") as (path, _):
                result, rows = self.run_stability_fixture(
                    path, Path(directory) / "results.csv",
                    [("Empty fixture", self.url + "/empty-page")])
                self.assertEqual(result, 1, rows)
                self.assertEqual(rows[0]["title"], "Titled empty fixture")
                self.assertEqual(rows[0]["body_elements"], "0")
                self.assertEqual(rows[0]["body_text_length"], "0")

    def test_load_watchdog_does_not_truncate_a_parser_blocked_in_head(self):
        with tempfile.TemporaryDirectory() as directory:
            with browser(directory, "--private", "--control-timeout", "4") as (path, process):
                before = self.cli(path, "status")
                ctl.request(path, "open", uri=self.url + "/blocked-head")
                ctl.wait_for(path, "document.title === 'Blocked head fixture'", 5)
                self.assertFalse(ctl.request(path, "eval", script="!!document.body"))
                ctl.wait_for(path, "!!document.querySelector('#content') && "
                             "document.readyState !== 'loading'", 12)
                state = self.cli(path, "status")
                self.assertEqual(state["pid"], process.pid)
                self.assertEqual(state["load_stop_count"], before["load_stop_count"])
                self.assertEqual(state["reset_count"], before["reset_count"])
                self.assertEqual(state["web_process_generation"], before["web_process_generation"])

    def test_load_watchdog_preserves_a_pending_deferred_script(self):
        with tempfile.TemporaryDirectory() as directory:
            with browser(directory, "--private", "--control-timeout", "4") as (path, _):
                before = self.cli(path, "status")
                ctl.request(path, "open", uri=self.url + "/deferred-document")
                ctl.wait_for(path, "document.readyState === 'interactive'", 5)
                self.assertFalse(ctl.request(path, "eval", script="!!window.deferredReady"))
                ctl.wait_for(path, "window.deferredReady === true", 12)
                state = self.cli(path, "status")
                self.assertEqual(state["load_stop_count"], before["load_stop_count"])
                self.assertEqual(state["reset_count"], before["reset_count"])

    def test_stability_runner_waits_for_document_parsing_within_control_budget(self):
        with tempfile.TemporaryDirectory() as directory:
            with browser(directory, "--private", "--control-timeout", "15") as (path, _):
                result, rows = self.run_stability_fixture(
                    path, Path(directory) / "results.csv",
                    [("Delayed parser", self.url + "/blocked-head")])
                self.assertEqual(result, 0, rows)
                self.assertIn(rows[0]["ready_state"], ("interactive", "complete"))
                self.assertGreater(int(rows[0]["body_elements"]), 0)
                self.assertEqual(rows[0]["load_stop_count_after"], "0")

    def test_reset_cancels_a_blocked_internal_load_probe(self):
        with tempfile.TemporaryDirectory() as directory:
            with browser(directory, "--private", "--control-timeout", "4") as (path, process):
                before = self.cli(path, "status")
                ctl.request(path, "open", uri=self.url + "/probe-wedge")
                time.sleep(6.5)
                old_web = self.web_processes(process.pid)
                operation = ctl.request(path, "reset", hard=True, timeout=3)
                ctl.wait_for_recovery(path, operation, 15)
                self.cli(path, "open", self.url, "--wait")
                time.sleep(4)
                state = self.cli(path, "status")
                self.assertEqual(state["pid"], process.pid)
                self.assertEqual(state["reset_count"], before["reset_count"] + 1)
                self.assertEqual(state["load_stop_count"], before["load_stop_count"])
                self.assertFalse(old_web & self.web_processes(process.pid))

    def test_navigation_target_matching(self):
        self.assertTrue(ctl.navigation_target_reached(
            "example.com", "https://www.example.com/"))
        self.assertTrue(ctl.navigation_target_reached(
            "https://www.wikipedia.org/", "https://en.wikipedia.org/wiki/Main_Page"))
        self.assertTrue(ctl.navigation_target_reached(
            "https://example.com/path/", "http://www.example.com/path"))
        self.assertFalse(ctl.navigation_target_reached(
            "https://example.com/path", "https://example.com/other"))
        self.assertFalse(ctl.navigation_target_reached(
            "https://en.wikipedia.org/wiki/WebKit", "https://www.amazon.com/"))
        self.assertFalse(ctl.navigation_target_reached("about:blank", "https://example.com/"))

    def test_webaudio_output_device_probe(self):
        for options in ((), ("--low-memory",)):
            with self.subTest(options=options), tempfile.TemporaryDirectory() as directory:
                with browser(directory, "--private", *options) as (path, process):
                    self.cli(path, "open", self.url, "--wait")
                    before = self.cli(path, "status")
                    result = self.cli(path, "eval", """(async () => {
                        const values = [];
                        for (let i = 0; i < 3; i++) {
                            const context = new AudioContext();
                            values.push(context.destination.maxChannelCount);
                            await context.close();
                        }
                        return values;
                    })()""")
                    self.assertEqual(len(result), 3)
                    self.assertTrue(all(isinstance(value, int) and value >= 0
                                        for value in result), result)
                    after = self.cli(path, "status")
                    self.assertEqual(after["pid"], process.pid)
                    self.assertEqual(after["reset_count"], before["reset_count"])
                    self.assertEqual(after["web_process_generation"],
                                     before["web_process_generation"])

    def test_xml_xslt_and_international_text(self):
        # Exercise the XML/XSLT allocator boundary and ICU in the real process.
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private") as (path, _):
                self.cli(path, "open", self.url, "--wait")
                result = self.cli(path, "eval", """(() => {
                    const parser = new DOMParser();
                    const xml = parser.parseFromString('<root><name>世界</name></root>', 'text/xml');
                    const sheet = parser.parseFromString(`<xsl:stylesheet version="1.0"
                        xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
                        <xsl:output method="xml"/><xsl:template match="/">
                        <result><xsl:value-of select="root/name"/></result>
                        </xsl:template></xsl:stylesheet>`, 'text/xml');
                    const processor = new XSLTProcessor();
                    processor.importStylesheet(sheet);
                    const output = processor.transformToDocument(xml);
                    return {text: output.documentElement.textContent,
                        xml: new XMLSerializer().serializeToString(output),
                        normalized: 'e\\u0301'.normalize('NFC'),
                        upper: 'istanbul'.toLocaleUpperCase('tr-TR'),
                        date: new Intl.DateTimeFormat('en-US', {timeZone: 'UTC',
                            year: 'numeric'}).format(new Date('2026-01-01T00:00:00Z'))};
                })()""")
                self.assertEqual(result["text"], "世界")
                self.assertIn("<result>世界</result>", result["xml"])
                self.assertEqual(result["normalized"], "é")
                self.assertEqual(result["upper"], "İSTANBUL")
                self.assertEqual(result["date"], "2026")

    def test_standalone_media_navigation(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private") as (path, process):
                initial = self.cli(path, "status")
                destination = self.url + "/media.mp4"
                self.cli(path, "open", destination, "--wait")
                state = self.cli(path, "status")
                self.assertIsNone(state["load_error"])
                self.assertEqual(state["committed_uri"], destination)
                self.assertEqual(state["pid"], process.pid)
                self.assertEqual(state["reset_count"], initial["reset_count"])
                self.assertEqual(state["web_process_generation"],
                                 initial["web_process_generation"])
                self.assertTrue(self.cli(path, "eval",
                    "!!document.querySelector('audio, video')"))

    def test_navigation_javascript_and_client(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private") as (path, process):
                self.assertEqual(stat.S_IMODE(os.stat(path).st_mode), 0o700)
                self.cli(path, "open", self.url, "--wait")
                self.cli(path, "wait", "document.body.dataset.answer === '42'")
                state = self.cli(path, "status")
                self.assertEqual(state["title"], "Lightview test")
                self.assertTrue(state["private"])
                self.assertFalse(state["low_memory"])
                self.assertEqual(state["memory_limit_mib"], 768)
                self.assertEqual(state["pid"], process.pid)
                self.assertFalse(state["reset_flash_active"])
                self.assertEqual(state["running_mode"], "Private")
                self.assertGreater(state["ram_pss_mib"], 0)
                self.assertGreaterEqual(state["cpu_percent"], 0)
                self.assertGreaterEqual(state["resource_processes"], 1)
                self.assertEqual(state["control_timeout_seconds"], 60)
                self.assertEqual(state["control_clients"], 1)
                self.assertEqual(state["page_requests_active"], 0)
                self.assertFalse(state["navigation_watchdog_active"])
                self.assertEqual(state["running_mode_short"], "P")
                self.assertIn(" · MODE:P, ", state["window_title"])
                self.assertIn("M, CPU ", state["window_title"])
                self.assertTrue(state["toolbar_telemetry"].startswith("MODE:P, "))
                self.assertIn("M, CPU ", state["toolbar_telemetry"])
                self.assertEqual(self.cli(path, "eval", "({answer: 6 * 7, list: [true, null]})"),
                                 {"answer": 42, "list": [True, None]})
                self.assertEqual(self.cli(path, "eval", "new Promise(r => setTimeout(() => r(42), 50))"), 42)
                self.assertIsNone(self.cli(path, "eval", "undefined"))
                self.assertIn("failure", self.cli(path, "eval", "(() => {throw Error('failure')})()", success=False))
                self.assertIn("rejected", self.cli(path, "eval", "Promise.reject(Error('rejected'))", success=False))
                self.cli(path, "eval", "(() => {const value = {}; value.self = value; return value;})()", success=False)
                self.assertEqual(self.cli(path, "eval", "6 * 7"), 42)
                self.cli(path, "fill", "#field", "quotes '\" and Unicode 世界")
                self.assertEqual(self.cli(path, "eval", "document.body.dataset.input"), "quotes '\" and Unicode 世界")
                self.cli(path, "fill", "#notes", "a\nb")
                self.assertEqual(self.cli(path, "eval", "document.querySelector('#notes').value"), "a\nb")
                self.cli(path, "click", "#button")
                self.assertEqual(self.cli(path, "eval", "document.querySelector('#button').textContent"), "clicked")
                self.cli(path, "click", "#absent", success=False)
                self.cli(path, "open", self.url + "/next", "--wait")
                self.cli(path, "back")
                self.cli(path, "wait", f"location.href === '{self.url}/' && document.readyState === 'complete'")
                self.cli(path, "forward")
                self.cli(path, "wait", "location.pathname === '/next' && document.readyState === 'complete'")
                self.cli(path, "reload")
                self.cli(path, "wait")
                self.cli(path, "eval", "(() => { window.beforeReset = 42; localStorage.setItem('across-reset', 'yes'); return true; })()")
                before_reset = self.cli(path, "status")
                socket_inode = os.stat(path).st_ino
                reset = self.cli(path, "reset")
                self.assertEqual(reset["engine_state"], "ready")
                self.assertGreater(reset["web_process_generation"],
                                   before_reset["web_process_generation"])
                self.cli(path, "wait", "location.href === 'about:blank' && document.readyState === 'complete'")
                self.assertEqual(self.cli(path, "eval", "typeof window.beforeReset"), "undefined")
                after_reset = self.cli(path, "status")
                self.assertEqual(after_reset["pid"], process.pid)
                self.assertEqual(os.stat(path).st_ino, socket_inode)
                self.assertEqual(after_reset["engine_state"], "ready")
                self.assertEqual(after_reset["last_termination_reason"], "automation-request")
                self.assertEqual(after_reset["last_committed_uri"], self.url + "/next")
                self.assertEqual(after_reset["committed_uri"], "about:blank")
                self.assertTrue(after_reset["reset_flash_active"])
                self.cli(path, "open", self.url, "--wait")
                self.assertEqual(self.cli(path, "eval", "localStorage.getItem('across-reset')"), "yes")
                memory = subprocess.check_output([str(ROOT / "tools/lightview-memory"), str(process.pid)], text=True)
                report = json.loads(memory)
                self.assertGreater(report["total_rss_mib"], 0)
                print(f"Fixture memory: PSS {report['total_pss_mib']} MiB, RSS {report['total_rss_mib']} MiB, complete={report['complete']}")
            self.assertFalse(Path(path).exists())
            self.assertEqual(process.returncode, 0)

    def test_protocol_validation_and_socket_ownership(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private", allow_main_exit=True) as (path, process):
                for payload in (b"oops\n", b"[]\n", b'{"command":42}\n',
                                b'{"command":"eval","script":false}\n',
                                b'{"command":"open","uri":null}\n',
                                b'{"command":"bogus"}\n', b"x" * (1024 * 1024 + 1) + b"\n"):
                    with socket.socket(socket.AF_UNIX) as connection:
                        connection.settimeout(5)
                        connection.connect(path)
                        connection.sendall(payload)
                        response = json.loads(connection.makefile("rb").readline())
                        self.assertFalse(response["ok"])
                with socket.socket(socket.AF_UNIX) as stalled:
                    stalled.connect(path)
                    stalled.sendall(b'{"command":')
                    self.assertEqual(ctl.request(path, "status")["pid"], process.pid)
                collision = subprocess.run([str(BROWSER), "--private", "--socket", path],
                                           capture_output=True, text=True, timeout=10)
                self.assertNotEqual(collision.returncode, 0)
                self.assertEqual(ctl.request(path, "status")["pid"], process.pid)
                self.assertIn("Control socket is already in use", collision.stderr)
                # Incremental framing works when a request arrives in multiple chunks.
                with socket.socket(socket.AF_UNIX) as connection:
                    connection.settimeout(5)
                    connection.connect(path)
                    connection.sendall(b'{"command":')
                    connection.sendall(b'"status"}\n')
                    self.assertTrue(json.loads(connection.makefile("rb").readline())["ok"])
                process.terminate()
                process.wait(timeout=5)
                self.assertFalse(Path(path).exists())

    def test_reset_cancels_page_operation_with_retryable_error(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private") as (path, process):
                response = {}
                sent = threading.Event()

                def long_evaluation():
                    payload = json.dumps({
                        "command": "eval",
                        "script": "new Promise(resolve => setTimeout(() => resolve(42), 10000))",
                    }).encode() + b"\n"
                    with socket.socket(socket.AF_UNIX) as connection:
                        connection.settimeout(10)
                        connection.connect(path)
                        connection.sendall(payload)
                        sent.set()
                        response.update(json.loads(connection.makefile("rb").readline()))

                worker = threading.Thread(target=long_evaluation)
                worker.start()
                self.assertTrue(sent.wait(timeout=2))
                time.sleep(0.1)
                operation = ctl.request(path, "reset", timeout=5)
                worker.join(timeout=5)
                self.assertFalse(worker.is_alive())
                self.assertFalse(response["ok"])
                self.assertEqual(response["error_code"], "webkit_reset")
                self.assertTrue(response["retryable"])
                deadline = time.monotonic() + 10
                while True:
                    state = ctl.request(path, "status", timeout=2)
                    if (state["engine_state"] == "ready" and
                            state["web_process_generation"] >= operation["target_generation"]):
                        break
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(0.1)
                self.assertEqual(state["pid"], process.pid)
                self.assertEqual(ctl.request(path, "eval", timeout=2, script="6 * 7"), 42)

    def test_unexpected_termination_during_page_operation_is_retryable(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private",
                         "--memory-recycle-threshold", "0") as (path, process):
                self.cli(path, "open", self.url, "--wait")
                response = {}
                sent = threading.Event()

                def long_evaluation():
                    payload = json.dumps({
                        "command": "eval",
                        "script": "new Promise(resolve => setTimeout(() => resolve(42), 10000))",
                    }).encode() + b"\n"
                    with socket.socket(socket.AF_UNIX) as connection:
                        connection.settimeout(10)
                        connection.connect(path)
                        connection.sendall(payload)
                        sent.set()
                        response.update(json.loads(connection.makefile("rb").readline()))

                worker = threading.Thread(target=long_evaluation)
                worker.start()
                self.assertTrue(sent.wait(timeout=2))
                time.sleep(0.1)
                web_processes = self.web_processes(process.pid)
                self.assertTrue(web_processes, "WebKit web process was not found")
                os.kill(max(web_processes), signal.SIGKILL)
                worker.join(timeout=5)
                self.assertFalse(worker.is_alive())
                self.assertFalse(response["ok"])
                self.assertEqual(response["error_code"], "webkit_reset")
                self.assertTrue(response["retryable"])
                deadline = time.monotonic() + 15
                while True:
                    state = self.cli(path, "status")
                    if (state["engine_state"] == "ready" and
                            state["uri"].rstrip("/") == self.url.rstrip("/") and
                            not state["loading"]):
                        break
                    self.assertNotEqual(state["engine_state"], "failed", state)
                    self.assertLess(time.monotonic(), deadline, state)
                    time.sleep(0.1)
                self.assertEqual(state["pid"], process.pid)
                self.assertEqual(state["page_requests_active"], 0)
                self.assertEqual(state["control_clients"], 1)
                self.assertEqual(self.cli(path, "eval", "document.title"),
                                 "Lightview test")

    def test_successful_automatic_recoveries_do_not_form_a_failure_burst(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private", "--memory-recycle-threshold", "0") as (path, process):
                self.cli(path, "open", self.url, "--wait")
                for expected_resets in range(1, 4):
                    web_processes = self.web_processes(process.pid)
                    self.assertTrue(web_processes, "WebKit web process was not found")
                    os.kill(max(web_processes), signal.SIGKILL)
                    deadline = time.monotonic() + 15
                    while True:
                        state = self.cli(path, "status")
                        if (state["engine_state"] == "ready" and
                                state["reset_count"] >= expected_resets and
                                state["uri"].rstrip("/") == self.url.rstrip("/") and
                                not state["loading"]):
                            break
                        self.assertNotEqual(state["engine_state"], "failed", state)
                        self.assertLess(time.monotonic(), deadline, state)
                        time.sleep(0.1)
                    self.assertEqual(self.cli(path, "eval", "document.title"),
                                     "Lightview test")
                self.assertEqual(state["reset_count"], 3)
                self.assertEqual(self.cli(path, "eval", "6 * 7"), 42)

    def test_proactive_memory_recycle_preserves_socket_and_navigation(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            profile = str(Path(directory) / "profile")
            with browser(directory, "--profile", profile,
                         "--memory-recycle-threshold", "512") as (path, process):
                self.cli(path, "open", self.url, "--wait")
                cookie = self.cli(path, "eval",
                    "(document.cookie='lightview-recycle=preserved; Max-Age=3600; path=/',"
                    "document.cookie)")
                self.assertIn("lightview-recycle=preserved", cookie)
                network_before = self.network_processes(process.pid)
                self.assertEqual(len(network_before), 1)
                before = self.cli(path, "status")
                self.assertEqual(self.cli(path, "eval",
                    "(window.memoryFixture=new Uint8Array(640*1024*1024),"
                    "memoryFixture.fill(1),memoryFixture.byteLength)"), 640 * 1024 * 1024)
                socket_inode = os.stat(path).st_ino
                # Navigate immediately, before the two-second UI/resource
                # sampler can refresh its cached PSS. The navigation decision
                # must take its own current process-tree sample.
                self.cli(path, "open", self.url + "/next")
                deadline = time.monotonic() + 15
                while True:
                    state = self.cli(path, "status")
                    if (state["engine_state"] == "ready" and
                            state["last_committed_uri"] == self.url + "/next"):
                        break
                    self.assertNotEqual(state["engine_state"], "failed", state)
                    self.assertLess(time.monotonic(), deadline, state)
                    time.sleep(0.1)
                self.assertEqual(state["pid"], process.pid)
                self.assertEqual(os.stat(path).st_ino, socket_inode)
                self.assertGreater(state["web_process_generation"], before["web_process_generation"])
                self.assertEqual(state["proactive_recycle_count"], 1)
                self.assertEqual(state["network_process_recycle_count"], 1)
                self.assertEqual(state["last_termination_reason"], "memory-maintenance")
                self.assertEqual(self.cli(path, "eval", "document.title"), "Lightview test")
                self.assertIn("lightview-recycle=preserved",
                              self.cli(path, "eval", "document.cookie"))
                network_after = self.network_processes(process.pid)
                self.assertEqual(len(network_after), 1)
                self.assertTrue(network_before.isdisjoint(network_after))

    def test_proactive_recycle_with_empty_cookie_jar_completes(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            profile = str(Path(directory) / "profile")
            with browser(directory, "--profile", profile,
                         "--memory-recycle-threshold", "512") as (path, process):
                before = self.cli(path, "status")
                self.assertEqual(before["uri"], "about:blank")
                # Exceed 512 MiB with the allocation itself. A 320 MiB fixture
                # depends on variable PSS attribution of shared library pages.
                self.assertEqual(self.cli(path, "eval",
                    "(window.memoryFixture=new Uint8Array(640*1024*1024),"
                    "memoryFixture.fill(1),memoryFixture.byteLength)"), 640 * 1024 * 1024)
                self.cli(path, "open", self.url, "--wait")
                state = self.cli(path, "status")
                self.assertEqual(state["pid"], process.pid)
                self.assertEqual(state["proactive_recycle_count"], 1)
                self.assertEqual(state["network_process_recycle_count"], 1)
                self.assertEqual(state["last_termination_reason"], "memory-maintenance")
                self.assertEqual(state["uri"].rstrip("/"), self.url.rstrip("/"))
                self.assertEqual(self.cli(path, "eval", "document.title"),
                                 "Lightview test")

    def test_network_only_recycle_preserves_web_process_and_session(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            profile = str(Path(directory) / "profile")
            with browser(directory, "--profile", profile,
                         "--network-recycle-threshold", "1") as (path, process):
                self.cli(path, "open", self.url, "--wait")
                self.assertIn("network-session=preserved", self.cli(path, "eval",
                    "(document.cookie='network-session=preserved; path=/',"
                    "document.cookie)"))
                before = self.cli(path, "status")
                network_before = self.network_processes(process.pid)
                self.assertEqual(len(network_before), 1)
                socket_inode = os.stat(path).st_ino

                self.cli(path, "open", self.url + "/next", "--wait")
                after = self.cli(path, "status")
                network_after = self.network_processes(process.pid)
                self.assertEqual(after["pid"], process.pid)
                self.assertEqual(os.stat(path).st_ino, socket_inode)
                self.assertEqual(after["web_process_generation"],
                                 before["web_process_generation"])
                self.assertEqual(after["reset_count"], before["reset_count"])
                self.assertGreater(after["network_process_recycle_count"],
                                   before["network_process_recycle_count"])
                self.assertEqual(len(network_after), 1)
                self.assertTrue(network_before.isdisjoint(network_after))
                self.assertIn("network-session=preserved",
                              self.cli(path, "eval", "document.cookie"))

    def test_network_thread_guard_recycles_only_network_process(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private",
                         "--network-recycle-threshold", "0",
                         "--network-thread-recycle-threshold", "1") as (path, process):
                before = self.cli(path, "status")
                socket_inode = os.stat(path).st_ino
                self.cli(path, "open", self.url, "--wait")
                after = self.cli(path, "status")
                self.assertEqual(after["pid"], process.pid)
                self.assertEqual(os.stat(path).st_ino, socket_inode)
                self.assertEqual(after["web_process_generation"],
                                 before["web_process_generation"])
                self.assertEqual(after["reset_count"], before["reset_count"])
                self.assertGreater(after["network_process_recycle_count"],
                                   before["network_process_recycle_count"])
                self.assertEqual(after["uri"].rstrip("/"), self.url.rstrip("/"))

    def test_failed_engine_is_non_retryable_and_hard_reset_recovers(self):
        if os.environ.get("LIGHTVIEW_TEST_BROWSER"):
            self.skipTest("host signal injection is not reliable across the launcher's PID namespace")
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private") as (path, process):
                previous = self.web_processes(process.pid)
                operation = ctl.request(path, "reset", timeout=3, hard=True)
                deadline = time.monotonic() + 5
                replacement = set()
                while not replacement:
                    replacement = self.web_processes(process.pid) - previous
                    self.assertLess(time.monotonic(), deadline,
                                    "replacement WebKit process was not observed")
                    if not replacement:
                        time.sleep(0.005)
                target = max(replacement)
                self.assertNotEqual(target, process.pid)
                self.assertTrue(Path(f"/proc/{target}/comm").read_text().startswith("WebKitWeb"))
                print(f"Fault injection: browser PID {process.pid}, WebProcess PID {target}", flush=True)
                os.kill(target, signal.SIGKILL)
                deadline = time.monotonic() + 10
                while (state := self.cli(path, "status"))["engine_state"] != "failed":
                    self.assertLess(time.monotonic(), deadline, state)
                    time.sleep(0.05)
                payload = json.dumps({"command": "eval", "script": "6 * 7"}).encode() + b"\n"
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                    connection.settimeout(5)
                    connection.connect(path)
                    connection.sendall(payload)
                    response = json.loads(connection.makefile("rb").readline())
                self.assertFalse(response["ok"])
                self.assertEqual(response["error_code"], "webkit_failed")
                self.assertFalse(response["retryable"])
                recovered = self.cli(path, "reset", "--hard")
                self.assertGreater(recovered["web_process_generation"],
                                   operation["target_generation"] - 1)
                self.assertEqual(self.cli(path, "status")["engine_state"], "ready")

    def test_page_operation_timeout_cancels_without_resetting_engine(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private", "--control-timeout", "2",
                         "--memory-recycle-threshold", "0") as (path, process):
                self.cli(path, "open", self.url, "--wait")
                before = self.cli(path, "status")
                with self.assertRaises(ctl.ControlError) as raised:
                    ctl.request(path, "eval", timeout=8,
                                script="new Promise(() => {})")
                self.assertEqual(raised.exception.code, "operation_timeout")
                self.assertTrue(raised.exception.retryable)
                deadline = time.monotonic() + 10
                while True:
                    state = self.cli(path, "status")
                    if (state["engine_state"] == "ready" and
                            state["uri"].rstrip("/") == self.url.rstrip("/") and
                            not state["loading"]):
                        break
                    self.assertLess(time.monotonic(), deadline, state)
                    time.sleep(0.1)
                self.assertEqual(state["pid"], process.pid)
                self.assertEqual(state["web_process_generation"],
                                 before["web_process_generation"])
                self.assertEqual(state["reset_count"], before["reset_count"])
                self.assertEqual(state["automation_timeout_count"],
                                 before["automation_timeout_count"] + 1)
                self.assertIsNone(state["last_termination_reason"])
                self.assertEqual(state["page_requests_active"], 0)
                self.assertEqual(state["control_clients"], 1)
                self.assertEqual(self.cli(path, "eval", "document.title"),
                                 "Lightview test")
                for _ in range(2 * 16):
                    healthy = self.cli(path, "status")
                    self.assertEqual(healthy["engine_state"], "ready")
                    self.assertLessEqual(healthy["control_clients"], 1)
                self.assertEqual(self.cli(path, "eval", "6 * 7"), 42)

    def test_uncancellable_page_operation_timeout_still_recovers_engine(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private", "--control-timeout", "2",
                         "--memory-recycle-threshold", "0") as (path, process):
                self.cli(path, "open", self.url, "--wait")
                generation = self.cli(path, "status")["web_process_generation"]
                with self.assertRaises(ctl.ControlError) as raised:
                    ctl.request(path, "eval", timeout=15,
                                script="(() => { while (true) {} })()")
                self.assertEqual(raised.exception.code, "webkit_reset")
                self.assertTrue(raised.exception.retryable)
                deadline = time.monotonic() + 12
                while True:
                    state = self.cli(path, "status")
                    if (state["engine_state"] == "ready" and
                            state["web_process_generation"] > generation and
                            state["uri"].rstrip("/") == self.url.rstrip("/") and
                            not state["loading"]):
                        break
                    self.assertNotEqual(state["engine_state"], "failed", state)
                    self.assertLess(time.monotonic(), deadline, state)
                    time.sleep(0.1)
                self.assertEqual(state["pid"], process.pid)
                self.assertEqual(state["last_termination_reason"], "automation-timeout")
                self.assertEqual(state["automation_timeout_count"], 1)
                self.assertEqual(state["page_requests_active"], 0)
                self.assertEqual(self.cli(path, "eval", "6 * 7"), 42)

    def test_slow_resource_is_stopped_without_resetting_webkit(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private", "--control-timeout", "4",
                         "--memory-recycle-threshold", "0") as (path, process):
                before = self.cli(path, "status")
                ctl.request(path, "open", timeout=2, uri=self.url + "/slow-page")
                deadline = time.monotonic() + 10
                saw_loading = False
                while True:
                    state = ctl.request(path, "status", timeout=2)
                    saw_loading = saw_loading or state["loading"]
                    if state["load_stop_count"] > before["load_stop_count"]:
                        break
                    self.assertLess(time.monotonic(), deadline, state)
                    time.sleep(0.1)
                while state["loading"]:
                    self.assertLess(time.monotonic(), deadline, state)
                    time.sleep(0.1)
                    state = ctl.request(path, "status", timeout=2)
                self.assertTrue(saw_loading)
                self.assertEqual(state["pid"], process.pid)
                self.assertEqual(state["web_process_generation"],
                                 before["web_process_generation"])
                self.assertEqual(state["reset_count"], before["reset_count"])
                self.assertEqual(state["last_committed_uri"], self.url + "/slow-page")
                self.assertEqual(
                    ctl.request(path, "eval", timeout=3, script="document.title"),
                    "Slow resource fixture")

    def test_navigation_away_from_loading_page_is_not_lost(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private", "--control-timeout", "4",
                         "--memory-recycle-threshold", "0") as (path, process):
                before = self.cli(path, "status")
                ctl.request(path, "open", timeout=2, uri=self.url + "/slow-page")
                deadline = time.monotonic() + 5
                while True:
                    loading = ctl.request(path, "status", timeout=2)
                    if (loading["loading"] and
                            loading["last_committed_uri"] == self.url + "/slow-page"):
                        break
                    self.assertLess(time.monotonic(), deadline, loading)
                    time.sleep(0.1)

                self.cli(path, "open", self.url + "/next", "--wait")
                state = self.cli(path, "status")
                self.assertEqual(state["pid"], process.pid)
                self.assertEqual(state["uri"], self.url + "/next")
                self.assertEqual(state["last_committed_uri"], self.url + "/next")
                self.assertFalse(state["loading"])
                self.assertEqual(state["web_process_generation"],
                                 before["web_process_generation"])
                self.assertEqual(state["reset_count"], before["reset_count"])

    def test_old_page_load_watchdog_cannot_cancel_replacement(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private", "--control-timeout", "4",
                         "--memory-recycle-threshold", "0") as (path, process):
                ctl.request(path, "open", timeout=2, uri=self.url + "/slow-page")
                deadline = time.monotonic() + 5
                while True:
                    before = ctl.request(path, "status", timeout=2)
                    if (before["loading"] and
                            before["last_committed_uri"] == self.url + "/slow-page"):
                        break
                    self.assertLess(time.monotonic(), deadline, before)
                    time.sleep(0.1)

                # Leave little time on the first page's five-second load
                # watchdog, then replace it with a main resource that takes
                # three seconds to commit. A stale timer used to stop this new
                # request and force the navigation watchdog to reset WebKit.
                time.sleep(2.5)
                ctl.request(path, "open", timeout=2, uri=self.url + "/slow-main")
                deadline = time.monotonic() + 12
                while True:
                    state = ctl.request(path, "status", timeout=2)
                    if (state["last_committed_uri"] == self.url + "/slow-main" and
                            not state["loading"]):
                        break
                    self.assertEqual(state["reset_count"], before["reset_count"], state)
                    self.assertEqual(state["load_stop_count"],
                                     before["load_stop_count"], state)
                    self.assertLess(time.monotonic(), deadline, state)
                    time.sleep(0.1)

                self.assertEqual(state["pid"], process.pid)
                self.assertEqual(state["web_process_generation"],
                                 before["web_process_generation"])
                self.assertEqual(state["reset_count"], before["reset_count"])
                self.assertEqual(state["load_stop_count"],
                                 before["load_stop_count"])

    def test_network_recycle_while_leaving_loading_page_is_not_lost(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private", "--control-timeout", "4",
                         "--memory-recycle-threshold", "0",
                         "--network-recycle-threshold", "1") as (path, process):
                ctl.request(path, "open", timeout=2, uri=self.url + "/slow-page")
                deadline = time.monotonic() + 5
                while True:
                    loading = ctl.request(path, "status", timeout=2)
                    if (loading["loading"] and
                            loading["last_committed_uri"] == self.url + "/slow-page"):
                        break
                    self.assertLess(time.monotonic(), deadline, loading)
                    time.sleep(0.1)

                before = self.cli(path, "status")
                self.cli(path, "open", self.url + "/next", "--wait")
                state = self.cli(path, "status")
                self.assertEqual(state["pid"], process.pid)
                self.assertEqual(state["uri"], self.url + "/next")
                self.assertEqual(state["last_committed_uri"], self.url + "/next")
                self.assertFalse(state["loading"])
                self.assertEqual(state["web_process_generation"],
                                 before["web_process_generation"])
                self.assertEqual(state["reset_count"], before["reset_count"])
                self.assertGreater(state["network_process_recycle_count"],
                                   before["network_process_recycle_count"])

    def test_navigation_replaces_uncommitted_startup_load(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private", "--control-timeout", "4",
                         "--memory-recycle-threshold", "0",
                         self.url + "/slow-main") as (path, process):
                before = self.cli(path, "status")
                self.assertTrue(before["loading"], before)
                self.cli(path, "open", self.url + "/next", "--wait")
                state = self.cli(path, "status")
                self.assertEqual(state["pid"], process.pid)
                self.assertEqual(state["uri"], self.url + "/next")
                self.assertEqual(state["last_committed_uri"], self.url + "/next")
                self.assertFalse(state["loading"])
                self.assertEqual(state["web_process_generation"],
                                 before["web_process_generation"])
                self.assertEqual(state["reset_count"], before["reset_count"])

    def test_wedged_web_process_is_rebuilt_for_navigation(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private", "--control-timeout", "2",
                         "--memory-recycle-threshold", "0") as (path, process):
                self.cli(path, "open", self.url, "--wait")
                before = self.cli(path, "status")
                socket_inode = os.stat(path).st_ino
                self.cli(path, "eval",
                         "(() => { setTimeout(() => { while (true) {} }, 200); return true; })()")
                time.sleep(0.4)
                self.cli(path, "open", self.url + "/next")
                deadline = time.monotonic() + 25
                while True:
                    state = ctl.request(path, "status")
                    if (state["engine_state"] == "ready" and
                            state["uri"] == self.url + "/next" and
                            not state["loading"]):
                        break
                    self.assertLess(time.monotonic(), deadline,
                                    f"navigation watchdog did not recover: {state}")
                    time.sleep(0.1)
                self.assertEqual(state["pid"], process.pid)
                self.assertEqual(os.stat(path).st_ino, socket_inode)
                self.assertGreater(state["web_process_generation"],
                                   before["web_process_generation"])
                self.assertEqual(state["reset_count"], before["reset_count"])
                self.assertEqual(state["navigation_retry_count"],
                                 before["navigation_retry_count"] + 2)
                self.assertEqual(state["navigation_view_rebuild_count"],
                                 before["navigation_view_rebuild_count"] + 1)
                self.assertGreater(state["navigation_sequence"],
                                   before["navigation_sequence"])
                self.assertFalse(state["navigation_watchdog_active"])

    def test_uncommitted_navigation_retries_before_reset(self):
        with Handler.retry_lock:
            Handler.retry_requests = 0
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private", "--control-timeout", "2",
                         "--memory-recycle-threshold", "0") as (path, process):
                self.cli(path, "open", self.url, "--wait")
                before = self.cli(path, "status")
                ctl.request(path, "open", timeout=2,
                            uri=self.url + "/retry-main")
                deadline = time.monotonic() + 15
                while True:
                    state = ctl.request(path, "status")
                    if (state["last_committed_uri"] == self.url + "/retry-main" and
                            not state["loading"]):
                        break
                    self.assertEqual(state["reset_count"],
                                     before["reset_count"], state)
                    self.assertLess(time.monotonic(), deadline, state)
                    time.sleep(0.1)
                self.assertEqual(state["pid"], process.pid)
                self.assertEqual(state["web_process_generation"],
                                 before["web_process_generation"])
                self.assertEqual(state["reset_count"], before["reset_count"])
                self.assertEqual(state["navigation_retry_count"],
                                 before["navigation_retry_count"] + 1)
                self.assertEqual(state["navigation_view_rebuild_count"],
                                 before["navigation_view_rebuild_count"])
                self.assertFalse(state["navigation_watchdog_active"])

    def test_slow_navigation_within_control_budget_does_not_recover(self):
        with Handler.retry_lock:
            Handler.retry_requests = 0
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private", "--control-timeout", "12",
                         "--memory-recycle-threshold", "0") as (path, process):
                self.cli(path, "open", self.url, "--wait")
                before = self.cli(path, "status")
                ctl.request(path, "open", timeout=2,
                            uri=self.url + "/retry-main")
                deadline = time.monotonic() + 15
                while True:
                    state = ctl.request(path, "status")
                    if (state["last_committed_uri"] == self.url + "/retry-main" and
                            not state["loading"]):
                        break
                    self.assertLess(time.monotonic(), deadline, state)
                    time.sleep(0.1)
                self.assertEqual(state["pid"], process.pid)
                self.assertEqual(state["last_committed_uri"],
                                 self.url + "/retry-main")
                self.assertEqual(state["navigation_retry_count"],
                                 before["navigation_retry_count"])
                self.assertEqual(state["navigation_view_rebuild_count"],
                                 before["navigation_view_rebuild_count"])
                self.assertEqual(state["reset_count"], before["reset_count"])
                with Handler.retry_lock:
                    self.assertEqual(Handler.retry_requests, 1)

    def test_uncommitted_navigation_rebuilds_before_reset(self):
        with Handler.retry_lock:
            Handler.rebuild_requests = 0
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private", "--control-timeout", "2",
                         "--memory-recycle-threshold", "0") as (path, process):
                self.cli(path, "open", self.url, "--wait")
                before = self.cli(path, "status")
                socket_inode = os.stat(path).st_ino
                ctl.request(path, "open", timeout=2,
                            uri=self.url + "/rebuild-main")
                deadline = time.monotonic() + 25
                while True:
                    state = ctl.request(path, "status")
                    if (state["last_committed_uri"] == self.url + "/rebuild-main" and
                            not state["loading"]):
                        break
                    self.assertEqual(state["reset_count"], before["reset_count"], state)
                    self.assertLess(time.monotonic(), deadline, state)
                    time.sleep(0.1)
                self.assertEqual(state["pid"], process.pid)
                self.assertEqual(os.stat(path).st_ino, socket_inode)
                self.assertEqual(state["web_process_generation"],
                                 before["web_process_generation"] + 1)
                self.assertEqual(state["reset_count"], before["reset_count"])
                self.assertEqual(state["navigation_retry_count"],
                                 before["navigation_retry_count"] + 2)
                self.assertEqual(state["navigation_view_rebuild_count"],
                                 before["navigation_view_rebuild_count"] + 1)
                self.assertFalse(state["navigation_watchdog_active"])

    def test_navigation_recovery_is_bounded_by_reset(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private", "--control-timeout", "2",
                         "--memory-recycle-threshold", "0") as (path, process):
                self.cli(path, "open", self.url, "--wait")
                before = self.cli(path, "status")
                socket_inode = os.stat(path).st_ino
                ctl.request(path, "open", timeout=2,
                            uri=self.url + "/always-slow-main")
                deadline = time.monotonic() + 25
                while True:
                    state = ctl.request(path, "status")
                    if state["reset_count"] > before["reset_count"]:
                        break
                    self.assertLess(time.monotonic(), deadline, state)
                    time.sleep(0.1)
                self.assertEqual(state["pid"], process.pid)
                self.assertEqual(os.stat(path).st_ino, socket_inode)
                self.assertEqual(state["last_termination_reason"], "navigation-timeout")
                self.assertEqual(state["navigation_retry_count"],
                                 before["navigation_retry_count"] + 2)
                self.assertEqual(state["navigation_view_rebuild_count"],
                                 before["navigation_view_rebuild_count"] + 1)

    def test_stale_socket_recovery_after_crash(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private", allow_main_exit=True) as (path, process):
                process.kill()
                process.wait(timeout=5)
                self.assertTrue(Path(path).exists())
            with browser(directory, "--private") as (recovered_path, recovered):
                self.assertEqual(recovered_path, path)
                self.assertEqual(ctl.request(path, "status")["pid"], recovered.pid)

    def test_persistent_profile_and_lock(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            profile = str(Path(directory) / "profile")
            with browser(directory, "--profile", profile) as (path, _):
                self.cli(path, "open", self.url, "--wait")
                self.assertEqual(self.cli(path, "status")["running_mode"], "Normal")
                self.cli(path, "eval", "(() => {document.cookie = 'saved=yes; Max-Age=3600; Path=/'; return true;})()")
                with tempfile.TemporaryFile(mode="w+") as collision_log:
                    collision = subprocess.Popen(
                        [str(BROWSER), "--profile", profile,
                         "--no-control"], stdout=collision_log, stderr=collision_log)
                    try:
                        if shutil.which("xdotool"):
                            deadline = time.monotonic() + 10
                            while True:
                                found = subprocess.run(
                                    ["xdotool", "search", "--onlyvisible", "--pid", str(collision.pid),
                                     "--name", "Choose Lightview profile folder"],
                                    capture_output=True, text=True)
                                if found.returncode == 0 and found.stdout.strip():
                                    chooser_window = found.stdout.splitlines()[-1]
                                    break
                                self.assertIsNone(collision.poll(), "Startup exited instead of opening the folder chooser")
                                self.assertLess(time.monotonic(), deadline,
                                                "Startup profile chooser did not appear")
                                time.sleep(0.1)
                            subprocess.run(["xdotool", "windowfocus", chooser_window],
                                           check=True)
                            subprocess.run(["xdotool", "key", "Escape"], check=True)
                            self.assertEqual(collision.wait(timeout=10), 1)
                            collision_log.seek(0)
                            self.assertIn("Profile selection cancelled", collision_log.read())
                        else:
                            time.sleep(1)
                            self.assertIsNone(collision.poll(), "Startup exited instead of opening the folder chooser")
                    finally:
                        if collision.poll() is None:
                            collision.terminate()
                            collision.wait(timeout=5)
            with browser(directory, "--profile", profile) as (path, _):
                self.cli(path, "open", self.url, "--wait")
                self.assertIn("saved=yes", self.cli(path, "eval", "document.cookie"))
                before = self.cli(path, "status")
                reset = self.cli(path, "reset", "--hard")
                self.assertGreater(reset["web_process_generation"],
                                   before["web_process_generation"])
                time.sleep(2.2)
                recovered = self.cli(path, "status")
                self.assertGreater(recovered["ram_pss_mib"], 0)
                self.assertIn(" · MODE:N, ", recovered["window_title"])
                self.assertTrue(recovered["toolbar_telemetry"].startswith("MODE:N, "))
                self.assertTrue(recovered["reset_flash_active"])
                self.cli(path, "open", self.url, "--wait")
                self.assertIn("saved=yes", self.cli(path, "eval", "document.cookie"))
            with browser(directory, "--private") as (path, _):
                self.cli(path, "open", self.url, "--wait")
                self.assertIsNone(self.cli(path, "eval", "localStorage.getItem('saved')"))
                self.assertEqual(self.cli(path, "eval", "document.cookie"), "")

    def test_runtime_profile_switch_keeps_process_socket_and_lease(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            first_profile = str(Path(directory) / "first-profile")
            second_profile = str(Path(directory) / "second-profile")
            with browser(directory, "--private") as (path, process):
                initial = self.cli(path, "status")
                self.assertIsNone(initial["profile_path"])
                socket_inode = os.stat(path).st_ino
                token = self.cli(path, "lease", "acquire")["token"]

                switched = self.cli(path, "--lease", token,
                                    "profile", first_profile)
                self.assertTrue(switched["changed"])
                self.assertEqual(switched["profile_path"], first_profile)
                state = self.cli(path, "status")
                self.assertEqual(state["profile_path"], first_profile)
                self.assertFalse(state["private"])
                self.assertTrue(state["lease_active"])
                self.assertEqual(state["pid"], process.pid)
                self.assertEqual(state["committed_uri"], "about:blank")
                self.assertEqual(os.stat(path).st_ino, socket_inode)
                self.assertEqual(stat.S_IMODE(os.stat(first_profile).st_mode), 0o700)

                self.cli(path, "--lease", token, "open", self.url, "--wait")
                resumed = self.cli(path, "status")
                self.assertEqual(resumed["committed_uri"], self.url + "/")
                self.assertEqual(resumed["last_committed_uri"], self.url + "/")
                self.cli(path, "--lease", token, "eval",
                         "(() => { localStorage.setItem('profile-value', 'first'); "
                         "document.cookie = 'profile-value=first; Max-Age=3600; Path=/'; "
                         "return true; })()")
                locked_profile = Path(directory) / "locked-profile"
                locked_profile.mkdir(mode=0o700)
                with (locked_profile / ".lock").open("w") as lock:
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    before_failure = self.cli(path, "status")
                    failure = self.cli(path, "--lease", token,
                                       "profile", str(locked_profile), success=False)
                    self.assertIn("Cannot lock profile", failure)
                    after_failure = self.cli(path, "status")
                    self.assertEqual(after_failure["profile_path"], first_profile)
                    self.assertEqual(after_failure["web_process_generation"],
                                     before_failure["web_process_generation"])
                    self.assertEqual(self.cli(path, "--lease", token, "eval",
                                              "localStorage.getItem('profile-value')"),
                                     "first")
                self.cli(path, "--lease", token, "profile", second_profile)
                self.cli(path, "--lease", token, "open", self.url, "--wait")
                self.assertIsNone(self.cli(path, "--lease", token, "eval",
                                           "localStorage.getItem('profile-value')"))
                self.assertNotIn("profile-value=first",
                                 self.cli(path, "--lease", token, "eval",
                                          "document.cookie"))
                self.cli(path, "--lease", token, "eval",
                         "(() => { localStorage.setItem('profile-value', 'second'); "
                         "document.cookie = 'profile-value=second; Max-Age=3600; Path=/'; "
                         "return true; })()")

                self.cli(path, "--lease", token, "profile", first_profile)
                self.cli(path, "--lease", token, "open", self.url, "--wait")
                self.assertIn("profile-value=first",
                              self.cli(path, "--lease", token, "eval",
                                       "document.cookie"))
                unchanged = self.cli(path, "--lease", token,
                                     "profile", first_profile)
                self.assertFalse(unchanged["changed"])
                details = self.cli(path, "version", "--show")
                self.assertIn("lightview", details)
                state = self.cli(path, "status")
                self.assertTrue(state["profile_button_visible"])
                self.assertEqual(state["profile_control_path"], first_profile)
                self.assertEqual(state["pid"], process.pid)
                self.assertEqual(os.stat(path).st_ino, socket_inode)
                self.cli(path, "--lease", token, "lease", "renew")
                self.cli(path, "--lease", token, "lease", "release")

    def test_startup_locked_profile_can_choose_another_folder(self):
        if not shutil.which("xdotool"):
            self.skipTest("xdotool is required to exercise the GTK folder chooser")
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            locked_profile = Path(directory) / "locked-profile"
            locked_profile.mkdir(mode=0o700)
            new_profile = Path(directory) / "new-profile"
            new_profile.mkdir(mode=0o700)
            control = str(Path(directory) / "startup-control.sock")
            with (locked_profile / ".lock").open("w") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                with tempfile.TemporaryFile(mode="w+") as log:
                    process = subprocess.Popen(
                        [str(BROWSER), "--profile", str(locked_profile),
                         "--socket", control], stdout=log, stderr=log)
                    try:
                        deadline = time.monotonic() + 10
                        while True:
                            found = subprocess.run(
                                ["xdotool", "search", "--onlyvisible", "--pid",
                                 str(process.pid), "--name", "Choose Lightview profile folder"],
                                capture_output=True, text=True)
                            if found.returncode == 0 and found.stdout.strip():
                                chooser_window = found.stdout.splitlines()[-1]
                                break
                            self.assertIsNone(process.poll(), "Browser exited before folder selection")
                            self.assertLess(time.monotonic(), deadline, "Folder chooser did not appear")
                            time.sleep(0.1)
                        subprocess.run(["xdotool", "windowfocus", chooser_window], check=True)
                        subprocess.run(["xdotool", "key", "ctrl+l"], check=True)
                        subprocess.run(["xdotool", "type", "--clearmodifiers", str(new_profile)],
                                       check=True)
                        subprocess.run(["xdotool", "key", "Return"], check=True)
                        subprocess.run(["xdotool", "key", "Return"], check=True)
                        while not Path(control).exists():
                            self.assertIsNone(process.poll(), "Browser exited after folder selection")
                            self.assertLess(time.monotonic(), deadline, "Browser did not start")
                            time.sleep(0.1)
                        while True:
                            try:
                                state = ctl.request(control, "status", timeout=2)
                                break
                            except (OSError, RuntimeError):
                                self.assertIsNone(process.poll(), "Browser exited during startup")
                                self.assertLess(time.monotonic(), deadline, "Browser control did not become ready")
                                time.sleep(0.1)
                        self.assertEqual(state["profile_path"], str(new_profile))
                        self.assertEqual(state["pid"], process.pid)
                        self.assertEqual(state["engine_state"], "ready")
                    finally:
                        if process.poll() is None:
                            try:
                                ctl.request(control, "quit", timeout=2)
                            except (OSError, RuntimeError):
                                process.terminate()
                            process.wait(timeout=5)

    def test_startup_validation_and_load_errors(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            too_long = str(Path(directory) / ("a" * 110))
            invalid = subprocess.run([str(BROWSER), "--private", "--socket", too_long],
                                     capture_output=True, text=True, timeout=10)
            self.assertNotEqual(invalid.returncode, 0)
            self.assertIn("path is too long", invalid.stderr)
            self.assertEqual(list(Path(directory).iterdir()), [])
            invalid_policy = subprocess.run(
                [str(BROWSER), "--private",
                 "--memory-kill-threshold", "256", "--no-control"],
                capture_output=True, text=True, timeout=10)
            self.assertNotEqual(invalid_policy.returncode, 0)
            self.assertIn("--memory-kill-threshold", invalid_policy.stderr)
            invalid_recycle = subprocess.run(
                [str(BROWSER), "--private", "--memory-recycle-threshold", "3072",
                 "--no-control"], capture_output=True, text=True, timeout=10)
            self.assertNotEqual(invalid_recycle.returncode, 0)
            self.assertIn("--memory-recycle-threshold", invalid_recycle.stderr)
            invalid_control_timeout = subprocess.run(
                [str(BROWSER), "--private", "--control-timeout", "0", "--no-control"],
                capture_output=True, text=True, timeout=10)
            self.assertNotEqual(invalid_control_timeout.returncode, 0)
            self.assertIn("--control-timeout", invalid_control_timeout.stderr)
            with browser(directory, "--private") as (path, _):
                # A bound, non-listening port gives a deterministic refused connection.
                with socket.socket() as refused:
                    refused.bind(("127.0.0.1", 0))
                    self.cli(path, "open", f"http://127.0.0.1:{refused.getsockname()[1]}")
                    deadline = time.monotonic() + 10
                    while not ctl.request(path, "status")["load_error"]:
                        self.assertLess(time.monotonic(), deadline, "Load failure was not reported")
                        time.sleep(0.1)
                    self.cli(path, "wait", success=False)
                self.cli(path, "open", self.url, "--wait")
                self.assertIsNone(ctl.request(path, "status")["load_error"])

    def test_low_memory_options(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private", "--low-memory") as (path, _):
                state = self.cli(path, "status")
                self.assertTrue(state["low_memory"])
                self.assertEqual(state["memory_limit_mib"], 384)
                self.assertEqual(state["memory_kill_threshold_mib"], 3072)
                self.assertEqual(state["idle_hibernate_seconds"], 3600)
                self.assertTrue(state["memory_protection_enabled"])
                self.assertEqual(state["configured_memory_kill_threshold_mib"], 3072)
                # WebKit's kill threshold is per process, while this optional
                # guard measures aggregate process-tree PSS. It must remain
                # opt-in rather than being derived from the kill threshold.
                self.assertEqual(state["memory_recycle_threshold_mib"], 0)
                self.assertEqual(state["network_recycle_threshold_mib"], 0)
                self.assertEqual(state["network_thread_recycle_threshold"], 512)
                self.assertGreater(state["network_threads"], 0)
                self.assertEqual(state["proactive_recycle_count"], 0)
                self.assertEqual(state["network_process_recycle_count"], 0)
                self.assertEqual(state["running_mode"], "Private / Low memory")
                self.assertEqual(state["running_mode_short"], "P/LM")
                self.assertIn(" · MODE:P/LM, ", state["window_title"])
                self.assertTrue(state["toolbar_telemetry"].startswith("MODE:P/LM, "))
                self.cli(path, "open", self.url, "--wait")
                self.cli(path, "wait",
                    "document.querySelector('#image').complete && document.querySelector('#image').naturalWidth === 8")
                webgl = self.cli(path, "eval",
                    "Boolean(document.createElement('canvas').getContext('webgl'))")
                self.assertFalse(webgl)
                media_apis = self.cli(path, "eval",
                    "({audioContext: typeof AudioContext, mediaSource: typeof MediaSource})")
                self.assertEqual(media_apis, {"audioContext": "function", "mediaSource": "function"})
            with browser(directory, "--private", "--memory-kill-threshold", "4096",
                         "--disable-memory-kill") as (path, _):
                state = self.cli(path, "status")
                self.assertFalse(state["memory_protection_enabled"])
                self.assertEqual(state["memory_kill_threshold_mib"], 0)
                self.assertEqual(state["configured_memory_kill_threshold_mib"], 4096)
                self.assertEqual(state["memory_recycle_threshold_mib"], 0)

    def test_version_sources_match(self):
        command_line = subprocess.check_output(
            [str(BROWSER), "--version"], text=True).strip()
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private") as (path, process):
                before = self.cli(path, "status")
                socket_inode = os.stat(path).st_ino
                details = self.cli(path, "version", "--show")
                self.assertIn(f"Lightview {details['lightview']}", command_line)
                self.assertIn(f"WebKitGTK {details['webkitgtk']}", command_line)
                state = self.cli(path, "status")
                self.assertEqual(state["version"], details["lightview"])
                self.assertIsInstance(details["software_rendering"], bool)
                self.assertEqual(state["software_rendering"],
                                 details["software_rendering"])
                self.assertIsInstance(details["media_gl_sink_disabled"], bool)
                self.assertEqual(state["media_gl_sink_disabled"],
                                 details["media_gl_sink_disabled"])
                self.assertTrue(state["version_dialog_visible"])
                self.assertTrue(state["mode_toggle_visible"])
                self.assertFalse(state["mode_toggle_active"])
                self.assertTrue(state["memory_protection_toggle_visible"])
                self.assertTrue(state["memory_protection_toggle_active"])
                self.assertEqual(state["memory_threshold_control_mib"], 3072)
                self.assertTrue(state["idle_hibernate_control_visible"])
                self.assertEqual(state["idle_hibernate_control_seconds"], 3600)
                self.cli(path, "hibernate-after", "120")
                state = self.cli(path, "status")
                self.assertEqual(state["idle_hibernate_seconds"], 120)
                self.assertEqual(state["idle_hibernate_control_seconds"], 120)
                self.cli(path, "hibernate-after", "3600")
                self.assertTrue(state["profile_button_visible"])
                self.assertEqual(state["profile_control_path"], "Private (ephemeral)")
                self.assertEqual(state["uri"], before["uri"])
                self.assertEqual(state["engine_state"], "ready")
                self.assertEqual(state["web_process_generation"], 1)
                switched = self.cli(path, "mode", "low-memory")
                self.assertTrue(switched["changed"])
                state = self.cli(path, "status")
                self.assertTrue(state["low_memory"])
                self.assertTrue(state["mode_toggle_active"])
                self.assertEqual(state["memory_limit_mib"], 384)
                self.assertEqual(state["memory_kill_threshold_mib"], 3072)
                self.assertEqual(state["pid"], process.pid)
                self.assertEqual(os.stat(path).st_ino, socket_inode)
                self.assertGreater(state["web_process_generation"], 1)
                switched = self.cli(path, "mode", "normal")
                self.assertTrue(switched["changed"])
                state = self.cli(path, "status")
                self.assertFalse(state["low_memory"])
                self.assertFalse(state["mode_toggle_active"])
                self.assertEqual(state["memory_limit_mib"], 768)
                generation = state["web_process_generation"]
                changed = self.cli(path, "memory-protection", "on", "--threshold", "4096")
                self.assertTrue(changed["changed"])
                state = self.cli(path, "status")
                self.assertTrue(state["memory_protection_enabled"])
                self.assertEqual(state["memory_kill_threshold_mib"], 4096)
                self.assertEqual(state["configured_memory_kill_threshold_mib"], 4096)
                self.assertEqual(state["memory_threshold_control_mib"], 4096)
                self.assertGreater(state["web_process_generation"], generation)
                generation = state["web_process_generation"]
                changed = self.cli(path, "memory-protection", "off")
                self.assertTrue(changed["changed"])
                state = self.cli(path, "status")
                self.assertFalse(state["memory_protection_enabled"])
                self.assertFalse(state["memory_protection_toggle_active"])
                self.assertEqual(state["memory_kill_threshold_mib"], 0)
                self.assertEqual(state["configured_memory_kill_threshold_mib"], 4096)
                self.assertEqual(state["pid"], process.pid)
                self.assertEqual(os.stat(path).st_ino, socket_inode)
                self.assertGreater(state["web_process_generation"], generation)
                changed = self.cli(path, "memory-protection", "on")
                self.assertTrue(changed["changed"])
                state = self.cli(path, "status")
                self.assertTrue(state["memory_protection_enabled"])
                self.assertTrue(state["memory_protection_toggle_active"])
                self.assertEqual(state["memory_kill_threshold_mib"], 4096)

    def test_version_idle_control_applies_without_rebuilding_webkit(self):
        try:
            import gi
            gi.require_version("Atspi", "2.0")
            from gi.repository import Atspi
        except (ImportError, ValueError):
            self.skipTest("Python AT-SPI bindings are required for the GTK control test")

        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private") as (path, process):
                self.cli(path, "version", "--show")

                def find_named(node, name):
                    if node.get_name() == name:
                        return node
                    for index in range(node.get_child_count()):
                        found = find_named(node.get_child_at_index(index), name)
                        if found:
                            return found
                    return None

                desktop = Atspi.get_desktop(0)
                app = next(desktop.get_child_at_index(index)
                           for index in range(desktop.get_child_count())
                           if desktop.get_child_at_index(index).get_name() == "lightview")
                spin = find_named(app, "Hibernate after idle (seconds)")
                apply_button = find_named(app, "Apply idle time")
                self.assertIsNotNone(spin)
                self.assertIsNotNone(apply_button)
                self.assertEqual(spin.get_value_iface().get_current_value(), 3600)
                generation = self.cli(path, "status")["web_process_generation"]
                self.assertTrue(spin.get_value_iface().set_current_value(120))
                self.assertTrue(apply_button.get_action_iface().do_action(0))
                state = self.cli(path, "status")
                self.assertEqual(state["idle_hibernate_seconds"], 120)
                self.assertEqual(state["pid"], process.pid)
                self.assertEqual(state["web_process_generation"], generation)
                self.cli(path, "hibernate-after", "0")
                self.assertEqual(spin.get_value_iface().get_current_value(), 0)

    def test_idle_hibernation_keeps_agent_lease_and_socket(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private", "--low-memory",
                         "--idle-hibernate", "2", "--lease-ttl", "30") as (path, process):
                socket_inode = os.stat(path).st_ino
                reserved = self.cli(path, "lease", "acquire")
                token = reserved["token"]
                self.assertTrue(reserved["active"])
                self.assertIn("reserved", self.cli(path, "lease", "acquire", success=False))
                deadline = time.monotonic() + 10
                while (state := self.cli(path, "status"))["engine_state"] != "suspended":
                    self.assertLess(time.monotonic(), deadline, state)
                    time.sleep(0.2)
                self.assertEqual(state["committed_uri"], "")
                self.assertTrue(state["lease_active"])
                self.assertEqual(state["pid"], process.pid)
                self.assertEqual(os.stat(path).st_ino, socket_inode)
                self.assertIn("reserved", self.cli(path, "open", self.url, success=False))
                self.cli(path, "--lease", token, "lease", "renew")
                self.assertEqual(self.cli(path, "status")["engine_state"], "suspended")
                self.cli(path, "--lease", token, "open", self.url, "--wait")
                self.assertIn("reserved", self.cli(path, "eval", "document.title",
                                                    success=False))
                resumed = self.cli(path, "status")
                self.assertEqual(resumed["committed_uri"], self.url + "/")
                self.assertEqual(resumed["last_committed_uri"], self.url + "/")
                self.assertEqual(self.cli(path, "--lease", token, "eval", "document.title"),
                                 "Lightview test")
                self.assertEqual(self.cli(path, "status")["engine_state"], "ready")
                self.cli(path, "--lease", token, "eval",
                         "localStorage.setItem('hibernation-test', 'retained')")
                deadline = time.monotonic() + 10
                while self.cli(path, "status")["engine_state"] != "suspended":
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(0.2)
                self.cli(path, "--lease", token, "open", self.url, "--wait")
                self.assertEqual(self.cli(path, "--lease", token, "eval",
                                          "localStorage.getItem('hibernation-test')"),
                                 "retained")
                self.cli(path, "--lease", token, "lease", "release")
                self.assertFalse(self.cli(path, "status")["lease_active"])
                self.assertEqual(self.cli(path, "hibernate-after", "0"),
                                 {"idle_hibernate_seconds": 0})
                time.sleep(2.5)
                self.assertEqual(self.cli(path, "status")["engine_state"], "ready")
                self.assertEqual(self.cli(path, "hibernate-after", "2"),
                                 {"idle_hibernate_seconds": 2})
                deadline = time.monotonic() + 10
                while self.cli(path, "status")["engine_state"] != "suspended":
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(0.2)
                self.assertEqual(self.cli(path, "hibernate-after", "3600"),
                                 {"idle_hibernate_seconds": 3600})
                self.assertEqual(self.cli(path, "status")["engine_state"], "suspended")

    def test_slow_destination_after_hibernation_uses_navigation_budget(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private", "--idle-hibernate", "1",
                         "--control-timeout", "20",
                         "--memory-recycle-threshold", "0") as (path, process):
                deadline = time.monotonic() + 10
                while (state := ctl.request(path, "status"))["engine_state"] != "suspended":
                    self.assertLess(time.monotonic(), deadline, state)
                    time.sleep(0.1)
                before = state
                destination = self.url + "/slow-wake-main"
                ctl.request(path, "open", timeout=2, uri=destination)
                deadline = time.monotonic() + 18
                while True:
                    state = ctl.request(path, "status", timeout=2)
                    self.assertNotEqual(state["engine_state"], "failed", state)
                    self.assertEqual(state["reset_count"], before["reset_count"], state)
                    if (state["committed_uri"] == destination and not state["loading"]):
                        break
                    self.assertLess(time.monotonic(), deadline, state)
                    time.sleep(0.1)
                self.assertEqual(state["pid"], process.pid)
                self.assertEqual(state["last_committed_uri"], destination)
                self.assertEqual(state["navigation_retry_count"], before["navigation_retry_count"])

    def test_agent_lease_expires(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private", "--lease-ttl", "1") as (path, _):
                first = self.cli(path, "lease", "acquire")["token"]
                time.sleep(1.2)
                self.assertIn("expired", self.cli(path, "--lease", first,
                                                   "open", self.url, success=False))
                second = self.cli(path, "lease", "acquire")["token"]
                self.assertNotEqual(first, second)
                self.assertIn("valid lease", self.cli(path, "--lease", first,
                                                       "lease", "release", success=False))
                self.cli(path, "--lease", second, "lease", "release")

    def test_downloads_save_automatically(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            downloads = Path(directory) / "Downloads"
            config = Path(directory) / "config"
            config.mkdir()
            (config / "user-dirs.dirs").write_text(
                f'XDG_DOWNLOAD_DIR="{downloads}"\n')
            # Select the standard download location without moving HOME and
            # letting background GTK/WebKit cache writes race temp cleanup.
            with mock.patch.dict(os.environ, {"XDG_CONFIG_HOME": str(config)}):
                with browser(directory, "--private") as (path, _):
                    state = self.cli(path, "status")
                    self.assertEqual(Path(state["download_dir"]), downloads)
                    self.cli(path, "open", self.url, "--wait")
                    self.cli(path, "click", "#download")
                    target = downloads / "fixture.txt"
                    deadline = time.monotonic() + 10
                    expected = b"download fixture\n"
                    while (not target.exists() or target.read_bytes() != expected or
                           self.cli(path, "status")["downloads_active"] != 0):
                        self.assertLess(time.monotonic(), deadline, "Download did not finish")
                        time.sleep(0.1)
                    self.assertEqual(target.read_bytes(), expected)
                    self.assertEqual(self.cli(path, "status")["downloads_active"], 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
