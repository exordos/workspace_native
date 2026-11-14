# Reply to selected text (issue #218)

Bubble text selection plus a bubble-menu Reply that quotes the exact selected
fragment. Parity-preserving clone of the reference flow at
`workspace_ui@230e02b8b0273dee556a406746d122180a68b3e7`. The reference flow is
menu-based: there is no floating selection toolbar.

## Reference contract (measured from source at 230e02b8)

Paths below are relative to `packages/web/src/` at that sha.

### Selection capture — `widgets/workspace-message-list/workspace-message-body-interactions.hook.ts:97-116`

`resolveSelectionInsideBody(bodyElement)` reads the DOM selection and returns
`undefined` unless:

- the selection string is non-null and its trimmed form is non-empty (the raw,
  untrimmed text is what gets returned),
- both endpoints are inside this bubble's body element: `anchorNode` and
  `focusNode` (via their `parentElement`) must be contained by the body.

A selection that reaches outside the body is therefore ignored, and a
whitespace-only selection is treated as no selection.

### Selection survives the menu — same hook, lines 364, 372-374, 383

Opening the context menu snapshots the live selection into
`capturedSelectionRef` (`openContextMenuAt`, line 383). `getSelectedText()`
(372-374) returns the snapshot if present, otherwise re-resolves from the DOM,
so activating a menu item after the DOM selection was lost still quotes the
fragment that was selected when the menu opened.

### Menu wiring — `widgets/workspace-message-list/workspace-message-bubble-menu.ui.tsx:295-317`

The `reply` item calls `onReplyMessage(message.uuid, getSelectedText())` then
closes the menu; the sibling `add-reply` item passes the same selection to
`onAddReplyMessage`. Labels are `message.reply` (EN "Reply", RU "Ответить") and
`message.addReply`. The hover quick-action Reply behaves identically
(`workspace-message-bubble-presentation.ui.tsx:404-411`).

### Encrypted-fragment drop — `workspace-message-bubble-presentation.ui.tsx:636-639`

`getSelectedText` is wrapped: `renderedBody.hasSecrets ? undefined :
getRawSelectedText()`. A bubble containing encrypted fragments never yields a
selection, because revealed text must not leak into a quoted line. The page
layer re-applies the same guard plus the whitespace rule when building the
quote
(`pages/chat/chat-page-workspace.ui.tsx:2747-2751`):
`selectedQuoteText` survives only when non-null, trimmed non-empty and free of
secret content; the quote source then falls back to the whole message body.

### Quote rendering — `widgets/workspace-message-list/workspace-message-quote.ui.tsx:145-152`

With `selectedText` set, the quote frame shows the author header plus the
fragment as plain wrapped text
(`whitespace-pre-wrap break-words text-sm`) - not the rendered message body.

### Composer tab label — `features/workspace-reply/workspace-reply.ui.tsx:28-37`

`getTabExcerpt = (tab.selectedText ?? tab.quotedContent).replace(/\s+/g, " ").trim()`;
content longer than 36 chars is cut to 36, `trimEnd()`-ed and suffixed with
`…`. `getTabLabel = excerpt.length > 0 ? "<sender>: <excerpt>" : "<sender>"`.
The selection wins the label over the full quoted content.

### Wire format

`buildWorkspaceQuoteReference` (`shared/lib/workspace-message-quote.lib.ts`)
serializes `[Author](urn:quote:<uuid>?text=<percent-encoded>)`; without a
selection the line stays `[Author](urn:quote:<uuid>)`. The native side already
ships both halves of the receiving contract: `?text=` is parsed in
`packages/native/src/workspace-reference.cpp:277-290`, built by
`buildWorkspaceQuoteUrn` (`workspace-reference.cpp:355-360`,
`QUrl::toPercentEncoding`), and displayed as `quoteText` on the receiving
bubble (`packages/native/qml/Main.qml:4305-4316`).

## Figma

Token available; consulted Exordos-Core `Q3BtN9qwfQV8rAsyBAgCy1` (tree cache
`~/dev/reference_state/figma/tree.json`, <7 days old). The desktop Messenger
section `2473:13434` contains **no** mockup of a text-selection or
quote-selection state (keyword scan for `цитат/ответ/selected/quote` over the
section: 0 hits) - the selection state is not mocked, so the reference-code
contract rules. Corroborating mockups elsewhere: the desktop frame
`3793:16275` ("Ответить") and note `3829:14037` in "Пересылка сообщений,
закреплённое сообщение", and the mobile note `13796:10448` in
"Цитирование / редактирование" which states a quoted text fragment renders
like a normal reply quote but shows only the fragment - matching
`workspace-message-quote.ui.tsx:145-152`.

