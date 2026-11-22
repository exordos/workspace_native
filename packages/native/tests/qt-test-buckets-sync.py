#!/usr/bin/env python3
"""Guard: the bucket file must cover exactly the expected QtTest functions.

Generalized sibling of window-buckets-sync.py for the e2ee suites added by
issue #657. The expected set is the binary's -functions list, optionally
restricted to an --only-file list (suites whose ctest entry historically ran
an explicit function subset) and with --exclude names removed (functions that
run in their own dedicated ctest entry, e.g. the RUN_SERIAL pulse cases).
Usage: qt-test-buckets-sync.py <binary> <buckets.cmake> --prefix PREFIX
       [--only-file FILE] [--exclude NAME ...]
"""
import argparse
import re
import subprocess
import sys


def functions(binary):
    out = subprocess.run([binary, "-functions"], capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"{binary} -functions failed: {out.stderr.strip()}")
    names = []
    for line in out.stdout.splitlines():
        name = re.sub(r"\(.*\)$", "", line.strip())
        if name in ("initTestCase", "cleanupTestCase", "init", "cleanup"):
            continue
        if name and name not in names:
            names.append(name)
    return names


def bucketed(path, prefix):
    names = []
    pattern = re.compile(rf"^set\({prefix}_BUCKET_\d+ (.*)\)\s*$")
    with open(path) as source:
        for line in source:
            match = pattern.match(line)
            if match:
                names.extend(n for n in match.group(1).split(";") if n)
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("buckets")
    ap.add_argument("--prefix", required=True)
    ap.add_argument("--only-file", default=None)
    ap.add_argument("--exclude", nargs="*", default=[])
    args = ap.parse_args()

    funcs = functions(args.binary)
    if args.only_file:
        with open(args.only_file) as handle:
            wanted = [line.strip() for line in handle if line.strip()]
        absent = [name for name in wanted if name not in funcs]
        if absent:
            sys.exit("--only-file names absent from the binary: "
                     + ", ".join(absent))
        want_set = set(wanted)
        funcs = [f for f in funcs if f in want_set]
    excluded = set(args.exclude)
    funcs = [f for f in funcs if f not in excluded]

    bucket_names = bucketed(args.buckets, args.prefix)
    missing = [f for f in funcs if f not in bucket_names]
    extra = sorted(set(bucket_names) - set(funcs))
    duplicates = sorted({n for n in bucket_names if bucket_names.count(n) > 1})
    if missing or extra or duplicates:
        print("bucket file is out of sync with the binary:")
        if missing:
            print("  missing from buckets: " + ", ".join(missing))
        if extra:
            print("  bucketed but absent from the expected set: " + ", ".join(extra))
        if duplicates:
            print("  duplicated across buckets: " + ", ".join(duplicates))
        sys.exit("regenerate with: python3 scripts/gen-test-buckets.py "
                 f"--binary <binary> --timings <run.xml> --out {args.buckets} "
                 f"--prefix {args.prefix}")


if __name__ == "__main__":
    main()
