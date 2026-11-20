#!/usr/bin/env python3
"""Consume one authenticated run's fixture in a non-root Wayland session."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    # A previous run's delayed approval or secrets must never operate on its fixture.
    try:
        lease = json.loads(os.environ.pop("FIXTURE_LEASE", ""))
        if (str(lease["run_id"]), str(lease["attempt"])) != (
            os.environ["GITHUB_RUN_ID"], os.environ["GITHUB_RUN_ATTEMPT"]
        ):
            raise ValueError()
        fixture = lease["fixture"]
        mode = os.environ["MODE"]
        if mode not in ("controls", "smoke", "extended") or not isinstance(fixture, dict):
            raise ValueError()
        fixture["reference"] = "github-ubuntu-24.04-container-26.04-sway-1440"
    except (ValueError, KeyError, TypeError):
        (args.output / "report.json").write_text(json.dumps({"status": "BLOCKED", "blocked": ["fixture-lease"]}))
        return 2
    cmd = ["runuser", "-u", "live-wayland", "--", sys.executable,
           str(Path(__file__).with_name("live-workspace-acceptance.py")),
           "--mode", mode, "--package", args.package, "--output", str(args.output)]
    limits = Path(__file__).with_name("hosted-wayland-reference.json")
    if lease.get("limits") is not None:
        limits = args.output / "limits.json"
        limits.write_text(json.dumps(lease["limits"]))
    cmd += ["--limits", str(limits)]
    return subprocess.run(cmd, input=json.dumps(fixture).encode(),
                          env={"PATH": os.environ["PATH"], "LANG": "C.UTF-8"}).returncode


if __name__ == "__main__":
    sys.exit(main())
