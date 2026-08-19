"""Localhost HTTP integration fixture for tests/http_integration_test.clj.

One process, two listeners on 127.0.0.1: a plain HTTP server and a
TLS server wrapped in the throwaway test certificate
(tests/fixtures/tls/server.pem, CN localhost). Both listeners share
the same route table; the path picks the mode. No network traffic
leaves loopback and every payload is fixed, so runs are
deterministic. The gzip body is compressed with mtime=0 so even the
wire bytes are stable.

Routes (GET unless noted):
  /hello         200 "hello world"
  /echo          200 echoes the request body and its Content-Type
  /echo-headers  200 JSON: the request path, parsed query params, and
                 selected request headers (user-agent, accept,
                 x-probe, content-type)
  /echo-json     POST 200 JSON {"echo": <parsed body>, "seen": the
                 request Content-Type}; 400 when the Content-Type is
                 not application/json or the body does not parse
  /items         200 JSON page N ("?page=N", default 1): two pages of
                 items, page 1 carries "next_page": 2, page 2 carries
                 "next_page": null
  /r1            301 -> /r2 -> /final (two-hop redirect chain)
  /r307          POST 307 -> /echo (method and body preserved)
  /chunked       200 chunked transfer encoding, dechunks to
                 "hello world"
  /gzip          200 Content-Encoding: gzip, decompresses to
                 "gz-integration-" repeated 40 times
  /slow          200 "late" after a 3 s sleep (timeout fixture)
  /conncount     200 the number of TCP connections this listener has
                 accepted (keep-alive oracle; counted at accept time,
                 so the value is settled before the response returns)
  anything else  404 "not here"

Run modes:
  python3 tests/fixtures/http/server.py
      Fixture mode (what the suite spawns): bind both listeners on
      kernel-chosen ports (bind 0), print "PORT TLSPORT PID" (the
      child's pid) with no trailing newline, then fork. The child
      detaches its stdio, starts its own session, and arms a 300 s
      alarm so a crashed test run never leaks it; SIGTERM (the kill
      in the test's finally block) or the alarm shuts it down. The
      parent exits as soon as the port line is printed, so the
      spawning sh! returns with both ports already listening.
  python3 tests/fixtures/http/server.py --foreground
      Debug mode: no fork; prints the same line with its own pid and
      serves until stdin closes or SIGTERM/SIGINT arrives.
"""

import gzip
import json
import os
import signal
import ssl
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
FIX = os.path.normpath(os.path.join(HERE, "..", "tls"))

GZIP_BODY = ("gz-integration-" * 40).encode()
ITEM_PAGES = {1: ["alpha", "beta"], 2: ["gamma"]}


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, addr):
        self.nconn = 0
        super().__init__(addr, Handler)

    def get_request(self):
        conn, addr = self.socket.accept()
        self.nconn += 1
        return conn, addr


class TlsServer(Server):
    def __init__(self, addr):
        super().__init__(addr)
        self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.ctx.load_cert_chain(os.path.join(FIX, "server.pem"),
                                 os.path.join(FIX, "server.key"))

    def get_request(self):
        conn, addr = self.socket.accept()
        self.nconn += 1
        return self.ctx.wrap_socket(conn, server_side=True), addr


def read_body(h):
    n = int(h.headers.get("Content-Length", 0))
    return h.rfile.read(n) if n > 0 else b""


def text(h, code, s, headers=()):
    body = s.encode("latin-1") if isinstance(s, str) else s
    h.send_response(code)
    for k, v in headers:
        h.send_header(k, v)
    h.send_header("Content-Type", "text/plain")
    h.send_header("Content-Length", str(len(body)))
    h.end_headers()
    h.wfile.write(body)


def json_route(h, code, obj):
    body = json.dumps(obj).encode()
    h.send_response(code)
    h.send_header("Content-Type", "application/json")
    h.send_header("Content-Length", str(len(body)))
    h.end_headers()
    h.wfile.write(body)


