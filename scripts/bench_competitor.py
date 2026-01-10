#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import asyncio
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple


@dataclass
class LatResult:
    ok: int
    failed: int
    lat_ms: List[float]
    elapsed_s: float
    bytes_total: int
    total: int


def _reserve_free_port() -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


async def _wait_tcp(host: str, port: int, timeout_s: float) -> None:
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        try:
            r, w = await asyncio.open_connection(host, port)
            w.close()
            try:
                await w.wait_closed()
            except Exception:
                pass
            return
        except Exception:
            await asyncio.sleep(0.05)
    raise TimeoutError(f"port not ready: {host}:{port}")


async def _backend_handler(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    try:
        data = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), timeout=2.0)
    except Exception:
        writer.close()
        return
    head = data.decode("latin1", errors="ignore")
    # First line: METHOD PATH HTTP/1.1
    first = head.split("\r\n", 1)[0]
    parts = first.split(" ")
    path = parts[1] if len(parts) >= 2 else "/"

    body = b"OK"
    if path.startswith("/download"):
        # /download?bytes=N
        n = 1024
        if "?" in path:
            _, q = path.split("?", 1)
            for kv in q.split("&"):
                if kv.startswith("bytes="):
                    try:
                        n = int(kv.split("=", 1)[1])
                    except Exception:
                        n = 1024
        if n < 0:
            n = 0
        if n > 8 * 1024 * 1024:
            n = 8 * 1024 * 1024
        body = b"a" * n

    resp = (
        b"HTTP/1.1 200 OK\r\n"
        b"Content-Type: application/octet-stream\r\n"
        + f"Content-Length: {len(body)}\r\n".encode("ascii")
        + b"Connection: close\r\n"
        + b"\r\n"
        + body
    )
    try:
        writer.write(resp)
        await writer.drain()
    except Exception:
        pass
    writer.close()
    try:
        await writer.wait_closed()
    except Exception:
        pass


async def _one_http(host: str, port: int, path: str, timeout_s: float) -> Tuple[float, int]:
    req = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: bench.local\r\n"
        f"Connection: close\r\n"
        f"\r\n"
    ).encode("ascii")
    t0 = time.perf_counter()
    reader, writer = await asyncio.wait_for(asyncio.open_connection(host, port), timeout=timeout_s)
    writer.write(req)
    await writer.drain()

    header = b""
    while b"\r\n\r\n" not in header:
        chunk = await asyncio.wait_for(reader.read(65536), timeout=timeout_s)
        if not chunk:
            break
        header += chunk
        if len(header) > 256 * 1024:
            break

    body_bytes = 0
    if b"\r\n\r\n" in header:
        _, rest = header.split(b"\r\n\r\n", 1)
        body_bytes += len(rest)
    while True:
        chunk = await asyncio.wait_for(reader.read(65536), timeout=timeout_s)
        if not chunk:
            break
        body_bytes += len(chunk)

    writer.close()
    try:
        await writer.wait_closed()
    except Exception:
        pass
    return (time.perf_counter() - t0) * 1000.0, body_bytes


async def _run_load(
    host: str,
    port: int,
    path: str,
    concurrency: int,
    total: int,
    timeout_s: float,
    progress_interval_s: float,
) -> LatResult:
    sem = asyncio.Semaphore(concurrency)
    lat_ms: List[float] = []
    ok = 0
    failed = 0
    bytes_total = 0
    start = time.perf_counter()
    last_report = start

    async def job() -> None:
        nonlocal ok, failed, bytes_total
        async with sem:
            try:
                ms, n = await _one_http(host, port, path, timeout_s)
                lat_ms.append(ms)
                bytes_total += n
                ok += 1
            except Exception:
                failed += 1

    tasks = [asyncio.create_task(job()) for _ in range(total)]
    done = 0
    for fut in asyncio.as_completed(tasks):
        await fut
        done += 1
        now = time.perf_counter()
        if progress_interval_s > 0 and now - last_report >= progress_interval_s:
            elapsed = now - start
            rate = done / elapsed if elapsed > 0 else 0.0
            eta = (total - done) / rate if rate > 0 else 0.0
            pct = (done * 100.0) / total if total > 0 else 100.0
            print(
                f"[progress] {path} {done}/{total} ({pct:.1f}%) "
                f"elapsed={elapsed:.1f}s eta={eta:.1f}s rate={rate:.1f} req/s",
                file=sys.stderr,
                flush=True,
            )
            last_report = now
    elapsed_s = time.perf_counter() - start
    return LatResult(ok=ok, failed=failed, lat_ms=lat_ms, elapsed_s=elapsed_s, bytes_total=bytes_total, total=total)


