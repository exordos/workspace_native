#!/usr/bin/env python3
"""Guard: the bucket layout and fragments must match the binary (issue #675).

Generalized sibling of window-buckets-sync.py for the e2ee suites. The
expected set is the binary's -functions list, optionally restricted to an
--only-file list (suites whose ctest entry historically ran an explicit
function subset) and with --exclude names removed (functions that run in
their own dedicated ctest entry, e.g. the RUN_SERIAL pulse cases). All
comparison logic lives in scripts/gen-test-buckets.py --check.
Usage: qt-test-buckets-sync.py <binary> <buckets.cmake> --prefix PREFIX
       --fragments DIR [--only-file FILE] [--exclude NAME ...]
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("buckets")
    ap.add_argument("--prefix", required=True)
    ap.add_argument("--fragments", required=True)
    ap.add_argument("--only-file", default=None)
    ap.add_argument("--exclude", nargs="*", default=[])
    args = ap.parse_args()

    cmd = [sys.executable, os.path.join(HERE, "..", "scripts", "gen-test-buckets.py"),
           "--check", "--binary", args.binary, "--out", args.buckets,
           "--prefix", args.prefix, "--fragments", args.fragments]
    if args.only_file:
        cmd += ["--only-file", args.only_file]
    for name in args.exclude:
        cmd += ["--exclude", name]
    sys.exit(subprocess.run(cmd).returncode)


if __name__ == "__main__":
    main()
