"""Guard the Linux jobs required on GitHub's composed merge-queue revision."""

import pathlib
import unittest

import yaml


WORKFLOW = pathlib.Path(__file__).resolve().parents[3] / ".github/workflows/linux-release.yml"


class MergeGroupWorkflowTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workflow = yaml.load(WORKFLOW.read_text(), Loader=yaml.BaseLoader)
        cls.jobs = cls.workflow["jobs"]

    def test_queue_requests_the_same_gates_as_pull_requests(self):
        self.assertEqual(self.workflow["on"]["merge_group"]["types"], ["checks_requested"])
        gate_events = {
            "github.event_name == 'pull_request'",
            "github.event_name == 'merge_group'",
            "github.ref == 'refs/heads/master'",
        }
        for job in ("headless-tests", "full-gate-build", "full-gate"):
            with self.subTest(job=job):
                self.assertEqual(set(self.jobs[job]["if"].split(" || ")), gate_events)
        for name, dependency in (("quick-linux", "headless-tests"), ("full-gate-linux", "full-gate")):
            with self.subTest(job=name):
                report = self.jobs[name]
                self.assertEqual(report["needs"], dependency)
                guard = report["if"]
                prefix = "${{ !cancelled() && ("
                suffix = ") }}"
                self.assertTrue(guard.startswith(prefix) and guard.endswith(suffix))
                self.assertEqual(set(guard[len(prefix):-len(suffix)].split(" || ")), gate_events)

    def test_all_gates_check_out_the_event_revision(self):
        for job in ("package-linux", "headless-tests", "full-gate-build", "full-gate"):
            with self.subTest(job=job):
                checkout = next(step for step in self.jobs[job]["steps"]
                                if step.get("uses", "").startswith("actions/checkout@"))
                self.assertNotIn("ref", checkout.get("with", {}))

    def test_queue_builds_the_package_without_publishing_it(self):
        package = self.jobs["package-linux"]
        self.assertIn("github.event_name != 'pull_request' || github.base_ref == 'master'", package["if"])
        artifacts = [step for step in package["steps"] if step.get("uses", "").startswith("actions/upload-artifact@")]
        self.assertTrue(artifacts)
        for artifact in artifacts:
            self.assertEqual(set(artifact["if"].split(" || ")), {
                "github.event_name == 'push'", "github.event_name == 'workflow_dispatch'"})
        publish = next(step for step in package["steps"] if "gh release create" in step.get("run", ""))
        self.assertEqual(publish["if"], "github.ref_type == 'tag'")


if __name__ == "__main__":
    unittest.main()
