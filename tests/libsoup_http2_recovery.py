"""Exercise retired-connection recovery against an explicitly selected libsoup.

Requires Python GI/Soup 3 bindings, python-h2, and the openssl executable.
Only the loopback TLS fixture is contacted. Neither system libraries nor the
running browser are changed. Cases cover younger-stream recovery, a single
HTTP/1.1 fallback, bounded silence, cancellation and preserved response bodies.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import threading
import time


PAYLOAD = b"guard-ok"
INTERACTION_CASES = ("coalesced", "body-preserved")
CASES = (*INTERACTION_CASES, "protocol-fallback", "fallback-timeout",
         "fallback-body", "fallback-cancel", "fallback-forbidden", "ordinary-http1")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class Server:
    def __init__(self, directory, case):
        import h2.config
        import h2.connection
        import h2.events

        self.case = case
        self.requests = []
        self.errors = []
        self.connections = []
        self.timers = []
        self.lock = threading.Lock()
        self.stopped = False
        self.stop_event = threading.Event()
        self.warm_connection = None
        self.certificate = directory / "certificate.pem"
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-days", "1", "-subj", "/CN=127.0.0.1",
            "-addext", "subjectAltName=IP:127.0.0.1",
            "-keyout", str(directory / "key.pem"),
            "-out", str(self.certificate),
        ], check=True, capture_output=True)
        self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.context.load_cert_chain(self.certificate, directory / "key.pem")
        self.context.set_alpn_protocols(
            ["http/1.1"] if case == "ordinary-http1" else ["h2", "http/1.1"])
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen()
        self.listener.settimeout(0.2)
        self.port = self.listener.getsockname()[1]
        self.thread = threading.Thread(target=self.accept, daemon=True)
        self.thread.start()

    def accept(self):
        while not self.stopped:
            try:
                connection, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            threading.Thread(target=self.serve, args=(connection,), daemon=True).start()

    def serve(self, raw):
        import h2.config
        import h2.connection
        import h2.events

        try:
            with self.context.wrap_socket(raw, server_side=True) as connection:
                connection.settimeout(0.2)
                with self.lock:
                    self.connections.append(connection)
                    identity = len(self.connections)
                protocol = connection.selected_alpn_protocol()
                if protocol == "http/1.1":
                    self.serve_http1(connection, identity)
                    return
                if protocol != "h2":
                    raise RuntimeError("Unexpected fixture protocol")
                state = h2.connection.H2Connection(config=h2.config.H2Configuration(
                    client_side=False, header_encoding="utf-8"))
                state.initiate_connection()
                connection.sendall(state.data_to_send())
                lock = threading.Lock()
                streams = {}

                def body(stream, payload):
                    try:
                        with lock:
                            state.send_data(stream, payload, end_stream=True)
                            connection.sendall(state.data_to_send())
                    except Exception as error:
                        if not self.stopped:
                            self.errors.append(repr(error))

                def respond(stream, payload, delayed_body=False):
                    with lock:
                        state.send_headers(stream, [
                            (":status", "200"), ("content-type", "text/plain"),
                            ("content-length", str(len(payload))),
                        ])
                        connection.sendall(state.data_to_send())
                    if delayed_body:
                        timer = threading.Timer(25, body, args=(stream, payload))
                        timer.daemon = True
                        self.timers.append(timer)
                        timer.start()
                    else:
                        body(stream, payload)

                while not self.stopped:
                    try:
                        data = connection.recv(65536)
                    except socket.timeout:
                        continue
                    if not data:
                        return
                    with lock:
                        events = state.receive_data(data)
                        output = state.data_to_send()
                        if output:
                            connection.sendall(output)
                    for event in events:
                        if isinstance(event, h2.events.RequestReceived):
                            streams[event.stream_id] = dict(event.headers)
                        elif isinstance(event, h2.events.StreamEnded):
                            headers = streams[event.stream_id]
                            path = headers[":path"]
                            with self.lock:
                                self.requests.append({
                                    "connection": identity, "protocol": "h2",
                                    "stream": event.stream_id,
                                    "path": path, "at": time.monotonic(),
                                })
                                if path == "/warm":
                                    self.warm_connection = identity
                            if path == "/warm":
                                respond(event.stream_id, b"warm")
                            elif path == "/late" and self.case == "body-preserved":
                                respond(event.stream_id, PAYLOAD, delayed_body=True)
                            elif (identity != self.warm_connection and
                                  self.case in INTERACTION_CASES):
                                respond(event.stream_id, PAYLOAD)
                        elif isinstance(event, h2.events.ConnectionTerminated):
                            return
        except Exception as error:
            if not self.stopped:
                self.errors.append(repr(error))
        finally:
            raw.close()

    def serve_http1(self, connection, identity):
        request = b""
        while b"\r\n\r\n" not in request and not self.stopped:
            try:
                chunk = connection.recv(8192)
            except socket.timeout:
                continue
            if not chunk:
                return
            request += chunk
            if len(request) > 65536:
                raise RuntimeError("Oversized fixture request")
        if self.stopped:
            return
        method, path, _ = request.split(b"\r\n", 1)[0].decode("ascii").split(" ", 2)
        if method != "GET" or path not in ("/warm", "/first", "/late"):
            raise RuntimeError("Unexpected fixture request")
        with self.lock:
            self.requests.append({"connection": identity, "protocol": "http/1.1",
                                  "path": path, "at": time.monotonic()})
        if path != "/warm" and self.case in ("fallback-timeout", "fallback-cancel"):
            self.stop_event.wait(60)
            return
        if path != "/warm" and self.case in ("ordinary-http1", "paused-headers"):
            if self.stop_event.wait(25):
                return
        forbidden = path != "/warm" and self.case == "fallback-forbidden"
        payload = b"warm" if path == "/warm" else b"forbidden" if forbidden else PAYLOAD
        status = b"403 Forbidden" if forbidden else b"200 OK"
        connection.sendall(b"HTTP/1.1 " + status + b"\r\nContent-Type: text/plain\r\n"
                           b"Content-Length: " + str(len(payload)).encode() +
                           b"\r\nConnection: close\r\n\r\n")
        if path != "/warm" and self.case == "fallback-body":
            if self.stop_event.wait(25):
                return
        connection.sendall(payload)

    def close(self):
        self.stopped = True
        self.stop_event.set()
        self.listener.close()
        for timer in self.timers:
            timer.cancel()
        for connection in self.connections:
            try:
                connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self.thread.join(timeout=1)


def client(port, certificate, case):
    import gi
    gi.require_version("Soup", "3.0")
    from gi.repository import Gio, GLib, Soup

    session = Soup.Session()
    session.props.timeout = 0
    proxy = Gio.SimpleProxyResolver.new(None, [])
    session.props.proxy_resolver = proxy
    expected_certificate = Gio.TlsCertificate.new_from_file(str(certificate))
    loop = GLib.MainLoop()
    started = time.monotonic()
    report = {"results": {}, "forced_abort": False}
    messages = []

    def send(name, path):
        message = Soup.Message.new("GET", f"https://127.0.0.1:{port}{path}")
        messages.append(message)
        cancellable = Gio.Cancellable()
        writes = []

        def wrote_headers(current):
            writes.append(current.get_connection_id())
            report.setdefault("connections", {}).setdefault(name, []).append(
                current.get_connection_id())
            if case == "fallback-cancel" and name == "first" and len(writes) == 2:
                GLib.timeout_add(3000, lambda: (cancellable.cancel(), False)[1])

        message.connect("accept-certificate", lambda _m, peer, _errors:
                        peer.is_same(expected_certificate))
        message.connect("wrote-headers", wrote_headers)

        def finished(_session, result):
            try:
                payload = session.send_and_read_finish(result).get_data()
                report["results"][name] = {
                    "status": message.get_status(), "bytes": len(payload),
                    "sha256": hashlib.sha256(payload).hexdigest(),
                    "connection": message.get_connection_id(),
                    "seconds": round(time.monotonic() - started, 3),
                }
            except GLib.Error as error:
                report["results"][name] = {
                    "error": str(error), "seconds": round(time.monotonic() - started, 3),
                    "cancelled": error.matches(Gio.io_error_quark(), Gio.IOErrorEnum.CANCELLED),
                    "timed_out": error.matches(Gio.io_error_quark(), Gio.IOErrorEnum.TIMED_OUT),
                }
            if name == "warm":
                if "error" in report["results"][name]:
                    loop.quit()
                    return
                send("first", "/first")
                if case in INTERACTION_CASES:
                    GLib.timeout_add(17000, lambda: (send("late", "/late"), False)[1])
            elif (({"first", "late"} if case in INTERACTION_CASES else {"first"})
                  <= report["results"].keys()):
                loop.quit()

        session.send_and_read_async(message, GLib.PRIORITY_DEFAULT, cancellable, finished)

    def deadline():
        report["forced_abort"] = True
        session.abort()
        loop.quit()
        return False

    GLib.timeout_add_seconds(55, deadline)
    send("warm", "/warm")
    loop.run()
    session.abort()
    context = GLib.MainContext.default()
    until = time.monotonic() + 1
    while time.monotonic() < until:
        while context.pending():
            context.iteration(False)
        time.sleep(0.001)
    mapped = next(line.split()[-1] for line in Path("/proc/self/maps").read_text().splitlines()
                  if "libsoup-3.0.so." in line)
    report["library"] = mapped
    report["library_sha256"] = digest(mapped)
    print(json.dumps(report), flush=True)


def check_fallback(case, first, requests, warm_requests, result):
    ids = result.get("connections", {}).get("first", [])
    warm = result["results"].get("warm", {})
    seconds = first.get("seconds", 999)
    if case == "ordinary-http1":
        return (len(requests) == 1 and requests[0]["protocol"] == "http/1.1" and
                warm_requests[0]["protocol"] == "http/1.1" and
                requests[0]["connection"] != warm_requests[0]["connection"] and
                ids == [first.get("connection")] and 24 < seconds < 30 and
                first.get("status") == 200 and
                first.get("sha256") == hashlib.sha256(PAYLOAD).hexdigest())
    wire_ok = (len(requests) == 2 and
               [r["protocol"] for r in requests] == ["h2", "http/1.1"] and
               warm_requests[0]["protocol"] == "h2" and
               requests[0]["connection"] == warm_requests[0]["connection"] and
               requests[1]["connection"] != requests[0]["connection"] and
               len(ids) == 2 and ids[0] == warm.get("connection") and ids[1] != ids[0])
    if case == "fallback-timeout":
        return (wire_ok and first.get("timed_out") and 39 < seconds < 45 and
                "HTTP/1.1 fallback response headers stalled" in first.get("error", ""))
    if case == "fallback-cancel":
        return wire_ok and first.get("cancelled") and 22 < seconds < 27
    payload = b"forbidden" if case == "fallback-forbidden" else PAYLOAD
    return (wire_ok and first.get("status") == (403 if case == "fallback-forbidden" else 200) and
            first.get("sha256") == hashlib.sha256(payload).hexdigest() and
            ids[1] == first.get("connection") and
            (44 < seconds < 50 if case == "fallback-body" else 19 < seconds < 25))


def check_case(case, output, library):
    output.mkdir()
    server = Server(output, case)
    try:
        process = subprocess.run([
            sys.executable, str(Path(__file__).resolve()), "--client",
            "--port", str(server.port), "--certificate", str(server.certificate),
            "--case", case,
        ], env={k: v for k, v in dict(os.environ, LD_LIBRARY_PATH=str(library.parent)).items()
                if k != "SOUP_FORCE_HTTP1"},
            text=True, capture_output=True, timeout=65)
        (output / "stderr.log").write_text(process.stderr)
        (output / "client.json").write_text(process.stdout)
        result = json.loads(process.stdout)
        requests = server.requests.copy()
        warm_requests = [r for r in requests if r["path"] == "/warm"]
        first_requests = [r for r in requests if r["path"] == "/first"]
        late_requests = [r for r in requests if r["path"] == "/late"]
        first, late = (result["results"].get(name, {}) for name in ("first", "late"))
        warm = result["results"].get("warm", {})
        expected = hashlib.sha256(PAYLOAD).hexdigest()
        base_passed = (process.returncode == 0 and not result["forced_abort"] and
                       result["library_sha256"] == digest(library) and
                       warm.get("status") == 200 and
                       warm.get("sha256") == hashlib.sha256(b"warm").hexdigest() and
                       len(warm_requests) == 1 and not server.errors and
                       not any(marker in process.stderr for marker in ("WARNING", "CRITICAL")))
        if case not in INTERACTION_CASES:
            passed = base_passed and check_fallback(case, first, first_requests,
                                                   warm_requests, result)
            return {"case": case, "passed": bool(passed), "client": result,
                    "server_requests": requests, "server_errors": server.errors}
        passed = (base_passed and
                  all(item.get("status") == 200 and item.get("sha256") == expected
                      for item in (first, late)) and
                  first.get("connection") != warm.get("connection") and
                  first.get("seconds", 999) < 25 and
                  len(warm_requests) == 1 and len(first_requests) == 2 and
                  first_requests[0]["connection"] == warm_requests[0]["connection"] and
                  first_requests[1]["connection"] != warm_requests[0]["connection"] and
                  bool(late_requests) and
                  16 <= late_requests[0]["at"] - first_requests[0]["at"] < 20 and
                  result.get("connections", {}).get("first", []) ==
                  [warm.get("connection"), first.get("connection")])
        if case == "coalesced":
            passed &= (late.get("seconds", 999) < 25 and
                       late.get("connection") != warm.get("connection") and
                       len(late_requests) == 2 and
                       late_requests[0]["connection"] == warm_requests[0]["connection"] and
                       late_requests[1]["connection"] != warm_requests[0]["connection"] and
                       result.get("connections", {}).get("late", []) ==
                       [warm.get("connection"), late.get("connection")])
        else:
            passed &= (40 < late.get("seconds", 0) < 50 and
                       late.get("connection") == warm.get("connection") and
                       len(late_requests) == 1 and
                       late_requests[0]["connection"] == warm_requests[0]["connection"])
        return {"case": case, "passed": bool(passed), "client": result,
                "server_requests": requests, "server_errors": server.errors}
    except Exception as error:
        return {"case": case, "passed": False, "error": repr(error)}
    finally:
        server.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--case", choices=CASES)
    parser.add_argument("--client", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--port", type=int, help=argparse.SUPPRESS)
    parser.add_argument("--certificate", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    os.umask(0o077)
    if args.client:
        client(args.port, args.certificate, args.case or "coalesced")
        return 0
    if not args.library or not args.output_dir:
        parser.error("--library and --output-dir are required")
    library = args.library.resolve(strict=True)
    if (library.parent / "libsoup-3.0.so.0").resolve() != library:
        parser.error("Selected directory must have a libsoup-3.0.so.0 link to --library")
    args.output_dir.mkdir(parents=True, exist_ok=False)
    initial_test_hash = digest(__file__)
    cases = [args.case] if args.case else CASES
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(cases)) as pool:
        results = list(pool.map(lambda case: check_case(
            case, args.output_dir / case, library), cases))
    test_unchanged = digest(__file__) == initial_test_hash
    report = {"passed": test_unchanged and all(item["passed"] for item in results),
              "results": results, "test_unchanged": test_unchanged,
              "test_sha256": initial_test_hash, "library_sha256": digest(library)}
    (args.output_dir / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
