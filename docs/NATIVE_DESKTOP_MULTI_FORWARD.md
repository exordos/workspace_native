# Multi-forward without duplicates (issue #7)

Reference: `exordos/workspace_ui` @ `75bd21f37de94728e2ebe100acfea24ac9c1a4e1`,
`packages/web/src/features/workspace-forward-message/` and
`packages/web/src/pages/message-redirect/`. The issue has no Reference contract
block; every reference fact below was measured from that sha and is cited by
file. The redirect page is only a login-anchor redirect and takes no part in
forwarding.

## Reference semantics (measured)

Source snapshot-forward model (`workspace-forward-message.lib.ts`,
`workspace-forward-message.ui.tsx`):

- The store keeps **UUIDs only** (`model.ts` `open()`); full bodies are
  fetched from the API with a **GET-only** call
  `getMessagesByUuids` -> `GET /messages/` with `project_id` + repeated `uuid`
  params, chunked (`shared/api/messenger-client.ts:179-199`, `ui.tsx:350-392`).
- **Duplicate suppression per batch** (`lib.ts uniqueForwardMessageUuids`
  `22-32`): each UUID is trimmed, empty entries dropped, and repeats are
  collapsed keeping first-occurrence order; applied on `open()` (`model.ts:27`)
  and again on resolve (`lib.ts resolveWorkspaceForwardMessages` `53-64`,
  which also drops UUIDs whose body never resolves). So one source message can
  appear at most once in the forwarded snapshot of one target conversation.
- One batch becomes **one message**: per source message a forward reference
  block `[Author](urn:forward:<uuid>?snapshot=...)` is built
  (`buildWorkspaceForwardMarkdown` `lib.ts:66-125`) and the blocks are joined
  with a blank line (`"\n\n"`, trailing whitespace trimmed). Only
  `markdown`/`poll` payload kinds build a snapshot; anything else yields an
  error. Single-message text selection forwards as a plain snapshot unless the
  body carries secrets. A `composer.messageTooLong` limit check runs before
  send (`ui.tsx:464-466`).
- Target = one conversation: channel tab picks stream + topic; direct tab
  picks a user and resolves to that peer's private stream default topic
  (creating it if needed, `lib.ts resolveWorkspaceForwardTarget` `191-221`).
- Submission is a single message POST with a client-generated uuid; close is
  blocked while submitting (`model.ts:46-54`); errors render in an alert and
  the modal stays open.

### Measured UI tokens (`workspace-forward-message.ui.tsx`, Tailwind scale, 14px root)

| Element | Geometry / palette token |
| --- | --- |
| Dialog | `max-w-md` = 448px wide, `max-h-[80vh]`, vertically centered; direct tab stretches `h-[80vh]` (lines 49-51) |
| Header | `px-4 py-3` (16/12px), `border-b border-border-subtle`; title `text-sm font-semibold text-text-primary`; close icon 18px, `aria-label=common.close` (547-563) |
| Tabs | two flex-1 buttons `px-4 py-2 text-sm font-medium`; active `border-b-2 border-accent text-accent`; inactive `text-text-muted hover:text-text-primary` (153-178) |
| Body | `p-4` 16px, `gap-3` 12px (182-185) |
| Labels | `text-sm text-text-muted` (188, 205) |
| Direct search | `rounded-lg border border-border-subtle bg-bg px-3 py-2 text-sm`, placeholder `message.searchUsers` (222-229) |
| User list | `rounded-lg border border-border-subtle`, rows `px-3 py-2 text-sm`, selected `bg-accent/20`, check icon 14px `text-accent`, email sub-row `text-[11px] text-text-secondary` (230-285) |
| Error | `role=alert bg-danger/10 rounded-lg px-3 py-2 text-sm text-danger` (289-293, 566) |
| Footer | `mt-auto flex justify-end gap-2 pt-2`; cancel `px-3 py-1.5 text-sm text-text-muted`; primary `px-3 py-1.5` (294-320) |

Labels: title `message.forwardToChannel`, tabs `message.channel` /
`message.directMessage`, selects `channel.selectChannel` / `chat.selectTopic`,
submit `message.forwardTo`, cancel `common.cancel`, close `common.close`,
user search `message.searchUsers`, empty search `search.noResults`, failure
`message.forwardError`.

## Native design

### Slice 1 (this PR): controller multi-forward API, GET-only check, dedupe

The native client already has the canonical GET-only confirmation pattern in
the ordinary send path: `postOrdinarySend` POSTs once with a client uuid, the
result is **confirmed by a GET read-back** (`readOrdinarySend`
`GET /messages/{publicId}`), and `checkPendingSend()` re-runs only that GET -
"Check does not send it again". Forward reuses exactly that pattern:

- `forwardMessages(messageUuids, streamUuid, topicUuid)` - entry point.
  Guards: authenticated route, valid target UUIDs, non-empty batch after
  dedupe, and no busy/unconfirmed forward (one batch at a time).
- Duplicate suppression mirrors `uniqueForwardMessageUuids`: trim, drop empty,
  collapse repeats keeping first order. The deduped set is also fetched once
  and resolved through a set, so one source can never appear twice in the
  snapshot.
