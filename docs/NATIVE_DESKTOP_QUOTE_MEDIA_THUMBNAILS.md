# Quote and reply previews show a media thumbnail (issue #217)

A quoted or replied message that carries an image (or, when there is no image, a
video) shows a small square media thumbnail as the leading element of the quote
frame — both in the composer reply preface and inside a received message bubble.
Today both native frames are text-only.

## Reference contract

Measured in the `workspace_ui` checkout at sha
`230e02b8b0273dee556a406746d122180a68b3e7` (the pinned sha; the checkout working
tree is on `master` at `75bd21f37de94728e2ebe100acfea24ac9c1a4e1`, every file
below was read from the pinned blob with `git show`).

### Composer reply preface

`widgets/message-composer/message-composer-reply-quote-preview.ui.tsx`:

- `:84-92` `mediaThumbnail` is rendered when the parsed quote source has a
  `mediaReference` **and** a preview loader is available. The element is keyed by
  `reference.fileUuid`, so a different quoted file remounts the thumbnail.
- `:95-101` the thumbnail is passed to `WorkspaceMessageQuoteFrame` as `leading`,
  on the `composer` surface, with header `t("message.replyTo"): <sender>`.
- `:74-83` once the thumbnail reports `ready` **for that same file uuid**
  (`readyMediaFileUuid === source.mediaReference.fileUuid`), the summary is
  re-rendered with `hiddenWorkspaceMediaFileUuids: source.mediaFileUuids`, i.e.
  every media markdown token is dropped from the text while the picture carries
  the meaning. Before `ready` the text keeps the media label.
- `:102-106` body: `line-clamp-2 whitespace-pre-wrap break-words text-sm
  text-text-primary`, rendered only when the summary is non-empty.

### In-message quote frame

`widgets/workspace-message-list/workspace-message-quote.ui.tsx`:

- `:257-285` the source document is only parsed when the quote is neither a
  forward snapshot, nor unresolved/locked, nor a selected-text quote
  (`reference.selectedText != null`), and `mediaReference` is forced to `null`
  for forward snapshots. `mediaFileUuids` is the set of **all** media uuids in
  the body, not only the picked one.
- `:286-296` the same hide-media-once-ready rule as the composer, applied to the
  rendered body segments.
- `:437-448` `leading` is the thumbnail, again suppressed for forward snapshots
  and whenever no preview loader is wired.

### Thumbnail element

`entities/messenger/messenger-workspace-media-thumbnail.ui.tsx`:

- `:150-160` container: `relative min-h-12 w-12 shrink-0 self-stretch
  overflow-hidden rounded-md bg-bg-elevated` → width 48 px, min height 48 px,
  stretches to the frame height, 6 px radius, `bg-bg-elevated` fill, never
  shrinks, content clipped. It is `aria-hidden` and carries
  `data-workspace-quote-media-thumbnail="true"` plus
  `data-workspace-quote-media-status="loading|ready|error"`.
- `:17` lazy: the load starts through an `IntersectionObserver` with root margin
  `640px 0px`.
- `:14`/`:111-113` state machine `loading | ready | error`; on `error` the element
  renders `null` — the frame falls back to the text-only layout.
- `:124-147` ready state: `<img>`/`<video>` absolutely inset, `object-cover`,
  `pointer-events-none`; a video uses `muted playsInline preload="metadata"`.
- `bg-bg-elevated` (blue-cold palette, `shared/lib/themes/blue-cold.ts:9`,
  `:50`-ish light block): `#ffffff` light, `#222328` dark.

### Which attachment is picked

`entities/messenger/messenger-workspace-message-body-files.lib.ts:22-35`
(`selectWorkspaceMessageMediaPreviewReference`): walk the body's preview file
references in document order; the **first image wins**, otherwise the **first
video**; anything else yields no thumbnail.

### Quote frame layout with a leading element

`shared/ui/workspace-message-quote-frame/workspace-message-quote-frame.ui.tsx`:

- `:8-9` frame layout: `my-1 min-w-0 rounded-md border-l-2 border-accent px-2 py-1.5`
  (radius 6 px, 2 px accent left bar, padding 8 px horizontal / 6 px vertical).
- `:60-64` with `leading`: `flex min-w-0 items-stretch gap-2` — 8 px gap, the
  header and body go into a `min-w-0 flex-1` column next to the thumbnail.

## Native gap

- `packages/native/qml/Main.qml:5918-6011` `replyNotice` → `replyQuoteFrame` is a
  `Column` with `replyAuthorText` (12 px / 16 px fixed, accent) and
  `replyExcerptText` (14 px / 20 px fixed, max 2 lines). No leading slot.
- `packages/native/qml/Main.qml:4535-4614` `messageQuoteFrame` is the same shape
  for a received bubble: accent bar + header + `quoteText`/`quoteHtml` body.
- `packages/native/src/native-controller.cpp` `replyExcerpt()` truncates the
  whitespace-collapsed plain render to `QUOTE_PREVIEW_MAX = 80` with an ASCII
  `"..."` marker. The plain render replaces an inline media token with its alt
  text or `tr("Image")` (`native-markdown.cpp:210-214`), so a quoted image-only
  message currently reads `Image` in the preview — that is exactly the text the
  reference hides once the thumbnail is ready.
