# In-chat attachment download state (issue #631)

## Purpose

Received file references show live download state inside the message bubble:
a progress ring with percent (or a spinner when the total size is unknown)
while downloading, a check + "Open" confirmation for 2 s after completion,
metadata + "Open" afterwards, and "Failed · Retry" for failed entries.
Clicking a completed reference re-opens the saved file instead of asking for
a new destination. This is a parity-preserving port of the reference behavior
shipped in reference PR #431; no existing feature is restyled.

## Reference oracle

Reference checkout `workspace_ui` at `6b9f27f656330d0f515a03ca79bdcc3b762f009f`
(all `file:line` citations below are against that sha):

- Selection: `packages/web/src/widgets/workspace-message-list/workspace-message-attachment-download.lib.ts:12-24`
  `findAttachmentDownload` scans entries by `ownerKey` + `fileUuid`; an active
  entry (`starting`/`downloading`, lines 8-10) wins; otherwise the terminal
  entry with the max `startedAt`. Lines 26-34
  `handleCurrentAttachmentDownload`: a click while active is ignored; a click
  on a `downloaded` entry with native downloads re-opens the saved file
  (`openWorkspaceDownload(entry.id)`).
- State description: `workspace-message-attachment-download.ui.tsx:12-27`
  `describeDownload`: `percent = min(100, round(receivedBytes / totalBytes * 100))`
  only while `status === "downloading"` with a known `totalBytes > 0`, else
  `null` (spinner). `DOWNLOAD_CONFIRMATION_MS = 2000` (line 10): after
  completion the card shows check + "Open" for 2 s, then restores the file
  metadata line while keeping the accent "Open" action label (native branch).
  Error entries map `cancelled` by `errorCode === "cancelled"` (line 18).
  Host accessible name/title become `"<fileName>. <status label>"` and the
  host is `aria-disabled` + `aria-busy` while downloading (lines 100-117).
- Card geometry: `packages/web/src/shared/ui/attachment-card/attachment-card.ui.tsx`
  frame `h-[58px] w-60 max-w-full gap-3 p-2` with the embedded appearance
  `rounded-lg border-transparent bg-transparent` (lines 14-15, 40-46); details
  column `w-[134px]`, name `12px/500 leading-[14px]`, details `12px
  leading-[15px] text-text-muted mt-0.5` (lines 49-58).
  `AttachmentDownloadPreview` (lines 120-137) replaces the `40x40`
  `rounded-lg bg-bg` preview slot: `Spinner size="sm"` when the percent is
  unknown, else `AttachmentProgressRing`; downloaded shows the `check` icon
  size 18 `text-accent`; error shows the upload-error icon `18x18`.
  `AttachmentProgressRing` (lines 250-261): `18x18`, accent conic arc over a
  24 %-alpha `text-muted` track, inner hole `inset-[3px]` (ring thickness 3px).
  Spinner `sm` is `h-3.5 w-3.5` (14 px) with a 2 px border, `border-border-subtle`
  and `border-t-accent` (`shared/ui/loading/spinner.ui.tsx:4-10`).
  Details column (lines 147-183): downloading -> `attachmentDownloading[WithPercent]`
  in `text-text-primary`; downloaded -> metadata line + accent
  `attachmentOpen` (native) label; error -> cancelled/failed text + accent
  `attachmentRetry`.
