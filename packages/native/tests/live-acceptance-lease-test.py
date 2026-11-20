#!/usr/bin/env python3
"""Exercise the operator and hosted job with in-memory synthetic leases."""
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import subprocess
import yaml
import tempfile
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"


def load(name):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


broker = load("live-acceptance-lease")
hosted = load("run-hosted-acceptance")
fixture = dict(origin="https://synthetic.invalid", project="test-project", username="member",
               password="example-only", session_token="example-session", read_boundary="marker",
               peers={"writer": {"token": "example-peer"}})


class BrokerTest(unittest.TestCase):
    def setUp(self):
        self.run = dict(id=18, run_attempt=2, head_sha="candidate", head_branch="master",
                        head_repository={"full_name": broker.REPO}, status="waiting",
                        path=".github/workflows/" + broker.WORKFLOW, event="schedule")
        self.requests = []
        self.secrets = {}
        self.pending = True
        self.approval_error = False
        self.rules = {"deployment_branch_policy": {"protected_branches": False, "custom_branch_policies": True},
                      "protection_rules": [{"type": "required_reviewers", "reviewers": [{"reviewer": {"id": 7}}]}]}

    def github(self, *args, data=None):
        self.requests.append(args)
        if args[:2] == ("api", "--paginate"):
            return "\n".join(self.secrets).encode()
        if args[:2] == ("api", "--method"):
            self.assertEqual(json.loads(data)["environment_ids"], [29])
            if self.approval_error:
                raise RuntimeError("Interrupted approval")
            return b"{}"
        if args[0] == "api":
            path = args[1]
            if path.endswith("/pending_deployments"):
                result = ([dict(environment={"name": broker.ENVIRONMENT, "id": 29},
                                current_user_can_approve=True)] if self.pending else [])
            elif path.endswith("/deployment-branch-policies"):
                result = {"branch_policies": [{"type": "branch", "name": "master"}]}
            elif "/environments/" in path:
                result = self.rules
            elif path == "user":
                result = {"id": 7}
            else:
                result = self.run
            return json.dumps(result).encode()
        self.assertEqual(list(args[3:]), ["--repo", broker.REPO, "--env", broker.ENVIRONMENT])
        if args[:2] == ("secret", "set"):
            self.secrets[args[2]] = json.loads(data)
        elif args[:2] == ("secret", "delete"):
            del self.secrets[args[2]]
        else:
            self.fail(args)
        return b""

    def invoke(self, action, attempt="2"):
        with patch.object(broker, "gh", self.github), patch.object(sys, "argv", ["lease", action, "18", attempt]), \
             patch.object(sys, "stdin", io.StringIO(json.dumps(fixture))):
            return broker.main()

    def test_grant_is_bound_to_the_requested_run_and_attempt(self):
        self.assertEqual(self.invoke("grant"), 0)
        self.assertEqual(self.secrets, {"LIVE_LEASE_18_2": {"run_id": 18, "attempt": 2,
                                                            "fixture": fixture, "limits": None}})

    def test_ineligible_run_and_missing_approval_make_no_secret(self):
        for field, value in (("event", "pull_request"), ("head_branch", "feature"),
                             ("head_repository", {"full_name": "example/fork"}), ("run_attempt", 3)):
            with self.subTest(field=field), patch.dict(self.run, {field: value}), self.assertRaises(RuntimeError):
                self.invoke("grant")
        with patch.dict(self.rules, protection_rules=[]), self.assertRaises(RuntimeError):
            self.invoke("grant")
        self.pending = False
        with self.assertRaises(RuntimeError):
            self.invoke("grant")
        self.assertFalse(self.secrets)
        self.assertFalse(any(request[0] == "secret" for request in self.requests))

    def test_unavailable_allows_an_uncredentialed_blocked_run(self):
        self.assertEqual(self.invoke("unavailable"), 0)
        self.assertFalse(self.secrets)
        self.invoke("grant")
        with self.assertRaises(RuntimeError):
            self.invoke("unavailable")

    def test_uncertain_approval_can_retry_then_release_only_its_own_lease(self):
        self.approval_error = True
        with self.assertRaises(RuntimeError):
            self.invoke("grant")
        self.assertIn("LIVE_LEASE_18_2", self.secrets)
        self.approval_error = False
        self.assertEqual(self.invoke("grant"), 0)
        with self.assertRaises(RuntimeError):
            self.invoke("release")
        self.run["status"] = "completed"
        self.secrets["LIVE_LEASE_18_3"] = {}
        self.assertEqual(self.invoke("release"), 0)
        self.assertEqual(self.invoke("release"), 0)
        self.assertEqual(self.secrets, {"LIVE_LEASE_18_3": {}})