def _pct(lat_ms: List[float], p: float) -> float:
    if not lat_ms:
        return 0.0
    arr = sorted(lat_ms)
    idx = int(p * (len(arr) - 1))
    return float(arr[idx])


def _summary(r: LatResult) -> Dict[str, float]:
    return {
        "ok": r.ok,
        "failed": r.failed,
        "elapsed_s": float(f"{r.elapsed_s:.6f}"),
        "qps": float(f"{(r.ok / r.elapsed_s) if r.elapsed_s > 0 else 0.0:.6f}"),
        "p50_ms": float(f"{_pct(r.lat_ms, 0.50):.6f}"),
        "p90_ms": float(f"{_pct(r.lat_ms, 0.90):.6f}"),
        "p99_ms": float(f"{_pct(r.lat_ms, 0.99):.6f}"),
        "bytes_total": r.bytes_total,
        "total": r.total,
    }


def _run(cmd: List[str], timeout_s: int, cwd: Optional[Path] = None) -> None:
    subprocess.check_call(cmd, cwd=str(cwd) if cwd else None, timeout=timeout_s)


def ensure_haproxy(src_dir: Path, version: str, global_timeout_s: int) -> Path:
    """
    Build HAProxy locally (no root needed) and return haproxy binary path.
    """
    out_bin = src_dir / "third_party" / "haproxy" / "haproxy"
    out_bin.parent.mkdir(parents=True, exist_ok=True)
    if out_bin.exists():
        return out_bin

    work = src_dir / "third_party" / "haproxy_src"
    work.mkdir(parents=True, exist_ok=True)
    tar = work / f"haproxy-{version}.tar.gz"
    url = f"https://www.haproxy.org/download/{version.rsplit('.',1)[0]}/src/haproxy-{version}.tar.gz"

    # Download
    if not tar.exists():
        _run(["bash", "-lc", f"timeout {global_timeout_s}s curl -L --fail -o '{tar}' '{url}'"], timeout_s=global_timeout_s)
    # Extract
    _run(["bash", "-lc", f"timeout {global_timeout_s}s tar -xzf '{tar}' -C '{work}'"], timeout_s=global_timeout_s)
    src = work / f"haproxy-{version}"
    if not src.exists():
        raise RuntimeError(f"haproxy source not found: {src}")

    # Build (static not required; use OpenSSL for typical setups)
    _run(
        ["bash", "-lc", f"timeout {global_timeout_s}s make -C '{src}' -j 4 TARGET=linux-glibc USE_OPENSSL=1 USE_ZLIB=1"],
        timeout_s=global_timeout_s,
    )
    built = src / "haproxy"
    if not built.exists():
        raise RuntimeError("haproxy build failed")
    shutil.copy2(built, out_bin)
    out_bin.chmod(0o755)
    return out_bin


def _write_proxy_conf(path: Path, listen_port: int, backend_port: int) -> None:
    text = f"""[global]
listen_port = {listen_port}
threads = 4
strategy = roundrobin
log_level = ERROR
io_model = epoll
reuse_port = 0

[backend:1]
ip = 127.0.0.1
port = {backend_port}
weight = 1
"""
    path.write_text(text, encoding="utf-8")


def _write_haproxy_conf(path: Path, listen_port: int, backend_port: int) -> None:
    text = f"""
global
  daemon
  maxconn 20000

defaults
  mode http
  timeout connect 2s
  timeout client  5s
  timeout server  5s

frontend fe
  bind 127.0.0.1:{listen_port}
  default_backend be

backend be
  server s1 127.0.0.1:{backend_port} maxconn 20000
"""
    path.write_text(text.strip() + "\n", encoding="utf-8")


