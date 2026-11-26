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


if __name__ == "__main__":
    unittest.main()
