#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import atexit
import json
import os
import random
import shutil
import signal
import subprocess
import sys
import threading
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed


def http_json(url: str, method: str = "GET", body: bytes | None = None, headers: dict | None = None, timeout: float = 2.0):
    req = urllib.request.Request(url, method=method)
    if headers:
        for k, v in headers.items():
            req.add_header(k, v)
    if body is not None:
        req.data = body
    with urllib.request.urlopen(req, timeout=timeout) as r:
        data = r.read()
    return json.loads(data.decode("utf-8"))


def wait_http_ok(url: str, timeout_sec: float = 10.0):
    end = time.time() + timeout_sec
    last_err = None
    while time.time() < end:
        try:
            _ = urllib.request.urlopen(url, timeout=1.0).read(1)
            return True
        except Exception as e:  # noqa: BLE001
            last_err = e
            time.sleep(0.2)
    raise RuntimeError(f"wait_http_ok timeout: {url}: {last_err}")


def kill_proc(p: subprocess.Popen, name: str):
    if p.poll() is not None:
        return
    try:
        p.send_signal(signal.SIGTERM)
    except Exception:
        return
    for _ in range(20):
        if p.poll() is not None:
            return
        time.sleep(0.1)
    try:
        p.kill()
    except Exception:
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--proxy-bin", default="./build/proxy_server")
    ap.add_argument("--proxy-config", default="./config/ai_demo.conf")
    ap.add_argument("--proxy-port", type=int, default=18080)
    ap.add_argument("--backend-count", type=int, default=20)
    ap.add_argument("--backend-port-base", type=int, default=19000)
    ap.add_argument("--duration", type=float, default=15.0)
    ap.add_argument("--concurrency", type=int, default=200)
    ap.add_argument("--work-ms", type=float, default=300.0)
    ap.add_argument("--strategy", default="gpu", choices=["gpu", "leastqueue", "roundrobin", "rtw", "leastconn", "hash"])
    args = ap.parse_args()

    if not os.path.exists(args.proxy_bin):
        raise SystemExit(f"proxy bin not found: {args.proxy_bin}")

    root = os.path.abspath(os.path.dirname(os.path.dirname(__file__)))
    back_bin = os.path.join(root, "scripts", "mock_ai_backend.py")
    if not os.path.exists(back_bin):
        raise SystemExit(f"backend script not found: {back_bin}")

    # Ensure config exists; patch strategy/listen_port for this run.
    cfg_path = os.path.abspath(os.path.join(root, args.proxy_config))
    if not os.path.exists(cfg_path):
        raise SystemExit(f"proxy config not found: {cfg_path}")
    cfg_text = open(cfg_path, "r", encoding="utf-8").read().splitlines()
    out_lines: list[str] = []
    in_global = False
    for line in cfg_text:
        s = line.strip()
        if s.startswith("[") and s.endswith("]"):
            in_global = (s == "[global]")
        if in_global and s.startswith("listen_port"):
            out_lines.append(f"listen_port = {args.proxy_port}")
            continue
        if in_global and s.startswith("strategy"):
            out_lines.append(f"strategy = {args.strategy}")
            continue
        out_lines.append(line)
    run_cfg_path = os.path.join(root, "config", f"ai_demo.run.{os.getpid()}.conf")
    with open(run_cfg_path, "w", encoding="utf-8") as f:
        f.write("\n".join(out_lines) + "\n")

    procs: list[subprocess.Popen] = []

    def cleanup():
        for p in reversed(procs):
            kill_proc(p, "proc")
        try:
            os.remove(run_cfg_path)
        except OSError:
            pass

    atexit.register(cleanup)

    # Start backends.
    models = ["llama", "qwen", "gemma", "mixtral"]
    for i in range(args.backend_count):
        port = args.backend_port_base + i + 1
        backend_id = f"127.0.0.1:{port}"
        model = models[i % len(models)]
        version = f"v{(i % 3) + 1}"
        loaded = 1 if (i % 5) != 0 else 0  # make some backends 'not ready'
        capacity = 6 + (i % 5) * 2
        p = subprocess.Popen(
            [sys.executable, back_bin, "--port", str(port), "--id", backend_id, "--model", model, "--version", version, "--loaded", str(loaded), "--capacity", str(capacity)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        procs.append(p)

    # Start proxy.
    proxy_out = open(os.path.join(root, "logs", f"ai_demo_proxy.{os.getpid()}.out"), "wb")
    os.makedirs(os.path.join(root, "logs"), exist_ok=True)
    proxy = subprocess.Popen([os.path.abspath(os.path.join(root, args.proxy_bin)), "-c", run_cfg_path], stdout=proxy_out, stderr=proxy_out)
    procs.append(proxy)

    # Wait ready.
    wait_http_ok(f"http://127.0.0.1:{args.proxy_port}/stats", timeout_sec=15.0)

    # Register backends via admin API.
    for i in range(args.backend_count):
        port = args.backend_port_base + i + 1
        body = json.dumps({"ip": "127.0.0.1", "port": port, "weight": 1}).encode("utf-8")
        http_json(f"http://127.0.0.1:{args.proxy_port}/admin/backend_register", method="POST", body=body, headers={"Content-Type": "application/json"})

    print("已启动演示环境：")
    print(f"- Proxy: http://127.0.0.1:{args.proxy_port}/dashboard")
    print(f"- History: http://127.0.0.1:{args.proxy_port}/history_ui")
    print(f"- Stats: http://127.0.0.1:{args.proxy_port}/stats")
    print(f"- Strategy: {args.strategy}")
    print(f"- Backends: {args.backend_count} (ports {args.backend_port_base+1}..{args.backend_port_base+args.backend_count})")
    print("")

    stop_flag = threading.Event()

    def worker_one(req_id: int):
        model = random.choice(models)
        url = f"http://127.0.0.1:{args.proxy_port}/infer?work_ms={args.work_ms}"
        headers = {"X-Model": model, "Content-Type": "application/json"}
        body = (b"{" + b"\"x\":" + (b"1" * 2000) + b"}")  # small payload; work_ms dominates
        t0 = time.time()
        j = http_json(url, method="POST", body=body, headers=headers, timeout=10.0)
        dt = (time.time() - t0) * 1000.0
        return j.get("backend", "-"), dt, model

    # Fire a burst of requests.
    end = time.time() + args.duration
    total = 0
    by_backend: dict[str, int] = {}
    by_model: dict[str, int] = {}
    lats: list[float] = []

    with ThreadPoolExecutor(max_workers=max(1, args.concurrency)) as ex:
        futures = []
        req_id = 0
        while time.time() < end:
            # keep the pipeline full
            while len(futures) < args.concurrency and time.time() < end:
                req_id += 1
                futures.append(ex.submit(worker_one, req_id))
            done = []
            for f in as_completed(futures, timeout=0.2):
                done.append(f)
                try:
                    backend, dt, model = f.result()
                    total += 1
                    by_backend[backend] = by_backend.get(backend, 0) + 1
                    by_model[model] = by_model.get(model, 0) + 1
                    lats.append(dt)
                except Exception:
                    pass
            # remove done
            if done:
                done_set = set(done)
                futures = [x for x in futures if x not in done_set]

    if lats:
        lats.sort()
        p50 = lats[int(0.50 * (len(lats) - 1))]
        p90 = lats[int(0.90 * (len(lats) - 1))]
        p99 = lats[int(0.99 * (len(lats) - 1))]
    else:
        p50 = p90 = p99 = 0.0

    print(f"压测完成：total={total} concurrency={args.concurrency} work_ms={args.work_ms}")
    print(f"延迟(ms)：p50={p50:.1f} p90={p90:.1f} p99={p99:.1f}")
    print("")
    print("按后端分布（Top 10）：")
    for k, v in sorted(by_backend.items(), key=lambda kv: (-kv[1], kv[0]))[:10]:
        print(f"- {k}: {v}")
    print("")
    print("按模型分布：")
    for k, v in sorted(by_model.items(), key=lambda kv: (-kv[1], kv[0])):
        print(f"- {k}: {v}")

    print("")
    print("提示：打开 dashboard 查看“后端 AI/GPU 指标表格 + GPU/显存/队列曲线”，它们来自后端 /ai/status 的真实 in-flight 推导。")


if __name__ == "__main__":
    main()

