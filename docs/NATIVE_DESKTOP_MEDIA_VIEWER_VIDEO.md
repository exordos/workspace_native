# Media viewer: video slots (issue #203)

Extends the image-only media viewer (docs/NATIVE_DESKTOP_MEDIA_VIEWER.md,
which deferred video to this follow-up at :54-58) with inline video slots:
the gallery collects image and video references, the viewer plays videos
inline with per-state labels, video slots load lazily, and video thumbnails
carry a play badge. The download action for video slots keeps the existing
save-dialog pipeline, matching the Electron shell routing the Download
action to the app download coordinator (packages/electron/src/
workspace-downloads.ts at the measured sha).

## Reference contract

Measured at `workspace_ui` sha `61dacef78dab2f5d0be76f283bebf49fe29b84af`
(the same sha as the base viewer doc):

- Gallery collection: `widgets/workspace-message-list/workspace-message-list-media.lib.ts`
  (:20-31) accepts a file reference when `kind == "media"` and
  `mediaKind in {image, video}` with a non-blank file uuid; (:44-66) walks
  every markdown-payload message, takes file references in document order,
  deduped by uuid keeping the first occurrence. There is no "image-only
  paragraph" restriction on the reference side: the base viewer's
  `NativeMarkdown::render` + `reference()` approximation (image parts only)
  is replaced by a full document scan that keeps both media kinds in
  document order (see Native design).
- Item model: `features/media-viewer/workspace-media-viewer-item.lib.ts`
  (:22-26 `mediaKind image|video`, :37 uuid required, :39-62 item carries
  `resourceState` and `width`/`height` when known) and
  `media-viewer.types.ts` (:8-9 `type: "image" | "video"`,
  `resourceState: loading | ready | load-error | display-error`).
- Slot loading: `workspace-media-viewer.hook.ts` (:205-212) - on open only
  the clicked slot and every image slot start loading; non-clicked video
  slots are skipped. (:137-151, :260-283) a slot that is reached by
  navigation and is still `loading` starts loading then (lazy video load).
- Video presentation: `media-viewer-overlay.ui.tsx` (:127-151) per-state
  labels - `loading`: `mediaViewer.videoLoading`, `load-error`:
  `mediaViewer.videoLoadFailed`, `display-error`:
  `mediaViewer.videoDisplayFailed` (image keeps `mediaViewer.loading` /
  `mediaViewer.unavailable`). (:152-161) the ready branch renders
  `<video src controls autoPlay class="max-h-[calc(100vh-12rem)] max-w-full
  object-contain">`, clicks stopPropagation, and `onError` replaces the slot
  with `resourceState: "display-error"` and an empty url while keeping the
  workspace file reference (:75-90).
