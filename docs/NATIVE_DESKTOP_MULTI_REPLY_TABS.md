# Multi-reply tabs in the composer (issue #200)

Ports the reference multi-tab reply session (several concurrent quote tabs,
per-tab answers, per-tab close, drag reorder, combined send) onto the native
composer. This document records the measured reference contract and the native
module plan. Parity-preserving clone; deviations are listed at the end.

## Reference contract

Measured at `workspace_ui` sha `61dacef78dab2f5d0be76f283bebf49fe29b84af`
(checked out from the pinned reference clone).

### Data model - `packages/web/src/features/workspace-reply/workspace-reply.types.ts` + `workspace-reply.model.ts`

- `WorkspaceReplyTab` = `{ id, createdAt, messageUuid, senderUuid, senderName,
  quotedContent, selectedText?, answer }`.
- `WorkspaceReplySession` = `{ tabs: WorkspaceReplyTab[], activeTabId: string | null }`.
- `EMPTY_WORKSPACE_REPLY_SESSION = { tabs: [], activeTabId: null }`.
- Normalization drops tabs with blank id/createdAt/messageUuid/senderUuid/senderName
  and duplicate ids; `activeTabId` falls back to the first surviving tab.
  Quoted content normalizes CRLF to LF; blank `selectedText` becomes undefined.

### Store operations - `workspace-reply.lib.ts`

- `replyToWorkspaceReply(session, quote, identity, initialAnswer = "")`
  (line 26-57): empty session -> create the first tab with `answer = initialAnswer`;
  otherwise replace only the active tab's quote fields (tab id, createdAt and
  answer are kept). Guards (`beginReply`) map here.
- `addWorkspaceReplyTab` (line 59-79): append a new tab at the end and make it
  active; duplicate id is a no-op.
- `appendWorkspaceReplyTabs` (line 81-111): bulk append; `initialAnswer` lands on
  the first appended tab only when the session was empty; any invalid or
  duplicate entry returns `null` and the caller must not change state.
- `setWorkspaceReplyAnswer` (line 113-127): edits the active tab's answer only.
- `selectWorkspaceReplyTab` (line 129-134): activate an existing tab; unknown id no-op.
- `removeWorkspaceReplyTab` (line 136-153): removing the only tab -> empty
  session; removing a non-active tab keeps active; removing the active tab ->
  active becomes `nextTabs[removedIndex] ?? nextTabs[removedIndex - 1]`.
- `reorderWorkspaceReplyTab` (line 159-192): insertion-position semantics in the
  list before removal; destination is truncated and clamped to `[0, tabs.length]`;
  `dest > source` is adjusted by -1; a single tab or unknown id is a no-op;
  active tab is untouched.
- `buildWorkspaceReplyMarkdown(tabs)` (line 190-211, chat-page send body): for
  each tab in tab order emit `[escaped senderName](urn:quote:<messageUuid>[?text=...])`;
  tabs whose quote reference cannot be built are skipped; a non-blank answer is
  trimmed and appended as `<quote>\n\n<answer>`; blocks join with `"\n\n"`;
  the whole body is trailing-trimmed. Example from `workspace-reply.lib.test.ts:343-351`:
  `[Иван](urn:quote:C)\n\nответ В\n\n[Алексей](urn:quote:A?text=...)\n\n[Алексей](urn:quote:A)\n\nответ А`.

### Page wiring - `packages/web/src/pages/chat/chat-page-workspace.ui.tsx`

- 3005-3099: select / remove / reorder handlers. Removing a tab: when the last
  tab disappears and the composer text is empty, the removed tab's answer is
  restored into the composer text. Composer value change while tabs exist:
  the value is written into the active tab's answer (`setWorkspaceReplyAnswer`);
  the composer input *is* the active tab's answer.
- 4166-4181 (`appendWorkspaceReplyTabs` call sites): the current composer text
  moves into the first appended tab only when the session was empty
  (`hasExistingReplyTabs ? "" : content.text`); a `null` result aborts silently.
- 4083: the outgoing body is `buildWorkspaceReplyMarkdown(tabs)` when the
  session has tabs, otherwise the plain composer text.
- 2960-2999 (post-send cleanup): session resets to EMPTY; composer text becomes
  the earliest-created tab's answer when the composer text itself was empty.
- Encrypted streams: reply tabs stay in page memory, never in durable drafts
  (2766-2795). The native e2ee composer gate already blocks replies.

### Tab strip UI - `packages/web/src/features/workspace-reply/workspace-reply.ui.tsx`

Rendered as composer `leadingContent` only when `tabs.length > 1`
(`chat-page-composer-section.ui.tsx:135-142`); the single-tab reply keeps the
plain quote preview.

- Strip (tablist): horizontal row, `gap-1.5` = 6 px, `overflow-x-auto`,
  aria-label `message.replyTo`. Rendered inside the reply chrome preface row
  `flex min-w-0 items-center gap-1.5` with `pt-2 pb-1` (8 px top, 4 px bottom)
  and the shared composer x-inset (8 px)
  (`message-composer-preface.ui.tsx:231-243`); the clear-reply dismiss button
  moves from the quote row into the tab row as a sibling of the tabs.
- Tab pill: `rounded-xl` (12 px radius), 1 px border `border-subtle`,
  background `bg-elevated` at 50 % alpha; content row height 32 px.
  Label button: `px-3 py-1.5` (12 px / 6 px), `text-sm` (14 px, 20 px line),
  `max-w-52` (208 px), sender name truncated (`font-medium`);
  selected: `bg-accent/15` + `text-text-primary`; unselected: `text-text-muted`.
  aria-label `${senderName}: ${excerpt}: ${replyLabel}`.
