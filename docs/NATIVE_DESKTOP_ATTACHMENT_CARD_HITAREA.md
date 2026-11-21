# Attachment card click targets and keyboard activation (issue #629)

Status: implemented (lane `cassi/attachment-card-hitarea`).
Reference contract: `workspace_ui` `56f0b894895fee06de67bd59f5e5318b063e0543`.

## Reference measurements (oracle: reference source)

The Figma API returned 403 with the stored token at lane start, so mockups were
not consulted; this scope is a parity-preserving input-layer port and every
decision below is measured from the reference source at the contract sha.

- Placeholder markup (`packages/web/src/shared/lib/workspace-message-render/
  workspace-message-render.lib.ts:206,275`): every attachment reference renders
  as one `<span role="button" tabindex="0" class="workspace-message-attachment-host"
  data-workspace-file="true" data-workspace-file-uuid=… data-workspace-file-kind=…
  title=… aria-label=…>`. The accessible name is the label
  `Файл: <name>` (ru hard-coded; en "File: <name>"). The icon svg, its `path`
  elements and the text label are all *inside* that span, so any click inside
  the card resolves to the card via `closest()` - there is no separate link.
- Click dispatch (`packages/web/src/widgets/workspace-message-list/
  workspace-message-body-interactions.hook.ts:282-305,574-604`): the body click
  handler maps any target inside `[data-workspace-file='true']` to the file
  reference and dispatches the UUID download action for primary unmodified
  clicks (hook.ts:118-120). The reference Vitest case
  'downloads Workspace attachments when clicking the file icon %s' clicks the
  `svg` and the `svg path` and expects `onDownloadFile` for the same uuid.
- Keyboard (hook.ts:481-521): Enter or Space on the focused card fires the same
  download action (reference test 'activates Workspace attachment placeholders
  with Enter and Space').
- Idle card presentation (`workspace-message-list.ui.test.tsx:5161-5206`):
  presentation div `h-[58px] w-60`, meta line `PDF · 1.2 MB` from the
  extension/size ladder, file glyph icon inside the 40x40 slot.

## Current native gap (Main.qml `AttachmentDownloadStateCard`)

The #631 port renders the card only while the download coordinator has an entry
for the uuid (`messageRow.attachmentDownloads` skips references without state)
and then:

1. The icon-slot chrome items are `NativeIconButton` (ToolButton) instances;
   their internal control mouse areas sit above the card fill `MouseArea` and
   swallow clicks aimed at the icon interior ("icon center").
2. The card `Rectangle` accepts no keyboard focus and has no Enter/Space
   handling, so a focused card cannot activate the download.
3. `Accessible.name` was `"<fileName>. <status label>"`, not the reference
   contract name `File: <name>`.

## Design (input/hit-area layer only; no geometry, palette or spacing change)

1. **Idle cards.** `messageRow.attachmentDownloads` now also projects
   references the coordinator has no entry for, as `state: {}` (idle). The card
   is `idle` when the state object is empty. This mirrors the reference where
   the card exists for every attachment reference; it is the minimum model
   touch (one skip removed) that lets the hit-area and keyboard acceptance test
   exist. Lane note: sibling #646 reworks the card *model*; this change is
   deliberately one projection guard so both lanes merge cleanly.
2. **Single hit area.** The card's fill `MouseArea` gets `z: 1` so it sits
   above the presentation children (icon slot chrome, name/details columns).
   Every child inside the card is presentation-only - all of them already carry
   `Accessible.ignored: true` or are plain `Text` - so the whole card area
   (icon center incl. glyph, label, details, background, edges) delivers one
   activation. No child keeps independent click behaviour.
3. **Idle activation.** An idle card click/activation opens the same download
   route a text link takes: `downloadOwner/downloadId` set to the file uuid and
   `attachmentSaveDialog.open()`. State cards keep the #631 routing untouched
   (active = ignore, downloaded = re-open, error = retry).
4. **Keyboard.** The card gains `Keys.onPressed` for Return/Enter/Space and
   `Accessible.onPressAction`, both funneled through one `activate()` function;
   tests focus the card via `QQuickItem::setFocus(true)`.
5. **Accessible name.** `Accessible.name` becomes `qsTr("File: %1").arg(fileName)`
   (ru «Файл: %1», `Main` context in `workspace_ru.ts`), matching the
   reference aria-label exactly. The status wording moves to
   `Accessible.description` (name-first, status after - same screen-reader
   content as before). The #631 window-test expectations for the old name are
   updated in the same commit.

States: light + dark, ru + en - hit-area and keyboard contract only, colors and
geometry unchanged (all chrome already shipped with #631).

## Verification mapping

- `attachmentCardHitAreaAndKeyboardActivateDownload` (native-window-test):
  icon center, label and card edge clicks and Enter/Space on a focused card all
  open `attachmentSaveDialog` with `downloadId ==` the file uuid, and the
  accessible name equals `File: <name>` / «Файл: <name>».
- The #631 `receivedAttachmentCardShowsDownloadProgressOpenAndRetry` rows keep
  the state-routing contract (updated only for the new accessible name and the
  now-projected idle card).
