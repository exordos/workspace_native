#!/usr/bin/env python3
"""Release a hosted Wayland job once its disposable Workspace fixture is ready."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

REPO = "exordos/workspace_native"
API = "repos/" + REPO
WORKFLOW = "live-gui-acceptance.yml"
ENVIRONMENT = "workspace-native-live-acceptance"


def gh(*args, data=None):
    result = subprocess.run(["gh", *args], input=data, capture_output=True)
    if result.returncode:
        raise RuntimeError("GitHub request failed (" + " ".join(args[:2]) + ")")
    return result.stdout


def get(path):
    return json.loads(gh("api", API + "/" + path))


def eligible(run):
    return (run["event"] in ("schedule", "workflow_dispatch")
            and run["path"] == ".github/workflows/" + WORKFLOW
            and run["head_branch"] == "master"
            and run["head_repository"]["full_name"] == REPO)


def pending(run_id):
    for wait in get(f"actions/runs/{run_id}/pending_deployments"):
        if wait["environment"]["name"] == ENVIRONMENT and wait["current_user_can_approve"]:
            return wait["environment"]["id"]
    raise RuntimeError("The live acceptance job is not waiting for this operator")


def check_protection():
    url = "environments/" + ENVIRONMENT
    policy = get(url)
    if policy["deployment_branch_policy"] != {"protected_branches": False, "custom_branch_policies": True}:
        raise RuntimeError("Environment branch protection changed")
    branches = get(url + "/deployment-branch-policies")["branch_policies"]
    if [(p["name"], p["type"]) for p in branches] != [("master", "branch")]:
        raise RuntimeError("Environment deployment refs changed")
    broker = json.loads(gh("api", "user"))["id"]
    reviewers = [r["reviewer"]["id"] for rule in policy["protection_rules"]
                 if rule["type"] == "required_reviewers" for r in rule["reviewers"]]
    if reviewers != [broker]:
        raise RuntimeError("The current operator does not own fixture release")


def stored(name):
    path = f"{API}/environments/{ENVIRONMENT}/secrets?per_page=100"
    names = gh("api", "--paginate", path, "--jq", ".secrets[].name").decode().splitlines()
    return name in names


def waiting_runs():
    # The operator can be installed before the workflow lands on the default branch.
    workflows = gh("api", "--paginate", API + "/actions/workflows?per_page=100",
                   "--jq", ".workflows[].path").decode().splitlines()
    if ".github/workflows/" + WORKFLOW not in workflows:
        return []
    return [run for run in get(f"actions/workflows/{WORKFLOW}/runs?status=waiting&per_page=100")["workflow_runs"]
            if eligible(run)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    ops = parser.add_subparsers(dest="action", required=True)
    ops.add_parser("pending")
    grant = ops.add_parser("grant", help="Read one ordinary test fixture from stdin")
    grant.add_argument("--limits", type=Path)
    unavailable = ops.add_parser("unavailable", help="Allow this job to record an unavailable fixture")
    for op in (grant, unavailable, ops.add_parser("release")):
        op.add_argument("run", type=int)
        op.add_argument("attempt", type=int)
    args = parser.parse_args()
    if args.action == "pending":
        for run in waiting_runs():
            print(run["id"], run["run_attempt"])
        return 0

    if args.run < 1 or args.attempt < 1:
        raise RuntimeError("Run and attempt must be positive")
    run = get(f"actions/runs/{args.run}/attempts/{args.attempt}")
    if not eligible(run) or run["id"] != args.run or run["run_attempt"] != args.attempt:
        raise RuntimeError("Run identity does not match the accepted workflow")
    name = f"LIVE_LEASE_{args.run}_{args.attempt}"
    target = ["--repo", REPO, "--env", ENVIRONMENT]

    if args.action == "release":
        if run["status"] != "completed":
            raise RuntimeError("The run has not finished")
        if stored(name):
            gh("secret", "delete", name, *target)
            if stored(name):
                raise RuntimeError("The expired fixture secret is still present")
        return 0

    current = get(f"actions/runs/{args.run}")
    if (current["run_attempt"] != args.attempt or current["status"] != "waiting"
            or current["head_sha"] != run["head_sha"]):
        raise RuntimeError("The requested attempt is no longer waiting")
    check_protection()
    env_id = pending(args.run)
    if args.action == "unavailable":
        if stored(name):
            raise RuntimeError("A fixture is already assigned to this attempt")
        comment = "The fixture was unavailable. Record the infrastructure result."
    else:
        fixture = json.load(sys.stdin)
        if not isinstance(fixture, dict) or not all(fixture.get(k) for k in
                ("origin", "project", "username", "password", "peers", "read_boundary", "session_token")):
            raise RuntimeError("The provider did not return a complete test fixture")
        lease = {"run_id": args.run, "attempt": args.attempt, "fixture": fixture,
                 "limits": json.loads(args.limits.read_text()) if args.limits else None}
        gh("secret", "set", name, *target, data=json.dumps(lease).encode())
        comment = "The dedicated Workspace fixture is ready for this run."
    # An interrupted approval can be retried for this attempt. After completion,
    # release removes the secret even if the original approval response was lost.
    payload = {"environment_ids": [env_id], "state": "approved", "comment": comment}
    gh("api", "--method", "POST", f"{API}/actions/runs/{args.run}/pending_deployments",
       "--input", "-", data=json.dumps(payload).encode())
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, OSError, KeyError, ValueError) as error:
        # Never include a provider's input or GitHub's response in diagnostics.
        print(str(error) if isinstance(error, RuntimeError) else "Invalid fixture, run, or GitHub response", file=sys.stderr)
        sys.exit(2)
