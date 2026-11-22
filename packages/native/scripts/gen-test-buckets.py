#!/usr/bin/env python3
"""Generate deterministic ctest bucket layouts from committed timing fragments (issue #675).

Membership rule: each QtTest function belongs to bucket crc32(name) % N.
The rule is stable across branches, so a lane that adds a case edits only that
case's own lines - rebase diffs between lanes touching the suite stay mergeable
and a rebase no longer requires a full solo timing rerun. N (the bucket count)
is kept as-is on normal regenerations; --rebalance is the explicit operation
that moves the whole layout (grow/shrink N until every bucket sums under the
ceiling), run when the suite outgrows its current layout.

Committed truth: timing fragments under --fragments, one file per bucket
(partNN.txt), one line per function:

    <functionName>=<measured-seconds>

with the names inside each fragment sorted. A line's value is the QtTest
duration for that function with all data rows summed, taken from a solo run:

    <binary> <functionName> -o run.xml,xml

The layout file (--out) is fully derived from the fragments and the member
names the binary reports (-functions). --check is the CI guard: it fails
whenever a function has no fragment, a fragment is stale, the committed layout
drifts from the fragments, or a bucket sums over --ceiling seconds. Timings
recorded with --timings only set fragment values (max of old and new); a
function that has neither a fragment nor a fresh timing is a hard error, so a
new case can never silently join the layout unmeasured.

The old source-order packing mode (issue #657) is removed on purpose: keeping
both would let a stale recipe silently rewrite the layout in an unmergeable
format.
"""
import argparse
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
import zlib

PART_RE = re.compile(r"^part(\d+)\.txt$")
FRAGMENT_RE = re.compile(r"^([^=]+)=([0-9.]+)\s*$")
OVER_FILE = "over.txt"


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


def read_list_file(path):
    with open(path) as handle:
        return [line.strip() for line in handle if line.strip()]


def filter_funcs(names, args):
    if args.only_file:
        wanted = read_list_file(args.only_file)
        absent = [name for name in wanted if name not in names]
        if absent:
            sys.exit(f"--only-file names absent from {args.binary} -functions: "
                     + ", ".join(absent))
        want_set = set(wanted)
        names = [f for f in names if f in want_set]
    if args.exclude:
        excluded = set(args.exclude)
        absent = [name for name in sorted(excluded) if name not in names]
        if absent:
            sys.exit(f"--exclude names absent from {args.binary} -functions: "
                     + ", ".join(absent))
        names = [f for f in names if f not in excluded]
    return names


def timings_from_xml(paths):
    """Per-function seconds from QtTest xml files; max across files."""
    per = {}
    for path in paths:
        root = ET.parse(path).getroot()
        run = {}
        for tf in root.iter("TestFunction"):
            base = re.sub(r"\(.*\)$", "", tf.get("name") or "")
            seconds = sum(float(d.get("msecs") or 0.0)
                          for d in tf.findall("Duration")) / 1000.0
            run[base] = run.get(base, 0.0) + seconds
        for name, seconds in run.items():
            per[name] = max(per.get(name, 0.0), seconds)
    return per


def load_fragments(directory):
    per = {}
    if not os.path.isdir(directory):
        return per
    for entry in sorted(os.listdir(directory)):
        if not (PART_RE.match(entry) or entry == OVER_FILE):
            continue
        with open(os.path.join(directory, entry)) as handle:
            for line in handle:
                if not line.strip():
                    continue
                frag = FRAGMENT_RE.match(line.strip())
                if not frag:
                    sys.exit(f"{entry}: unparseable fragment line: {line!r}")
                per[frag.group(1)] = float(frag.group(2))
    return per


def bucket_index(name, count):
    return zlib.crc32(name.encode()) % count