- Thumbnails: `media-viewer-thumbnails.ui.tsx` (:23) thumbnail still images
  are rendered for image slots only; (:44-48) video slots get a play badge:
  an inset-0 overlay `bg-black/40` (#66000000) with the `videos` icon at
  size 20 tinted `text-text-primary`, above the `bg-elevated` tile
  (rounded-md 6 px per the base viewer doc).
- States/locales: light + dark, ru + en; loading, ready, load-failed,
  display-failed.

Measured geometry (Tailwind scale `tailwind.config.ts:107-122`, default
palette `blue-cold` as in the base viewer doc):

| Element | Value |
| --- | --- |
| Video box | same media area as images: 64/56/112 padding, `max-height: 100vh - 192 px`, `object-contain` (media-viewer-overlay.ui.tsx:126,158) |
| Video click | consumed by the media element, never closes the backdrop (media-viewer-overlay.ui.tsx:159) |
| Play/pause | reference uses browser-native `<video controls>` (media-viewer-overlay.ui.tsx:156): a click on the picture toggles play/pause; the native viewer reproduces that single affordance |
| Video thumbnail | 56x56 tile stays `bg-elevated` (no still frame, media-viewer-thumbnails.ui.tsx:23); play badge overlay inset-0 #66000000 radius 6, `videos` glyph 20 px `text-primary` (media-viewer-thumbnails.ui.tsx:44-48) |
| Load-failed | dedicated video strings, both locales, no download fallback button (toolbar stays disabled while the display url is unusable, media-viewer-actions.lib.ts:71-79) |

Strings (`packages/web/src/i18n/locales/en.json:1413-1417`,
`ru.json:1424-1428` at the measured sha):

| Key | en | ru |
| --- | --- | --- |
| mediaViewer.videoLoading | Video is loading… | Видео загружается… |
| mediaViewer.videoLoadFailed | Failed to load video | Не удалось загрузить видео |
| mediaViewer.videoDisplayFailed | This video cannot be played | Не удалось воспроизвести видео |
| mediaViewer.openVideo | Open video viewer | Открыть видео в просмотрщике |
| mediaViewer.videoQueued | Video | Видео |

`openVideo` and `videoQueued` label the reference's inline in-message video
preview (messenger-workspace-message-file-preview.hook.ts:484-485), a slice
the native message list does not render yet; they are intentionally not
shipped as unused catalog strings. The native entry point is the existing
workspace-reference link click in the message body (see below).

## Qt Multimedia dependency decision

Inline video playback needs a decoder. The viewer adopts
**Qt 6 Multimedia** (`Qt6::Multimedia`, QML `import QtMultimedia`;
`MediaPlayer` driving a `VideoOutput` - the same composition the bundled
`Video` component uses), because:

- it is the only Qt-sanctioned Qt Quick video path (no WebView - QWidget is
  banned by AGENTS.md and the WebView path is reserved for the Jitsi call
  window behind its own build option);
- the default backend on Linux is FFmpeg decoding in software: no GPU or
  VAAPI is required, so the viewer works on llvmpipe CI hosts exactly like
  the existing software-rasterized window tests;
- the FFmpeg audio sink and the audio device are optional at runtime: the
  fixture used by CI has no audio track and playback proceeds without any
  sound server;
- degradation is contained: `QMediaPlayer::errorChanged` maps onto the
  reference `display-error` state (the same `videoDisplayFailed` label a
  browser shows for an undecodable file), so a host without the multimedia
  plugins or with an undecodable file still gets the correct per-state
  label and the unchanged download path.

CI: the `package-linux` apt list (`.github/workflows/linux-release.yml`)
gains `qt6-multimedia-dev` and `qml6-module-qtmultimedia` (the runtime
`libqt6multimedia6` and its FFmpeg deps come in as package dependencies).
Tests observe playback through player state (playbackState/mediaStatus and
the model slot states), never decoded pixels.

## Native design

### C++: `MediaViewerModel` (`packages/native/src/media-viewer.{h,cpp}`)

- Gallery collection switches to a new
  `NativeMarkdown::mediaReferences(markdown)`: parses the same GFM document
  and walks fragments in document order, keeping every workspace reference
  whose kind is `image` or `video` with a non-empty id (matches
  `collectWorkspaceMessageFileReferences` + the `:20-31` filter). The model
  dedupes by uuid keeping the first occurrence, as before. This also closes
  the base doc's known approximation that mixed-position inline images were
  not collected; the viewer can now open what the reference opens.
- `Slot` gains `kind` ("image" | "video") and a `started` flag. `items()`
  now maps `{uuid, kind, state, url, name}`.
- Slot states adopt the reference vocabulary: `loading | ready | load-error
  | display-error` (media-viewer.types.ts:9). The previous image-only
  `error` state is renamed to `load-error`; the QML label mapping is
  unchanged for images (`mediaViewer.unavailable`).
- Load policy on open: clicked slot first, then all image slots; video
  slots other than the clicked one are not started
  (workspace-media-viewer.hook.ts:205-212). `goTo`/`next`/`prev` start a
  not-yet-started `loading` slot they land on (the `lazyLoadRef` contract,
  hook :137-151, :260-283). Staging still runs through `FileClient::download`
  into the per-session temp dir (checksum verified, file-backed).
- `markSlotDisplayError(index)` - the `replaceItem` analogue for the video
  `onError` path (media-viewer-overlay.ui.tsx:75-90): only while open, only
  on a slot that is currently `ready`, clears `url` (download keeps using
  the slot uuid) and sets `display-error`.

### QML: `qml/NativeMediaViewer.qml`

- The media element switches on `currentSlot.kind`. Ready video slots bind a
  `QtMultimedia MediaPlayer` (same box and 192 px cap as the image,
  `autoPlay`) feeding a `VideoOutput` (`fillMode: PreserveAspectFit`),
  with a MouseArea that toggles play/pause on click (browser-native
  controls parity; objectName `mediaViewer-video-toggle`) and never closes
  the viewer. Player `errorOccurred` calls `viewer.markSlotDisplayError(index)`.
- Label text per state and kind exactly as measured above.
- Strip delegates: video slots show no still image; a play badge overlay
  (`#66000000`, radius 6, bundled `videos.svg` 20 px tinted
  `colors["text-primary"]`) is shown instead (thumbnail still-image rule
  :23, badge :44-48).
- Entry point in `qml/Main.qml`: `openReference` tries the viewer for
  `reference.kind === "video"` as well, falling back to the save dialog
  when the viewer declines (file not in the gallery), same contract as
  images.
- `videos.svg` joins the `native_icons` bundle from
  `packages/web/src/shared/assets/icons/`.

### CMake

`find_package(Qt6 ... COMPONENTS ... Multimedia)`,
`workspace-native-ui` links `Qt6::Multimedia`, and the CI apt list gains
`qt6-multimedia-dev qml6-module-qtmultimedia`.

### i18n

`videoLoading`, `videoLoadFailed`, `videoDisplayFailed` ship in
`translations/workspace_ru.ts` (the locale guard test enforces the catalog).

## Tests

- `native-native-controller`:
  - existing failure-path test re-anchored from `error` to `load-error`;
  - gallery collects image + video slots in document order with `kind`,
    dedup unchanged, open-on-video uuid anchors the video slot;
  - non-clicked video slots do not issue a download until navigation
    reaches them, then stage like images;
  - `markSlotDisplayError` flips a ready slot to `display-error` and
    clears its url, ignores closed sessions and non-ready slots.
- `native-native-window`:
  - image + video messages open the viewer with two slots reachable by
    prev/next; the video slot shows "Video is loading…" then the player,
    reaches the playing state (observed through `playbackState`, not
    pixels), toggles to paused and back via the click affordance, and the
    video thumbnail carries the play badge while the image one does not;
  - a video download failure renders `videoLoadFailed` in both locales;
  - undecodable video bytes drive the player error path to the
    `videoDisplayFailed` label and disable the toolbar download.

Every geometry assertion cites the reference file and line it was measured
from above.
