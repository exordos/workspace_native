# Per-conversation reply session persistence

Issue #201: the reply session must survive navigating away from a conversation and
back. Before this change the native controller kept one global reply session
(`m_replyTabs`/`m_replyActiveTab` plus the `m_replyRoute` owner key) and
`settleEditRouteChange()` closed it whenever the route moved to another
conversation, so opening any other chat silently dropped a half-composed reply.

## Measured reference behaviour (workspace_ui 61dacef78dab2f5d0be76f283bebf49fe29b84af)

- The reply session is **composer content, scoped per conversation**.
  `WorkspaceComposerDraftContent`/`WorkspaceEncryptedComposerState` carry
  `replySession` next to `text`
  (`packages/web/src/pages/chat/chat-page-workspace.ui.tsx:260-267`), and the
  page selects the content of exactly one conversation through the draft-store
  scope key `createWorkspaceComposerDraftKey(ownerKey, conversationId)`
  (`chat-page-workspace.ui.tsx:1024-1027`, read-back at `:1056-1066`). The scope
  is `ownerKey + conversationId`; encrypted streams are excluded, matching the
  native e2ee reply gate.
- Every composer update spreads the previous content and therefore keeps
  `replySession` in the **same conversation's** entry
  (`chat-page-workspace.ui.tsx:1080-1100`, `:1218-1242`); the handoff path folds
  a cross-conversation reply intent into the target conversation's content only
  after that conversation's draft has hydrated, scoped by
  `ownerKey + runtimeGeneration + conversationId`
  (`packages/web/src/features/workspace-reply/workspace-reply-handoff.model.ts`,
  behaviour contracts in `workspace-reply-handoff.model.test.ts`).
- Navigation never writes another conversation's entry: leaving A keeps A's
  `replySession` intact, entering B shows B's own (normally empty) content, and
  returning to A re-renders A's stored session unchanged. Cancelling writes the
  empty session into A's content, so A stays cancelled on revisit
  (`packages/web/src/widgets/message-composer/message-composer-draft.hook.ts`
  swaps the composer value per `draftSessionKey`; restore contracts in
  `packages/web/src/features/workspace-reply/workspace-reply-restore.lib.test.ts`).
- Tab answers are per-tab fields of the session (`answer`), and the active tab's
  answer is the composer text: moving between tabs moves the composer value and
  the untouched answer stays inside the tab
  (`workspace-reply.lib.ts:113-127`, `chat-page-workspace.ui.tsx:3085-3097`).

Measured scoping: a reply session belongs to one **conversation**, identified by
the draft scope key (`ownerKey`, `conversationId`). The native analogue of the
conversation identity is `conversationKey()` (`stream/topic`); `ownerKey` scoping
is already structural - the controller (and the new store) is rebuilt on account
switch/reset like every other owner-scoped state.

## Native design

The live session keeps its current shape (`m_replyTabs`, `m_replyActiveTab`,
`m_replyRoute`) so begin/add/select/remove/reorder/cancel and the QML composer
keep working unchanged. Persistence adds a parked-session store:

```cpp
struct StashedReplySession { QList<ReplyTab> tabs; QString activeTab; };
QHash<QString /*conversationKey()*/, StashedReplySession> m_replySessions;
```

- **Stash + restore on route commit.** `settleEditRouteChange()` is the single
  place both route-commit paths (stream switch, topic switch) pass through after
  `m_stream`/`m_topic` are committed. Instead of closing a session whose
  `m_replyRoute` differs from the new `conversationKey()`, the open session is
  parked under its own `m_replyRoute` and the parked entry for the new
  `conversationKey()` - if any - is moved back into the live variables. Park is
  a move (not a copy), so at most one live session and no duplicate parked entry
  can exist for a conversation. Tab identities and the active pointer are kept
  verbatim, mirroring the reference restore contract
  (`workspace-reply-restore.lib.test.ts` keeps tab ids/answers unchanged).
- **Cancel persists.** `closeReplySession()` now also drops the parked entry for
  the session's route, so cancelling in A (and the edit-exclusivity and
  confirmed-send closes, which are current-conversation closes) survives
  revisits. `reset()` clears the whole store: the reference scopes content by
  `ownerKey`, and account switch is the native owner boundary.
- **Display continuity.** `m_messages` is cleared on every route commit and only
  refilled when the destination history commits, so a restored tab's quoted
  message may be unresolvable for a short window. Each `ReplyTab` therefore
  carries `mutable` `authorMemo`/`excerptMemo` fields refreshed whenever live
  resolution succeeds; `replyAuthor()`, `replyExcerpt()` and the `replyTabs()`
  strip rows fall back to the memo only while live resolution is empty. The
  properties a QML binding reads are correct at every instant without extra
  emissions; `replySessionChanged()` fires on stash (session becomes empty for
  the new route) and on restore.
- **Draft lockstep needs no extra work.** The active tab's answer is the
  conversation draft (`setDraft` keeps them in lockstep), and drafts are already
  parked per `conversationKey()`, so returning to A restores composer text and
  tab answer together, as in the reference.
- **Send cleanup is conversation-scoped.** The confirmed-send close compares the
  live session with the pre-send snapshot only when the session still lives in
  that conversation; if the session was parked meanwhile (navigation during the
  in-flight send), the parked entry is compared and removed instead - the
  reference clears exactly the sending conversation's stored content
  (`WorkspaceComposerSendCleanup` carries its own `conversationId`).
- **Edit session unchanged.** The inline-edit route guard still closes the edit
  session on navigation (`docs/NATIVE_DESKTOP_INLINE_EDIT.md`); only the reply
  session's route-close rule is replaced here.

## Deviations

- Cross-conversation reply handoff (start a reply aimed at a conversation that
  is not yet open) remains unported; this issue is about persistence across
  navigation, which the per-conversation store provides. The
  `docs/NATIVE_DESKTOP_REPLIES.md` note that leaving a conversation closes the
  reply session is superseded by this document.
- Parked sessions live in memory only, like `m_drafts` fallback state; the
  durable draft store keeps persisting composer text and is not extended to
  reply tab state in this change.

## Tests

`native-native-controller`:

- begin a reply in conversation A (topic 1), open conversation B (topic 2),
  return to A: `replyMessageId`/`replyAuthor`/`replyExcerpt` and the tab answer
  are restored unchanged, and `replySessionChanged` fired on the return.
- cancel in A and revisit: the session stays cancelled.
- per-conversation isolation: independent sessions in A and B keep their own
  tabs/answers; closing the session in B leaves A's parked session untouched.
- a multi-tab session (order, active pointer, per-tab answers, composer
  lockstep) survives navigation away and back.