def split_heavy(per, ceiling):
    """Functions measured over the ceiling alone get their own ctest entries.

    They already dictate the wall clock of whatever bucket holds them; a
    dedicated entry keeps every hash bucket honestly under the ceiling and
    lets CI run (and retry) the heavyweight case on its own."""
    heavy = sorted(n for n, secs in per.items() if secs > ceiling)
    rest = {n: secs for n, secs in per.items() if n not in set(heavy)}
    return rest, heavy


def assign(per, count):
    buckets = [[] for _ in range(count)]
    for name in per:
        buckets[bucket_index(name, count)].append(name)
    return [sorted(b) for b in buckets]


def bucket_secs(buckets, per):
    return [round(sum(per[n] for n in bucket), 2) for bucket in buckets]


def render(prefix, buckets, heavy, fragments_id):
    lines = [
        "# Generated by scripts/gen-test-buckets.py (issue #675) - do not edit.",
        "# membership: crc32(name) mod COUNT; timings: " + fragments_id + "/partNN.txt",
        f"set({prefix}_BUCKETS_COUNT {len(buckets)})",
    ]
    for i, bucket in enumerate(buckets, 1):
        lines.append(f"set({prefix}_BUCKET_{i}")
        lines.extend(f"    {name}" for name in bucket)
        lines.append(")")
    if heavy:
        lines.append(f"set({prefix}_BUCKETS_OVER")
        lines.extend(f"    {name}" for name in heavy)
        lines.append(")")
    return "\n".join(lines) + "\n"


def code_lines(text):
    return [line for line in text.splitlines() if line.startswith("set(")]


def layout_count(path):
    if not os.path.isfile(path):
        return 0
    with open(path) as handle:
        for line in handle:
            match = re.match(r"^set\(([A-Z0-9_]+)_BUCKETS_COUNT (\d+)\)", line)
            if match:
                return int(match.group(2))
    return 0


def write_fragments(directory, per, bucketable, heavy, count):
    os.makedirs(directory, exist_ok=True)
    for entry in os.listdir(directory):
        if PART_RE.match(entry) or entry == OVER_FILE:
            os.remove(os.path.join(directory, entry))
    buckets = assign(bucketable, count)
    pad = max(2, len(str(count)))
    for i, bucket in enumerate(buckets, 1):
        with open(os.path.join(directory, f"part{i:0{pad}d}.txt"), "w") as out:
            for name in bucket:
                out.write(f"{name}={bucketable[name]:g}\n")
    if heavy:
        with open(os.path.join(directory, OVER_FILE), "w") as out:
            for name in heavy:
                out.write(f"{name}={per[name]:g}\n")


