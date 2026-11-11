# Native desktop inline message editing (release 0.8.0)

Issue: #5 (P2-high) - "Inline editing with Source states: up-to-edit-last, draft
preservation while editing, edit dialogs matching Source."

Version: `0.8.0`. The latest published release is `0.7.0`; this is an additive user-facing
feature, so the bump level is MINOR and the version equals the milestone label `0.8.0`.

Reference contract measured from `workspace_ui` at sha
`61dacef78dab2f5d0be76f283bebf49fe29b84af` (the revision pinned in
`NATIVE_DESKTOP_PARITY.md`; the reference tree has not moved since).

## What the reference actually does

The reference app has **no edit dialog**. Editing is a mode of the message composer: the
message body is loaded into the composer input, a notice bar appears above the input, the
send control becomes the save control, and `Escape` leaves the mode. Today's native client
uses a modal `editMessageDialog`, which the Source does not have; this release replaces the
dialog with the composer mode and deletes the dialog.

### Edit notice bar - `packages/web/src/widgets/message-composer/message-composer-preface.ui.tsx:26-46`

Mounted only while an edit session is active and `hideEditNotice` is not set
(`message-composer-preface.ui.tsx:124`). `hideEditNotice` is only set by the Workspace
multi-reply restore path (`preserveWorkspaceReplyContext`), which belongs to the replies
release #6 and has no native counterpart yet.

Container class string comes from `chatBottomNoticeBarClassName({gap: "3", joinedBelow: true})`
(`packages/web/src/shared/lib/chat-bottom-notice-bar.lib.ts`), which resolves to:

- surface `bg-composer-outer`, `flex items-center`, gap `gap-3` = 12 px,
  padding `px-2` = 8 px horizontal and `py-1.5` = 6 px vertical (compact padding),
- `joinedBelow: true` -> `rounded-t-xl` (10 px) and `border-b border-border-subtle` (1 px).

Measured strip: `6 + 20 + 6 = 32` px of content plus the 1 px bottom rule = **33 px**.

- Label: `min-w-0 flex-1 text-sm text-text-primary` - 14 px font, 20 px line box, takes the
  remaining width, text `t("message.edit")` = "Edit message" / "Изменить сообщение".
- Cancel control: `rounded-lg px-3 py-1 text-sm text-text-muted hover:text-text-primary`,
  text `t("common.cancel")` = "Cancel" / "Отменить" -> 4 px + 20 px + 4 px = 28 px high,
  12 px horizontal padding, 8 px radius.
- The strip is `role="status" aria-live="polite"`.

### Composer input in edit mode - `packages/web/src/widgets/message-composer/message-composer-draft.hook.ts:33-64`

- Starting a session snapshots the current composer text and then loads
  `editSession.initialMarkdown` into the input. Ending the session (cancel or a confirmed
  save) restores the snapshot.
- While a session is active `setValue` does **not** call `onValueChange`
  (`message-composer-draft.hook.ts:93-103`), which is the draft-persistence callback. The
  chat draft store is therefore never written with edit text, and the pre-edit draft is what
  the user finds again after cancelling. This is the exact meaning of "draft preservation
  while editing" in the issue.
- `initialMarkdown` is the message body (`ComposerEditSession` -
  `packages/web/src/widgets/message-composer/message-composer.types.ts:46-53`).

### Save control - `packages/web/src/widgets/message-composer/message-composer.ui.tsx:2177-2192`

The same 40x40 `rounded-xl bg-composer-send` control, with two changes while editing:

- `disabled` additionally when `isEditing && value.trim().length === 0 && no attachments`;
- `aria-label` becomes `t("common.save")` ("Save" / "Сохранить") instead of the send label.

Keyboard submit follows the same rule: Enter submits the edit instead of sending a message.

### Leaving the mode - `packages/web/src/widgets/message-composer/message-composer-write-body-keydown.lib.ts:121-126`

`Escape` while `isEditing` cancels the edit (and is swallowed: `preventDefault` +
`stopPropagation`). Local editor keys run before application shortcuts, and the mention
dropdown keeps priority over the edit cancel (same function, earlier branch).

### Up-to-edit-last - `message-composer-write-body.ui.tsx:36-39`, `shared/lib/hotkeys/hotkeys.lib.ts:29,186`, `pages/chat/chat-page-workspace.ui.tsx:3557-3571`

- Hotkey id `composer.editLastMessage`, default binding `ArrowUp`, and the matcher requires
  that Shift is not part of the chord (`hotkeys.lib.ts:186`).
- Enabled only when the composer is enabled, no suggestion dropdown is open, no edit session
  is active and the composer text is empty or whitespace only.
