#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import asyncio
import json
import os
import signal
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple, Any


HTTP_STATS_REQ = (
    b"GET /stats HTTP/1.1\r\n"
    b"Host: bench.local\r\n"
    b"Connection: close\r\n"
    b"\r\n"
)

def _http_download_req(payload_bytes: int) -> bytes:
    return (
        f"GET /download?bytes={payload_bytes} HTTP/1.1\r\n"
        f"Host: bench.local\r\n"
        f"Connection: close\r\n"
        f"\r\n"
    ).encode("ascii")


@dataclass
class RunResult:
    ok: int
    failed: int
    lat_ms: List[float]
    elapsed_s: float
    bytes_total: int = 0


@dataclass
class ProcSample:
    ts: float
    cpu_time_s: float
    rss_bytes: int
    fd_count: int


def _parse_env_mode(mode: str) -> Dict[str, str]:
    env: Dict[str, str] = {}
    if mode == "epoll":
        return env
    if mode == "poll":
        env["PROXY_USE_POLL"] = "1"
        return env
    if mode == "select":
        env["PROXY_USE_SELECT"] = "1"
        return env
    if mode == "uring":
        env["PROXY_USE_URING"] = "1"
        return env
    raise ValueError(f"unknown mode: {mode}")


async def _one_http_stats(host: str, port: int, timeout_s: float) -> float:
    t0 = time.perf_counter()
    reader, writer = await asyncio.wait_for(asyncio.open_connection(host, port), timeout=timeout_s)
    writer.write(HTTP_STATS_REQ)
    await writer.drain()
    # Read until EOF
    while True:
        chunk = await asyncio.wait_for(reader.read(65536), timeout=timeout_s)
        if not chunk:
            break
    writer.close()
    try:
        await writer.wait_closed()
    except Exception:
        pass
    return (time.perf_counter() - t0) * 1000.0


async def _one_http_download(host: str, port: int, payload_bytes: int, timeout_s: float) -> Tuple[float, int]:
    req = _http_download_req(payload_bytes)
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


async def _one_connect_hold(host: str, port: int, hold_s: float, timeout_s: float) -> None:
    reader, writer = await asyncio.wait_for(asyncio.open_connection(host, port), timeout=timeout_s)
    try:
        await asyncio.sleep(hold_s)
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass


async def _run_once(
    mode: str,
    host: str,
    port: int,
    concurrency: int,
    total: int,
    timeout_s: float,
    bench_kind: str,
    hold_s: float,
    payload_bytes: int,
) -> RunResult:
    sem = asyncio.Semaphore(concurrency)
    lat_ms: List[float] = []
    ok = 0
    failed = 0
    bytes_total = 0
    start = time.perf_counter()

    async def job() -> None:
        nonlocal ok, failed, bytes_total
        async with sem:
            try:
                if bench_kind == "http_stats":
                    ms = await _one_http_stats(host, port, timeout_s)
                    lat_ms.append(ms)
                elif bench_kind == "connect_hold":
                    await _one_connect_hold(host, port, hold_s, timeout_s)
                elif bench_kind == "http_download":
                    ms, nbytes = await _one_http_download(host, port, payload_bytes, timeout_s)
                    lat_ms.append(ms)
                    bytes_total += nbytes
                else:
                    raise ValueError(f"unknown bench kind: {bench_kind}")
                ok += 1
            except Exception:
                failed += 1

    tasks = [asyncio.create_task(job()) for _ in range(total)]
    await asyncio.gather(*tasks)
    elapsed = time.perf_counter() - start
    return RunResult(ok=ok, failed=failed, lat_ms=lat_ms, elapsed_s=elapsed, bytes_total=bytes_total)


def _fmt_latency(lat_ms: List[float]) -> str:
    if not lat_ms:
        return "latency: n/a"
    lat_sorted = sorted(lat_ms)
    p50 = lat_sorted[int(len(lat_sorted) * 0.50)]
    p90 = lat_sorted[int(len(lat_sorted) * 0.90)]
    p99 = lat_sorted[int(len(lat_sorted) * 0.99)]
    return (
        f"latency_ms(p50={p50:.2f}, p90={p90:.2f}, p99={p99:.2f}, "
        f"avg={statistics.mean(lat_sorted):.2f})"
    )