def pick_count(per, ceiling, start):
    count = max(1, start)
    while True:
        sums = bucket_secs(assign(per, count), per)
        empty = any(not b for b in assign(per, count))
        if sums and not empty and (max(sums) <= ceiling or count >= len(per)):
            return count
        if count >= len(per):
            return count
        count += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary")
    ap.add_argument("--out")
    ap.add_argument("--fragments")
    ap.add_argument("--fragments-id", default=None,
                    help="repo-relative fragments path for the layout header")
    ap.add_argument("--prefix", default="WORKSPACE_NATIVE_WINDOW")
    ap.add_argument("--timings", action="append", default=[],
                    help="QtTest xml; may be repeated; records fragment values")
    ap.add_argument("--ceiling", type=float, default=120.0)
    ap.add_argument("--rebalance", action="store_true",
                    help="re-pick the bucket count so every bucket sums under --ceiling")
    ap.add_argument("--only-file", default=None)
    ap.add_argument("--exclude", action="append", default=[])
    ap.add_argument("--check", action="store_true", help="guard mode (no writes)")
    args = ap.parse_args()

    missing = [flag for flag, value in (("--binary", args.binary),
                                        ("--out", args.out),
                                        ("--fragments", args.fragments))
               if not value]
    if missing:
        sys.exit("gen-test-buckets.py now needs " + " ".join(missing)
                 + " - see the module docstring (issue #675). The old packing"
                 " invocation is removed: fragments make the layout mergeable.")
    if not args.fragments_id:
        args.fragments_id = args.fragments

    funcs = filter_funcs(functions(args.binary), args)
    per = load_fragments(args.fragments)

    if args.check:
        errors = []
        stale = sorted(set(per) - set(funcs))
        if stale:
            errors.append("fragments without a binary function (delete the lines): "
                          + ", ".join(stale))
        unmeasured = [f for f in funcs if f not in per]
        if unmeasured:
            errors.append("binary functions without a fragment (record timing via "
                          f"`{args.binary} <name> -o run.xml,xml` then regenerate): "
                          + ", ".join(unmeasured))
        count = layout_count(args.out)
        if not count:
            errors.append(f"{args.out} has no BUCKETS_COUNT")
        else:
            bucketable, heavy = split_heavy(per, args.ceiling)
            buckets = assign(bucketable, count)
            empty = [i for i, b in enumerate(buckets, 1) if not b]
            if empty:
                errors.append(f"empty buckets: {empty} (rebalance needed)")
            sums = bucket_secs(buckets, bucketable)
            over = [(i, secs) for i, secs in enumerate(sums, 1)
                    if secs > args.ceiling]
            if over:
                errors.append("buckets over the "
                              f"{args.ceiling:g}s ceiling: "
                              + ", ".join(f"bucket {i} at {s:g}s" for i, s in over)
                              + f" - run with --rebalance (count target: "
                              f"{pick_count(bucketable, args.ceiling, count)})")
        if not errors:
            with open(args.out) as handle:
                committed = handle.read()
            bucketable, heavy = split_heavy(per, args.ceiling)
            if code_lines(committed) != code_lines(
                    render(args.prefix, assign(bucketable, count),
                           heavy, args.fragments_id)):
                errors.append("layout is not the deterministic rendering of the fragments")
        if errors:
            print("bucket layout is out of sync (issue #675):")
            for err in errors:
                print("  " + err)
            sys.exit("regenerate with: python3 scripts/gen-test-buckets.py "
                     f"--binary <binary> --out {args.out} --fragments {args.fragments} "
                     f"--prefix {args.prefix} --timings <run.xml> [--rebalance]")
        return

    record = timings_from_xml(args.timings)
    unmeasured = [f for f in funcs if f not in per and f not in record]
    if unmeasured:
        sys.exit("functions without recorded timings (no fragment, not in "
                 "--timings xml): " + ", ".join(unmeasured)
                 + f"\nrun e.g.: {args.binary} {' '.join(unmeasured[:3])} ... "
                 "-o run.xml,xml and pass it via --timings")
    unknown = sorted(set(record) - set(funcs))
    if unknown:
        print("WARNING: --timings contains names absent from the filtered binary "
              "function set (ignored): " + ", ".join(unknown), file=sys.stderr)
    for name in funcs:
        if name in record:
            per[name] = max(per.get(name, 0.0), record[name])
    per = {name: per[name] for name in funcs}
    bucketable, heavy = split_heavy(per, args.ceiling)
    if not bucketable:
        sys.exit("every function is over the ceiling; nothing left to bucket")

    count = 0
    if os.path.isdir(args.fragments):
        count = len([e for e in os.listdir(args.fragments) if PART_RE.match(e)])
    if not count:
        count = layout_count(args.out)
    if args.rebalance or count == 0:
        count = pick_count(bucketable, args.ceiling, max(count, 1))
    while any(not b for b in assign(bucketable, count)):
        count += 1
    buckets = assign(bucketable, count)
    write_fragments(args.fragments, per, bucketable, heavy, count)
    with open(args.out, "w") as out:
        out.write(render(args.prefix, buckets, heavy, args.fragments_id))
    sums = bucket_secs(buckets, bucketable)
    print(f"{len(funcs)} functions ({len(heavy)} over-ceiling solo) -> "
          f"{count} buckets (max bucket {max(sums) if sums else 0:.1f}s, "
          f"ceiling {args.ceiling:g}s) written to {args.out}")


if __name__ == "__main__":
    main()