## Native approach

Decision: make the bubble body a read-only `TextEdit` with `selectByMouse`.
`Text` has no selection model; `TextEdit` under Qt 6 rich text
(`textFormat: TextEdit.RichText`) renders the same document layout the `Text`
item used (same `contentHeight`, `lineHeightMode: FixedHeight 20`,
`wrapMode: Wrap`) and exposes an assertable `selectedText` property plus
`positionAt`/`positionToRectangle` - the only selection mechanism a window
test can drive deterministically with `QTest` mouse drags.

`packages/native/qml/Main.qml`:

- `messageBody` (`:5048`, previously a non-selectable `Text`, `textFormat:
  Text.RichText`): becomes `TextEdit { readOnly: true; selectByMouse: true;
  cursorVisible: false }` with `selectByMouse` disabled for `keyControl`
  rows (centered control labels must not select). Everything else (objectName
  `message-content-<id>`, width/height formula, fonts, colors, `linkColor`,
  `onLinkActivated`, `Accessible.ignored`) is preserved; `forceLayout()` and
  `contentHeight` exist on `TextEdit` with the same meaning.
- `openMessageActions(message, anchor)` gains a `selectedText` parameter and
  stores it in `root.menuSelectedText` when the menu opens - the native mirror
  of the reference snapshot at hook line 383. The body item is in scope at
  both call sites (`Main.qml:5323,5328`), so the value comes from
  `messageBody.selectedText` directly.
- `replyMessageAction.onTriggered` (`:8498`) forwards `root.menuSelectedText`
  into `beginReply`/`addReplyTab`, mirroring the reference menu item passing
  `getSelectedText()`.

`packages/native/src/native-controller.{h,cpp}`:

- `ReplyTab` gains `selectedText`. `beginReply(id)` and `addReplyTab(id)` take
  an optional `selectedText` argument (default-`{}`, so all existing QML and
  C++ call sites keep compiling).
- Normalization on entry (the page-layer guard, chat-page-workspace:2747-2751):
  the selection is dropped when it trims to empty or when the quoted message
  body contains an encrypted fragment (`workspace::isWorkspaceSecretReference`
  semantics - `urn:secret:` markers). A dropped selection degrades to the
  whole-message quote, exactly like the reference.
- `replyTabPlain()` prefers the tab's selection (collapsed like
  `replace(/\s+/g," ").trim()`), so `replyTabs()` rows, `replyExcerpt()` and
  the composer banner show the fragment while the 36-char tab rule and the
  80-char banner rule apply unchanged.
- The send snapshot (`native-controller.cpp:10538-10552`) passes
  `tab.selectedText` into `workspace::buildWorkspaceQuoteReference`, producing
  `urn:quote:<uuid>?text=<percent-encoded>`; empty selection keeps the plain
  line. Edit restore (`parseEditQuoteRestore`, `:8751-8759`) parses `?text=`
  from the restored quote line into the tab, so the banner shows the fragment
  and the verbatim `tab.reference` continues to round-trip the identity.

No new `tr()` strings: the menu label `qsTr("Reply")` already exists in
Main.qml and `workspace_ru.ts`.

## Risks / notes

- Body `TextEdit` consumes primary-button presses inside the bubble for
  selection. Hover quick actions and the hover "..." button sit above it and
  keep working; right-click menu opening is unaffected (`TextEdit` only
  accepts the left button for selection, the right-click still reaches the
  bubble `MouseArea`). Press-and-hold menu opening on touch inside body text
  degrades to selection - accepted for desktop-first parity, matching the
  browser behavior of the reference.
- The poll-card work on a parallel lane touches `NativePollCard.qml` and poll
  code only; this change does not edit those files.
- `docs/NATIVE_DESKTOP_REPLIES.md:212-214` recorded this deferral; this
  document supersedes that line for selection capture.
- Color/geometry claims in tests cite their reference file at 230e02b8.

## Tests

- `native-window-test.cpp`: drag-select a fragment inside a live bubble body
  with `QTest` mouse events, open the bubble menu, trigger
  `replyMessageAction`, assert the composer banner shows exactly the fragment,
  the tab row label applies the 36-char rule, the sent wire carries
  `urn:quote:<uuid>?text=<percent-encoded>`; whitespace-only selection falls
  back to the whole message; a decrypt-locked body exposes no selectable
  surface.
- `native-controller-test.cpp`: selection wins the excerpt/label and the wire
  (`?text=` percent-encoding incl. Cyrillic/`&`), whitespace selection drops
  to the full quote, encrypted-fragment bodies reject a selection (and reply
  itself), edit restore re-reads `?text=`.