- `top-bar-download-center.ui.tsx` `browserHandoff` is the web-only fallback;
  the native client always uses the in-process coordinator (#74), so the
  browser-handoff branch is out of scope (same "n/a for native" carve-out as in
  the issue's Electron note).
- i18n (`downloads.*`): en `en.json:1366-1372` / ru `ru.json:1380-1386`:
  `attachmentDownloading` "Downloading…" / "Скачивание…",
  `attachmentDownloadingWithPercent` "Downloading… {{percent}}%" /
  "Скачивание… {{percent}}%", `attachmentOpen` "Open" / "Открыть",
  `attachmentRetry` "Retry" / "Повторить", `attachmentDownloadAgain`
  "Download again" / "Скачать снова", `browserHandoff` "Handed off to browser"
  / "Передано браузеру", plus `cancelled` "Cancelled" / "Отменено" and
  `failed` "Failed" / "Ошибка".

## Native design

The in-process coordinator from #74 is the data source: `NativeController`
`downloadEntries` (Q_PROPERTY, NOTIFY `downloadsChanged`;
`src/native-controller.cpp:10774-10786`) exposes `id`, `fileUuid`, `status`
(`starting|downloading|downloaded|error`), `error`, `receivedBytes`,
`totalBytes`, and entries are prepended newest-first (`startDownload`,
`src/native-controller.cpp:10852`), which stands in for the reference
`startedAt` ordering. Click routing already exists: `retryDownload` restarts a
terminal entry, `openDownload` opens the verified local path, `downloadFile`
guards duplicates. `FileClient` metadata validation requires `size_bytes`
(`src/file-client.cpp:76-78`), so a native `totalBytes <= 0` is only observed
while a fresh entry is `starting`; the spinner branch therefore covers the
pre-metadata phase of a transfer (reference: unknown `Content-Length` keeps
the spinner).

### Controller reduction (C++)

`NativeController::attachmentDownload(const QString &fileUuid)` (`Q_INVOKABLE`,
read-only) ports `findAttachmentDownload` + the `describeDownload` percent
rule for the current owner:

- returns an empty map when unauthenticated or no entry matches;
- an active entry (`starting`/`downloading`) wins over terminal entries;
- otherwise the first terminal entry in list order (newest-first list ==
  max `startedAt` in the reference);
- `percent` = `min(100, round(received * 100 / total))` only while
  `status == "downloading" && total > 0`, else `-1` (the "unknown" sentinel
  the QML maps to the spinner).

The QML re-reads it through bindings that reference `controller.downloadEntries`
so it re-evaluates exactly on `downloadsChanged`; no new property and no
`stateChanged`-triggered fetch.

### Message bubble card (QML, `qml/Main.qml`)

Received file references currently render as inline Markdown links and every
file/image/video click opens `attachmentSaveDialog` (`openReference`,
`qml/Main.qml:1697-1727`). Two scoped changes:

1. When a download entry exists for a file uuid referenced by a message
   (`urn:file:<uuid>` scan of the row's `bodyMarkdown`), the bubble renders a
   compact embedded card (`attachmentCard-<messageId>-<uuid>`) below the body,
   mirroring the reference frame: width `min(240, bubble)`, height 58,
   transparent, radius 8, `40x40` radius-8 `bg` preview slot at x8/y9,
   details column at x60 width 134 (name 12px Medium leading 14, details
   12px leading 15). The preview slot shows:
   - `downloading`: 18x18 progress ring (same port as the upload card ring at
     `qml/Main.qml:11478-11486`: `lineWidth 3`, radius 7.5, 24 %-alpha
     `text-muted` track + accent arc) when the percent is known, else the 14px
     spinner (2px `border-subtle` ring with accent leading edge) while the
     total is not yet known (the `starting` phase, see above);
   - `downloaded` within the 2 s confirmation: `check` icon 18 in accent;
     afterwards the file glyph (files/images/videos icon 18 `text-muted`)
     while the details keep metadata + accent "Open";
   - `error`: upload-error icon 18x18, details "Failed" + accent "Retry"
     (the native coordinator removes cancelled transfers instead of keeping a
     `cancelled` entry, so the cancelled wording has no reachable state here).
   `Accessible.name` mirrors the reference `"<fileName>. <status label>"`
   pattern and the card is not clickable while active.
2. `openReference` gates the save-dialog route through the entry state, like
   `handleCurrentAttachmentDownload`: active -> ignore the click; `downloaded`
   -> `controller.openDownload(id)` (no save dialog); `error` ->
   `controller.retryDownload(id)`; no entry -> the existing save-dialog flow
   unchanged. The media-viewer branch keeps priority for gallery image/video
   clicks (`docs/NATIVE_DESKTOP_MEDIA_VIEWER.md`).

The card is visible only while the coordinator keeps the entry (until the user
clears the download center), so the existing bubble appearance for messages
without downloads is byte-identical - no restyle of existing features.

### i18n

New `Main.qml` UI texts (`Downloading…`, `Downloading… %1%`, `Open`, `Failed`)
plus the reference `downloads.*` catalog keys (`attachmentOpen`,
`attachmentRetry`, `attachmentDownloadAgain`, `browserHandoff`) ship reviewed
Russian forms in `translations/workspace_ru.ts` (`native-native-locale`
catalog guard). Reference forms: Скачивание…, Открыть, Повторить,
Скачать снова, Передано браузеру, Ошибка, Отменено.

## Figma

`FIGMA_TOKEN` from the token helper returned an expired token (API 403
"Token expired") at design time, so the mockups were not consulted: **Figma
token unavailable - mockups not consulted.** The reference checkout is the
geometry/palette oracle, which is the authority for this parity port.

## Tests

- `native-controller-transfers` (state/reduction):
  `attachmentDownloadProjectionFollowsTheFileUuidReference` - no entry ->
  empty; active entry wins; percent exposed only with a known total; terminal
  entry survives after completion; error entry surfaces for Retry; cleared
  entries return empty.
- `native-native-window` (painted card, locale x mode rows):
  `receivedAttachmentCardShowsDownloadProgressOpenAndRetry` - card appears
  with ring + "Downloading… 0%" while a transfer is in flight, transitions to
  check + "Open", restores the metadata line after the 2 s window, clicking
  the downloaded card opens the saved file via a registered `file:` URL
  handler without opening the save dialog, an active card click is ignored,
  and a failed entry shows "Failed"/"Ошибка" + "Retry" whose click restarts
  the download.