- Source bodies: `GET /messages/` with `project_id` + repeated `uuid` query
  items (same contract as `getMessagesByUuids`).
- Snapshot markdown: per resolved source, `workspace::buildWorkspaceForwardReference`
  (already ported in `workspace-reference.cpp`) with author display name,
  source label (`Stream` or `Stream · Topic`, matching the reference
  `resolveSourceLabel`), and ISO `created_at`; blocks joined with `"\n\n"` and
  trimmed. Unresolvable sources are dropped; an all-empty batch errors without
  any POST.
- Post: one `POST /messages/` with client uuid + target stream/topic +
  markdown payload. A clean 4xx (except 404/408/409/429) is a definite
  failure; anything else that is not a verified success is **unconfirmed** -
  the batch keeps its identity and `checkForwardStatus()` confirms it with a
  GET alone, never a second POST.
- Confirm: the GET read-back must echo uuid/target/content/author/own like
  `readOrdinarySend` does; on success the message is observed into the model
  when the target conversation is currently open.

Deferred, recorded here and on the issue:

- Poll payload sources and secret-content single-selection plain-snapshot
  override are not forwarded yet (markdown sources only).
- The direct-target tab (resolve-or-create private stream) is not ported; the
  controller takes an explicit stream+topic target, so the picker supplies a
  resolved conversation.
- The length limit check (`composer.messageTooLong`) rides on the send route
  later; no native limit constant is duplicated here.
- Cache projection of the confirmed forward is deferred with the slice-2 send
  route alignment (keep the edit surface away from lane #38).

The dialog blocks dismissal while it sends or checks. Once a reply is
ambiguous, Escape, an outside click, Close, or Cancel returns to the
conversation with its selection intact. Reopening the dialog offers Check
for the same batch. A token renewal for the same account and project keeps
that read-back available, including when renewal interrupts the POST.
Renewal during source loading releases the batch for a fresh attempt.
Changing the session discards the previous batch and its snapshot.

### Review round 1 (PR #357, exordy cids 4586012787/4586012796/4586012826/4586012837/4586012848)

Measured reference behavior at `workspace_ui`
`75bd21f37de94728e2ebe100acfea24ac9c1a4e1` (all lines verified at that sha):

- **Encrypted destinations (cid 4586012787).** The reference forward picker
  itself does not filter encrypted streams out of the channel list -
  `buildWorkspaceForwardStreamOptions` (`workspace-forward-message.lib.ts:127-141`)
  drops only archived streams and private direct streams (the direct tab owns
  those). The refusal lives in the submit path: the dialog submits through
  `sendMessengerMessage`, which throws
  `"Encrypted chats require an available crypto provider"` when
  `stream.encryption === true` (`entities/messenger/messenger-message-actions.lib.ts:309-311`)
  before any request leaves the client. So in the reference a plaintext
  snapshot can never reach an encrypted conversation. The native forward route
  has no crypto submit path (it always POSTs markdown plaintext), so native
  mirrors the guarantee at both layers instead: the picker omits encrypted
  conversations from the channel select
  (`NativeForwardDialog.qml` `streamRows` filters `stream.encrypted`) and the
  controller refuses an encrypted destination in `forwardMessages` without
  creating an intent, fetching sources, or posting
  ("Forwarding to encrypted chats is not supported.").
- **Mixed selections (cid 4586012837).** The reference keeps every server
  message selectable - the selection control renders for any server row
  (`widgets/workspace-message-list/workspace-message-list.ui.tsx:503`,
  `rendersSelectionControl = selectionMode && serverMessage != null`) - and
  rejects the whole batch at submit with the visible `message.forwardError`
  alert when any source payload kind is not `markdown`/`poll`
  (`workspace-forward-message.lib.ts:78-84` returns null;
  `workspace-forward-message.ui.tsx:449-451` turns that into the error and
  keeps the modal open). Native mirrors that UX: checkboxes stay on every row,
  and `fetchForwardSources` fails the batch with "Only text messages can be
  forwarded." (the native markdown-only subset) without posting a truncated
  snapshot.
- **Pending batch lock (cid 4586012796).** `canForwardMessages()` now requires
  `!busy && !pending` - an unconfirmed batch owns its identity until Check
  settles it; a second begin can no longer strand the first ambiguous POST.
- **Topic cache freshness (cid 4586012826).** `fetchForwardTopics(stream, refresh)`
  gained a refresh flag; the picker passes `refresh=true` on every stream
  activation, so topics created mid-session appear when the picker reopens
  (the per-stream cache only short-cuts passive reads and concurrent fetches).
- **Stale confirm vs newer selection (cid 4586012848).** The dialog snapshots
  the selection at submit time (`submittedIds`) and emits
  `forwardSucceeded(submittedIds)`; `Main.qml` clears the chat selection only
  when it still equals the submitted batch, so confirming an old batch can no
  longer discard a newer selection.

### Slice 2 (next): UI

Message multi-select entry + `NativeForwardDialog.qml` (channel picker tab
first) reproducing the measured geometry table above, wired from the feed page
menu. Blocked in this lane because `NativeFeedPage.qml` is owned by another
lane; live verification of a forward through the app therefore lands with
slice 2.