def _latency_summary(lat_ms: List[float]) -> Dict[str, float]:
    if not lat_ms:
        return {}
    lat_sorted = sorted(lat_ms)
    p50 = lat_sorted[int(len(lat_sorted) * 0.50)]
    p90 = lat_sorted[int(len(lat_sorted) * 0.90)]
    p99 = lat_sorted[int(len(lat_sorted) * 0.99)]
    return {
        "p50_ms": float(f"{p50:.6f}"),
        "p90_ms": float(f"{p90:.6f}"),
        "p99_ms": float(f"{p99:.6f}"),
        "avg_ms": float(f"{statistics.mean(lat_sorted):.6f}"),
    }


def _read_proc_cpu_time_s(pid: int) -> float:
    # /proc/<pid>/stat: utime (14), stime (15) in clock ticks
    with open(f"/proc/{pid}/stat", "r", encoding="utf-8") as f:
        parts = f.read().split()
    utime = int(parts[13])
    stime = int(parts[14])
    ticks = os.sysconf(os.sysconf_names["SC_CLK_TCK"])
    return (utime + stime) / float(ticks)


def _read_proc_rss_bytes(pid: int) -> int:
    rss_kb = 0
    try:
        with open(f"/proc/{pid}/status", "r", encoding="utf-8") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    rss_kb = int(line.split()[1])
                    break
    except FileNotFoundError:
        return 0
    return rss_kb * 1024


def _read_proc_fd_count(pid: int) -> int:
    try:
        return len(os.listdir(f"/proc/{pid}/fd"))
    except FileNotFoundError:
        return 0


async def _sample_proc(pid: int, interval_s: float, stop: asyncio.Event, samples: List[ProcSample]) -> None:
    while not stop.is_set():
        try:
            samples.append(
                ProcSample(
                    ts=time.perf_counter(),
                    cpu_time_s=_read_proc_cpu_time_s(pid),
                    rss_bytes=_read_proc_rss_bytes(pid),
                    fd_count=_read_proc_fd_count(pid),
                )
            )
        except Exception:
            # best-effort sampling
            pass
        try:
            await asyncio.wait_for(stop.wait(), timeout=interval_s)
        except asyncio.TimeoutError:
            pass


def _summarize_samples(samples: List[ProcSample]) -> Dict[str, Any]:
    if len(samples) < 2:
        return {}
    t0, t1 = samples[0].ts, samples[-1].ts
    c0, c1 = samples[0].cpu_time_s, samples[-1].cpu_time_s
    elapsed = max(1e-9, t1 - t0)
    cpu_pct = ((c1 - c0) / elapsed) * 100.0
    rss_max = max(s.rss_bytes for s in samples)
    rss_avg = int(sum(s.rss_bytes for s in samples) / len(samples))
    fd_max = max(s.fd_count for s in samples)
    fd_avg = sum(s.fd_count for s in samples) / len(samples)
    return {
        "cpu_pct_single_core_avg": float(f"{cpu_pct:.6f}"),
        "rss_bytes_max": int(rss_max),
        "rss_bytes_avg": int(rss_avg),
        "fd_count_max": int(fd_max),
        "fd_count_avg": float(f"{fd_avg:.6f}"),
        "sample_count": len(samples),
    }


def _start_server(server_cmd: List[str], mode: str) -> subprocess.Popen:
    env = os.environ.copy()
    env.update(_parse_env_mode(mode))
    # Ensure unbuffered logs
    env["PYTHONUNBUFFERED"] = "1"
    return subprocess.Popen(
        server_cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=env,
        text=True,
    )


def _stop_server(p: subprocess.Popen, grace_s: float = 0.5) -> str:
    if p.poll() is None:
        try:
            p.send_signal(signal.SIGTERM)
        except Exception:
            pass
        t0 = time.perf_counter()
        while p.poll() is None and (time.perf_counter() - t0) < grace_s:
            time.sleep(0.02)
    if p.poll() is None:
        try:
            p.kill()
        except Exception:
            pass
    try:
        out, _ = p.communicate(timeout=0.2)
        return out or ""
    except Exception:
        return ""


async def _wait_port(host: str, port: int, timeout_s: float) -> None:
    deadline = time.perf_counter() + timeout_s
    while time.perf_counter() < deadline:
        try:
            reader, writer = await asyncio.open_connection(host, port)
            writer.close()
            try:
                await writer.wait_closed()
            except Exception:
                pass
            return
        except Exception:
            await asyncio.sleep(0.05)
    raise TimeoutError(f"server not ready on {host}:{port} within {timeout_s}s")