- The handler walks the visible messages from the newest to the oldest and starts an edit of
  the first own message that can be edited (markdown body; a forward snapshot is excluded;
  an e2ee body only in an encrypted stream). When nothing matches, the chat page shows the
  `message.editUnavailable` error instead of starting a session.

### Starting an edit from the message menu - `workspace-message-list/workspace-message-bubble-menu.ui.tsx:396-407`

Own-messages section of the context menu: icon `pen`, label `t("message.edit")`. Only shown
for own messages. Starting an edit for a message that is not own, not markdown, in a
different project/stream/topic, or in a stream whose encryption state does not match the
payload kind, is refused with `message.editUnavailable`
(`chat-page-workspace.ui.tsx:2440-2462`).

### Server action - `entities/messenger/messenger-message-actions.lib.ts:468-600`, `shared/lib/roles.ts:160-163`

`PUT /messages/{uuid}` with `{"payload": {"kind": "markdown", "content": ...}}`. The action
requires the original message to be loaded, own, in the current project, stream and topic,
and refuses a markdown edit of a message in an encrypted stream ("Encrypted messages cannot
be edited as Markdown"). `canEditMessage` grants own-message editing from the Guest role and
other people's messages only to Admin; the native client only ever edits own messages, as
today. A failed save keeps the edit session open and shows `message.editFailed`
("Failed to save edit" / "Не удалось сохранить правку").

### i18n keys used by the flow

| key | en | ru |
| --- | --- | --- |
| `message.edit` | Edit message | Изменить сообщение |
| `common.cancel` | Cancel | Отменить |
| `common.save` | Save | Сохранить |
| `message.editUnavailable` | This message can no longer be edited | Это сообщение уже нельзя редактировать |
| `message.editFailed` | Failed to save edit | Не удалось сохранить правку |

`message.editSaving`, `message.retryEdit`, `message.cancelEdit`,
`message.editLoadingMarkdown` and `message.editPlaceholder` exist in the reference catalog
but have no consumer anywhere in `packages/` at the measured sha; they are not ported.

## Native design

### Controller (`packages/native/src/native-controller.{h,cpp}`)

New state, one active session per conversation:

- `QString m_editingMessage` (message uuid, empty when no session) plus
  `Q_PROPERTY QString editingMessageId` and signal `editSessionChanged()`.
- `QString m_editBuffer` - the composer text while a session is active. `draft()` returns it,
  `setDraft()` writes to it, and neither touches the durable draft store or `m_drafts`, so
  the stored draft of the conversation is preserved without a copy (restoring it after
  cancel is simply dropping the buffer). This reproduces the reference rule measured above
  while keeping the native single-`draft()` surface that QML and the draft tests use.
- `Q_INVOKABLE bool beginEditMessage(const QString &id)` - validates: authoritative route,
  message found, own (`own` or author equals the session user), `kind == "markdown"`,
  message in the selected stream and topic, stream not encrypted. A rejected start reports
  "This message can no longer be edited" and starts nothing, as in the reference. Starting a
  session while one is open replaces the buffer (the stored draft is still untouched).
- `Q_INVOKABLE void cancelEditMessage()` - drops the session and the buffer.
- `Q_INVOKABLE void submitEditMessage()` - submits the buffer through the existing
  `editMessage()` PUT path (reusing its validation, `verifyMutation` confirmation of the
  server body, cache projection and stale-route guards). The session closes only when the
  server accepted the edit for the still-selected conversation ("confirmed server result");
  on failure the session and the typed text stay open with the existing error report.
- `Q_INVOKABLE void editLastMessage()` - scans `m_messages` from the newest backwards and
  edits the first own markdown message of a non-encrypted stream; reports
  "This message can no longer be edited" when none qualifies.
- `Q_PROPERTY bool canSaveEdit` - a session is open, the buffer is not blank, the buffer is
  within the 40000-character limit, the route is authoritative and the stream is not
  encrypted.
- The session ends when the conversation, project or account changes (`selectStream`,
  `selectTopic`, sign-out), mirroring the reference clearing
  `composerEditSession` on route changes. `send()` refuses to send while a session is open.

### QML (`packages/native/qml/Main.qml`)

- Inside `messageComposerShell`, an `editMessageNotice` strip (objectName
  `editMessageNotice`, `Accessible` role notification) rendered only while
  `controller.editingMessageId` is not empty: height 33 px (32 + 1 px bottom rule in
  `border-subtle`), `composer-outer` background, 8 px horizontal inset, 12 px gap, label
  `editMessageNoticeText` "Edit message" (14 px / 20 px, `text-primary`) and a `Cancel`
  control `cancelEditButton` in `text-muted`. The shell's preferred height grows by the
  strip height and the existing input row moves down by the same amount, so the
  non-editing geometry of `messageComposerShell` is bit-identical to 0.7.0.
- The message menu action `editMessageAction` now calls
  `controller.beginEditMessage(menuMessageId)`; its visibility and enablement rules are
  unchanged (`menuMessageOwn && menuMessageOrdinaryReadable`, which already excludes
  encrypted streams and non-markdown kinds, matching the reference contract).
- `editDialog`, `editMessageDialog`, `editMessageInput` and `saveMessageEditButton` are
  deleted, including their terms in `readActive` and `composerText.formattingAllowed` and
  the `editDialog.close()` route guard.
- Composer keys (on the composer input `composerText`): `Escape` closes the emoji picker
  first (unchanged), otherwise cancels an open edit session; `Enter` submits an open edit
  session when `canSaveEdit`; `ArrowUp` without Shift on a blank composer with no session
  open calls `editLastMessage()`.
- The single send control keeps the objectName `sendButton` and gains objectName
  `saveEditButton` while a session is open, with `help` "Save" and enablement from
  `canSaveEdit`.

Geometry parity note: the label uses 14 px / 20 px as measured. The reference strip is
`text-sm` (14 px) while the native composer input is 16 px; the notice is not the input, so
the measured 14 px applies. The window test asserts 33 px strip height, the 8/12 px insets
and the palette hex of `text-primary`/`text-muted` in `blue-cold`.

### Deviations from Source (named and justified)

1. No encrypted-stream editing. Native E2EE read exists, but the native client has no
   encrypted send/edit path (`editMessage` already rejects encrypted streams, and the
   reference refuses "Encrypted messages cannot be edited as Markdown"). The Edit menu
   entry stays hidden for encrypted conversations, and `editLastMessage` skips e2ee bodies.
2. No "edited" marker on saved messages. The reference catalog carries no label for one (no
   `message.edited` key exists at the measured sha), so there is nothing to port.
3. No attachment or reply-context preservation inside an edit session. Composer attachments
   and the restored-reply tab strip are separate features (#6 and the attachments slice);
   the native composer has no per-message attachment set to carry.

## Tests

- `native-native-controller`: session start keeps the stored draft and shows the message
  body, blank-guarded save, typing in a session does not touch the durable draft store,
  cancel restores the stored draft, a confirmed save closes the session and keeps the stored
  draft, a failed save keeps the session and the typed text, a route change clears the
  session, `editLastMessage` picks the newest own markdown message and reports when none.
- `native-native-controller` (#115 save races): text typed after Save survives the deferred
  completion of the earlier save and stays savable; cancel-plus-reopen of the same UUID while
  a save is in flight leaves the reopened session untouched; a second save submitted while
  the first PUT is pending is refused without issuing a second PUT; both transitions of the
  in-flight guard emit `editSessionChanged`. All four use the mock
  server's `defer` gate, never sleeps.
- `native-native-window`: menu Edit opens the notice (33 px), prefills the composer, the
  save control replaces send and is disabled on a blank buffer, Escape restores the previous
  draft, ArrowUp on a blank composer edits the last own message, Enter submits the edit.

## Save race contract (issue #115, patch 0.9.1)

Reported defect: at 0.8.0/0.9.0 a confirmed save closed "the edit session for message M"
by UUID alone, so text entered after the Save press - or typed in a session reopened for
the same M while the earlier PUT was still in flight - was silently discarded when the
earlier completion arrived.

Contract (this release):

1. **Session identity.** Every `beginEditMessage` assigns a fresh `m_editSession` id, and
   every buffer replacement (session open or `setDraft` inside the session) bumps
   `m_editRevision`.
2. **A completion closes only what it saved.** `submitEditMessage` captures
   `(m_editingMessage, m_editSession, m_editRevision)` at submit time; the confirmation
   closes the session only when all three still match. A newer buffer revision or a newer
   session - including a cancel-plus-reopen of the same UUID - makes the completion inert
   for the session, while the verified server body still updates the message row
   (`verifyMutation` is unchanged in that respect).
3. **Concurrent saves are prevented, not ordered.** While an edit PUT for message M is in
   flight (`m_editSaveInFlight`), `canSaveEdit()` is false and `submitMessageEdit()` refuses
   a second PUT for M. Both transitions of the guard emit `editSessionChanged` (the NOTIFY
   signal of `canSaveEdit`) so bound save controls re-evaluate immediately. The composer
   stays editable throughout, so typing continues; the save
   control re-enables when the pending save completes (confirmed, failed or unverified),
   after which the newer text can be saved. This keeps at most one PUT per message
   in flight, so two saves of one message can never land at the server out of order.
4. **The ordinary draft stays untouched.** None of the race paths write the conversation
   draft store; the buffer change is session-local, as in the original contract.
