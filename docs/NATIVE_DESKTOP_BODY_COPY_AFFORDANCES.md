# Body copy affordances and link protocols: code copy button, Copy link item, mailto links

Issue #110 (part of #105). Design fidelity: parity-preserving (docs/NATIVE_DESKTOP_PARITY.md).
Reference: workspace_ui `61dacef78dab2f5d0be76f283bebf49fe29b84af`.
Figma token unavailable - mockups not consulted; the reference code is the oracle.

## Reference contract

- Code copy button (`workspace-message-body-interactions.hook.ts:25,137-214`). A
  `button[data-code-copy-button]` is mounted into every rendered `pre > code` block.
  `CODE_COPY_RESET_MS = 1200` (:25). The click handler (:192-214) writes the code
  element's `textContent` to the clipboard (empty-after-trim bodies write nothing),
  then swaps the button state (`setCodeCopyButtonState`, :137-162): success shows
  `message.copied` = "Copied", failure shows `message.copyFailed` = "Failed to copy"
  as the aria-label/title (the check icon replaces the copy icon), and the state
  resets to the idle `message.copy` = "Copy" after 1200 ms. A pending timer from an
  earlier click is cleared before a new state is applied.
- Copy link context item (`workspace-message-bubble-menu.ui.tsx:349-360`). The body
  context-menu resolver (`workspace-message-body-interactions.hook.ts:266-280`) walks
  `target.closest("a[href]")` and keeps the href only through `resolveExternalHttpUrl`
  (`isValidUrl` in `shared/lib/validation.ts:8,20-27`: parseable URL with an `http:`
  or `https:` protocol). When a link URL is present the menu gains a "Copy link"
  (`message.copyLink`) action in the secondary section, before "Copy text", that
  writes the href and closes the menu.
- mailto links (`workspace-message-render.lib.ts:53`). The rendered body admits the
  `SAFE_LINK_PROTOCOL_PATTERN = /^(?:https?:|mailto:)/i` protocol family, so a
  `mailto:` address stays an anchor; the click path hands it to the OS default mail
  handler (`hook.ts:689-692`). A mailto href is not an `isValidUrl` URL, so
  right-clicking a mailto anchor never offers "Copy link".
- States: light + dark themes, `ru` + `en`, copy success and copy error feedback.

## Native design

### 1. Code block copy button

`NativeMarkdown::render()` gains two entries in its result map, computed from the
parsed `QTextDocument` before export:

- `codeBlocks`: one map per fenced block with the joined line `text` and the
  document `start` position. Fenced code survives import as blocks whose
  `blockFormat().nonBreakableLines()` is set; consecutive such blocks form one
  code block (the exporter turns each line back into its own block, matching the
  `containedCodeBlocks` re-tag from earlier work). TextEdit re-imports the
  exported body with a 1:1 block/text mapping, so the offsets hold in the bubble's
  `messageBody` document.

The message bubble (`Main.qml`, message row) adds a `Repeater` over
`messageRow.rendered.codeBlocks`. Each delegate is a small flat button
(objectName `codeCopyButton-<messageId>-<index>`, idle label `Copy`) positioned
at the top-right corner of its code block: `messageBody` +
`positionToRectangle(start)` for the y, right edge of the body for the x.
The button is parented to the text item that laid out the code. Hovering its
label highlights the button, and a click anywhere on the button copies the block.
Clicking writes the block text through `controller.copyToClipboard(text)`,
swaps the label to `Copied` / `Failed to copy`, and a
`Timer { interval: 1200 }` (mirrors `CODE_COPY_RESET_MS`) restores
`Copy`; a repeat click restarts the timer, matching the reference's clear-and-reset.
The label Text carries objectName `codeCopyButtonText-<messageId>-<index>` so
tests and AT see the feedback. Text runs on either side of inline media have
their own copy buttons. Locked encrypted messages and key-control rows have no
rendered code blocks.
The message-permalink row was relabeled `Copy message link`
(`message.copyMessageLink`, reference :247) so it cannot be confused with the
hyperlink `Copy link` item introduced below.

### 2. Copy link on right-click

The bubble's right-click `MouseArea` calls `messageBody.linkAt(bodyLocalPoint)`
(Qt 6.10 `TextEdit.linkAt(x, y)`, which returns the href of the anchor under the
point or the empty string). `openMessageActions` gains the link argument and stores
it in `root.menuContextLink`. The `messageMenu` shows a new
`MessageMenuAction` `copyBodyLinkAction` with label `qsTr("Copy link")` only when
`menuContextLink` parses as http/https (the native `isValidUrl` analogue:
`root.markdown.reference(link).kind === "external"` with an http(s) URL, i.e. the
same gate as `workspace-reference.cpp`), placed in the secondary section right
before `copyMessageAction` ("Copy text"), mirroring the reference ordering.
Triggering writes the href to the clipboard and closes the menu. Because
`parseWorkspaceReference` still classifies mailto separately (below) and the gate
requires http/https, a right-click on a mailto anchor opens the menu without the
item, exactly like `resolveExternalHttpUrl` returning null.

### 3. mailto links

`workspace-reference.cpp` accepts the `mailto:` scheme in the external branch of
`parseWorkspaceReference` (the `SAFE_LINK_PROTOCOL_PATTERN` family): strict-mode
`QUrl`, no userinfo/host, non-empty address part. The reference keeps
`WorkspaceReferenceKind::External` with the full URL, so the render sanitization
loop (`native-markdown.cpp`) stops stripping the anchor, `openReference` routes it
to `NativeMarkdown::openExternal`, and `QDesktopServices::openUrl("mailto:...")`
opens the configured mail handler - the Qt equivalent of the reference OS mail
path. `isValidUrl` gating elsewhere (Copy link) is unaffected because it checks the
scheme explicitly.

## Strings

New `Main.qml` strings: `Copied`, `Failed to copy` (`Copy link` already exists).
Russian entries follow the reference locale values: "Скопировано",
"Не удалось скопировать" (workspace_ru.ts; "Copy link" -> "Копировать ссылку" is
already translated).

## Test plan (native-native-window)

`packages/native/tests/native-window-test.cpp`, inside `NativeWindowTest`:

- `codeBlockCopyButtonWritesClipboardAndResetsFeedback`: a seeded message with a
  fenced block shows `codeCopyButton-<id>-0`; clicking it puts the code text on the
  clipboard, the button label becomes "Copied", and after ~1200 ms it returns to
  "Copy".
- `rightClickOnBodyLinkOffersCopyLinkItem`: right-click on rendered link text shows
  the menu with `copyBodyLinkAction`; triggering copies the href. Right-click on
  plain text keeps the item hidden.
- `mailtoAnchorStaysInteractive`: the rendered body keeps a `mailto:` anchor (the
  body document and `markdown.reference("mailto:...")` classify it as external),
  and right-click does not offer "Copy link" for it.

Clipboard assertions set their own contents inside the case so the
`clipboardSentinelIntact` guards keep passing. No `window->close()` at case end.
