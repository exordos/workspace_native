#!/usr/bin/env python3
"""Test master-push provenance, reruns and fail-closed pagination."""
import contextlib
import importlib.util
import io
import json
import pathlib
import subprocess
import sys
import unittest
import unittest.mock
import urllib.parse


SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "scripts/master-gate-verdict.py"
SPEC = importlib.util.spec_from_file_location("master_gate_verdict", SCRIPT)
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)
SHA = "deadbeef"


def workflow(suite=10, **fields):
    return dict({"check_suite_id": suite, "head_sha": SHA, "event": "push",
                 "head_branch": "master", "path": ".github/workflows/linux-release.yml",
                 "status": "completed"}, **fields)


def check(name, suite=10, status="completed", conclusion="success"):
    return {"check_suite": {"id": suite}, "name": name, "status": status,
            "conclusion": conclusion if status == "completed" else None}


def pair(suite=10):
    return [check("package-linux", suite), check("full-gate-linux", suite)]


class VerdictTests(unittest.TestCase):
    def test_tag_push_and_pr_suites_cannot_block_or_authorize_reuse(self):
        runs = [workflow(), workflow(20, head_branch="0.51.1"),
                workflow(30, event="pull_request"), workflow(40, head_sha="other"),
                workflow(50, path=".github/workflows/unrelated.yml")]
        other = [check("package-linux", 20, status="in_progress"),
                 check("full-gate-linux", 20, conclusion="skipped"),
                 check("full-gate-linux", 30, conclusion="failure"),
                 check("package-linux", 40), check("full-gate-linux", 50)]
        self.assertEqual(gate.decide(SHA, runs, pair() + other, True), "reuse")
        self.assertEqual(gate.decide(SHA, runs, other, True), "wait")

    def test_pending_check_alongside_green_pair_waits(self):
        for status in ("queued", "in_progress"):
            with self.subTest(status=status):
                checks = pair() + [check("package-linux", status=status)]
                self.assertEqual(gate.decide(SHA, [workflow()], checks, True), "wait")

    def test_reported_red_blocks_with_partial_results_and_pending_checks(self):
        for conclusion in ("failure", "cancelled", "timed_out", "skipped"):
            with self.subTest(conclusion=conclusion):
                checks = pair() + [check("package-linux", status="queued"),
                                   check("full-gate-linux", conclusion=conclusion)]
                self.assertEqual(gate.decide(SHA, [workflow()], checks, False), "block")

    def test_second_master_push_needs_a_complete_green_pair_of_its_own(self):
        for runs, checks in [
            ([workflow(), workflow(20, status="queued")], pair()),
            ([workflow(), workflow(20)], pair() + [check("package-linux", 20)]),
            ([workflow(), workflow(20)], [check("package-linux"), check("full-gate-linux", 20)]),
            ([workflow()], [check("package-linux")] * 2),
            ([], pair()), ([workflow()], []),
        ]:
            with self.subTest(runs=runs, checks=checks):
                self.assertEqual(gate.decide(SHA, runs, checks, True), "wait")
        self.assertEqual(gate.decide(SHA, [workflow(), workflow(20)],
                                     pair() + pair(20), True), "reuse")

    def test_incomplete_green_listing_never_authorizes_reuse(self):
        self.assertEqual(gate.decide(SHA, [workflow()], pair(), False), "wait")


class FetchTests(unittest.TestCase):
    def response(self, items, total):
        return subprocess.CompletedProcess([], 0, json.dumps({"workflow_runs": items,
                                                               "total_count": total}))

    def fetch(self, pages, max_pages=3):
        with unittest.mock.patch.object(subprocess, "run", side_effect=pages) as gh:
            result = gate.fetch_listing("o/r", "actions/runs", {"head_sha": SHA},
                                        "workflow_runs", per_page=1, max_pages=max_pages)
        return result, [call.args[0][-1] for call in gh.call_args_list]

    def test_pages_are_joined_until_a_short_page(self):
        result, urls = self.fetch([self.response([workflow()], 2),
                                   self.response([workflow(20)], 2), self.response([], 2)])
        self.assertEqual(result, ([workflow(), workflow(20)], True))
        self.assertEqual([urllib.parse.parse_qs(urllib.parse.urlsplit(url).query)["page"]
                          for url in urls], [["1"], ["2"], ["3"]])

    def test_page_limit_failed_page_and_server_cap_preserve_partial_results(self):
        first = self.response([workflow()], 2)
        for pages, limit in [
            ([first], 1), ([first, subprocess.CalledProcessError(1, ["gh"])], 3),
            ([first, self.response([], 2)], 3),
            ([first, subprocess.CompletedProcess([], 0, "{}")], 3),
            ([first, subprocess.CompletedProcess([], 0, "bad json")], 3),
        ]:
            with self.subTest(pages=pages):
                result, _ = self.fetch(pages, limit)
                self.assertEqual(result, ([workflow()], False))

    def test_red_with_missing_owner_triggers_a_suite_specific_lookup(self):
        red = check("package-linux", suite=20, conclusion="failure")
        for lookup, expected in [
            (([workflow(20)], True), "block"),
            (([], True), "incomplete"),  # Tag suite excluded by master/branch filter.
            (([], False), "unknown-red"),
        ]:
            replies = [([workflow()], False), (pair() + [red], True), lookup]
            with (self.subTest(expected=expected),
                  unittest.mock.patch.object(gate, "fetch_listing", side_effect=replies) as fetch):
                self.assertEqual(gate.resolve_verdict("o/r", SHA), expected)
                self.assertEqual(fetch.call_args.args[2], {"head_sha": SHA, "branch": "master",
                                                            "event": "push", "check_suite_id": 20})

    def test_cli_joins_endpoint_shapes_and_returns_workflow_exit_codes(self):
        for jobs, expected, rc in [(pair(), "reuse", 0), (pair()[:1], "wait", 1),
                                   ([check("package-linux", conclusion="failure")], "block", 2)]:
            with self.subTest(expected=expected):
                jobs = jobs + [check("full-gate-linux", 20, conclusion="skipped")]
                replies = [self.response([workflow()], 1),
                           subprocess.CompletedProcess([], 0, json.dumps(
                               {"check_runs": jobs, "total_count": len(jobs)}))]
                with (unittest.mock.patch.object(sys, "argv", ["gate", "--fetch", SHA, "--repo", "o/r"]),
                      unittest.mock.patch.object(subprocess, "run", side_effect=replies) as gh,
                      contextlib.redirect_stdout(io.StringIO()) as output):
                    self.assertEqual(gate.main(), rc)
                self.assertEqual(output.getvalue().strip(), expected)
                urls = [call.args[0] for call in gh.call_args_list]
                self.assertEqual(urls, [
                    ["gh", "api", "repos/o/r/actions/runs?head_sha=deadbeef&event=push&branch=master&per_page=100&page=1"],
                    ["gh", "api", "repos/o/r/commits/deadbeef/check-runs?filter=latest&per_page=100&page=1"],
                ])


if __name__ == "__main__":
    unittest.main()
