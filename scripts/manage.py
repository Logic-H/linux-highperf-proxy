#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import os
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description="Proxy management helper (minimal).")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_check = sub.add_parser("check", help="Check config is parseable by proxy_server (-C)")
    p_check.add_argument("-c", "--config", default="config/proxy.conf")

    p_run = sub.add_parser("run", help="Run proxy_server foreground")
    p_run.add_argument("-c", "--config", default="config/proxy.conf")

    p_build = sub.add_parser("build", help="Configure+build with CMake")
    p_build.add_argument("-B", "--build-dir", default="build")

    p_test = sub.add_parser("test", help="Run unit/integration tests with timeouts")
    p_test.add_argument("-B", "--build-dir", default="build")
    p_test.add_argument("--timeout", type=int, default=20)

    p_bench = sub.add_parser("bench", help="Run local benchmark (wrap scripts/benchmark.py)")
    p_bench.add_argument("--mode", default="all")
    p_bench.add_argument("--bench", default="http_stats")
    p_bench.add_argument("--port", type=int, default=8084)
    p_bench.add_argument("--config", default="config/bench.conf")
    p_bench.add_argument("--output", default="bench_results_managed.json")

    args = parser.parse_args()

    if args.cmd == "check":
        return subprocess.call(["./build/proxy_server", "-c", args.config, "-C"])

    if args.cmd == "run":
        return subprocess.call(["./build/proxy_server", "-c", args.config])

    if args.cmd == "build":
        build_dir = Path(args.build_dir)
        build_dir.mkdir(parents=True, exist_ok=True)
        if subprocess.call(["cmake", "-S", ".", "-B", str(build_dir)]) != 0:
            return 1
        return subprocess.call(["cmake", "--build", str(build_dir), "-j"])

    if args.cmd == "test":
        build_dir = Path(args.build_dir)
        # Discover test binaries by prefix.
        tests = sorted(p.name for p in build_dir.glob("test_*") if p.is_file())
        if not tests:
            print("ERROR: no test binaries found; run manage.py build first", file=sys.stderr)
            return 1
        env = os.environ.copy()
        for t in tests:
            print(f"RUN {t}")
            code = subprocess.call(["timeout", f"{args.timeout}s", str(build_dir / t)], env=env)
            if code != 0:
                print(f"FAIL {t} exit={code}", file=sys.stderr)
                return code
        print("OK")
        return 0

    if args.cmd == "bench":
        return subprocess.call(
            [
                sys.executable,
                "scripts/benchmark.py",
                "--host",
                "127.0.0.1",
                "--port",
                str(args.port),
                "--mode",
                args.mode,
                "--bench",
                args.bench,
                "--global-timeout",
                "60",
                "--spawn-server",
                "--server-bin",
                "./build/proxy_server",
                "--server-config",
                args.config,
                "--output",
                args.output,
                "--include-uring",
            ]
        )

    return 1


if __name__ == "__main__":
    raise SystemExit(main())
