## Issue

Part of #796 (umbrella: shipped interactions with no window-level test witness), item 3:
the media viewer controls auto-hide contract from issue #731 had no window witness - the
four existing #731 cases pin the bar *visible*/*pinned* only. Nothing asserted that the
bar actually hides mid-playback after the 2500 ms countdown, that it returns on pointer
move, that the countdown re-arms, or that a paused clip keeps it pinned.

## Changes

- `native-window-test.cpp`: new window case
  `mediaViewerVideoControlsAutoHideReturnsOnMoveAndPause` inside `NativeWindowTest`:
  login -> open viewer on the video slot -> reach `PlayingState` -> assert the bar starts
  shown -> park the cursor off the picture -> bar hides while still playing ->
  pointer move over the picture re-shows it -> leaving again hides it again (re-arm) ->
  pause through the picture click path re-shows and pins it past the countdown -> close.
- New committed witness fixture `mediaViewerLongVideoBytes()`: a 7.0 s h264 clip
  (5708 B, still frame, same decoder path as the existing fixture). The existing
  0.5 s fixture can never exercise auto-hide: the clip ends before the 2500 ms
  countdown fires and an ended clip pins the bar open by design.
- Generated bucket plan + timing fragment regenerated for the new case.

Test-only change: no QML/C++ product code touched.

## Contract

`packages/native/qml/NativeMediaViewer.qml` `controlsHideTimer` (interval 2500, gated on
`PlayingState && !videoHover.pointing`), `pokeControls()` restart seam, and the
`onPlaybackStateChanged` pin - plus `docs/NATIVE_DESKTOP_VIDEO_PLAYBACK.md` (browser
convention: paused/ended always shows the bar; move re-shows and re-arms).

## Verification

- Solo on Xvnc :31 with openbox (Qt xcb), three consecutive runs: PASS, PASS, PASS
  (9.94 s / 9.97 s / 12.84 s per run; QtTest XML shows 0 failures, the h264 decode
  path goes through the software decoder as in the existing #731 cases).
- `native-window-test` builds clean (AutoMoc green); bucket plan regenerated:
  the case lands in window bucket part11 (timing 9.93972 s), 24 buckets,
  max bucket 113.1 s below the 120 s ceiling.

## Screenshots

n/a (test-only).
