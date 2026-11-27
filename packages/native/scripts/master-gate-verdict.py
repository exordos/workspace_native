#!/usr/bin/env python3
"""Resolve the tag's master-push gate using Actions runs and their check suites."""
import argparse
import json
import subprocess
import sys
import urllib.parse


REQUIRED_NAMES = {"package-linux", "full-gate-linux"}
WORKFLOW = ".github/workflows/linux-release.yml"


def fetch_listing(repo, endpoint, params, key, gh_bin="gh", per_page=100,
                  max_pages=50):
    """Return every observed item, plus whether the listing was exhausted."""
    items = []
    for page in range(1, max_pages + 1):
        query = urllib.parse.urlencode(dict(params, per_page=per_page, page=page))
        url = f"repos/{repo}/{endpoint}?{query}"
        try:
            result = subprocess.run([gh_bin, "api", url], capture_output=True,
                                    check=True, timeout=45)
            payload = json.loads(result.stdout)
            batch = payload[key]
            if not isinstance(batch, list) or not all(isinstance(item, dict)
                                                      for item in batch):
                return items, False
            items.extend(batch)
            if len(batch) < per_page:
                # Filtered Actions searches can cap the returned records.
                return items, len(items) >= payload["total_count"]
        except (OSError, subprocess.SubprocessError, KeyError, ValueError, TypeError):
            return items, False
    return items, False


def decide(sha, workflows, checks, complete):
    suites = {run["check_suite_id"]: run for run in workflows
              if run.get("head_sha") == sha and run.get("event") == "push"
              and run.get("head_branch") == "master" and run.get("path") == WORKFLOW
              and isinstance(run.get("check_suite_id"), int)}
    green = {suite: set() for suite in suites}
    for check in checks:
        suite = (check.get("check_suite") or {}).get("id")
        name = check.get("name")
        if suite not in suites or name not in REQUIRED_NAMES:
            continue
        if check.get("status") != "completed":
            complete = False
        elif check.get("conclusion") != "success":
            return "block"
        else:
            green[suite].add(name)
    if complete and suites and all(run.get("status") == "completed"
                                   and green[suite] == REQUIRED_NAMES
                                   for suite, run in suites.items()):
        return "reuse"
    return "wait"


def resolve_verdict(repo, sha):
    filters = {"head_sha": sha, "event": "push", "branch": "master"}
    workflows, runs_complete = fetch_listing(repo, "actions/runs", filters, "workflow_runs")
    checks, checks_complete = fetch_listing(
        repo, f"commits/{sha}/check-runs", {"filter": "latest"}, "check_runs")
    unresolved_red = False
    if not runs_complete:
        # A missing page can hide the master run that owns a visible red.
        # A suite-specific lookup distinguishes this from the tag's own skips.
        known = {run.get("check_suite_id") for run in workflows}
        failed_suites = {check["check_suite"]["id"] for check in checks
                         if check.get("name") in REQUIRED_NAMES
                         and check.get("status") == "completed"
                         and check.get("conclusion") != "success"
                         and isinstance((check.get("check_suite") or {}).get("id"), int)}
        for suite_id in failed_suites - known:
            matches, complete = fetch_listing(repo, "actions/runs",
                                              dict(filters, check_suite_id=suite_id),
                                              "workflow_runs")
            workflows.extend(matches)
            if not complete:
                unresolved_red = True
    verdict = decide(sha, workflows, checks, runs_complete and checks_complete)
    if verdict == "wait" and unresolved_red:
        return "unknown-red"
    if verdict == "wait" and not (runs_complete and checks_complete):
        return "incomplete"
    return verdict


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fetch", required=True, metavar="SHA")
    parser.add_argument("--repo", required=True)
    args = parser.parse_args()
    verdict = resolve_verdict(args.repo, args.fetch)
    print(verdict)
    return {"reuse": 0, "wait": 1, "block": 2, "unknown-red": 3, "incomplete": 4}[verdict]


if __name__ == "__main__":
    sys.exit(main())
