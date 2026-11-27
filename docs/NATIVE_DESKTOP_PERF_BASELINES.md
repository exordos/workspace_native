# Warm-navigation and aggregate performance baselines (issue #13)

The [live Wayland acceptance suite](NATIVE_DESKTOP_LIVE_ACCEPTANCE.md) exercises the packaged client against a deployed Workspace.


`docs/NATIVE_DESKTOP_CACHE_BENCHMARK.md` records cache-component workload
baselines with RSS sampling, and its Window226 paragraph reported a single
warm-navigation observation: four topic selections needed two topic-history
GETs, two stream selections needed one topic-list and one aggregate-history
GET, with the repeated selections answered from the verified cache. That was a
one-off `qInfo` line, not a gate: nothing re-measured the scenario later and
nothing tracked a regression per release. This document defines the mechanical
replacement.

## Scope

One repeatable scenario, `warm-navigation`, built on the existing QtTest case
`warmNavigationUsesVerifiedSavedPagesWithoutExtraHttpRequests` in
`packages/native/tests/native-window-test.cpp`. It signs into a local
`MockServer`, opens the project, opens one stream, selects topics four times
(cold, cold, warm, warm interleaved with a stream revisit), and now emits a
machine-readable measurement document through the new
`CASSI_NATIVE_TEST_PERF_METRICS=<file>` seam. Aggregate navigation (the stream
selections) and the warm-cache path (the repeated topic selections reduced to
two GET requests) are both inside this scenario.

## Measurement contract

Each capture run records:

1. `requests` - exact GET counters per endpoint family
   (`topic-history`, `aggregate-history`, `topic-list`) and the cache hit
   `NativeCacheService::stats()`. These are deterministic against the local
   mock server, so they are exact gates: 4 topic selections must
   reduce to at most 2 topic-history GETs, 2 stream selections to at most 1
   aggregate-history GET, and the topic list may be fetched at most once.
   A counter above its gate is a redundant-request regression; this is the
   regression the issue names first. The approved baseline records the exact
   measured counts (2/1/1), so a change in either direction is still visible
   in the report delta.
2. `phases` - wall-clock milliseconds for named interaction phases: sign-in,
   project open, first (cold) stream open, each of the four topic selections
   and the warm stream revisit. Timings on this host fluctuate (shared
   llvmpipe/software GL, no panel-less compositor jitter), so per
   `docs/NATIVE_DESKTOP_PARITY.md` issue #16 policy every timing gate is a
   documented tolerance band derived from at least two measured runs on the
   reference stand, never one host's hard number without a named reason.
3. `process` - peak RSS in KiB sampled externally from `/proc/<pid>/status`
   (`VmHWM` plus the maximum `VmRSS` sample) at a 20 ms interval while the
   test binary runs, plus the harness-measured whole-case wall seconds.

## Files and roles

- `packages/native/tests/native-window-test.cpp` - the measurement seam. The
  seam is passive: without the environment variable the case behaves exactly
  as before, so the 52 CTest groups are unaffected by this work.
- `packages/native/tests/warm-navigation-benchmark.json` - the registry:
  scenario list, exact request gates, minimum cache hits, per-phase bands with
  a named `tolerance_reason`, total-time and peak-RSS bands. New scenarios
  register here.
- `packages/native/tests/perf-baselines/warm-navigation.json` - the approved
  baseline capture with provenance (repository revision, UTC timestamp,
  measured numbers). Committed like screenshot baselines; rebaselining is a
  deliberate commit naming the release and reason.
- `packages/native/scripts/warm-navigation-benchmark.py` - the harness:
  `capture` runs the case with the seam and the RSS sampler, `baseline`
  approves the capture into the baseline file, `check` compares a fresh (or
  reused) capture against the registry gates and reports the delta against
  the approved baseline. Exit codes match the screenshot harness: 0 all
  gates pass, 3 a gate failed, 1 a harness/registry error. A scenario whose
  baseline is missing reports NOT_RUN, never a fake PASS.
- `packages/native/tests/warm-navigation-benchmark-test.py` - the tooling
  test. Like the other release-tooling checks it runs in the workflow's
  Python check step and is deliberately not registered in CTest, so the
  local full-suite gate keeps counting 52 groups.

