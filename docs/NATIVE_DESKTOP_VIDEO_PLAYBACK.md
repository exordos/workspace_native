# Video playback controls in the media viewer (issue #731)

Status: draft. Scope: the Playback half of #731. The cached-poster half of
#731 shares the disk-limit and authorization rules of open issue #729 and is
delivered as a follow-up once #729 lands (issue comment 2026-11-23).

## Reference contract (workspace_ui e0194ffb667986b58a12256ce41ca5dc99b56b83)

- `packages/web/src/features/media-viewer/media-viewer-overlay.ui.tsx:160-167`:
  the ready video renders as a plain HTML `<video src controls autoPlay
  className="max-h-[calc(100vh-12rem)] max-w-full object-contain">` with
  `onClick` stopPropagation and `onError` routed to the display-error
  replacement (`media-viewer-overlay.ui.tsx:75-90`). The box padding is
  `px-16 pb-28 pt-14` (`:126,158`).
- Gap: the reference never draws its own controls - Chromium renders the
  native media controls, so no seek/time/volume geometry exists in the source
  to measure. Figma token unavailable (401) - mockups not consulted.
  Per the design rule this scope is NEW native functionality (no pixel clone
  target exists), so the control bar is modeled on the standard Chromium
  layout: play/pause, current time, seek track, total duration, mute,
  a volume slider; controls overlay the bottom of the painted video frame.
- Chromium behaviour cloned here: controls show while paused or hovered and
  auto-hide a couple of seconds into playback.

## Existing native state

`packages/native/qml/NativeMediaViewer.qml` already wires autoPlay, the
click-to-toggle on the painted area (`mediaViewer-video-toggle`, :172-192),
audio routing (`videoAudio`) and the `onErrorOccurred` display-error path
(:140-143). Missing: visible seek bar, time/duration label, mute/volume,
stop-on-close/switch as an explicit `stop()`, and an actionable retry on a
failed attachment download.

## Design

All controls live in a bar pinned to the bottom of the `mediaVideo` painted
rect, visible only while the slot is a ready video:

- `mediaViewer-play` - flat toggle button (play/pause icons), toggles
  `videoPlayer.play()/pause()`.
- `mediaViewer-time` - `Text`, `m:ss / m:ss` from `videoPlayer.position` and
  `videoPlayer.duration`; duration `0` renders `--:-- / --:--`.
- `mediaViewer-seek` - `Slider`, `from 0`, `to duration`, value follows
  `position` while not pressed; `onMoved` calls `videoPlayer.seek(value)`.
- `mediaViewer-mute` - flat toggle writing `videoPlayer.muted`.
- `mediaViewer-volume` - `Slider`, `0..1`, writes `videoPlayer.volume`;
  disabled while muted.
- Auto-hide: while `playbackState === PlayingState` the bar hides 2.5 s after
  the last pointer movement over the video; any move or pause shows it
  (`Timer` + `HoverHandler` on the media area).
- Keyboard focus (#784): active focus on any focusable child of the bar
  (`mediaViewer-play`, `mediaViewer-seek`, `mediaViewer-mute`,
  `mediaViewer-volume`) pins the bar open - the pin resolves through
  `Window.activeFocusItem`, so no per-control wiring exists - and moving
  focus out re-arms the countdown. Because Qt drops invisible items from the
  tab chain, a gated `Shortcut` (enabled only while a ready video slot sits
  hidden with focus outside the bar) consumes the first Tab press to reveal
  the bar, and the next Tab walks onto its controls, so keyboard users can
  reach the controls after an auto-hide.
- Stop on close/switch: the viewer calls `videoPlayer.stop()` when the index
  changes and when the viewer closes (in addition to the source binding
  clearing, so the audio track is guaranteed silent).
- Actionable error: the status label for `load-error` gets a `Retry` button
  (`mediaViewer-retry`) that re-dispatches the attachment fetch through the
  existing slot loader path.

Geometry: bar height 40 px, horizontal padding 12 px, icons 20 px, texts 13
px using the viewer's existing `#B3FFFFFF` on the dark overlay; the bar
background is a 60%-opacity black gradient over the bottom 56 px (Chromium
convention), so the video itself is never letterboxed by layout.

## Tests (native-window-test, media viewer cases)

- `mediaViewerVideoControlsVisibleAndSeekRoundTrip`: synthetic MP4 fixture
  committed in tests fixtures (same generator pattern as image fixtures),
  play -> position advances; press-seek to mid -> `videoPlayer.position`
  lands within +-60 ms of the target; time label text matches the derived
  mm:ss of position and duration (derived from the player, never hardcoded).
- `mediaViewerVolumeAndMuteBinding`: volume slider writes `volume`, mute
  writes `muted`.
- `mediaViewerSwitchStopsAudio`: switching slots (and closing) asserts
  `playbackState === StoppedState`.
- `mediaViewerLoadErrorOffersRetry`: delayed/failed attachment fetch shows
  `mediaViewer-retry`; activating it re-issues exactly one fetch (traffic
  counter).
Buckets regenerate via `gen-test-buckets.py` per the standard.

## Installed-client check

`toolkit/installed-update.py` is not the fit; the acceptance run happens in
the live media lane: staged-install binary plays the two committed fixtures
(MP4 H.264/AAC and WebM VP9/Opus) - play/pause, seek, mute - plus the failed
download path with an unreachable attachment URL. Recorded in the PR Live
verification section.

## Cached previews (deferred, NOT this lane)

Poster-frame strip thumbnails and quote previews are blocked on the cache
policy of #729 (disk limit, authorization, prefetch). A follow-up issue will
be filed when #729 lands; the play badge overlay on video entries stays
generic until then.
