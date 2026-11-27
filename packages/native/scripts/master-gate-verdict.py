#!/usr/bin/env python3
"""Decide whether a tag publish may reuse the master-push gate verdict.

Reads the commits/{sha}/check-runs payload and applies the reuse contract:
the master push of the tagged sha must have produced a successful verdict for
EACH distinct required check name, every matching run must be completed, and
no matching run may carry a non-success verdict.

Input : one or more JSON payloads of
        GET /repos/{owner}/{repo}/commits/{sha}/check-runs pages. The listing
        is paginated, so the caller passes EVERY page file (exordy findings
        4712353365/369); the check_runs lists are merged before deciding.
Output: one verdict on stdout, one exit code:
        reuse -> 0  (publish reuses the master-push gate)
        wait  -> 1  (keep polling; the caller decides the fallback deadline)
        block -> 2  (a matching check completed non-success: never publish)
"""
import json
import sys

REQUIRED_NAMES = ("package-linux", "full-gate-linux")


def matching_runs(payload):
    """Only master-push gate runs; the tag's own runs must never match."""
    runs = []
    for run in payload.get("check_runs", []):
        suite = run.get("check_suite") or {}
        if run.get("name") not in REQUIRED_NAMES:
            continue
        if suite.get("event") != "push":
            continue
        if suite.get("head_branch") != "master":
            continue
        runs.append(run)
    return runs


def decide(payload):
    runs = matching_runs(payload)
    if any(run.get("status") == "completed"
           and run.get("conclusion") != "success" for run in runs):
        return "block"
    green_names = {run.get("name") for run in runs
                   if run.get("status") == "completed"
                   and run.get("conclusion") == "success"}
    # Reuse is accepted only once EVERY matching run has completed: an older
    # green pair must never authorize publication while a newer matching run
    # (re-check, rerun, or a second master push of the same sha) is still
    # queued or in progress and may yet return a blocking verdict.
    if any(run.get("status") != "completed" for run in runs):
        return "wait"
    if all(name in green_names for name in REQUIRED_NAMES):
        return "reuse"
    return "wait"


def load_pages(paths):
    if len(paths) == 1:
        with open(paths[0], encoding="utf-8") as handle:
            return json.load(handle)
    merged = {"check_runs": []}
    for path in paths:
        with open(path, encoding="utf-8") as handle:
            merged["check_runs"].extend(json.load(handle).get("check_runs", []))
    return merged


def fetch_pages(sha, out_prefix, repo, gh_bin="gh", per_page=100,
                max_pages=50):
    """Page through GET commits/{sha}/check-runs via gh, writing
    out_prefix<N>.json for every page. Returns (paths, status):
    complete  - a short page ended the listing; every run was fetched;
    truncated - max_pages budget exhausted on a FULL page: runs beyond the
                budget were never examined, so the caller must NOT decide
                reuse from these pages (exordy findings 4712353365/369);
    error     - gh or JSON failure.
    """
    import subprocess
    paths = []
    page = 1
    while True:
        dest = "%s%d.json" % (out_prefix, page)
        url = ("repos/%s/commits/%s/check-runs?per_page=%d&page=%d"
               % (repo, sha, per_page, page))
        try:
            with open(dest, "w", encoding="utf-8") as handle:
                proc = subprocess.run([gh_bin, "api", url],
                                      stdout=handle,
                                      stderr=subprocess.DEVNULL)
            if proc.returncode != 0:
                return paths, "error"
            with open(dest, encoding="utf-8") as handle:
                payload = json.load(handle)
        except (OSError, ValueError):
            return paths, "error"
        paths.append(dest)
        if len(payload.get("check_runs", [])) < per_page:
            return paths, "complete"
        if page >= max_pages:
            return paths, "truncated"
        page += 1


def main(argv):
    args = argv[1:]
    if args and args[0] == "--fetch":
        rest = args[1:]
        opts = {}
        sha = rest[0] if rest else ""
        idx = 1
        while idx < len(rest):
            opts[rest[idx].lstrip("-")] = rest[idx + 1]
            idx += 2
        if not sha or "repo" not in opts or "out-prefix" not in opts:
            print("usage: master-gate-verdict.py --fetch SHA --repo R "
                  "--out-prefix P [--gh GH] [--per-page N] [--max-pages N]",
                  file=sys.stderr)
            return 1
        paths, status = fetch_pages(
            sha, opts["out-prefix"], opts["repo"],
            gh_bin=opts.get("gh", "gh"),
            per_page=int(opts.get("per-page", 100)),
            max_pages=int(opts.get("max-pages", 50)))
        print("%s (%d pages)" % (status, len(paths)))
        return {"complete": 0, "truncated": 3, "error": 4}[status]
    if len(argv) < 2:
        print("usage: master-gate-verdict.py CHECK_RUNS_JSON [PAGE_JSON...]",
              file=sys.stderr)
        return 1
    payload = load_pages(argv[1:])
    verdict = decide(payload)
    print(verdict)
    return {"reuse": 0, "wait": 1, "block": 2}[verdict]


if __name__ == "__main__":
    sys.exit(main(sys.argv))