- Tab label excerpt (`getTabExcerpt`): `selectedText ?? quotedContent`,
  whitespace-collapsed, first 36 chars + `…` (`senderName: excerpt`,
  or just `senderName` when the excerpt is empty).
- Close button: 32 x 32 (`h-8 w-8`), `Icon name="close" size={14}`,
  `text-text-muted`, hover `bg-bg-elevated`; aria-label `${closeLabel}: ${senderName}`.
- Drop indicator (`WorkspaceReplyDropIndicator`): 2 px x 32 px
  (`w-0.5 h-8`) rounded-full `bg-accent`, inserted at the computed insertion
  index while dragging (`workspace-reply.dnd.ts`: left/right half of a pill or
  the list decides the index from clientX).
- Keyboard: ArrowLeft/ArrowRight/Home/End move the active tab (tablist roving
  tabIndex).
- i18n: `message.replyTo` = "Reply" / "Ответить"
  (`packages/web/src/i18n/locales/en.json:720`, `ru.json:723`);
  `common.close` = "Close" / "Закрыть" (`en.json:1447`, `ru.json:1458`).

## Native plan

All store work lands in `packages/native/src/native-controller.cpp/.h`
(`native-native-controller` CTest group); the strip is QML in
`packages/native/qml/Main.qml` inside `messageComposerShell`
(`native-native-window` group). No CMake group is added.

- Generalize the single slot `m_replyMessage` into
  `QList<ReplyTab> m_replyTabs` + `m_replyActiveTab`, `ReplyTab =
  { id, message, createdAt, answer }`. `replyMessageId/replyAuthor/replyExcerpt`
  become derived from the active tab so every existing single-quote consumer
  (composer chip, Escape handling, route settle, edit exclusivity) keeps working.
- New invokables mirroring the reference lib: `addReplyTab(id)` (append + activate;
  first tab of an empty session keeps the composer draft as its answer),
  `selectReplyTab(id)`, `removeReplyTab(id)`, `reorderReplyTab(id, destination)`,
  `setReplyAnswer(answer)` (active tab). `beginReply(id)` keeps the
  `replyToWorkspaceReply` replace-the-active-tab semantics.
- `QVariantList replyTabs` QProperty (list of `{id, message, author, excerpt,
  label, answer}` maps, NOTIFY replySessionChanged) drives the QML strip.
- Draft lockstep: while tabs exist the composer draft mirrors the active tab's
  answer (`setDraft` writes both; tab switch/remove reloads it). At send time the
  active tab's answer is the current draft with mention serialization applied
  once (existing `mentionSerializeUrnsBeforeReplyQuote` order preserved).
- `send()` builds the wire body from all tabs in order
  (`quote[+blank line+answer]` joined by blank lines), reusing
  `workspace::buildWorkspaceQuoteReference`. Send cleanup closes the session as
  the single-slot code does today.
- QML strip above the reply quote preview, visible when `replyTabs.length > 1`,
  measured as above; close cancels only that tab; strip exposes
  `replyTabStrip` / `replyTab-<id>` / `replyTabClose-<id>` object names.

## Deviations (new-behaviour only)

- Post-send composer text: native keeps its existing draft-clearing cleanup
  (single flow clears the draft after a confirmed send) instead of the
  reference's "restore earliest tab answer" swap. The existing contract
  (`replySessionComposesTheQuoteLineOnAConfirmedSend` asserts an empty draft)
  must keep passing.
- A reply tab whose quoted message cannot be resolved (e.g. transport miss,
  deleted) aborts the send with the existing "This message can no longer be
  replied to." notice instead of the reference silently skipping the block;
  this matches the established native single-slot contract and the rule that
  transport failures are never treated as authoritative misses.
- QML drag-reorder gesture and the drop indicator are not ported in this pass
  (native has no drag surface precedent for composer pills; the replies doc
  already deferred the reference drag). `reorderReplyTab` exposes the full
  reference semantics and is covered at controller level, including the
  acceptance case "reorder changes block order"; the drop-indicator geometry is
  documented above for the follow-up that adds the gesture. Keyboard tab
  navigation follows the roving model through strip clicks only.
- Rendering notes for the strip (not behaviour deviations): the strip collapses
  to `height: 0` instead of toggling `visible`, because a view with `visible:
  false` never materializes its ListView delegates; the horizontal overflow is
  a `ListView.Horizontal` (the reference `overflow-x-auto` equivalent); delegate
  items attach to the visual (item) tree asynchronously after the model change,
  so window tests walk `childItems()` until pills appear; the pill hit-test
  excludes the close button with a 1 px overlap so the geometric centre always
  selects rather than closes.

## Tests

- `native-native-controller`: `multiReplyTabsAnswerEachTabAndSendInTabOrder`
  (append two tabs, answer each, send -> both `[Name](urn:quote:<uuid>)` blocks
  plus answers in tab order; reorder changes block order) and
  `multiReplyTabCloseSelectAndReorderSemantics` (add/select/close/reorder store
  rules, active-tab fallback, draft restore on last close, unknown-id guards).
- `native-native-window`: `replyTabStripRendersRowsAndCloseCancelsOnlyThatTab` -
  strip shows one row per tab when tabs > 1, single tab keeps the plain notice,
  close on one tab removes only that tab and re-points the preview.