def echo_headers_route(h):
    q = urlparse(h.path).query
    json_route(h, 200, {
        "path": h.path,
        "query": {k: v for k, v in parse_qs(q).items()},
        "user-agent": h.headers.get("User-Agent", ""),
        "accept": h.headers.get("Accept", ""),
        "x-probe": h.headers.get("X-Probe", ""),
        "content-type": h.headers.get("Content-Type", ""),
    })


def echo_json_route(h):
    ctype = h.headers.get("Content-Type", "")
    if not ctype.startswith("application/json"):
        json_route(h, 400, {"error": "expected application/json, got "
                                    + ctype})
        return
    try:
        parsed = json.loads(read_body(h).decode("utf-8"))
    except (ValueError, UnicodeDecodeError):
        json_route(h, 400, {"error": "body is not valid JSON"})
        return
    json_route(h, 200, {"echo": parsed, "seen": ctype})


def items_route(h):
    q = parse_qs(urlparse(h.path).query)
    try:
        page = int(q.get("page", ["1"])[0])
    except ValueError:
        page = 1
    page = page if page in ITEM_PAGES else 1
    more = page < max(ITEM_PAGES)
    json_route(h, 200, {"page": page,
                        "items": ITEM_PAGES[page],
                        "next_page": (page + 1) if more else None})


def route(h):
    p = urlparse(h.path).path
    if p == "/hello":
        text(h, 200, "hello world")
    elif p == "/echo":
        text(h, 200, read_body(h))
    elif p == "/echo-headers":
        echo_headers_route(h)
    elif p == "/echo-json":
        echo_json_route(h)
    elif p == "/items":
        items_route(h)
    elif p == "/r1":
        text(h, 301, "moved", [("Location", "/r2")])
    elif p == "/r2":
        text(h, 301, "moved", [("Location", "/final")])
    elif p == "/final":
        text(h, 200, "final-landing")
    elif p == "/r307":
        # Drain the offered body: a redirect that keeps the connection
        # open must consume the request before the next one arrives.
        read_body(h)
        text(h, 307, "keep it", [("Location", "/echo")])
    elif p == "/chunked":
        h.send_response(200)
        h.send_header("Transfer-Encoding", "chunked")
        h.end_headers()
        h.wfile.write(b"6\r\nhello \r\n")
        h.wfile.write(b"5\r\nworld\r\n")
        h.wfile.write(b"0\r\n\r\n")
        h.wfile.flush()
    elif p == "/gzip":
        text(h, 200, gzip.compress(GZIP_BODY, mtime=0),
             [("Content-Encoding", "gzip")])
    elif p == "/slow":
        time.sleep(3)
        text(h, 200, "late")
    elif p == "/conncount":
        text(h, 200, str(h.server.nconn))
    else:
        text(h, 404, "not here")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        route(self)

    def do_POST(self):
        route(self)

    def log_message(self, fmt, *args):
        pass


def serve(servers):
    threads = [threading.Thread(target=s.serve_forever, daemon=True)
               for s in servers]
    for t in threads:
        t.start()
    for t in threads:
        t.join()


def main():
    plain = Server(("127.0.0.1", 0))
    secure = TlsServer(("127.0.0.1", 0))
    port, tls_port = plain.server_address[1], secure.server_address[1]

    if "--foreground" in sys.argv[1:]:
        sys.stdout.write("%d %d %d" % (port, tls_port, os.getpid()))
        sys.stdout.flush()

        def stop_on_stdin_close():
            sys.stdin.read()
            os.kill(os.getpid(), signal.SIGTERM)

        threading.Thread(target=stop_on_stdin_close, daemon=True).start()
        for sig in (signal.SIGTERM, signal.SIGINT):
            signal.signal(sig, lambda *_: os._exit(0))
        serve([plain, secure])
        return

    pid = os.fork()
    if pid == 0:
        devnull = os.open(os.devnull, os.O_RDWR)
        os.dup2(devnull, 0)
        os.dup2(devnull, 1)
        os.dup2(devnull, 2)
        os.setsid()
        # Orphan guard: a crashed test run never leaks the server.
        signal.alarm(300)
        serve([plain, secure])
        os._exit(0)
    sys.stdout.write("%d %d %d" % (port, tls_port, pid))
    sys.exit(0)


if __name__ == "__main__":
    main()