## Per-release regression tracking

The scenario is re-run as part of the release checklist. `check` compares the
fresh capture against the registry bands and prints the per-metric delta
against the approved baseline; the measured row is appended to the
"Navigation regression log" table in
`docs/NATIVE_DESKTOP_CACHE_BENCHMARK.md`. A FAIL blocks the release: either
the regression is fixed, or a follow-up commit re-baselines with a named
reason and re-measured two-run bands. Request-count gates may only be relaxed
by editing the registry with an issue reference; timing/RSS bands are re-derived
by measuring twice on the reference stand.

## Reproducing locally

```sh
DISPLAY=:30 XAUTHORITY= QT_QPA_PLATFORM=xcb \
  python3 packages/native/scripts/warm-navigation-benchmark.py \
    --build <build-dir> --capture-dir <dir> --action capture
python3 packages/native/scripts/warm-navigation-benchmark.py \
    --capture-dir <dir> --action check
```

`capture` expects the reference stand (bare Xvnc at 1440x900 plus a panel-less
window manager) because the case creates a real Quick window; `check` is
pure offline comparison and runs anywhere.

## What these numbers are not

Localhost mock-server counters and developer-machine timings are not
live-network savings, not cold physical-disk reads, and not Electron
comparisons. They exist to make the redundant-request contract enforceable
and to catch order-of-magnitude navigation or memory regressions between
releases - the same role the cache-component baselines play for the worker.

### Reopening a cached topic after a realtime deletion

When another member deletes a message, the desktop removes its body and its indexed copies from saved topic and aggregate history. A page with other messages retains its cursor and original age, and the client revalidates it on the next visit. If the connection is unavailable, those remaining messages can still be displayed. The controller regression opens a three-message topic, receives the deletion, changes topics and returns while the history request fails.

## Warm saved-stream restore

The `native-native-window-warm-restore` CTest opens the actual Qt Quick
window. It resumes the saved account through `WorkspaceSessions`, using a
temporary SQLite cache, a loopback HTTPS/WebSocket server, 120 channels,
800 user records, 364 topics in the saved channel and 100 messages. The
fixture holds the refresh grant until the cached channel is visible. Once
the session is authenticated it holds history revalidation until the last
cached message appears in the restored aggregate viewport. It then releases
the network replies and checks the reaction control on hover.

The `WARM_RESTORE` JSON line records time from loading the window to its
first exposure and to visible history. Its 10 ms `GuiStallSampler` starts
after exposure and continues through the initial layout and revalidation.
The same line includes the controller phase aggregates in milliseconds.

On the desktop performance host, enable the 300 ms heartbeat budget:

```sh
WORKSPACE_NATIVE_STRICT_STALL_BUDGET=1 ctest --test-dir build \
  -V -R '^native-native-window-warm-restore$'
```

Shared software-rendered CI uses a 3 s upper guard and also checks the
visible route, viewport buffer and on-demand controls. For comparisons,
warm the Qt disk cache once per executable, then alternate baseline and
candidate runs on the same display, build configuration and CPU allocation.

### Large-channel navigation

`largeTopicNavigationStallBudget` opens a 512-topic channel from a one-message
channel. It exercises both the aggregate and individual topic routes with an
empty cache and with their first pages saved to an independent SQLite cache. Each page contains 50 Markdown messages. The
test waits for the rendered list, viewport restoration, a frame swap and the
next event-loop heartbeat before ending each measurement. It also checks the
visible header and shortcut rail, the absence of redundant full topic
projections, and the presence of both 50-row pages in SQLite on the warm
pass. Background topic preloading is disabled to keep the first visit cold.
The report records any foreground revalidation requests.

Run the reference timing gate with a display and a window manager. The report
contains the 10 ms GUI timer statistics and phase spans for each transition.
With the report enabled, any single gap of 300 ms or longer fails the case.
The normal window bucket checks the same routes and deterministic assertions
on shared CI workers.

```sh
WORKSPACE_NATIVE_TOPIC_PRELOAD=0 \
WORKSPACE_NATIVE_NAVIGATION_STALL_REPORT="$PWD/navigation-stalls.json" \
  <build-dir>/native-window-test largeTopicNavigationStallBudget
```
