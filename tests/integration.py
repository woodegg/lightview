"""Exercise the real GTK/WebKit browser through its public control socket."""
import contextlib
import http.server
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import socket
import stat
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("lightviewctl", str(ROOT / "tools/lightviewctl"))
spec = importlib.util.spec_from_loader(loader.name, loader)
ctl = importlib.util.module_from_spec(spec)
loader.exec_module(ctl)

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
    def do_GET(self):
        if self.path == "/api":
            payload, content_type = b'{"value":42}', "application/json"
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
        self.wfile.write(payload)

    def log_message(self, *args):
        pass


@contextlib.contextmanager
def browser(directory, *options):
    path = str(Path(directory) / "control.sock")
    with tempfile.TemporaryFile(mode="w+") as log:
        process = subprocess.Popen([str(ROOT / "build/lightview"), "--socket", path, *options],
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
            yield path, process
        finally:
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
                self.assertEqual(after_reset["last_termination_reason"], "terminated-by-api")
                self.assertEqual(after_reset["last_committed_uri"], self.url + "/next")
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
            with browser(directory, "--private") as (path, process):
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
                collision = subprocess.run([str(ROOT / "build/lightview"), "--private", "--socket", path],
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

    def test_stale_socket_recovery_after_crash(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private") as (path, process):
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
                self.cli(path, "eval", "(() => {localStorage.setItem('saved', 'yes'); document.cookie = 'saved=yes; Max-Age=3600; Path=/'; return true;})()")
                collision = subprocess.run([str(ROOT / "build/lightview"), "--profile", profile, "--no-control"],
                                           capture_output=True, text=True, timeout=10)
                self.assertNotEqual(collision.returncode, 0)
                self.assertIn("Cannot lock profile", collision.stderr)
            with browser(directory, "--profile", profile) as (path, _):
                self.cli(path, "open", self.url, "--wait")
                self.assertEqual(self.cli(path, "eval", "localStorage.getItem('saved')"), "yes")
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
                self.assertEqual(self.cli(path, "eval", "localStorage.getItem('saved')"), "yes")
                self.assertIn("saved=yes", self.cli(path, "eval", "document.cookie"))
            with browser(directory, "--private") as (path, _):
                self.cli(path, "open", self.url, "--wait")
                self.assertIsNone(self.cli(path, "eval", "localStorage.getItem('saved')"))
                self.assertEqual(self.cli(path, "eval", "document.cookie"), "")

    def test_startup_validation_and_load_errors(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            too_long = str(Path(directory) / ("a" * 110))
            invalid = subprocess.run([str(ROOT / "build/lightview"), "--private", "--socket", too_long],
                                     capture_output=True, text=True, timeout=10)
            self.assertNotEqual(invalid.returncode, 0)
            self.assertIn("path is too long", invalid.stderr)
            self.assertEqual(list(Path(directory).iterdir()), [])
            invalid_policy = subprocess.run(
                [str(ROOT / "build/lightview"), "--private",
                 "--memory-kill-threshold", "256", "--no-control"],
                capture_output=True, text=True, timeout=10)
            self.assertNotEqual(invalid_policy.returncode, 0)
            self.assertIn("--memory-kill-threshold", invalid_policy.stderr)
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
                self.assertTrue(state["memory_protection_enabled"])
                self.assertEqual(state["configured_memory_kill_threshold_mib"], 3072)
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

    def test_version_sources_match(self):
        command_line = subprocess.check_output(
            [str(ROOT / "build/lightview"), "--version"], text=True).strip()
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            with browser(directory, "--private") as (path, process):
                before = self.cli(path, "status")
                socket_inode = os.stat(path).st_ino
                details = self.cli(path, "version", "--show")
                self.assertIn(f"Lightview {details['lightview']}", command_line)
                self.assertIn(f"WebKitGTK {details['webkitgtk']}", command_line)
                state = self.cli(path, "status")
                self.assertEqual(state["version"], details["lightview"])
                self.assertTrue(state["version_dialog_visible"])
                self.assertTrue(state["mode_toggle_visible"])
                self.assertFalse(state["mode_toggle_active"])
                self.assertTrue(state["memory_protection_toggle_visible"])
                self.assertTrue(state["memory_protection_toggle_active"])
                self.assertEqual(state["memory_threshold_control_mib"], 3072)
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

    def test_downloads_save_automatically(self):
        with tempfile.TemporaryDirectory(prefix="lightview-test-") as directory:
            fake_home = Path(directory) / "home"
            downloads = fake_home / "Downloads"
            fake_home.mkdir()
            old_home = os.environ.get("HOME")
            os.environ["HOME"] = str(fake_home)
            try:
                with browser(directory, "--private") as (path, _):
                    state = self.cli(path, "status")
                    self.assertEqual(Path(state["download_dir"]), downloads)
                    self.cli(path, "open", self.url, "--wait")
                    self.cli(path, "click", "#download")
                    target = downloads / "fixture.txt"
                    deadline = time.monotonic() + 10
                    while not target.exists():
                        self.assertLess(time.monotonic(), deadline, "Download did not finish")
                        time.sleep(0.1)
                    self.assertEqual(target.read_bytes(), b"download fixture\n")
                    self.assertEqual(self.cli(path, "status")["downloads_active"], 0)
            finally:
                if old_home is None:
                    os.environ.pop("HOME", None)
                else:
                    os.environ["HOME"] = old_home


if __name__ == "__main__":
    unittest.main(verbosity=2)