class WorkflowTest(unittest.TestCase):
    def test_failed_packaging_writes_and_publishes_its_own_blocked_result(self):
        workflow = SCRIPTS.parents[2] / ".github/workflows/live-gui-acceptance.yml"
        job = yaml.safe_load(workflow.read_text())["jobs"]["package-linux"]
        failure_steps = [step for step in job["steps"] if step.get("if") == "failure()"]
        commands = [step["run"] for step in failure_steps if "run" in step]
        artifacts = [step["with"] for step in failure_steps if "actions/upload-artifact@" in step.get("uses", "")]
        self.assertEqual(len(commands), 1)
        self.assertEqual(len(artifacts), 1)
        with tempfile.TemporaryDirectory() as directory:
            env = dict(os.environ, RUNNER_TEMP=directory, GITHUB_RUN_ID="71", GITHUB_RUN_ATTEMPT="1")
            subprocess.run(["bash", "-euc", commands[0]], env=env, check=True)
            path = artifacts[0]["path"].replace("${{ runner.temp }}", directory)
            path = path.replace("${{ github.run_id }}", "71").replace("${{ github.run_attempt }}", "1")
            self.assertEqual(json.loads(Path(path).read_text()), {"status": "BLOCKED", "blocked": ["linux-package"]})


class HostedTest(unittest.TestCase):
    def test_missing_and_stale_leases_produce_a_sanitized_blocked_report(self):
        for raw in ("", json.dumps({"run_id": 8, "attempt": 1, "fixture": fixture})):
            with self.subTest(raw=bool(raw)), tempfile.TemporaryDirectory() as directory, \
                 patch.dict(os.environ, FIXTURE_LEASE=raw, GITHUB_RUN_ID="8", GITHUB_RUN_ATTEMPT="2"), \
                 patch.object(sys, "argv", ["hosted", "--package", "package", "--output", directory]), \
                 patch.object(hosted.subprocess, "run") as child:
                self.assertEqual(hosted.main(), 2)
                child.assert_not_called()
                report = json.loads((Path(directory) / "report.json").read_text())
                self.assertEqual(report, {"status": "BLOCKED", "blocked": ["fixture-lease"]})

    def test_job_uses_hosted_reference_and_sends_fixture_only_through_stdin(self):
        raw = json.dumps({"run_id": 8, "attempt": 2, "fixture": fixture})
        with tempfile.TemporaryDirectory() as directory, \
             patch.dict(os.environ, FIXTURE_LEASE=raw, GITHUB_RUN_ID="8", GITHUB_RUN_ATTEMPT="2", MODE="smoke"), \
             patch.object(sys, "argv", ["hosted", "--package", "package", "--output", directory]), \
             patch.object(hosted.subprocess, "run") as child:
            child.return_value.returncode = 3
            self.assertEqual(hosted.main(), 3)
            args, kwargs = child.call_args
            self.assertEqual(args[0][:4], ["runuser", "-u", "live-wayland", "--"])
            self.assertEqual(set(kwargs["env"]), {"PATH", "LANG"})
            self.assertNotIn(fixture["password"], repr(args))
            self.assertEqual(json.loads(kwargs["input"])["reference"], "github-ubuntu-24.04-container-26.04-sway-1440")


if __name__ == "__main__":
    unittest.main()
