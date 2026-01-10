#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import os
import subprocess
import sys
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser(description="Run all test_* binaries with timeouts (auto-stop).")
    ap.add_argument("-B", "--build-dir", default="build")
    ap.add_argument("--timeout", type=int, default=20)
    args = ap.parse_args()

    build_dir = Path(args.build_dir)
    tests = sorted(p for p in build_dir.glob("test_*") if p.is_file() and os.access(p, os.X_OK))
    if not tests:
        print("ERROR: no test binaries found; build first", file=sys.stderr)
        return 1

    env = os.environ.copy()
    for t in tests:
        print(f"RUN {t.name}")
        code = subprocess.call(["timeout", f"{args.timeout}s", str(t)], env=env)
        if code != 0:
            print(f"FAIL {t.name} exit={code}", file=sys.stderr)
            return code
    print("OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