- Message rows already carry `quoteContent` (`native-controller.cpp:9077-9078`),
  and `NativeMarkdown::mediaReferences()` returns image/video references in
  document order (used by the media viewer, `media-viewer.cpp:72`).

## Native design

- **Pick rule** lives in the controller: `NativeController::mediaPreviewReference
  (markdown)` returns `{urn, kind}` for the first image, else the first video,
  `{}` when there is none. It reuses `NativeMarkdown::mediaReferences()` so the
  same display-limit guard applies, and returns `{}` for bodies the quote frame
  must not preview (forward snapshot / selected-text quotes, which native carries
  as `quoteText`).
- **Reply side** exposes `replyMediaUrn` / `replyMediaKind` next to
  `replyAuthor` / `replyExcerpt`, and `replyMediaSummary` — the same 80-character
  summary built with the media tokens hidden, i.e. what the reference shows after
  `ready`.
- **Hide-media render** is a `NativeMarkdown::render(markdown, hideMedia)` opt-in
  that drops media fragments from the document before `html`/`plain`/`parts` are
  produced, mirroring `hiddenWorkspaceMediaFileUuids`.
- **Thumbnail view**: new `qml/NativeQuoteMediaThumbnail.qml`, 48 px wide,
  minimum 48 px high, `Layout.fillHeight` so it stretches like `self-stretch`,
  radius 6 px, `colors["bg-elevated"]` fill, `Image.PreserveAspectCover` inside a
  clipped rounded mask (`object-cover`). It holds exactly one lease on
  `nativeMessageImages` while active and publishes
  `status: "loading" | "ready" | "failed"`.
- **Frame wiring**: `replyQuoteFrame` and `messageQuoteFrame` keep their current
  chrome and wrap their contents in `RowLayout { spacing: 8; alignment stretch }`
  with the thumbnail as the first child when a media reference exists.
  `replyExcerptText` switches to `replyMediaSummary` when the thumbnail reports
  `ready`, so the media label disappears exactly when the picture arrives.
- **Video**: the bounded image manager only decodes `urn:image:` sources, so a
  video reference gets the same 48 px `bg-elevated` box with the existing media
  viewer play badge treatment instead of a decoded frame. That matches how the
  media viewer strip already renders video slots
  (`qml/NativeMediaViewer.qml:352-390`) and is the one deliberate visual deviation
  from the reference `<video preload="metadata">` poster frame.
- **Failure**: `status: "failed"` hides the thumbnail and keeps the text-only
  layout (reference returns `null` on error), including the un-hidden media text.
- **Theme note**: the default `blue-cold` palette already carries the reference
  values (`NativeTheme.qml` `bg-elevated`: `#ffffff` light, `#222328` dark), so
  the token matches the reference exactly there. Non-`blue-cold` native palettes
  keep their own `bg-elevated` (`#ffffff` / `#333333`) — the deviation is accepted
  because the token, not the literal hex, is the contract.
- **Accessibility**: the thumbnail is decorative (`Accessible.ignored`), the
  quoted author line stays the accessible name of the frame.

## States

| State | Reference | Native |
| --- | --- | --- |
| No media reference | text-only frame | text-only frame |
| Media reference, loading | 48 px elevated box, media label still in text | same (`status: loading`) |
| Ready | picture (object-cover), media label dropped from text | same (`status: ready`) |
| Failed | no box, media label back | same (`status: failed`) |
| Forward snapshot / selected text | never a thumbnail | never a thumbnail |

Covered in `light` and `dark` modes and in `ru` / `en` locales; the only new
translatable string is the accessible label, which is registered in
`packages/native/translations/workspace_ru.ts`.

## Tests

`native-native-controller`: pick rule (image wins over video, video when no
image, none for text-only), forward-snapshot / selected-text suppression, and the
media-hidden summary dropping the `Image` label.

`native-native-window`: a quoted image message renders
`quoteMediaThumbnail-<id>` at 48 px with a painted image inside the bubble frame,
and the composer reply preface renders `replyMediaThumbnail` whose excerpt loses
the media label once the preview is ready.

## Live measurements (stream "native qa", 2026-11-14)

- Composer reply preface: thumbnail rendered at 48 px wide (x=397..444) with the
  quoted text column shifted right; the media token is dropped from the excerpt
  once the image is ready (no `![Image` text remains).
- In-bubble quote frame (sent reply): thumbnail rendered at exactly 48 px wide
  (x=998..1045) as the frame's leading element, same text dedupe behavior.
- Recorded deviation: the reference box is `min-h-12` with `self-stretch`, so the
  web frame never renders the thumbnail shorter than 48 px. The native thumbnail
  self-stretches with the frame minus 6 px insets (`height: parent.height - 12`),
  which matches the reference in the tall composer preface (measured 48x60) but
  yields 48x36 inside a compact two-line in-bubble quote frame. The frame height
  is driven by the excerpt text instead of a thumbnail min-height; widening the
  frame to force the 48 px floor was rejected because it would change bubble
  rhythm for every quote, not just media ones.
