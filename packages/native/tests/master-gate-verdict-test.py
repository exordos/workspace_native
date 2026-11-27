#!/usr/bin/env python3
"""Regression tests for scripts/master-gate-verdict.py (tag publish gate).

Fixtures replay real check-run shapes the master-gate-reuse lookup sees,
including the exact races the review findings produced:
- an older green pair must not authorize reuse while a newer matching run
  (re-check / rerun / second master push of the same sha) is still running;
- a completed non-success must block even when the other check is green.
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


def load_script():
    spec = importlib.util.spec_from_file_location("master_gate_verdict", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(name, status="completed", conclusion="success",
        event="push", branch="master"):
    return {
        "name": name,
        "status": status,
        "conclusion": conclusion if status == "completed" else None,
        "check_suite": {"event": event, "head_branch": branch},
    }


class DecideTests(unittest.TestCase):
    def decide(self, payload):
        return load_script().decide(payload)

    def payload(self, *runs):
        return {"check_runs": list(runs)}

    def test_green_pair_all_completed_reuses(self):
        payload = self.payload(run("package-linux"), run("full-gate-linux"))
        self.assertEqual(self.decide(payload), "reuse")

    def test_queued_matching_run_blocks_reuse_finding1(self):
        # exordy finding: an earlier green pair must not authorize reuse
        # while another matching gate check is still running.
        payload = self.payload(
            run("package-linux"), run("full-gate-linux"),
            run("full-gate-linux", status="queued"),
        )
        self.assertEqual(self.decide(payload), "wait")

    def test_newer_in_progress_run_blocks_reuse_finding2(self):
        # exordy finding: a previous green verdict must not be reused while a
        # newer matching check (a re-check that may return red) is in flight.
        payload = self.payload(
            run("package-linux"), run("full-gate-linux"),
            run("package-linux", status="in_progress"),
        )
        self.assertEqual(self.decide(payload), "wait")

    def test_one_red_blocks_immediately(self):
        payload = self.payload(
            run("package-linux"),
            run("full-gate-linux", status="completed", conclusion="failure"),
        )
        self.assertEqual(self.decide(payload), "block")

    def test_red_beside_queued_green_other_still_blocks(self):
        payload = self.payload(
            run("package-linux"),
            run("full-gate-linux", status="completed", conclusion="cancelled"),
            run("package-linux", status="queued"),
        )
        self.assertEqual(self.decide(payload), "block")

    def test_duplicate_green_single_name_waits(self):
        # Regression: two green runs sharing one name must not satisfy a
        # gate shaped like a count while the other check never ran.
        payload = self.payload(
            run("package-linux"), run("package-linux"),
        )
        self.assertEqual(self.decide(payload), "wait")

    def test_only_one_required_name_green_waits(self):
        payload = self.payload(run("package-linux"))
        self.assertEqual(self.decide(payload), "wait")

    def test_no_matching_runs_waits(self):
        self.assertEqual(self.decide(self.payload()), "wait")

    def test_non_master_push_runs_are_ignored(self):
        # The tag's own runs (event=tag) and PR runs must never match.
        payload = self.payload(
            run("package-linux"), run("full-gate-linux"),
            run("package-linux", event="tag", branch="0.50.3"),
            run("full-gate-linux", event="pull_request", branch="cassi/x"),
        )
        self.assertEqual(self.decide(payload), "reuse")

    def test_missing_check_suite_is_ignored(self):
        payload = {"check_runs": [{"name": "package-linux",
                                   "status": "completed",
                                   "conclusion": "success"}]}
        self.assertEqual(self.decide(payload), "wait")


class CliTests(unittest.TestCase):
    def invoke(self, payload):
        with tempfile.NamedTemporaryFile("w", suffix=".json",
                                         delete=False) as handle:
            json.dump(payload, handle)
            path = handle.name
        try:
            proc = subprocess.run([sys.executable, SCRIPT, path],
                                  capture_output=True, text=True)
            return proc.returncode, proc.stdout.strip()
        finally:
            os.unlink(path)

    def test_cli_exit_codes_match_verdicts(self):
        reuse = {"check_runs": [run("package-linux"), run("full-gate-linux")]}
        wait = {"check_runs": [run("package-linux"),
                               run("full-gate-linux", status="queued")]}
        block = {"check_runs": [
            run("full-gate-linux", status="completed", conclusion="failure")]}
        self.assertEqual(self.invoke(reuse), (0, "reuse"))
        self.assertEqual(self.invoke(wait), (1, "wait"))
        self.assertEqual(self.invoke(block), (2, "block"))

    def invoke_pages(self, *pages):
        paths = []
        for payload in pages:
            with tempfile.NamedTemporaryFile("w", suffix=".json",
                                             delete=False) as handle:
                json.dump(payload, handle)
                paths.append(handle.name)
        try:
            proc = subprocess.run([sys.executable, SCRIPT, *paths],
                                  capture_output=True, text=True)
            return proc.returncode, proc.stdout.strip()
        finally:
            for path in paths:
                os.unlink(path)

    def test_cli_merges_all_pagination_pages(self):
        # exordy findings 4712353365/369: the commit check-runs listing is
        # paginated; every page must reach the decision - the green pair split
        # across pages is still reuse, and an in-flight match on the LAST page
        # still forces wait.
        split = self.invoke_pages(
            {"check_runs": [run("package-linux")]},
            {"check_runs": [run("full-gate-linux")]})
        self.assertEqual(split, (0, "reuse"))
        late_race = self.invoke_pages(
            {"check_runs": [run("package-linux"), run("full-gate-linux")]},
            {"check_runs": [run("full-gate-linux", status="in_progress")]})
        self.assertEqual(late_race, (1, "wait"))
        late_red = self.invoke_pages(
            {"check_runs": [run("package-linux"), run("full-gate-linux")]},
            {"check_runs": [run("full-gate-linux", status="completed",
                                conclusion="failure")]})
        self.assertEqual(late_red, (2, "block"))


STUB_GH = """#!/usr/bin/env python3
import json
import os
import sys

