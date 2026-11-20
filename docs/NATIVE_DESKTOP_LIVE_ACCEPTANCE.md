# Live Wayland acceptance

The Linux acceptance runner launches the packaged desktop application twice against a disposable, deployed Workspace. The first launch starts with an empty profile. The second restores its real keyring session and populated cache. A private Sway compositor, session bus, keyring, user data directory and cache keep the run isolated from the operator's desktop. Every run needs a dedicated dataset and approved test identities. The runner deletes the synthetic messages it creates.

## Run locally

Use a Linux host with the release build dependencies, Python 3 with PyGObject/Atspi, Pillow and requests, D-Bus, gnome-keyring, Sway, wtype, wf-recorder, a C compiler, wayland-scanner, pkg-config and libwayland-client development files. The pointer driver uses the published wlroots virtual-pointer protocol, distributed with its upstream MIT notice in the protocol XML. The compositor runs a 1440 × 900 Wayland output at scale 1. A headless compositor uses the real Wayland input and screen-capture protocols. Physical touchpad acceleration, GPU drivers and presentation on a physical monitor need a separate run.

Build the Release target and use the same packager as the release workflow. The package must include the anonymous-pipe acceptance observer. The `probe` command opens its real login window, enters a neutral URL, and checks that Next becomes available and that Qt receives the compositor click. It makes no authenticated request. For an authentication-only check, pipe the fixture into the same command with `--mode login`. It signs in, closes the app normally, and verifies that a new process restores the session without re-entering credentials. Its PREFLIGHT result is separate from the workload decision.

```sh
cmake -S packages/native -B build-live -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-live --target workspace-native --parallel 2
version=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' build-live/CMakeCache.txt)
python3 packages/native/scripts/package-linux.py --build-dir build-live --output-dir package-live --version "$version"
package="package-live/workspace-native-$version-linux-x86_64"
python3 packages/native/scripts/live-workspace-acceptance.py --package "$package" --mode probe --output live-probe
fixture-provider | python3 packages/native/scripts/live-workspace-acceptance.py \
    --package "$package" --mode smoke --peer-role writer --limits approved-limits.json --output live-smoke
```

The fixture provider is a local executable that writes exactly one JSON object to stdout. Pipe its output directly to the runner. Load credentials from the runner's secret store and mint a project-scoped peer token immediately before execution. `otp` is optional and, when present, is the current code. The origin must be the deployed test service. The caller selects the peer with `--peer-role`.

```json
{
  "origin": "https://workspace.example.invalid",
  "backend_version": "1.2.3",
  "reference": "linux-sway-1440",
  "project": "00000000-0000-4000-8000-000000000001",
  "stream": "00000000-0000-4000-8000-000000000002",
  "topic_a": "00000000-0000-4000-8000-000000000003",
  "topic_b": "00000000-0000-4000-8000-000000000004",
  "read_boundary": "00000000-0000-4000-8000-000000000005",
  "username": "<dedicated client login>",
  "password": "<secret provider>",
  "peers": {"writer": {"uuid": "00000000-0000-4000-8000-000000000006", "token": "<short-lived scoped token>"}}
}
```

Prepare two accessible topics in one ordinary stream. Seed topic A with enough history for several older requests, multiple authors, long and nested replies, date groups, reactions and asynchronously loaded images. Place the designated read-boundary message in topic A. Make topic B independently warmable. The peer posts during sustained scroll, verifies the route, and edits, reacts to and removes the synthetic message. A second temporary arrival invalidates the other warmed conversation before delayed revalidation. It advances its own read cursor within topic A and publishes presence. Application traffic still uses end-to-end TLS. A loopback CONNECT tunnel forwards encrypted bytes to the exact fixture host, delays replies and drops connections for the fault scenarios.

## Evidence and review

`timeline.jsonl` contains numeric monotonic timestamps, UI-phase markers, pane-delivered wheel deltas and phases, GUI heartbeats, Qt frame swaps, cache hits/misses, anonymous row anchors and request-class timing. The runner samples process RSS/CPU externally. During authenticated history interaction it records low-resolution per-cell occupancy and separate digests for the message pane and composer. A keyboard-dispatch marker and the observed draft length associate visible changes with typing. Original pixels, visible text, URLs, identifiers, credentials, transport bodies and application output stay out of shared artifacts. The recorder continuously downsamples the conversation area in memory. The delayed-cache check requires a captured frame from the selected topic while its own history request is still awaiting a response. Captured-frame latency includes compositor capture and filtering time. Qt frame swaps count renderer submissions.

Exit 0 marks a successful run, exit 2 marks a missing precondition and exit 3 marks a measured regression. Initial reference runs can omit `--limits` to collect measurements. They remain BLOCKED until the desktop maintainer reviews repeated runs on the same reference environment and records the tolerances. A reference file includes `reference`, `gui_stall_ms`, `active_frame_ms`, `visible_response_ms`, `settled_anchor_px` and `min_occupancy`. Record the compositor/scale, package and backend versions, hardware, repeated measurements, and rationale when approving a baseline. A failed gate never rewrites the baseline.

`--mode controls` deliberately pauses the isolated process, moves a stopped viewport, briefly switches to a blank compositor workspace, and omits a requested wheel delivery. Each control checks its own expected result. The credential-free detector tests also replay a single missing frame or input event in ordinary CI.

## Cadence

The **Live Wayland acceptance** workflow runs smoke Monday–Saturday and extended on Sunday at 03:19 UTC. Maintainers can dispatch smoke, extended or controls on master. Register a dedicated self-hosted Linux x64 runner with label `workspace-native-wayland`, then configure repository variables `WORKSPACE_NATIVE_FIXTURE_PROVIDER` and `WORKSPACE_NATIVE_ACCEPTANCE_LIMITS` as absolute paths on that runner. Before releases affecting startup, cache, history, scrolling or layout, the release maintainer runs the relevant mode against the actual candidate package and reviews the timeline. The desktop and release maintainers triage failures, distinguish unavailable infrastructure from observed product defects, and keep the associated issue open until the failed transition passes again.