async def main_async() -> int:
    ap = argparse.ArgumentParser(description="Benchmark proxy vs competitor (local build HAProxy).")
    ap.add_argument("--proxy-bin", default="./build/proxy_server")
    ap.add_argument("--haproxy-version", default="2.9.12")
    ap.add_argument("--concurrency", type=int, default=200)
    ap.add_argument("--total", type=int, default=20000)
    ap.add_argument("--timeout", type=float, default=3.0)
    ap.add_argument("--global-timeout", type=int, default=600)
    ap.add_argument("--output", default="bench_competitor_results.json")
    ap.add_argument(
        "--progress-interval",
        type=float,
        default=2.0,
        help="Print progress every N seconds (stderr). Use 0 to disable.",
    )
    args = ap.parse_args()

    repo = Path(__file__).resolve().parents[1]
    proxy_bin = (repo / args.proxy_bin).resolve()
    if not proxy_bin.exists():
        raise SystemExit(f"proxy bin not found: {proxy_bin} (build first)")

    haproxy_bin = ensure_haproxy(repo, args.haproxy_version, args.global_timeout)

    backend_port = _reserve_free_port()
    proxy_port = _reserve_free_port()
    haproxy_port = _reserve_free_port()

    backend = await asyncio.start_server(_backend_handler, "127.0.0.1", backend_port)
    try:
        with tempfile.TemporaryDirectory(prefix="proxy_bench_") as td:
            td = Path(td)
            proxy_conf = td / "proxy.conf"
            hap_conf = td / "haproxy.cfg"
            _write_proxy_conf(proxy_conf, proxy_port, backend_port)
            _write_haproxy_conf(hap_conf, haproxy_port, backend_port)

            proxy_p = subprocess.Popen(
                ["timeout", f"{args.global_timeout}s", str(proxy_bin), "-c", str(proxy_conf)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            hap_p = subprocess.Popen(
                ["timeout", f"{args.global_timeout}s", str(haproxy_bin), "-f", str(hap_conf), "-db"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            try:
                await _wait_tcp("127.0.0.1", proxy_port, 3.0)
                await _wait_tcp("127.0.0.1", haproxy_port, 3.0)

                scenarios = [
                    ("small_2b", "/ok"),
                    ("download_4k", "/download?bytes=4096"),
                    ("download_1m", "/download?bytes=1048576"),
                ]

                out: Dict[str, object] = {
                    "ts": time.time(),
                    "python": sys.version,
                    "proxy": {"port": proxy_port, "bin": str(proxy_bin)},
                    "haproxy": {"port": haproxy_port, "bin": str(haproxy_bin), "version": args.haproxy_version},
                    "backend": {"port": backend_port},
                    "params": {"concurrency": args.concurrency, "total": args.total, "timeout": args.timeout},
                    "results": {},
                }

                for name, path in scenarios:
                    print(
                        f"[scenario] {name} path={path} total={args.total} concurrency={args.concurrency} "
                        f"proxy_port={proxy_port} haproxy_port={haproxy_port}",
                        file=sys.stderr,
                        flush=True,
                    )
                    pr = await _run_load(
                        "127.0.0.1",
                        proxy_port,
                        path,
                        args.concurrency,
                        args.total,
                        args.timeout,
                        args.progress_interval,
                    )
                    hr = await _run_load(
                        "127.0.0.1",
                        haproxy_port,
                        path,
                        args.concurrency,
                        args.total,
                        args.timeout,
                        args.progress_interval,
                    )
                    out["results"][name] = {
                        "proxy": _summary(pr),
                        "haproxy": _summary(hr),
                    }

                (repo / args.output).write_text(json.dumps(out, indent=2, sort_keys=True), encoding="utf-8")
            finally:
                for p in (proxy_p, hap_p):
                    try:
                        p.send_signal(signal.SIGTERM)
                    except Exception:
                        pass
                for p in (proxy_p, hap_p):
                    try:
                        p.wait(timeout=2.0)
                    except Exception:
                        try:
                            p.kill()
                        except Exception:
                            pass
    finally:
        backend.close()
        await backend.wait_closed()
    return 0


def main() -> int:
    try:
        return asyncio.run(main_async())
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
