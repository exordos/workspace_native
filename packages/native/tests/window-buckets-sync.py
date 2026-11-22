#!/usr/bin/env python3
"""Guard: the window bucket layout and fragments must match the binary (issue #675).

Thin wrapper over scripts/gen-test-buckets.py --check so CI sees the same
deterministic recomputation the regeneration performs.
Usage: window-buckets-sync.py <binary> <buckets.cmake>
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: window-buckets-sync.py <binary> <buckets.cmake>")
    binary, buckets = sys.argv[1], sys.argv[2]
    cmd = [sys.executable, os.path.join(HERE, "..", "scripts", "gen-test-buckets.py"),
           "--check", "--binary", binary, "--out", buckets,
           "--prefix", "WORKSPACE_NATIVE_WINDOW",
           "--fragments", os.path.join(HERE, "bucket-timings", "window"),
           "--fragments-id", "packages/native/tests/bucket-timings/window"]
    sys.exit(subprocess.run(cmd).returncode)


if __name__ == "__main__":
    main()