async def _start_download_backend(payload_bytes: int) -> Tuple[asyncio.AbstractServer, int]:
    body = b"x" * payload_bytes

    async def handler(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            buf = b""
            while b"\r\n\r\n" not in buf:
                chunk = await reader.read(65536)
                if not chunk:
                    break
                buf += chunk
                if len(buf) > 256 * 1024:
                    break
            hdr = (
                f"HTTP/1.1 200 OK\r\n"
                f"Content-Length: {payload_bytes}\r\n"
                f"Connection: close\r\n"
                f"\r\n"
            ).encode("ascii")
            writer.write(hdr)
            writer.write(body)
            await writer.drain()
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except Exception:
                pass

    server = await asyncio.start_server(handler, host="127.0.0.1", port=0)
    port = int(server.sockets[0].getsockname()[1])
    return server, port


def _generate_temp_proxy_config(listen_port: int, threads: int, backend_port: int) -> str:
    import tempfile

    content = (
        "[global]\n"
        f"listen_port = {listen_port}\n"
        f"threads = {threads}\n"
        "strategy = roundrobin\n"
        "log_level = ERROR\n"
        "io_model = epoll\n"
        "reuse_port = 0\n"
        "\n"
        "[backend:1]\n"
        "ip = 127.0.0.1\n"
        f"port = {backend_port}\n"
        "weight = 1\n"
    )
    f = tempfile.NamedTemporaryFile(prefix="bench_proxy_", suffix=".conf", delete=False, mode="w", encoding="utf-8")
    f.write(content)
    f.flush()
    f.close()
    return f.name


def main() -> int:
    parser = argparse.ArgumentParser(description="Local benchmark for proxy_server (auto-stop).")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--mode", choices=["epoll", "poll", "select", "uring", "all"], default="epoll")
    parser.add_argument("--bench", choices=["http_stats", "connect_hold", "http_download"], default="http_stats")
    parser.add_argument("--concurrency", type=int, default=200)
    parser.add_argument("--total", type=int, default=2000)
    parser.add_argument("--timeout", type=float, default=2.0, help="per-op timeout seconds")
    parser.add_argument("--hold", type=float, default=2.0, help="hold seconds for connect_hold")
    parser.add_argument("--payload-bytes", type=int, default=8 * 1024 * 1024, help="payload bytes for http_download")
    parser.add_argument("--global-timeout", type=float, default=15.0, help="hard stop seconds")
    parser.add_argument("--spawn-server", action="store_true", help="spawn proxy_server and kill it after run")
    parser.add_argument("--server-bin", default="./build/proxy_server")
    parser.add_argument("--server-config", default="config/bench.conf")
    parser.add_argument("--server-extra-args", nargs="*", default=[])
    parser.add_argument("--server-threads", type=int, default=4, help="threads in generated server config (http_download)")
    parser.add_argument("--spawn-backend", action="store_true", help="spawn local backend for http_download")
    parser.add_argument("--startup-timeout", type=float, default=2.5)
    parser.add_argument("--warmup", type=int, default=10, help="warmup requests before measuring (http_stats only)")
    parser.add_argument("--include-uring", action="store_true", help="include io_uring when --mode all (experimental)")
    parser.add_argument("--per-mode-timeout", type=float, default=8.0, help="timeout seconds per mode run (mode=all)")
    parser.add_argument("--output", default="", help="write results to JSON file (overwrite)")
    parser.add_argument("--show-server-log", action="store_true", help="print server log tail")
    parser.add_argument("--sample-interval", type=float, default=0.2, help="proc sample interval seconds (spawn-server)")
    args = parser.parse_args()

    async def warmup_http() -> None:
        if args.bench != "http_stats" or args.warmup <= 0:
            return
        for _ in range(args.warmup):
            try:
                await _one_http_stats(args.host, args.port, args.timeout)
            except Exception:
                # Warmup best-effort
                pass

    async def runner_one(mode: str) -> Tuple[RunResult, str, Dict[str, Any]]:
        server_log = ""
        proc = None
        backend_server: Optional[asyncio.AbstractServer] = None
        tmp_cfg = ""
        samples: List[ProcSample] = []
        stop = asyncio.Event()
        sampler_task: Optional[asyncio.Task] = None
        try:
            if args.bench == "http_download" and args.spawn_backend:
                backend_server, backend_port = await _start_download_backend(args.payload_bytes)
                tmp_cfg = _generate_temp_proxy_config(args.port, args.server_threads, backend_port)
            if args.spawn_server:
                cfg = tmp_cfg if tmp_cfg else args.server_config
                cmd = [args.server_bin, "-c", cfg] + list(args.server_extra_args)
                proc = _start_server(cmd, mode)
                await _wait_port(args.host, args.port, args.startup_timeout)
                sampler_task = asyncio.create_task(_sample_proc(proc.pid, args.sample_interval, stop, samples))
            await warmup_http()

            res = await _run_once(
                mode=mode,
                host=args.host,
                port=args.port,
                concurrency=args.concurrency,
                total=args.total,
                timeout_s=args.timeout,
                bench_kind=args.bench,
                hold_s=args.hold,
                payload_bytes=args.payload_bytes,
            )
        finally:
            stop.set()
            if sampler_task is not None:
                try:
                    await asyncio.wait_for(sampler_task, timeout=0.5)
                except Exception:
                    pass
            if proc is not None:
                server_log = _stop_server(proc)
            if backend_server is not None:
                backend_server.close()
                try:
                    await backend_server.wait_closed()
                except Exception:
                    pass
            if tmp_cfg:
                try:
                    os.unlink(tmp_cfg)
                except Exception:
                    pass
        return res, server_log, _summarize_samples(samples)

    async def runner() -> Tuple[List[Tuple[str, RunResult, str]], int]:
        if args.mode != "all":
            modes = [args.mode]
        else:
            modes = ["epoll", "poll", "select"]
            if args.include_uring:
                modes.append("uring")
        results: List[Tuple[str, RunResult, str, Dict[str, Any]]] = []
        for m in modes:
            try:
                res, log, metrics = await asyncio.wait_for(runner_one(m), timeout=args.per_mode_timeout)
                results.append((m, res, log, metrics))
            except asyncio.TimeoutError:
                results.append((m, RunResult(ok=0, failed=args.total, lat_ms=[], elapsed_s=args.per_mode_timeout), "ERROR: per-mode timeout", {}))
        return results, 0

    try:
        results, _ = asyncio.run(asyncio.wait_for(runner(), timeout=args.global_timeout))
    except asyncio.TimeoutError:
        print(f"ERROR: global timeout reached ({args.global_timeout}s)", file=sys.stderr)
        return 2
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1

    json_out: Dict[str, object] = {
        "ts_unix": int(time.time()),
        "host": args.host,
        "port": args.port,
        "bench": args.bench,
        "concurrency": args.concurrency,
        "total": args.total,
        "timeout_s": args.timeout,
        "hold_s": args.hold,
        "payload_bytes": args.payload_bytes,
        "sample_interval_s": args.sample_interval,
        "modes": [],
    }

    for mode, res, server_log, metrics in results:
        qps = res.ok / res.elapsed_s if res.elapsed_s > 0 else 0.0
        throughput_gbps = 0.0
        if args.bench == "http_download" and res.elapsed_s > 0:
            throughput_gbps = (res.bytes_total * 8.0) / res.elapsed_s / 1e9
        print(f"mode={mode} bench={args.bench} ok={res.ok} failed={res.failed} elapsed_s={res.elapsed_s:.2f} qps={qps:.2f}")
        if args.bench == "http_stats":
            print(_fmt_latency(res.lat_ms))
        elif args.bench == "connect_hold":
            print(f"connect_hold: hold_s={args.hold:.2f} concurrency={args.concurrency} total={args.total}")
        elif args.bench == "http_download":
            print(_fmt_latency(res.lat_ms))
            print(f"download_bytes={res.bytes_total} throughput_gbps={throughput_gbps:.3f}")
        if args.spawn_server and args.show_server_log and server_log:
            print("---- server log (tail) ----")
            lines = server_log.splitlines()[-20:]
            for ln in lines:
                print(ln)

        mode_entry: Dict[str, object] = {
            "mode": mode,
            "ok": res.ok,
            "failed": res.failed,
            "elapsed_s": float(f"{res.elapsed_s:.6f}"),
            "qps": float(f"{qps:.6f}"),
        }
        if args.bench == "http_stats":
            mode_entry["latency_ms"] = _latency_summary(res.lat_ms)
        if args.bench == "http_download":
            mode_entry["latency_ms"] = _latency_summary(res.lat_ms)
            mode_entry["bytes_total"] = int(res.bytes_total)
            mode_entry["throughput_gbps"] = float(f"{throughput_gbps:.6f}")
        if metrics:
            mode_entry["proc"] = metrics
        json_out["modes"].append(mode_entry)

    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            json.dump(json_out, f, ensure_ascii=False, indent=2)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
