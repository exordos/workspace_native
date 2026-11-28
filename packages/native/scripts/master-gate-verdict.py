#!/usr/bin/env python3
"""Decide whether a tag publish may reuse the master-push gate verdict.

Issue #955: the commits/{sha}/check-runs listing returns a STUB check_suite
carrying only {"id": ...} - never event/head_branch - so attribution cannot
read those fields off check-runs. Instead, the master-push workflow runs are
resolved via GET /repos/{owner}/{repo}/actions/runs?head_sha={sha}
&event=push&branch=master (records DO carry event, head_branch and
check_suite_id), and a check-run participates in the verdict only when its
check_suite.id belongs to one of those push runs. The tag's own same-name
check-runs (its full-gate-linux is "skipped") live on a foreign suite and
therefore never block or authorize reuse.

Reuse contract (unchanged discipline): the master push of the tagged sha must
have produced a successful verdict for EACH distinct required check name,
every matching run must be completed (including the actions/runs records
themselves - a still-running newer push of the same sha forbids reuse), and
no matching run may carry a non-success verdict.

Input : the decide pass takes --check-runs PAGES... --runs PAGES..., the JSON
        payloads of the two paginated listings (every page of each, exordy
        findings 4712353365/369); the lists are merged before deciding. A
        payload without a runs listing can never attribute suites and fails
        closed to "wait".
Output: one verdict on stdout, one exit code:
        reuse -> 0  (publish reuses the master-push gate)
        wait  -> 1  (keep polling; the caller decides the fallback deadline)
        block -> 2  (a matching check completed non-success: never publish)
"""
import json
import sys

REQUIRED_NAMES = ("package-linux", "full-gate-linux")


def master_push_records(payload):
    """actions/runs records for the master push (defensive re-filter: the
    listing is normally already queried with event=push&branch=master)."""
    return [record for record in payload.get("master_push_runs", [])
            if record.get("event") == "push"
            and record.get("head_branch") == "master"]


def matching_runs(payload):
    """Only check-runs whose suite belongs to a master-push run; the tag's
    own runs and every foreign event must never match (issue #955)."""
    allowed = set()
    for record in master_push_records(payload):
        suite = record.get("check_suite_id")
        if suite is not None:
            allowed.add(suite)
    if not allowed:
        return []
    runs = []
    for run in payload.get("check_runs", []):
        if run.get("name") not in REQUIRED_NAMES:
            continue
        suite = run.get("check_suite") or {}
        if suite.get("id") not in allowed:
            continue
        runs.append(run)
    return runs


def decide(payload):
    runs = matching_runs(payload)
    if any(run.get("status") == "completed"
           and run.get("conclusion") != "success" for run in runs):
        return "block"
    # A master-push workflow run that has not completed yet forbids reuse even
    # when an older green pair is already recorded (re-check, rerun, or a
    # second master push of the same sha may still return red). Its check
    # runs may not even exist yet, so this gate reads the runs listing.
    if any(record.get("status") != "completed"
           for record in master_push_records(payload)):
        return "wait"
    green_names = {run.get("name") for run in runs
                   if run.get("status") == "completed"
                   and run.get("conclusion") == "success"}
    # Reuse is accepted only once EVERY matching run has completed: an older
    # green pair must never authorize publication while a newer matching run
    # is still queued or in progress and may yet return a blocking verdict.
    if any(run.get("status") != "completed" for run in runs):
        return "wait"
    if all(name in green_names for name in REQUIRED_NAMES):
        return "reuse"
    return "wait"


def load_merged(paths, list_key):
    merged = []
    for path in paths:
        with open(path, encoding="utf-8") as handle:
            merged.extend(json.load(handle).get(list_key, []))
    return merged


def load_payload(check_paths, run_paths):
    return {"check_runs": load_merged(check_paths, "check_runs"),
            "master_push_runs": load_merged(run_paths, "workflow_runs")}


