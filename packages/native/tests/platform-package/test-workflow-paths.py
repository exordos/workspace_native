"""Ensure a change to any native shell state producer runs the two platform checks."""
from fnmatch import fnmatchcase
from pathlib import Path
import unittest

import yaml


ROOT = Path(__file__).resolve().parents[4]


class DesktopAcceptanceWorkflowTest(unittest.TestCase):
    def test_platform_adapters_and_their_callers_trigger_acceptance(self):
        workflow = (ROOT / ".github/workflows/desktop-platform-packages.yml").read_text()
        # GitHub uses YAML 1.2. BaseLoader preserves its literal "on" key.
        patterns = yaml.load(workflow, Loader=yaml.BaseLoader)["on"]["pull_request"]["paths"]
        sources = ROOT / "packages/native/src"
        for source in sources.rglob("*"):
            if source.suffix not in {".cpp", ".mm"}:
                continue
            if '#include "native-platform-shell.h"' not in source.read_text(encoding="utf-8"):
                continue
            path = source.relative_to(ROOT).as_posix()
            with self.subTest(path=path):
                self.assertTrue(any(fnmatchcase(path, pattern) for pattern in patterns), path)


if __name__ == "__main__":
    unittest.main()
