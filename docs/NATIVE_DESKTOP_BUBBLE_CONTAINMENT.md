# Message bubble containment (issue #131)

Reported symptom: an occasional message painted its text past the bubble's
right edge while the timestamp stayed inside. The reproduced cause is the
Markdown code block: Qt rich text marks every `<pre>` block non-breakable, so
one long monospace exception-trace line never wraps, regardless of the view's
wrap mode (`Text.Wrap` maps to the word-boundary-or-anywhere mode in Qt 6 and
already breaks ordinary long tokens).

## Fix

`NativeMarkdown::render` re-tags the blocks in its own Qt-serialized export:
each exported `<pre>` code line becomes a `<div>` with the identical block
style, while the exported span keeps the monospace face. The message view then
lays every exported line breakably and wraps it inside the bubble. Only
Qt-generated markup is rewritten; user text is escaped in the export and
cannot contain a tag. Each source line remains its own block, so `plain`,
line structure and the inline-timestamp classification are unchanged: an
exported code line ends `</div>`, so a message ending in a code block still
keeps its timestamp in the footer row instead of gaining an inline tail.

## Checks

Local, this host (Qt 6.10.2, `QT_QPA_PLATFORM=offscreen`):

- `native-markdown-test`: new `longCodeLinesAndUnbrokenTokensStayInsideTheBubbleWidth`
  (fenced trace, indented trace, unbroken paragraph token and mixed prose)
  observed RED before the fix and GREEN after; the two earlier expectations
  that pinned the old non-breakable `<pre>` export now pin the contained form.
  Suite: 35 passed, 0 failed.
- `native-window-test`: new `longCodeTraceAndUnbrokenTokenStayInsideTheBubble`
  loads the real window over the synthetic channel, serves a peer fenced
  exception trace and an own long-token message, and asserts for both that the
  item's actual text, font, wrap mode and width keep the layout inside the
  item, the bubble contains the body and stays inside the window, and this
  holds after resizing 1440x900 to 900x700, back, and after reopening the
  conversation. The pinned geometry slots (`realQuickWindowSignsInNavigatesAndSends`,
  `inlineMetadataRequiresTheActualTrailingParagraph`) were re-run as affected
  binaries.

Live-service and other-platform acceptance: not exercised by this change.
