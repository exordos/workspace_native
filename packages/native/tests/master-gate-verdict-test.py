#!/usr/bin/env python3
"""Regression tests for scripts/master-gate-verdict.py (tag publish gate).

Fixtures replay the REAL API shapes (issue #955): the
commits/{sha}/check-runs listing returns a stub check_suite carrying ONLY an
"id" field - it never carries event/head_branch - so master-push attribution
comes from the actions/runs listing (head_sha+event=push+branch=master), whose
records do carry event, head_branch and check_suite_id. Check-runs are matched
only when their check_suite.id belongs to one of those push runs. The exact
races the review findings produced stay covered:
- an older green pair must not authorize reuse while a newer matching run
  (re-check / rerun / second master push of the same sha) is still running;
- a completed non-success must block even when the other check is green;
- the tag's own same-name check-runs (its full-gate-linux is "skipped") must
  neither block nor authorize reuse.
"""
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "..", "scripts", "master-gate-verdict.py")

# Suite ids of the check-runs listing's stub check_suite objects.
PUSH_SUITE = 124700721961      # the master-push gate run's check suite
TAG_SUITE = 124712543381       # the tag run's own check suite (event=tag)
PR_SUITE = 124799999999        # an unrelated pull_request run's suite


def load_script():
    spec = importlib.util.spec_from_file_location("master_gate_verdict", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def check(name, status="completed", conclusion="success", suite=PUSH_SUITE):
    """Real commits/{sha}/check-runs record: check_suite is a {id} stub."""
    return {
        "name": name,
        "status": status,
        "conclusion": conclusion if status == "completed" else None,
        "check_suite": {"id": suite},
    }


def push_run(suite=PUSH_SUITE, status="completed", conclusion="success",
             event="push", branch="master"):
    """actions/runs listing record for the master-push workflow run."""
    return {
        "id": 46095110765,
        "name": "Linux release artifacts",
        "event": event,
        "head_branch": branch,
        "status": status,
        "conclusion": conclusion if status == "completed" else None,
        "check_suite_id": suite,
    }


class DecideTests(unittest.TestCase):
    def decide(self, check_runs, push_runs=(PUSH_SUITE,), **kw):
        runs = []
        for suite in push_runs:
            if isinstance(suite, dict):
                runs.append(suite)
            else:
                runs.append(push_run(suite, **kw))
        return load_script().decide({"check_runs": list(check_runs),
                                     "master_push_runs": runs})

    def test_real_shape_green_pair_reuses(self):
        # Req 3 regression: a real-shaped green master-push pair yields reuse.
        self.assertEqual(self.decide(
            [check("package-linux"), check("full-gate-linux")]), "reuse")

    def test_green_pair_plus_tag_own_skipped_reuses(self):
        # Req 2/3: the tag's own same-name check-runs on the same sha (its
        # full-gate-linux is skipped, package-linux is its fallback job) must
        # neither block nor authorize reuse - they belong to another suite.
        # A tag-shaped record sneaking into the listing is ignored too.
        self.assertEqual(self.decide(
            [check("package-linux"), check("full-gate-linux"),
             check("full-gate-linux", conclusion="skipped", suite=TAG_SUITE),
             check("package-linux", status="in_progress", suite=TAG_SUITE)],
            push_runs=[PUSH_SUITE,
                       push_run(TAG_SUITE, event="tag", branch="0.51.1")]),
            "reuse")

    def test_incomplete_master_push_run_waits(self):
        # A newer master-push run still in flight (re-check / rerun) forbids
        # reuse even when an older green pair is already recorded.
        self.assertEqual(self.decide(
            [check("package-linux"), check("full-gate-linux")],
            push_runs=[push_run(),
                       push_run(PUSH_SUITE + 1, status="in_progress",
                                conclusion=None)]), "wait")

    def test_queued_matching_check_run_waits(self):
        # exordy finding: an earlier green pair must not authorize reuse
        # while another matching gate check is still running.
        self.assertEqual(self.decide(
            [check("package-linux"), check("full-gate-linux"),
             check("full-gate-linux", status="queued")]), "wait")

    def test_in_progress_recheck_waits(self):
        # exordy finding: a previous green verdict must not be reused while a
        # newer matching check (a re-check that may return red) is in flight.
        self.assertEqual(self.decide(
            [check("package-linux"), check("full-gate-linux"),
             check("package-linux", status="in_progress")]), "wait")

    def test_one_red_blocks_immediately(self):
        self.assertEqual(self.decide(
            [check("package-linux"),
             check("full-gate-linux", conclusion="failure")]), "block")

    def test_red_beside_queued_green_other_still_blocks(self):
        self.assertEqual(self.decide(
            [check("package-linux"),
             check("full-gate-linux", conclusion="cancelled"),
             check("package-linux", status="queued")]), "block")

    def test_red_in_flight_run_still_blocks_first(self):
        # The block honor outranks the in-flight wait: one red must never be
        # outraced, even while another master-push run is still running.
        self.assertEqual(self.decide(
            [check("full-gate-linux", conclusion="failure")],
            push_runs=[push_run(),
                       push_run(PUSH_SUITE + 1, status="in_progress",
                                conclusion=None)]), "block")

    def test_duplicate_green_single_name_waits(self):
        # Regression: two green runs sharing one name must not satisfy a
        # gate shaped like a count while the other check never ran.
        self.assertEqual(self.decide(
            [check("package-linux"), check("package-linux")]), "wait")

    def test_only_one_required_name_green_waits(self):
        self.assertEqual(self.decide([check("package-linux")]), "wait")

    def test_no_matching_check_runs_waits(self):
        self.assertEqual(self.decide([]), "wait")

    def test_unknown_suites_are_ignored(self):
        # PR runs and any other event: their required-name checks belong to
        # foreign suites and must never participate in the verdict.
        self.assertEqual(self.decide(
            [check("package-linux", conclusion="failure", suite=PR_SUITE),
             check("full-gate-linux", suite=PR_SUITE)]), "wait")

    def test_missing_master_push_runs_fails_closed(self):
        # Legacy/defensive shape: without the actions/runs listing the script
        # cannot attribute suites, so it must never reuse (issue #955 is
        # exactly the failure mode of trusting unverifiable payload fields).
        module = load_script()
        self.assertEqual(module.decide(
            {"check_runs": [check("package-linux"),
                            check("full-gate-linux")]}), "wait")

    def test_push_run_without_suite_id_authorizes_nothing(self):
        broken = push_run()
        del broken["check_suite_id"]
        self.assertEqual(self.decide(
            [check("package-linux"), check("full-gate-linux")],
            push_runs=[broken]), "wait")


class CliTests(unittest.TestCase):
    def write(self, payload):
        handle = tempfile.NamedTemporaryFile("w", suffix=".json",
                                             delete=False)
        json.dump(payload, handle)
        handle.close()
        return handle.name

    def invoke(self, check_pages, run_pages):
        paths = [self.write(p) for p in check_pages]
        rpaths = [self.write(p) for p in run_pages]
        try:
            proc = subprocess.run(
                [sys.executable, SCRIPT, "--check-runs", *paths,
                 "--runs", *rpaths],
                capture_output=True, text=True)
            return proc.returncode, proc.stdout.strip()
        finally:
            for path in paths + rpaths:
                os.unlink(path)

    def pages(self, *check_runs):
        return [{"check_runs": list(group)} for group in check_runs]

    def runs_page(self, *records):
        return [{"workflow_runs": list(records)}]

    def test_cli_exit_codes_match_verdicts(self):
        pair = (self.pages([check("package-linux"),
                            check("full-gate-linux")]),
                self.runs_page(push_run()))
        wait = (self.pages([check("package-linux"),
                            check("full-gate-linux", status="queued")]),
                self.runs_page(push_run()))
        block = (self.pages([check("full-gate-linux", conclusion="failure")]),
                 self.runs_page(push_run()))
        self.assertEqual(self.invoke(*pair), (0, "reuse"))
        self.assertEqual(self.invoke(*wait), (1, "wait"))
        self.assertEqual(self.invoke(*block), (2, "block"))

    def test_cli_merges_all_pagination_pages(self):
        # exordy findings 4712353365/369: both listings are paginated; every
        # page must reach the decision - the green pair split across pages is
        # still reuse, and an in-flight match on the LAST page still forces
        # wait, as does a late red.
        split = self.invoke(
            self.pages([check("package-linux")], [check("full-gate-linux")]),
            self.runs_page(push_run()))
        self.assertEqual(split, (0, "reuse"))
        late_race = self.invoke(
            self.pages([check("package-linux"), check("full-gate-linux")],
                       [check("full-gate-linux", status="in_progress")]),
            self.runs_page(push_run()))
        self.assertEqual(late_race, (1, "wait"))
        late_red = self.invoke(
            self.pages([check("package-linux"), check("full-gate-linux")],
                       [check("full-gate-linux", conclusion="failure")]),
            self.runs_page(push_run()))
        self.assertEqual(late_red, (2, "block"))


STUB_GH = """#!/usr/bin/env python3
import json
import os
import sys

url = sys.argv[-1]
page = int(url.split("page=")[-1])
spec = json.loads(os.environ["STUB_SPEC"])
is_runs = "actions/runs" in url
if spec.get("fail_at") == page and (spec.get("fail_on", "check") ==
                                    ("runs" if is_runs else "check")):
    sys.exit(1)
key = "runs_pages" if is_runs else "pages"
count = spec.get(key, {}).get(str(page), 0)
if is_runs:
    records = []
    for i in range(count):
        records.append({"id": page * 1000 + i, "name": "Linux release artifacts",
                        "event": "push", "head_branch": "master",
                        "status": "completed", "conclusion": "success",
                        "check_suite_id": 900000 + page * 1000 + i})
    records.extend(spec.get("extra_records", {}).get(str(page), []))
    print(json.dumps({"workflow_runs": records}))
else:
    runs = [{"name": "pad-%d-%d" % (page, i),
             "status": "completed", "conclusion": "success",
             "check_suite": {"id": 555}}
            for i in range(count)]
    runs.extend(spec.get("extra_runs", {}).get(str(page), []))
    print(json.dumps({"check_runs": runs}))
"""

GREEN_PAIR = [check("package-linux"), check("full-gate-linux")]


class FetchModeTests(unittest.TestCase):
    """exordy findings 4712353365/369 round 2: a truncated listing must
    never be treated as complete. In --fetch mode the script paginates both
    listings itself; running out of page budget while the last page was still
    full reports INCOMPLETE (rc 3) so the caller can never decide reuse from
    pages that stopped before the listing ended. A gh failure is rc 4.
    Issue #955: the actions/runs listing is subject to the same discipline.
    """

    def invoke_fetch(self, spec, max_pages):
        with tempfile.TemporaryDirectory() as tmp:
            stub = os.path.join(tmp, "gh")
            with open(stub, "w", encoding="utf-8") as handle:
                handle.write(STUB_GH)
            os.chmod(stub, 0o755)
            env = dict(os.environ, STUB_SPEC=json.dumps(spec))
            prefix = os.path.join(tmp, "check-runs-page-")
            proc = subprocess.run(
                [sys.executable, SCRIPT, "--fetch", "deadbeef",
                 "--repo", "o/r", "--gh", stub, "--out-prefix", prefix,
                 "--max-pages", str(max_pages)],
                capture_output=True, text=True, env=env)
            written = sorted(os.path.basename(p)
                             for p in os.listdir(tmp)
                             if p.startswith("check-runs-page-"))
            return proc.returncode, proc.stdout.strip(), written

    def test_fetch_paginates_until_short_page(self):
        rc, out, written = self.invoke_fetch(
            {"pages": {"1": 100, "2": 100, "3": 3},
             "runs_pages": {"1": 2}}, max_pages=50)
        self.assertEqual(rc, 0, out)
        self.assertEqual(written, ["check-runs-page-1.json",
                                   "check-runs-page-2.json",
                                   "check-runs-page-3.json",
                                   "check-runs-page-runs-1.json"])

    def test_fetch_full_page_at_budget_is_incomplete(self):
        # Both fetched pages hold a green required pair, yet the fetch must
        # report INCOMPLETE: runs beyond the budget were never examined.
        rc, out, _written = self.invoke_fetch(
            {"pages": {"1": 100, "2": 100}, "runs_pages": {"1": 0}},
            max_pages=2)
        self.assertEqual(rc, 3, out)

    def test_fetch_runs_truncation_is_incomplete(self):
        # Issue #955: a FULL actions/runs page at the budget means unknown
        # push suites may exist beyond it - completeness is not established.
        rc, out, _written = self.invoke_fetch(
            {"pages": {"1": 0}, "runs_pages": {"1": 100}}, max_pages=1)
        self.assertEqual(rc, 3, out)

    def test_fetch_gh_failure_is_incomplete(self):
        rc, _out, _written = self.invoke_fetch(
            {"pages": {"1": 100}, "runs_pages": {"1": 0},
             "fail_at": 2, "fail_on": "check"}, max_pages=50)
        self.assertEqual(rc, 4)

    def test_fetch_runs_gh_failure_is_incomplete(self):
        rc, _out, _written = self.invoke_fetch(
            {"pages": {"1": 0}, "runs_pages": {"1": 100, "2": 0},
             "fail_at": 2, "fail_on": "runs"}, max_pages=50)
        self.assertEqual(rc, 4)

    def test_truncated_listing_with_visible_red_blocks(self):
        # exordy finding 4714995326: the in-job fallback gate must never
        # outrace a red master-push check that is already visible on the
        # fetched pages. Truncation forbids REUSE only; a visible completed
        # non-success still blocks the publish immediately.
        red = check("full-gate-linux", conclusion="failure", suite=900001)
        spec = {"pages": {"1": 100}, "extra_runs": {"1": [red]},
                "runs_pages": {"1": 1},
                "extra_records": {"1": [push_run(900001)]}}
        rc, out, _written = self.invoke_fetch(spec, max_pages=1)
        self.assertEqual(rc, 2, out)
        self.assertEqual(out.split(" ")[0], "block")

    def test_fetch_error_with_visible_red_still_blocks(self):
        # A gh failure after a page that already shows a red must block too:
        # the red was observed, the failure only means the listing is wider.
        red = check("package-linux", conclusion="timed_out", suite=900001)
        spec = {"pages": {"1": 100}, "extra_runs": {"1": [red]},
                "fail_at": 2, "fail_on": "check",
                "runs_pages": {"1": 1},
                "extra_records": {"1": [push_run(900001)]}}
        rc, out, _written = self.invoke_fetch(spec, max_pages=50)
        self.assertEqual(rc, 2, out)
        self.assertEqual(out.split(" ")[0], "block")

    def test_truncated_listing_without_red_stays_incomplete(self):
        # Guard for the guard: no visible red on a truncated listing must
        # keep the old fail-closed-for-reuse verdict rc 3 (fallback allowed).
        spec = {"pages": {"1": 100}, "extra_runs": {"1": list(GREEN_PAIR)},
                "runs_pages": {"1": 0}}
        rc, out, _written = self.invoke_fetch(spec, max_pages=1)
        self.assertEqual(rc, 3, out)

    def test_fetch_writes_verdict_from_complete_real_shape(self):
        # End-to-end through --fetch: a complete real-shape listing (push
        # suite from actions/runs, green pair attributed by suite id) has
        # rc 0; the decide pass then reuses (checked via the pages written).
        pair_page = [dict(c) for c in GREEN_PAIR]
        for c in pair_page:
            c["check_suite"] = {"id": 900001}
        spec = {"pages": {"1": 0}, "extra_runs": {"1": pair_page},
                "runs_pages": {"1": 0},
                "extra_records": {"1": [push_run(900001)]}}
        with tempfile.TemporaryDirectory() as tmp:
            stub = os.path.join(tmp, "gh")
            with open(stub, "w", encoding="utf-8") as handle:
                handle.write(STUB_GH)
            os.chmod(stub, 0o755)
            env = dict(os.environ, STUB_SPEC=json.dumps(spec))
            prefix = os.path.join(tmp, "check-runs-page-")
            fetch = subprocess.run(
                [sys.executable, SCRIPT, "--fetch", "deadbeef",
                 "--repo", "o/r", "--gh", stub, "--out-prefix", prefix],
                capture_output=True, text=True, env=env)
            self.assertEqual(fetch.returncode, 0, fetch.stdout)
            import glob
            check_pages = sorted(glob.glob(prefix + "[0-9]*.json"))
            run_pages = sorted(glob.glob(prefix + "runs-[0-9]*.json"))
            decide = subprocess.run(
                [sys.executable, SCRIPT, "--check-runs", *check_pages,
                 "--runs", *run_pages],
                capture_output=True, text=True)
            self.assertEqual((decide.returncode, decide.stdout.strip()),
                             (0, "reuse"))


if __name__ == "__main__":
    unittest.main()
