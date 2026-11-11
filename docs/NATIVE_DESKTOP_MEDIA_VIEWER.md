# Media viewer (issue #8)

Full-size viewing of image media from a conversation, replacing the current
behaviour where clicking an inline preview opens the save dialog. The viewer
supports in-conversation navigation, a thumbnail strip with a position
counter, and an original download action.

This document is the design record for the 0.10.0 release (MINOR bump from the
latest published release `0.9.1`; the `0.11.0` milestone on issue #8 is a
provisional roadmap estimate, per the release-version policy note on the issue).

## Reference contract

Measured at `workspace_ui` sha `61dacef78dab2f5d0be76f283bebf49fe29b84af`:

- Store/overlay: `packages/web/src/features/media-viewer/media-viewer.model.ts`,
  `media-viewer-overlay.ui.tsx`, `media-viewer-backdrop.ui.tsx`.
- Controls/toolbar/thumbnails: `media-viewer-controls.ui.tsx` (:6-7, :16, :24),
  `media-viewer-toolbar.ui.tsx` (:7-8, :52, :79-95),
  `media-viewer-thumbnails.ui.tsx` (:26-48, :78-90).
- Gallery collection: `widgets/workspace-message-list/workspace-message-list-media.lib.ts`
  (:34-77) - rendered messages of the current conversation, markdown payloads
  only, image/video file references in document order, deduped by file uuid
  keeping the first occurrence.
- Slot loading: `features/media-viewer/workspace-media-viewer.hook.ts`
  (:181-327) - the clicked slot loads first, the other image slots load in
  parallel; each slot has `loading | ready | load-error | display-error` state.
- Actions: `media-viewer-actions.lib.ts` - download delegates to the app
  download pipeline; the toolbar disables actions while the display URL is not
  usable (:71-79).

Measured geometry and colors (Tailwind scale `tailwind.config.ts:107-122`,
palette `shared/lib/themes/blue-cold.ts`, default palette `blue-cold`,
`registry.ts:8`):

