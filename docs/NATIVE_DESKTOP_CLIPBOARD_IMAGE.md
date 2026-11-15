# Copy image to clipboard (issue #79)

Adds the "Copy image" action to the message context menu for image-bearing
messages and to the media viewer toolbar, so the native desktop writes the
decoded message image onto the system clipboard exactly like the reference
desktop shell does.

## Reference contract

Measured at `workspace_ui` sha `61dacef78dab2f5d0be76f283bebf49fe29b84af`:

- Context menu entry: `packages/web/src/widgets/workspace-message-list/workspace-message-bubble-menu.ui.tsx:335-349`.
  The `copy-image` secondary item exists only when the menu has a context
  image file and a preview loader (`contextImageFile != null &&
  onLoadWorkspaceFilePreview != null`). `onSelect` fires the fetch and
  `void writeImage(imageBlob)` (result deliberately ignored, no toast on any
  outcome) and closes the menu immediately. Item order inside the secondary
  section: copy-image, copy-link, copy (`:335-377`).
- Media viewer entry: `packages/web/src/features/media-viewer/media-viewer-actions.lib.ts:144-151`
  (`copyMediaImage`: fetch the display-URL blob, else the source-URL blob; a
  null blob is turned into a swallowed rejection) and the toolbar button in
  `media-viewer-toolbar.ui.tsx:67-79` rendered only for image items
  (`media-viewer-overlay.ui.tsx:118` `showCopy={item.type === "image"}`),
  placed before download and close.
- Shared write path: `packages/web/src/shared/lib/clipboard.ts:39-74`
  (`writeImage`). In the Electron shell the raw blob bytes are handed to the
  `clipboard:writeImage` IPC as a `Uint8Array`; the browser branch converts to
  PNG, which does not apply to the desktop shell.
- Electron main handler: `packages/electron/src/main.ts:1054-1067`:
  1. Reject non-`Uint8Array` payloads and payloads larger than
     `MAX_CLIPBOARD_IMAGE_BYTES = 64 * 1024 * 1024` (`main.ts:50`) **before**
     any decoding.
  2. Decode with `nativeImage.createFromBuffer`; an empty image returns false.
  3. `clipboard.writeImage(image, "clipboard")` (the clipboard selection, not
     the primary selection) and return true; any throw returns false.
  Every failure path is silent - the IPC result is a bare boolean and no
  notification is shown.
- i18n: `message.copyImage` = "Copy image" / "Копировать изображение"
  (`packages/web/src/i18n/locales/en.json:742`, `ru.json:744`).

## Native design

- `NativeController::clipboardImageBytes(const QByteArray &bytes)` is the
  counterpart of the `clipboard:writeImage` IPC handler: reject
  `bytes.size() > 64 * 1024 * 1024` before decoding, decode with
  `QImage::fromData`, treat a null image like `nativeImage.isEmpty()`,
  otherwise `QGuiApplication::clipboard()->setImage(image)` (the Clipboard
  mode, matching Electron's `"clipboard"` target). Returns the boolean the IPC
  handler returns; logs no user-visible state.
- `NativeController::copyMessageImage(id)` is the context-menu entry for image
  messages: resolve the first image file reference in the message body
  (the same media-reference walk `mediaPreviewReference` uses, image kind
  only), fetch the bytes through the authenticated `FileClient` download
  pipeline (the counterpart of `onLoadWorkspaceFilePreview`), and hand them to
  `clipboardImageBytes` when the download completes. Like the reference the
  caller ignores the result; failures leave the clipboard unchanged and raise
  no toast. The staged file is read back through the bounded helper
  `readClipboardBounded` (at most the cap plus one byte), and the completion
  is bound to the owner identity of the session that dispatched it: after
  sign-out or an account switch the completion is discarded and the clipboard
  is left untouched.
- `MediaViewerModel::copyCurrentImage()` is the viewer toolbar action: for the
  current image slot it feeds the staged full-size bytes to
  `clipboardImageBytes`, read back through the same bounded helper so a
  staged file over the cap is rejected without being loaded whole.
- QML: a `copyMessageImageAction` `MessageMenuAction` is inserted before
  `copyMessageLinkAction`, visible when the menu message is readable and its
  body resolves to an image reference first (`mediaPreviewReference(...).kind
  === "image"`); label `qsTr("Copy image")`, asset `copy.svg`.
  `NativeMediaViewer.qml` gains a `mediaViewer-copy` toolbar button before the
  download button, visible only for image slots, delegating to
  `copyCurrentImage()`.
- State coverage required by the issue: corrupt image bytes and payloads over
  the 64 MiB cap must fail quietly (clipboard unchanged, no toast, no error
  notice) in addition to the success path.

## Deviations from the reference

- Media viewer copy reads the slot's already-staged session bytes instead of
  re-fetching the blob from the display URL (`copyMediaImage` re-fetches).
  The bytes are the same authenticated payload; observable clipboard content
  is identical and no second network fetch is added to the viewer.
- The message menu shows "Copy image" whenever the message body contains an
  image reference. The reference additionally tracks which element the pointer
  was over (`setContextImageFile` on image right-press); the native bubble menu
  has no click-target distinction, so the first image of the body is copied,
  the same resolution order as `mediaPreviewReference`.
- Decode codecs follow the Qt image plugin set (PNG, JPEG, GIF, WebP, BMP,
  ...) rather than Chromium's `nativeImage` set; for the server-side image
  types in use (png/jpeg/gif/webp) the sets agree.

## Visual surface

Action parity, no new visual: the context-menu row and viewer toolbar button
reuse the existing `MessageMenuAction` / `NativeIconButton` chrome and the
existing `copy.svg` asset; no Figma surface change is requested.

## Tests

`native-controller-test.cpp` (existing `native-native-controller` group only):
context-menu copy puts a non-null image with the expected pixel size on
`QGuiApplication::clipboard()`; undecodable message bytes and an over-cap
payload leave a sentinel clipboard unchanged; a body without an image never
starts a file download; the viewer copy writes the staged slot bytes and a
failed slot leaves the clipboard unchanged.
