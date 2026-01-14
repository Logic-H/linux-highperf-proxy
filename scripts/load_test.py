#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import json
import random
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed, TimeoutError as FuturesTimeoutError


def http_json(url: str, method: str, body: bytes, headers: dict[str, str], timeout: float) -> dict:
    req = urllib.request.Request(url, data=body, method=method)
    for k, v in headers.items():
        req.add_header(k, v)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        data = r.read()
    return json.loads(data.decode("utf-8"))


def percentile(sorted_vals: list[float], p: float) -> float:
    if not sorted_vals:
        return 0.0
    if p <= 0:
        return sorted_vals[0]
    if p >= 1:
        return sorted_vals[-1]
    idx = int(p * (len(sorted_vals) - 1))
    return sorted_vals[idx]


def main() -> int:
    ap = argparse.ArgumentParser(description="Simple load test script (stdlib only).")
    ap.add_argument("--base", default="http://127.0.0.1:18080", help="proxy base URL")
    ap.add_argument("--path", default="/infer", help="request path")
    ap.add_argument("--duration", type=float, default=10.0, help="seconds")
    ap.add_argument("--concurrency", type=int, default=200)
    ap.add_argument("--timeout", type=float, default=10.0)
    ap.add_argument("--work-ms", type=float, default=350.0, help="passed as query ?work_ms=... if backend supports it")
    ap.add_argument("--payload-bytes", type=int, default=2048, help="JSON body size (approx)")
    ap.add_argument("--mode", choices=["spread", "model_affinity"], default="spread")
    ap.add_argument("--models", default="llama,qwen,gemma,mixtral")
    args = ap.parse_args()

    base = args.base.rstrip("/")
    url = f"{base}{args.path}?work_ms={args.work_ms}"
    models = [m.strip() for m in args.models.split(",") if m.strip()]
    if not models:
        models = ["mock"]

    payload = {"x": "1" * max(0, args.payload_bytes)}
    body = json.dumps(payload).encode("utf-8")

    end = time.time() + args.duration
    lats_ms: list[float] = []
    by_backend: dict[str, int] = {}
    by_model: dict[str, int] = {}
    errors = 0
    total = 0

    def one(req_id: int):
        headers = {"Content-Type": "application/json", "X-Request-Id": str(req_id)}
        model = ""
        if args.mode == "model_affinity":
            model = random.choice(models)
            headers["X-Model"] = model
        t0 = time.time()
        try:
            j = http_json(url, method="POST", body=body, headers=headers, timeout=args.timeout)
            dt = (time.time() - t0) * 1000.0
            backend = str(j.get("backend", "-"))
            return backend, dt, model, None
        except (urllib.error.URLError, json.JSONDecodeError) as e:
            dt = (time.time() - t0) * 1000.0
            return "-", dt, model, str(e)

    with ThreadPoolExecutor(max_workers=max(1, args.concurrency)) as ex:
        futures = []
        req_id = 0
        while time.time() < end:
            while len(futures) < args.concurrency and time.time() < end:
                req_id += 1
                futures.append(ex.submit(one, req_id))

            done = []
            try:
                for f in as_completed(futures, timeout=0.2):
                    done.append(f)
                    backend, dt, model, err = f.result()
                    total += 1
                    lats_ms.append(dt)
                    if err:
                        errors += 1
                        continue
                    by_backend[backend] = by_backend.get(backend, 0) + 1
                    if args.mode == "model_affinity" and model:
                        by_model[model] = by_model.get(model, 0) + 1
            except FuturesTimeoutError:
                pass

            if done:
                done_set = set(done)
                futures = [x for x in futures if x not in done_set]

        # drain remaining
        for f in as_completed(futures):
            backend, dt, model, err = f.result()
            total += 1
            lats_ms.append(dt)
            if err:
                errors += 1
                continue
            by_backend[backend] = by_backend.get(backend, 0) + 1
            if args.mode == "model_affinity" and model:
                by_model[model] = by_model.get(model, 0) + 1

    lats_ms.sort()
    p50 = percentile(lats_ms, 0.50)
    p90 = percentile(lats_ms, 0.90)
    p99 = percentile(lats_ms, 0.99)

    print(f"url={url}")
    print(f"total={total} errors={errors} concurrency={args.concurrency} duration={args.duration:.1f}s work_ms={args.work_ms}")
    print(f"latency_ms: p50={p50:.1f} p90={p90:.1f} p99={p99:.1f}")
    print("")
    print("backend distribution (top 10):")
    for k, v in sorted(by_backend.items(), key=lambda kv: (-kv[1], kv[0]))[:10]:
        print(f"- {k}: {v}")
    if args.mode == "model_affinity":
        print("")
        print("model distribution:")
        for k, v in sorted(by_model.items(), key=lambda kv: (-kv[1], kv[0])):
            print(f"- {k}: {v}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

