#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import json
import os
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs


class BackendState:
    def __init__(self, backend_id: str, model: str, version: str, loaded: bool, capacity: int, vram_total_mb: int):
        self.backend_id = backend_id
        self.model = model
        self.version = version
        self.loaded = loaded
        self.capacity = max(1, capacity)
        self.vram_total_mb = max(0, vram_total_mb)

        self._lock = threading.Lock()
        self._inflight = 0
        self._total = 0
        self._by_model = {}

    def on_start(self):
        with self._lock:
            self._inflight += 1
            self._total += 1

    def on_end(self):
        with self._lock:
            self._inflight = max(0, self._inflight - 1)

    def snapshot(self):
        with self._lock:
            inflight = self._inflight
            total = self._total
        return inflight, total

    def record_model(self, model: str):
        if not model:
            return
        with self._lock:
            self._by_model[model] = self._by_model.get(model, 0) + 1

    def ai_status(self):
        inflight, _ = self.snapshot()
        # “拟真”指标：由真实 in-flight 推导，避免手工随意注入。
        queue_len = inflight
        gpu_util = min(1.0, float(inflight) / float(self.capacity))
        vram_used = 1024 + inflight * 512  # 仅用于展示：随着并发增长
        if self.vram_total_mb > 0:
            vram_used = min(vram_used, self.vram_total_mb)
        return {
            "queue_len": queue_len,
            "gpu_util": round(gpu_util, 3),
            "vram_used_mb": int(vram_used),
            "vram_total_mb": int(self.vram_total_mb),
            "model": self.model,
            "version": self.version,
            "loaded": 1 if self.loaded else 0,
        }


class Handler(BaseHTTPRequestHandler):
    server_version = "MockAIB/1.0"

    def _send(self, code: int, body: bytes, content_type: str = "application/json; charset=utf-8"):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            self.wfile.write(body)
        except BrokenPipeError:
            pass

    def _json(self, code: int, obj):
        self._send(code, json.dumps(obj, ensure_ascii=False).encode("utf-8"))

    def do_GET(self):
        st: BackendState = self.server.state  # type: ignore[attr-defined]
        u = urlparse(self.path)
        if u.path == "/health":
            self._send(200, b"OK\n", "text/plain; charset=utf-8")
            return
        if u.path == "/hello":
            inflight, total = st.snapshot()
            self._json(200, {"ok": True, "backend": st.backend_id, "inflight": inflight, "total": total})
            return
        if u.path == "/ai/status":
            self._json(200, st.ai_status())
            return
        if u.path == "/backend/info":
            inflight, total = st.snapshot()
            self._json(
                200,
                {
                    "backend": st.backend_id,
                    "model": st.model,
                    "version": st.version,
                    "loaded": st.loaded,
                    "capacity": st.capacity,
                    "inflight": inflight,
                    "total": total,
                },
            )
            return
        self._send(404, b"not found\n", "text/plain; charset=utf-8")

    def do_POST(self):
        st: BackendState = self.server.state  # type: ignore[attr-defined]
        u = urlparse(self.path)
        if u.path not in ("/infer", "/infer_stream"):
            self._send(404, b"not found\n", "text/plain; charset=utf-8")
            return

        st.on_start()
        try:
            qs = parse_qs(u.query or "")
            work_ms = float(qs.get("work_ms", ["200"])[0])
            work_ms = max(0.0, min(work_ms, 10_000.0))
            model = self.headers.get("X-Model", "")
            if model:
                st.record_model(model)

            length = int(self.headers.get("Content-Length", "0") or "0")
            if length > 0:
                _ = self.rfile.read(min(length, 2 * 1024 * 1024))

            # 模拟“巨大计算需求”：用 sleep 表示耗时推理（可通过 work_ms 调大）
            time.sleep(work_ms / 1000.0)

            inflight, total = st.snapshot()
            resp = {
                "ok": True,
                "backend": st.backend_id,
                "model": st.model,
                "version": st.version,
                "loaded": st.loaded,
                "inflight": inflight,
                "total": total,
                "work_ms": work_ms,
                "pid": os.getpid(),
            }
            self._json(200, resp)
        finally:
            st.on_end()

    def log_message(self, fmt, *args):
        # quieter; uncomment for debugging
        return


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--id", required=True, help="backend id, e.g. 127.0.0.1:19001")
    ap.add_argument("--model", default="mock")
    ap.add_argument("--version", default="v1")
    ap.add_argument("--loaded", type=int, default=1)
    ap.add_argument("--capacity", type=int, default=8, help="higher => lower gpu_util under same load")
    ap.add_argument("--vram-total-mb", type=int, default=24576)
    args = ap.parse_args()

    st = BackendState(
        backend_id=args.id,
        model=args.model,
        version=args.version,
        loaded=(args.loaded != 0),
        capacity=args.capacity,
        vram_total_mb=args.vram_total_mb,
    )

    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    httpd.state = st  # type: ignore[attr-defined]
    print(f"[mock_ai_backend] listen http://{args.host}:{args.port} id={args.id} model={args.model}@{args.version} loaded={st.loaded}")
    httpd.serve_forever()


if __name__ == "__main__":
    main()

