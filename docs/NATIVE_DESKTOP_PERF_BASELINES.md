# Warm-navigation and aggregate performance baselines (issue #13)

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