url = sys.argv[-1]
page = int(url.split("page=")[-1])
spec = json.loads(os.environ["STUB_SPEC"])
if spec.get("fail_at") == page:
    sys.exit(1)
count = spec["pages"].get(str(page), 0)
runs = [{"name": "pad-%d-%d" % (page, i),
         "status": "completed", "conclusion": "success",
         "check_suite": {"event": "push", "head_branch": "master"}}
        for i in range(count)]
runs.extend(spec.get("extra_runs", {}).get(str(page), []))
print(json.dumps({"check_runs": runs}))
"""


class FetchModeTests(unittest.TestCase):
    """exordy findings 4712353365/369 round 2: a truncated listing must
    never be treated as complete. In --fetch mode the script paginates gh
    itself; running out of page budget while the last page was still full
    reports INCOMPLETE (rc 3) so the caller can never decide reuse from
    pages that stopped before the listing ended. A gh failure is rc 4."""

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
            {"pages": {"1": 100, "2": 100, "3": 3}}, max_pages=50)
        self.assertEqual(rc, 0, out)
        self.assertEqual(written, ["check-runs-page-1.json",
                                   "check-runs-page-2.json",
                                   "check-runs-page-3.json"])

    def test_fetch_full_page_at_budget_is_incomplete(self):
        # Both fetched pages hold a green required pair, yet the fetch must
        # report INCOMPLETE: runs beyond the budget were never examined.
        pages = {"1": 100, "2": 100}
        rc, out, _written = self.invoke_fetch({"pages": pages}, max_pages=2)
        self.assertEqual(rc, 3, out)

    def test_fetch_gh_failure_is_incomplete(self):
        rc, _out, _written = self.invoke_fetch(
            {"pages": {"1": 100}, "fail_at": 2}, max_pages=50)
        self.assertEqual(rc, 4)

    def test_truncated_listing_with_visible_red_blocks(self):
        # exordy finding 4714995326: the in-job fallback gate must never
        # outrace a red master-push check that is already visible on the
        # fetched pages. Truncation forbids REUSE only; a visible completed
        # non-success still blocks the publish immediately.
        spec = {"pages": {"1": 100},
                "extra_runs": {"1": [run("full-gate-linux", status="completed",
                                         conclusion="failure")]}}
        rc, out, _written = self.invoke_fetch(spec, max_pages=1)
        self.assertEqual(rc, 2, out)
        self.assertEqual(out.split(" ")[0], "block")

    def test_fetch_error_with_visible_red_still_blocks(self):
        # A gh failure after a page that already shows a red must block too:
        # the red was observed, the failure only means the listing is wider.
        spec = {"pages": {"1": 100}, "fail_at": 2,
                "extra_runs": {"1": [run("package-linux", status="completed",
                                         conclusion="timed_out")]}}
        rc, out, _written = self.invoke_fetch(spec, max_pages=50)
        self.assertEqual(rc, 2, out)
        self.assertEqual(out.split(" ")[0], "block")

    def test_truncated_listing_without_red_stays_incomplete(self):
        # Guard for the guard: no visible red on a truncated listing must
        # keep the old fail-closed-for-reuse verdict rc 3 (fallback allowed).
        spec = {"pages": {"1": 100},
                "extra_runs": {"1": [run("package-linux"),
                                     run("full-gate-linux")]}}
        rc, out, _written = self.invoke_fetch(spec, max_pages=1)
        self.assertEqual(rc, 3, out)


if __name__ == "__main__":
    unittest.main()
