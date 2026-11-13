# Native desktop edit-time quote-reference restore (issue #202)

Issue: #202 (P2-high) - editing a message whose stored body starts with a quote
reference restores the reply session (chip + answer split) instead of exposing the
raw `urn:quote` markdown in the composer.

Version: no version bump in this change; it is a parity fix inside the released
inline-edit and multi-reply-tab features.

Reference contract measured from `workspace_ui` at sha
`61dacef78dab2f5d0be76f283bebf49fe29b84af`:

- `packages/web/src/features/workspace-reply/workspace-reply-edit-restore.lib.ts`
- `packages/web/src/features/workspace-reply/workspace-reply-restore.lib.ts`
- `packages/web/src/features/workspace-reply/workspace-reply-edit-restore.lib.test.ts`
- `packages/web/src/pages/chat/chat-page-workspace.ui.tsx:2497-2565` (edit start),
  `packages/web/src/widgets/message-composer/message-composer.ui.tsx:1160-1187`
  (edit submit), `chat-page-workspace.ui.tsx:4080-4096` (outgoing reply body).

## What the reference actually does

Opening a message for edit runs `restoreWorkspaceReplySessionFromMarkdown`
(`workspace-reply-restore.lib.ts`) over the stored markdown:

- The body is normalized (CRLF to LF, trimmed). A **quote-reference line** is a
  standalone first line `[Name](urn:quote:<uuid>[?text=...])` (case-insensitive, the
  urn must parse as a quote reference and the link label must be non-empty after
  backslash unescaping). The following lines up to the next quote are the answer.
- A **legacy blockquote header** `> [Name](urn:user:<uuid>) [wrote](urn:message:<uuid>):`
  followed by consecutive `>` lines is also recognised (marked TODO for the backend
  rollout); the blockquote lines are the stored quoted content and the answer follows
  after a blank line.
- Multiple quotes are only recognised at line starts preceded by a blank line; each
  gets its own reply tab with its own answer, and the composer shows the active
  (first) tab's answer. `activeAnswer = tabs[0].answer`.
- The tab-plus-answer split is what the composer edits: `initialMarkdown` becomes
  `activeWorkspaceReplyTab.answer` and typing stays in lockstep with the active tab's
  answer (`message-composer-draft.hook.ts:100` keeps routing value changes to the
  reply session while a preserved-context edit is open).
- Saving an edit submits `outgoingBody` (`message-composer.ui.tsx:1168-1172`), which
  is `buildWorkspaceReplyMarkdown(tabs)`: every tab re-serializes as its quote line
  plus blank line plus answer, joined by blank lines. The quote identity therefore
  round-trips unchanged, including the `?text=` selected-text fragment; a legacy
  header is rewritten to the current `[Name](urn:quote:uuid)` form because the tab
  serialization always uses the quote reference.
- Resolution: the edit-restore controller collects *structural* quote-reference
  blocks (`parseWorkspaceMessageBody`, so inline/code/fenced occurrences are not
  structural) and resolves each quoted message from the local store or via
  `GET /messages/?uuid=`; if any structural quote is unavailable the restore returns
  `null` and the reference falls back to editing the raw markdown.

## Native design

The native client already keeps a `ReplyTab` list in `native-controller.cpp`
(`docs/NATIVE_DESKTOP_MULTI_REPLY_TABS.md`), the composer quote frame
(`replyQuoteFrame`, single tab) and the `replyTabStrip` (multiple tabs) in
`Main.qml`, both of which already position themselves below the edit notice bar, and
`workspace-reference.cpp` already parses/builds `urn:quote:<uuid>[?text=]`. This
change wires those pieces into the edit session:

- New parser in `native-controller.cpp` (mirrors `workspace-reply-restore.lib.ts`):
  `parseEditQuoteRestore(body)` returns one entry per recognised quote with the raw
  reference line (kept verbatim, so `?text=` and label escaping round-trip), the
  quoted message uuid, the fallback sender name from the link label, and the trimmed
  answer. The legacy blockquote header is recognised the same way; its reference line
  is synthesized in the canonical `[Name](urn:quote:<uuid>)` form so a saved rewrite
  migrates the legacy form to the current contract, as the reference's tab
  serializer does. A body whose first line is not a standalone quote reference (or
  legacy header) restores nothing and edits raw, exactly as today.
