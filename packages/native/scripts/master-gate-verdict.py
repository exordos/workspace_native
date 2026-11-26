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


def main(argv):
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