def _paginate(gh_bin, url_for_page, list_key, out_prefix, per_page, max_pages):
    """Page a gh api listing until a short page ends it. Returns (paths,
    status): complete / truncated (budget exhausted on a FULL page - runs
    beyond the budget were never examined, the caller must NOT accept reuse)
    / error (gh or JSON failure)."""
    import subprocess
    paths = []
    page = 1
    while True:
        dest = "%s%d.json" % (out_prefix, page)
        try:
            with open(dest, "w", encoding="utf-8") as handle:
                proc = subprocess.run([gh_bin, "api", url_for_page(page)],
                                      stdout=handle,
                                      stderr=subprocess.DEVNULL)
            if proc.returncode != 0:
                return paths, "error"
            with open(dest, encoding="utf-8") as handle:
                payload = json.load(handle)
        except (OSError, ValueError):
            return paths, "error"
        paths.append(dest)
        if len(payload.get(list_key, [])) < per_page:
            return paths, "complete"
        if page >= max_pages:
            return paths, "truncated"
        page += 1


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
    def url_for_page(page):
        return ("repos/%s/commits/%s/check-runs?per_page=%d&page=%d"
                % (repo, sha, per_page, page))
    return _paginate(gh_bin, url_for_page, "check_runs", out_prefix,
                     per_page, max_pages)


def fetch_runs_pages(sha, out_prefix, repo, gh_bin="gh", per_page=100,
                     max_pages=50):
    """Page through GET actions/runs?head_sha=SHA&event=push&branch=master
    (issue #955: the records of this listing carry event, head_branch and
    check_suite_id - the data the verdict must be attributed from)."""
    def url_for_page(page):
        return ("repos/%s/actions/runs?head_sha=%s&event=push&branch=master"
                "&per_page=%d&page=%d" % (repo, sha, per_page, page))
    return _paginate(gh_bin, url_for_page, "workflow_runs", out_prefix,
                     per_page, max_pages)


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
                  "--out-prefix P [--runs-out-prefix P] [--gh GH] "
                  "[--per-page N] [--max-pages N]", file=sys.stderr)
            return 1
        per_page = int(opts.get("per-page", 100))
        max_pages = int(opts.get("max-pages", 50))
        gh_bin = opts.get("gh", "gh")
        runs_prefix = opts.get("runs-out-prefix",
                               opts["out-prefix"] + "runs-")
        paths, status = fetch_pages(sha, opts["out-prefix"], opts["repo"],
                                    gh_bin=gh_bin, per_page=per_page,
                                    max_pages=max_pages)
        rpaths, rstatus = fetch_runs_pages(sha, runs_prefix, opts["repo"],
                                           gh_bin=gh_bin, per_page=per_page,
                                           max_pages=max_pages)
        # exordy finding 4714995326: completeness is required only to ACCEPT
        # reuse, never to honour a visible block. A completed non-success
        # among the runs we did fetch must block the publish even when a
        # listing came back truncated or the gh call later failed - the
        # in-job fallback gate must never outrace an observed red. Issue
        # #955: attribution needs both listings, so block visibility requires
        # at least one page of each; without the runs listing a visible red
        # cannot be attributed and the fallback gate judges it itself.
        if paths and rpaths:
            try:
                merged = load_payload(paths, rpaths)
            except (OSError, ValueError):
                merged = None
            if merged is not None and decide(merged) == "block":
                print("block (%d check-run pages, %d run pages)"
                      % (len(paths), len(rpaths)))
                return 2
        if status == "complete" and rstatus == "complete":
            combined = "complete"
        elif "error" in (status, rstatus):
            combined = "error"
        else:
            combined = "truncated"
        print("%s (%d check-run pages, %d run pages)"
              % (combined, len(paths), len(rpaths)))
        return {"complete": 0, "truncated": 3, "error": 4}[combined]
    check_paths = []
    run_paths = []
    mode = None
    for arg in args:
        if arg == "--check-runs":
            mode = "check"
            continue
        if arg == "--runs":
            mode = "runs"
            continue
        if mode is None:
            mode = "legacy"
        if mode == "check":
            check_paths.append(arg)
        elif mode == "runs":
            run_paths.append(arg)
        else:
            check_paths.append(arg)
    if not check_paths:
        print("usage: master-gate-verdict.py --check-runs CHECK_RUNS_JSON "
              "[PAGE_JSON...] --runs RUNS_JSON [PAGE_JSON...]",
              file=sys.stderr)
        return 1
    try:
        if mode == "legacy":
            # No runs listing: suites cannot be attributed, fail closed.
            payload = {"check_runs": load_merged(check_paths, "check_runs"),
                       "master_push_runs": []}
        else:
            payload = load_payload(check_paths, run_paths)
    except (OSError, ValueError) as error:
        print("cannot read inputs: %s" % error, file=sys.stderr)
        return 1
    verdict = decide(payload)
    print(verdict)
    return {"reuse": 0, "wait": 1, "block": 2}[verdict]


if __name__ == "__main__":
    sys.exit(main(sys.argv))