- `beginEditMessage` runs the parser on the stored body. When quotes are recognised it
  opens a reply session from them (`ReplyTab.reference` carries the wire line), sets
  the active tab to the first, makes the edit buffer the first answer, and flags the
  session as a restored-edit session. Otherwise behaviour is unchanged.
- Quoted messages outside the loaded window are resolved through the existing quote
  lookup path (`scheduleQuoteFetch` -> `GET /messages/?uuid=`); the resolved-quote
  cache is now also consulted by `replyAuthorFor`, `replyExcerpt` and `replyTabs()`,
  and a landed lookup re-emits `replySessionChanged` so the chip fills in. The
  existing transport-failure rule (never cache a failure as an authoritative miss)
  applies unchanged.
- `setDraft` inside a restored-edit session keeps the active tab's answer in lockstep
  with the buffer (same rule the non-edit reply session already has).
- `submitEditMessage` composes the PUT body from the tabs exactly like `send()`
  composes a reply: `reference + blank line + answer` per tab in tab order, joined by
  blank lines, trailing whitespace trimmed, the active tab answering with the live
  buffer. The quote line - including any `?text=` fragment - is preserved verbatim.
- `closeEditSession` (cancel, confirmed save, route change) also closes a restored
  reply session, so the restored tabs never leak into the ordinary composer.
- No QML change: the quote frame/strip render whenever a reply session is open, and
  their geometry already accounts for the edit notice bar.

States covered: light + dark (palette unchanged), ru + en (no new user strings; the
restored flow reuses the existing composer reply chip labels).

## Deviations from Source (named and justified)

1. Optimistic restore: the reference awaits quote resolution before opening the
   session and falls back to raw editing when a structural quote is unavailable.
   The native edit start is synchronous (QML menu action), so native restores
   immediately using the link label as the fallback sender name and upgrades the
   chip asynchronously through the quote cache. The parsed label is retained per
   restored tab and keeps answering the chip author while its origin lookup is
   still in flight and, if the origin turns out to be unavailable or the lookup
   keeps failing, for the whole session (review thread 4559371974) - the chip
   never shows an empty author. Deleting the restored tab after an
   authoritative "unavailable" answer is deferred: the composer keeps the chip and
   the answer split, which never loses text (re-save round-trips the same quote).
2. Legacy blockquote quoted content lines are not carried into the tab (the native
   `ReplyTab` resolves the excerpt from the quoted message itself, in-window or via
   the quote cache); the header, the split and the round-trip contract are the same.
3. No attachment or `?text=` selection editing; same limitation as the existing
   reply session (#188 owns edit attachments).
4. Fence-aware scanner: the reference's `workspace-reply-restore.lib.ts` line
   scan has no fence tracking of its own - it relies on the edit-restore
   controller to only treat quote references that `parseWorkspaceMessageBody`
   surfaces as top-level blocks as structural, and a fenced code block never
   yields a `quote-reference` block. The native line scanner reproduces that
   guarantee directly: it tracks CommonMark ``` and ~~~ fences (up to 3 leading
   spaces, closing fence of the same character and at least the opening length)
   and never treats a quote-like line inside a fenced block as structural
   (review thread 4559371966). A quoted answer containing a fence therefore
   round-trips verbatim through a re-save.

## Tests

- `native-native-controller`:
  - edit of `[Name](urn:quote:id)\n\nanswer` opens the reply session, composer shows
    only the answer, tab follows typing in lockstep;
  - re-save preserves the same quote line including the `?text=` fragment and closes
    the session; cancel closes both the edit and the restored reply session;
  - two quote blocks restore two tabs, answers split per block, re-save serializes
    both in tab order;
  - inline/code occurrence and a plain body keep raw editing with no tabs;
  - legacy `[Name](urn:user:..) [wrote](urn:message:..):` blockquote restores the tab
    and the re-save migrates the body to the `urn:quote` form;
  - a quoted origin outside the loaded window is fetched once via `GET
    /messages/?uuid=` and fills the chip author/excerpt asynchronously;
  - while that lookup is pending (held response) - and after it reports the
    origin unavailable - the chip and tab label show the label parsed from the
    quote line, never an empty author;
  - a standalone `[Name](urn:quote:...)` line inside a fenced code block in the
    answer is code text: one tab is restored and the fenced answer round-trips
    verbatim through a re-save (fence-aware scanner, deviation 4).
- `native-native-window`: the message-menu Edit action on a quoted message shows the
  edit notice and the quote chip (`replyAuthorText`/`replyExcerptText`) above the
  composer, and the composer is pre-filled with only the answer.
