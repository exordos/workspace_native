# Message spoilers

The message renderer recognizes paired double bars in ordinary inline text and
fenced blocks whose info string starts with the language `spoiler`. An optional
label follows the language. A block without a label uses the translated title
“Спойлер” or “Spoiler”. Empty bar pairs and delimiters inside ordinary code stay
literal. A disclosed block renders its contents as Markdown.

A conversation bubble starts with each spoiler closed. Clicking an inline mask
or a block header toggles that occurrence. Keyboard users can focus the control
and activate it with Enter or Space. The revealed state belongs to the current
message and body revision. Sidebar and notification summaries expose the text of
ordinary spoilers. Encrypted fragments retain their existing key rules.

The pinned web implementation defines these tokens in
`shared/lib/workspace-message-render/workspace-message-marked.lib.ts` and the
click and keyboard behavior in `workspace-message-body-interactions.hook.ts`.
The block uses the reference disclosure arrow, semibold label and separate
content. In the live composer preview the inline text used a 3.52 px blur and
the block body stayed hidden. Its preview did not respond to clicks or Space.
The native composer uses the closed rendering, while message bubbles attach the
interactive controls.
