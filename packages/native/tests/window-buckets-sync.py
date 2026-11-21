#!/usr/bin/env python3
"""Guard: every window-suite function must live in exactly one ctest bucket.

Case authors add QtTest functions without regenerating the bucket file; this
test (issue #657) fails with the exact regenerate command whenever the two
drift apart. Usage: window-buckets-sync.py <binary> <buckets.cmake>
"""
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


def bucketed(path):
    names = []
    pattern = re.compile(r"^set\(WORKSPACE_NATIVE_WINDOW_BUCKET_\d+ (.*)\)\s*$")
    with open(path) as source:
        for line in source:
            match = pattern.match(line)
            if match:
                names.extend(n for n in match.group(1).split(";") if n)
    return names


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: window-buckets-sync.py <binary> <buckets.cmake>")
    funcs = functions(sys.argv[1])
    bucketed_names = bucketed(sys.argv[2])
    missing = [f for f in funcs if f not in bucketed_names]
    extra = sorted(set(bucketed_names) - set(funcs))
    duplicates = sorted({n for n in bucketed_names if bucketed_names.count(n) > 1})
    if missing or extra or duplicates:
        print("window bucket file is out of sync with the binary:")
        if missing:
            print("  missing from buckets: " + ", ".join(missing))
        if extra:
            print("  bucketed but absent from the binary: " + ", ".join(extra))
        if duplicates:
            print("  duplicated across buckets: " + ", ".join(duplicates))
        sys.exit("regenerate with: python3 scripts/gen-test-buckets.py "
                 "--binary <native-window-test> --timings <run.xml> "
                 "--out tests/window-test-buckets.cmake")


if __name__ == "__main__":
    main()
