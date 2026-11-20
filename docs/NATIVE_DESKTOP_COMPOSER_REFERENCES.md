# Workspace references in the composer

A hash at the start of a word opens the same suggestion list as a mention. The
list shows matching streams first, then topics from the loaded catalog. A stream
matches on its name. A topic matches on its stream and topic names. Enter or a
click inserts a Markdown link followed by a space. Topic labels use the
separator ` › `. Brackets and backslashes in labels are escaped.

Pasting a link to a known stream or topic from this Workspace inserts the same
Markdown link. Other text and trailing punctuation stay in the draft. A
different server, project or stream cannot supply a label from the current
conversation. The picker reads the local catalog without additional requests.

The source contracts are the reference client's
`message-composer-reference.lib.ts` and `message-composer-mention-dropdown.ui.tsx`.
