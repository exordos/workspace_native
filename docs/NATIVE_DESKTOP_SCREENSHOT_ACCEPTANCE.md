# Matched-screenshot acceptance

Issue #14. Turns visual parity claims into mechanical verdicts: every screen
the native test seams can capture is compared numerically against an approved
baseline and, when an approved source capture exists, against the reference
side, using the comparison discipline of `toolkit/compare-crops.py` pair mode.
No claim of visual parity may be made from an eyeballed screenshot again; it
must come from a `--action check` report.

## Roles per screen

| Artifact | Produced by | Purpose |
| --- | --- | --- |
| live capture | existing `CASSI_NATIVE_TEST_*_SCREENSHOT` seams in `packages/native/tests/native-window-test.cpp`, run on a windowed stand | the current build's pixels for a documented synthetic scene |
| approved native baseline | a human-reviewed live capture promoted with `--action baseline` | regression anchor; detects any drift of the native render |
| source reference | capture of the pinned reference presentation (see `docs/NATIVE_DESKTOP_SCREENS.md`, "Matched source/native comparison plan") | optional slot; NOT_RUN while no isolated source fixture exists |

A screen verdict is computed independently for baseline and reference:
`PASS`, `FAIL`, or `NOT_RUN`. A missing source fixture is `NOT_RUN`, never a
pass and never a silent skip. Overall exit codes: `0` no FAIL, `3` at least one
FAIL, `1` infrastructure error (missing binary, no display, missing Pillow).

## Comparison discipline (compare-crops pair-mode semantics)

Exactly the semantics of the handoff toolkit `compare-crops.py`, reimplemented
self-contained so the repository does not depend on the handoff tree:

- crops are taken only from documented rectangles; input pixels are never
  rescaled and different dimensions are retained and reported, not aligned;
- inputs converted to RGB, `ImageChops.difference`, per-channel mean
  (`mean_absolute_channel_difference`) and an identical-pixels check on the
  difference bounding box;
- SHA-256 and original size of every compared file are recorded in the report,
  so a report names the exact bytes it compared.

## Capture seams and the screen set

Every capture seam that exists today is a manifest screen
(`packages/native/tests/matched-screenshots.json`). At master
`099f1fb` (0.3.6) the seams are:

| screen id | QtTest case (`native-window-test`) | capture environment variable |
| --- | --- | --- |
| `chat-light` / `chat-dark` | `realQuickWindowSignsInNavigatesAndSends` | `CASSI_NATIVE_TEST_SCREENSHOT` / `..._DARK_SCREENSHOT` |
| `message-menu-light` / `message-menu-dark` | same case | `..._MENU_SCREENSHOT` / `..._MENU_DARK_SCREENSHOT` |
| `reaction-picker-light` / `reaction-picker-dark` | same case | `..._REACTION_SCREENSHOT` / `..._REACTION_DARK_SCREENSHOT` |
| `cache-updating` / `cache-update-failed` | `savedHistorySurvivesProcessRestartAndRevalidatesBeforeActions` (capture happens in the `cachePhase` child, which inherits the parent environment) | `..._CACHE_SCREENSHOT` / `..._CACHE_OFFLINE_SCREENSHOT` |
| `aggregate-en-light` / `aggregate-en-dark` / `aggregate-ru-light` / `aggregate-ru-dark` / `aggregate-identity-day` / `aggregate-visible-reads` | `aggregateStreamUsesSourceGroupsAndTopicChoices` (one data row each) | `..._AGGREGATE_CAPTURE_DIR` |

Screens without a capture seam are not silently absent: they are tracked as
seam work in issue #14 follow-up comments, and a screen joins this manifest
the day its seam lands. This document is the registry; the manifest is the
machine copy of the same list.

## Tolerances (issue #16 rule)

Font-derived pixels must never be asserted from one host's hardcoded metrics.
Therefore every screen records its tolerance explicitly:

- `requires_identical_pixels: true` where two independent captures on the
  reference stand reproduce byte-identical images (proven by the promotion
  run; the report shows it);
- otherwise `max_mean_abs_channel_difference` with a numeric band and a
  `tolerance_reason` naming the fluctuating pixel source (for example a
  mid-animation indicator frame captured at a fixed wall-clock point). A band
  is never widened without naming that cause.

The reference stand is the documented bare Xvnc `:30` at 1440x900 with the
panel-less openbox WM, `QT_QPA_PLATFORM=xcb`, `LC_ALL=C.UTF-8`, DPR 1. The
harness always exports `CASSI_NATIVE_TEST_DATE_FIXTURE=1` so the conversation
scene carries fixed timestamps.

## Approval and regression flow

```sh
# 1. produce live captures (windowed stand required)
python3 packages/native/scripts/matched-screenshot-acceptance.py \
  --build <native-build-dir> --capture-dir <dir-outside-source-tree> \
  --action capture

# 2. human review, then promote as approved baselines
python3 ... --action baseline          # rewrites baselines + provenance json

# 3. mechanical check: fresh captures vs approved baselines (+ references)
python3 ... --action check             # exit 0/3; --json-out for a report
python3 ... --action check --reuse-captures   # re-verify existing captures only
```

`--action baseline` records provenance per screen in `baselines.json`:
SHA-256, pixel size, native build version, repository revision, Qt version,
viewport and device pixel ratio. Baseline PNGs are test fixtures and live in
`packages/native/tests/screenshot-baselines/`; run captures always live
outside the source tree. Re-approval is a normal reviewed commit, never an
automatic rewrite from a failing check run.

## Verification

- Harness logic is covered by `packages/native/tests/matched-screenshot-acceptance-test.py`
  (pure Python, synthetic PNG pairs, no Qt): identical/cropped/dimension-mismatch
  maths, band pass/fail, NOT_RUN discipline, manifest validation, report shape.
  Like the installer and release-notes tooling checks it is run directly by the
  workflow and is deliberately not registered in CTest, so the CTest group count
  stays unchanged.
- The capture, baseline and check actions run on the windowed stand; CI has no
  windowed runner, so `--action check` is a local/acceptance step. A full
  GUI-suite run is unchanged by this tooling: no CTest group was added or
  modified.
- Source-side fixtures (`screenshot-references/`) remain NOT_RUN until the
  isolation requirements in `docs/NATIVE_DESKTOP_SCREENS.md` ("Matched
  source/native comparison plan") are met. The harness consumes them the moment
  they exist; until then no parity claim on this pipeline may call itself
  source-matched.
