#!/usr/bin/env python3
import argparse
import http.client
import json
import os
import signal
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class DemoBackendHandler(BaseHTTPRequestHandler):
    server_version = "DemoBackend/0.1"

    def do_GET(self):
        if self.path == "/health":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(b"ok\n")
            return

        if self.path.startswith("/hello"):
            msg = f"hello from backend {self.server.backend_name}\n".encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(msg)))
            self.end_headers()
            self.wfile.write(msg)
            return

        self.send_response(404)
        self.end_headers()

    def log_message(self, fmt, *args):
        # Keep demo quiet.
        return


def start_backend(port: int, name: str):
    httpd = ThreadingHTTPServer(("127.0.0.1", port), DemoBackendHandler)
    httpd.backend_name = name
    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()
    return httpd, t


def request(method: str, host: str, port: int, path: str, body: bytes | None = None, headers: dict | None = None):
    conn = http.client.HTTPConnection(host, port, timeout=2.0)
    conn.request(method, path, body=body, headers=headers or {})
    resp = conn.getresponse()
    data = resp.read()
    conn.close()
    return resp.status, data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--proxy-bin", default=os.path.join("build", "proxy_server"))
    ap.add_argument("--config", default=os.path.join("config", "demo.conf"))
    ap.add_argument("--timeout", type=float, default=10.0, help="Auto-stop after N seconds")
    args = ap.parse_args()

    deadline = time.time() + args.timeout

    b1, t1 = start_backend(19001, "19001")
    b2, t2 = start_backend(19002, "19002")
    proc = None

    try:
        if not os.path.exists(args.proxy_bin):
            print(f"missing proxy binary: {args.proxy_bin}", file=sys.stderr)
            return 2

        proc = subprocess.Popen([args.proxy_bin, "-c", args.config], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.5)

        # Push metrics: backend 19002 has lower GPU util, should be preferred by strategy=gpu.
        payload = {
            "backend": "127.0.0.1:19002",
            "queue_len": 3,
            "gpu_util": 0.10,
            "vram_used_mb": 1024,
            "vram_total_mb": 16384,
        }
        status, _ = request(
            "POST",
            "127.0.0.1",
            18080,
            "/admin/backend_metrics",
            body=json.dumps(payload).encode("utf-8"),
            headers={"Content-Type": "application/json", "Content-Length": str(len(json.dumps(payload)))},
        )
        if status != 200:
            print(f"admin update failed: {status}", file=sys.stderr)
            return 3

        # Mark backend 19001 as worse.
        payload2 = {
            "backend": "127.0.0.1:19001",
            "queue_len": 10,
            "gpu_util": 0.90,
            "vram_used_mb": 8192,
            "vram_total_mb": 16384,
        }
        status, _ = request(
            "POST",
            "127.0.0.1",
            18080,
            "/admin/backend_metrics",
            body=json.dumps(payload2).encode("utf-8"),
            headers={"Content-Type": "application/json", "Content-Length": str(len(json.dumps(payload2)))},
        )
        if status != 200:
            print(f"admin update failed: {status}", file=sys.stderr)
            return 3

        status, data = request("GET", "127.0.0.1", 18080, "/hello", headers={"Host": "demo"})
        body = data.decode("utf-8", errors="replace")
        print(f"/hello -> {status} {body.strip()}")
        if "19002" not in body:
            print("unexpected backend selection (expected 19002 via gpu strategy)", file=sys.stderr)
            return 4

        status, data = request("GET", "127.0.0.1", 18080, "/stats", headers={"Host": "demo"})
        print(f"/stats -> {status} bytes={len(data)}")

        # Hard stop if we overrun.
        if time.time() > deadline:
            print("demo timeout exceeded", file=sys.stderr)
            return 5

        print("DEMO OK")
        return 0
    finally:
        if proc is not None and proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                proc.kill()
        b1.shutdown()
        b2.shutdown()
        t1.join(timeout=1.0)
        t2.join(timeout=1.0)


if __name__ == "__main__":
    raise SystemExit(main())