| Element | Value |
| --- | --- |
| Backdrop | `fixed inset-0 z-modal` `rgba(0,0,0,0.9)`, identical in light and dark |
| Media area padding | 64 px sides, 56 px top, 112 px bottom; media `max-height: 100vh - 192 px`, `object-contain` |
| Prev/next buttons | only when item count > 1; 48x48 circle `rgba(0,0,0,0.5)`, hover `rgba(0,0,0,0.7)`, chevron glyph 24 (left is rotate-180), 16 px from the edge, vertically centered |
| Toolbar | top-right 16 px, gap 8 px, z-float; buttons 40x40, radius 6, `bg-elevated` at 80 % alpha (#222328CC dark / #FFFFFFCC light), glyph 20, `text-primary`; disabled at 40 % opacity |
| Toolbar buttons | download (`mediaViewer.download`) then close (`common.close`); "open in new tab" is rendered only when NOT Electron (`media-viewer-overlay.ui.tsx:56`), so the native desktop viewer omits it, matching the reference desktop shell |
| Copy image | tracked separately as issue #79, not part of this slice |
| Thumbnail strip | only when item count > 1; bottom bar, 1 px top border `rgba(255,255,255,0.1)`, background `rgba(0,0,0,0.7)`, padding 16 px sides / 12 px top; counter text 12 px `rgba(255,255,255,0.7)`, `{{current}} / {{total}}`, margin-bottom 8 px; thumbnails 56x56 radius 6, border 2 px `accent` (#7087ff) when active else transparent + opacity 0.7; gap 8 px; active thumbnail scrolled into view centered |
| Keyboard | Escape always closes; ArrowLeft/ArrowRight navigate only when count > 1 (document-level listener, no focus trap in source) |
| Click semantics | backdrop click closes; toolbar, nav buttons, strip and media consume their clicks |
| Navigation | clamped at both ends, no wrap-around; buttons stay inert at the ends |
| Empty/one item | `open()` with an empty list is ignored; startIndex clamps to [0, len-1]; strip and counter hidden for a single item |
| Scope | conversation switch or logout closes the viewer (source closes on scope change, `workspace-media-viewer.hook.ts:129-135`) |
| i18n | `mediaViewer.download` "Download"/"Скачать", `mediaViewer.prev` "Previous"/"Предыдущее", `mediaViewer.next` "Next"/"Следующее", `mediaViewer.position` "{{current}} / {{total}}" (both locales), `mediaViewer.thumbnail` "Media {{index}}"/"Медиа {{index}}", `mediaViewer.loading` "Loading media…"/"Медиа загружается…", `mediaViewer.unavailable` "This media cannot be displayed. You can download the file instead."/"Не удалось показать медиа. Файл можно скачать.", `a11y.mediaViewer` "Media viewer"/"Просмотр медиа", `common.close` "Close"/"Закрыть" |

## Scope decisions

- Image media only. The reference gallery also carries video slots
  (`mediaKind in {image, video}`), but the native client has no video preview,
  playback, or Qt Multimedia dependency at all, so there is no video entry
  point to reproduce. A follow-up issue will cover video slots together with
  the inline video preview decision.
- The viewer replaces the current `openReference` -> save-dialog routing for
  image references; file/video references keep the existing download path.
- Download action: the native app already routes every attachment download
  through a native save dialog (`attachmentSaveDialog` in `qml/Main.qml`), an
  established local convention that replaces the Electron shell's silent
  auto-save to the downloads folder. The viewer download button reuses that
  exact flow (same `controller.downloadFile(uuid, destination)` pipeline), so
  this release does not introduce a second download behaviour.
- "Open in new tab" is omitted deliberately: the reference shows it only in
  browser builds.
- Wheel zoom (`media-viewer-zoom.hook.ts`, 0.5x-3x) is deferred to a follow-up
  with the video slots: it is an interaction on top of the viewer shell, not
  the viewing/navigation/download contract named in issue #8.

## Native design

### C++: `MediaViewerModel` (`packages/native/src/media-viewer.{h,cpp}`)

Owned by `NativeController` after `useFiles()` and exposed to QML as
`controller.mediaViewer`. Constructor takes the controller and `FileClient`.

Properties (all `NOTIFY mediaViewerChanged`):

- `open` (bool), `items` (QVariantList of
  `{uuid, state: "loading"|"ready"|"error", url, name}`), `index` (int),
- `canPrev` / `canNext` (clamped, no wrap-around),
- `count` shortcut for QML.

Invokables:

- `openFromMessages(messages, fileUuid)` - builds the gallery from the
  controller's message rows exactly like the reference collection contract:
  rows with `kind == "markdown"` only, image references extracted with
  `NativeMarkdown::render` + `reference()` (kind `image`), document order,
  deduped by uuid keeping the first occurrence. Returns `false` (viewer not
  opened) when the list is empty or the uuid is absent, so the QML entry point
  can fall back to the previous behaviour. Start index = first slot with the
  uuid, clamped. Opening while open replaces the session (reference revokes
  the previous blob URLs).
- `goTo(index)` (clamped), `next()`, `prev()` (no-ops at the ends), `close()`.

Slot loading: on open a per-session `QTemporaryDir` is created; each slot is
fetched with `FileClient::download(uuid, dir/slot-N, completion)` - the same
authenticated, checksum-verified pipeline the attachment download uses, but
file-backed (no 25 MiB memory cap). The clicked slot starts first, the rest
launch immediately after, mirroring the reference parallel image loading. A
generation counter drops callbacks from a retired session. Ready slots expose
`url = file://<staged path>` and `name` from server file metadata. Failed
slots keep state `error` with an empty url; toolbar actions become unusable
for them, matching `canUseMediaViewerDisplayUrl` (actions disabled, not a
download fallback button). `close()` cancels in-flight downloads and removes
the session directory (the blob-revocation analogue). The controller closes
the viewer automatically when the session or conversation identity changes
while it is open.

### QML: `qml/NativeMediaViewer.qml`

A full-window overlay `Item` (sibling after all content, `visible:
mediaViewer.open`) mirroring the measured table: backdrop
`#E6000000` with a closing `MouseArea`, media area padding 64/56/112 with
`Image fillMode: PreserveAspectFit`, `Image` max height `window height - 192`,
nav buttons 48x48 `#80000000` hover `#B3000000` with the bundled
`chevron-right.svg` (left rotated 180), toolbar top-right with 40x40 buttons
(`colors["bg-elevated"]` at 80 % alpha, glyphs 20 px `text-primary`, opacity
0.4 when disabled) for download and close, and the bottom strip (counter
`#B3FFFFFF` 12 px, 56x56 thumbnails radius 6 border 2 px `accent`/transparent
+ 0.7 opacity, background `#B3000000`, top border 1 px `#1AFFFFFF`).
`window` here is the viewer's `Window.window`, keeping geometry relative to
the app window instead of the viewport. Keyboard is handled with `Shortcut`
elements enabled while the viewer is open: Escape closes, Left/Right navigate
only when `count > 1`.

Entry point: the inline image `onOpened` handler in `Main.qml` first calls
`controller.mediaViewer.openFromMessages(controller.messages, reference.id)`
and keeps the old `openReference` behaviour for any case the viewer declines
(non-markdown context, missing slot). Toolbar download sets the existing
`downloadId`/`attachmentSaveDialog` flow for the current slot uuid.

New `tr()` strings are added to `translations/workspace_ru.ts` (the locale
guard test enforces the catalog).

### Tests

- `native-native-controller`: gallery collection (markdown-only, dedupe,
  order), open/replace/clamped start index, clamped navigation, slot state
  transitions through the fixture `/files/` mock (loading -> ready, error),
  close during in-flight downloads, automatic close on conversation switch.
- `native-native-window`: clicking an inline image opens the overlay
  (in-process `NativeWindowTest` class), Escape and click-outside close it,
  next/prev update the counter, single-image scenes hide nav and strip,
  download click opens the save dialog.

Every geometry assertion in the window tests cites the reference file and
line it was measured from in the table above.
