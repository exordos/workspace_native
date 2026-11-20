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

The fixture provider is a local executable that writes exactly one JSON object to stdout. Pipe its output directly to the runner. Load credentials from the runner's secret store and mint project-scoped tokens for the designated peer and a second session of the primary GUI identity immediately before execution. `otp` is optional and, when present, is the current code. The origin must be the deployed test service. The caller selects the peer with `--peer-role`.

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
  "session_token": "<short-lived primary session token>",
  "peers": {"writer": {"uuid": "00000000-0000-4000-8000-000000000006", "token": "<short-lived scoped token>"}}
}
```

Prepare two accessible topics in one ordinary stream. Seed topic A with enough history for several older requests, multiple authors, long and nested replies, date groups, reactions and asynchronously loaded images. Provide a fresh primary-authored, peer-unread read-boundary message in topic A for each run. Make topic B independently warmable. The peer posts during sustained scroll, verifies the route, and edits, reacts to and removes the synthetic message. A second temporary arrival invalidates the other warmed conversation before delayed revalidation. It advances its own read cursor within topic A and publishes presence. Workspace routes read-state events to the reader. During typing, the peer creates a fresh marker in topic B. A second ordinary session of the primary identity reads it and the packaged client observes the matching personal read event. The marker is removed afterwards. Application traffic still uses end-to-end TLS. A loopback CONNECT tunnel forwards encrypted bytes to the exact fixture host, delays replies and drops connections for the fault scenarios.

## Evidence and review

`timeline.jsonl` contains numeric monotonic timestamps, UI-phase markers, pane-delivered wheel deltas and phases, GUI heartbeats, Qt frame swaps, cache hits/misses, anonymous row anchors and request-class timing. The runner samples process RSS/CPU externally. During authenticated history interaction it records low-resolution per-cell occupancy and separate digests for the message pane and composer. A keyboard-dispatch marker and the observed draft length associate visible changes with typing. Original pixels, visible text, URLs, identifiers, credentials, transport bodies and application output stay out of shared artifacts. The recorder continuously downsamples the conversation area in memory. The delayed-cache check requires a captured frame from the selected topic while its own history request is still awaiting a response. It separately measures whether delivered wheel input moves that cached viewport and changes the captured pane before the response arrives. Open `frames.gif` to replay the anonymous occupancy grids around each transition. The report gives the first and last monotonic timestamps, and the timeline preserves every original sample. Captured-frame latency includes compositor capture and filtering time. Qt frame swaps count renderer submissions. For rapid input, the report lists the number of individually attributed wheel responses and the number that remained unmatched. A viewport movement and a changed capture each contribute to at most one latency sample. Unmatched input keeps a complete response-latency decision blocked. Reconnection requires a successful head-history response for the selected topic after restoration and a later ready sample of that same topic.

Exit 0 marks a successful run, exit 2 marks a missing precondition and exit 3 marks a measured regression. Initial reference runs can omit `--limits` to collect measurements. They remain BLOCKED until the desktop maintainer reviews repeated runs on the same reference environment and records the tolerances. A reference file includes `reference`, `gui_stall_ms`, `active_frame_ms`, `visible_response_ms`, `settled_anchor_px` and `min_occupancy`. Record the compositor/scale, package and backend versions, hardware, repeated measurements, and rationale when approving a baseline. A failed gate never rewrites the baseline.

`--mode controls` deliberately pauses the isolated process, moves a stopped viewport, briefly switches to a blank compositor workspace, and omits a requested wheel delivery. Each control checks its own expected result. Without a limits file, this mode measures the idle pane immediately before injection, uses that sensitivity for the four controls, and returns CONTROLS. Those values never qualify a smoke or extended run. The credential-free detector tests also replay a single missing frame or input event in ordinary CI.

## Measured Linux candidate, 2026-11-20

I ran the packaged 0.41.0 candidate against disposable Workspace 3.1.6 using Qt 6.10.2, Sway 1.11, pixman, 1440 × 900 at scale 1 and eight logical CPUs. The fixture included more than 600 messages, two warmed topics, mixed authors, three dates, nested replies, images, reactions and contacts.

On an early extended run the longest interval between GUI heartbeats was **4,473 ms**. It occurred 16.780–21.253 seconds into cold start, as the selected topic went from zero to fifty rows. One Qt frame submission and 244 compositor captures occurred during that interval. A later smoke run recorded **2,208 ms** across the end of paged scrolling, the entire stopped interval and the start of typing. Two Qt submissions and 133 compositor captures occurred in that interval. These observations have no approved performance tolerance.

One repeated extended run recorded 4,936 compositor captures over the authenticated workload. The trace contains eight loaded-range wheel deliveries and 480 while traversing history, with ten older-history requests. Requiring one distinct viewport movement and one changed capture per attribution matched five loaded-range inputs (p95 91 ms) and 49 paging inputs (p95 763 ms). The remaining inputs could not be individually attributed under coalescing; the report records them and blocks complete input acceptance. Captured cached rows appeared while the selected topic's own delayed revalidation was pending. The four deliberate process, anchor, blank-frame and input controls each produced their expected result.

A further extended run with the updated paging and receipt handling captured **123 frames of cached rows** before the target topic's delayed history reply completed. A native wheel moved the cached viewport and the pane capture changed 67 ms after delivery, still inside that request. The client received the unique personal read event for the secondary-topic marker during typing, alongside presence and the peer reaction snapshot. This run recorded 4,680 compositor captures, nine older-history requests and 476 delivered paging inputs. The distinct-movement replay attributed 48 paging inputs (p95 752 ms). Its offline switch still showed zero rows in all 25 samples across 801 ms.

Both smoke and extended runs failed the offline navigation check. After opening the previously populated timeline during a connection cut, 25 successive samples in smoke and 31 in extended reported zero rows. The two observation windows ending at the independent TLS probe lasted 843 and 883 ms. The probe re-established TLS through the same tunnel, but the application did not demonstrate a successful request and ready history within the ensuing 20-second check. These are measurements from the virtual Wayland reference with software composition. Physical GPU and touchpad results, reviewed reference limits and a recurring deployed run are still outstanding.

## Cadence

The **Live Wayland acceptance** workflow packages the selected master revision on GitHub Actions. It runs smoke Monday–Saturday and extended on Sunday at 03:19 UTC. Maintainers can dispatch smoke, extended or controls on master. The Wayland job unpacks the same run’s checksummed Linux archive, then starts an unprivileged, isolated desktop in an Ubuntu 26.04 container. It retains the sanitized timeline, report and frames for 14 days. The hosted reference is `github-ubuntu-24.04-container-26.04-sway-1440`. Collect and review its own initial measurements before supplying thresholds.

The fixture operator manages the `workspace-native-live-acceptance` GitHub environment. Restrict deployments to the master branch and configure the operator as its sole required reviewer. The operator provisions a disposable Workspace with the test dataset and ordinary synthetic member accounts. Its private provider writes the fixture described above to a pipe. The following commands target this repository and require the operator’s authenticated GitHub CLI.

```sh
lease=packages/native/scripts/live-acceptance-lease.py
python3 "$lease" pending
fixture-provider | python3 "$lease" grant "$run_id" "$attempt"
# If approved tolerances exist, add --limits hosted-reference.json to grant.
python3 "$lease" release "$run_id" "$attempt"
```

Grant waits for packaging to finish and verifies the requesting workflow, attempt, master revision and environment rules. It assigns the fixture to that attempt and approves its waiting Wayland job. The operator calls release after the run terminates, verifies that GitHub removed the run-specific secret, and disposes of the fixture. An interrupted grant can be retried for the same waiting attempt. If provisioning fails before credentials have been assigned, `python3 "$lease" unavailable "$run_id" "$attempt"` lets the job record BLOCKED. An unavailable operator leaves the job visibly waiting for its fixture.

Before releases affecting startup, cache, history, scrolling or layout, the release maintainer runs the relevant mode against the candidate package and reviews its timeline. The desktop and release maintainers triage failures and keep an affected issue open until the failed transition passes again.
