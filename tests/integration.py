"""Exercise the real GTK/WebKit browser through its public control socket."""
import contextlib
import fcntl
import http.server
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import shutil
import socket
import stat
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BROWSER = Path(os.environ.get("LIGHTVIEW_TEST_BROWSER", ROOT / "build/lightview"))
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
                self.assertEqual(os.stat(path).st_ino, socket_inode)
                self.assertEqual(stat.S_IMODE(os.stat(first_profile).st_mode), 0o700)

                self.cli(path, "--lease", token, "open", self.url, "--wait")
                self.cli(path, "--lease", token, "eval",
                         "localStorage.setItem('profile-value', 'first')")
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
                self.cli(path, "--lease", token, "eval",
                         "localStorage.setItem('profile-value', 'second')")

                self.cli(path, "--lease", token, "profile", first_profile)
                self.cli(path, "--lease", token, "open", self.url, "--wait")
                self.assertEqual(self.cli(path, "--lease", token, "eval",
                                          "localStorage.getItem('profile-value')"),
                                 "first")
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
                self.assertTrue(state["lease_active"])
                self.assertEqual(state["pid"], process.pid)
                self.assertEqual(os.stat(path).st_ino, socket_inode)
                self.assertIn("reserved", self.cli(path, "open", self.url, success=False))
                self.cli(path, "--lease", token, "lease", "renew")
                self.assertEqual(self.cli(path, "status")["engine_state"], "suspended")
                self.cli(path, "--lease", token, "open", self.url, "--wait")
                self.assertIn("reserved", self.cli(path, "eval", "document.title",
                                                    success=False))
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
