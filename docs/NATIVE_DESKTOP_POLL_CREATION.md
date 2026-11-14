# Poll creation dialog (issue #215)

Ports the Reference create-poll flow onto the native desktop composer. The
oracle is `workspace_ui` at sha `230e02b8b0273dee556a406746d122180a68b3e7`:

- `packages/web/src/features/workspace-message-poll/create-poll-dialog.ui.tsx`
  (dialog body, QuestionEditor/OptionEditor, submit + error mapping)
- `packages/web/src/features/workspace-message-poll/create-poll.lib.ts`
  (`preparePollSend`: per-attempt canonical UUIDs + wire payload)
- `packages/web/src/features/workspace-message-poll/poll-form-controls.ui.tsx`
  (`PollToggle`, `PollDeadline`)
- `packages/web/src/features/workspace-message-poll/create-poll-dialog.ui.test.tsx`
  (behaviour contract: ordered draft, correct-answer marks, retry semantics)
- `packages/web/src/widgets/message-composer/message-composer.ui.tsx:470`
  (`!isEditing && onCreatePoll ? <PollButton/>` visibility gate)
- `packages/web/src/widgets/message-composer/message-composer-toolbar.ui.tsx:151`
  (poll trigger sits after the emoji trigger in the toolbar order)
- `packages/web/src/pages/chat/chat-page-workspace.ui.tsx:1858-1925`
  (`canCreatePoll` gate + `pollAttemptRef` retry identity)
- locales `packages/web/src/i18n/locales/{en,ru}.json` `pollForm.*`

The REST foundation already merged in #214 (`docs/NATIVE_DESKTOP_POLLS_FOUNDATION.md`):
`NativeController::sendPoll`, `supportsMessengerPolls`, `canCreatePoll` and the
`pollActionFailed(messageUuid, action, detail, questionUuid)` signal (`http-<status>` detail;
`questionUuid` is empty for create failures).

## Composer trigger

`NativeIconButton` with `objectName: "pollButton"` in the composer toolbar row of
`Main.qml`, after the emoji trigger (toolbar order file/call -> emoji -> poll).
The reference glyph is vendored verbatim:
`packages/web/src/shared/assets/icons/poll.svg` from the contract sha (added to
the `native_icons` CMake list), matching `Icon name="poll"` sized 24 in
`message-composer.ui.tsx:131-144`. Help/aria text is `pollForm.title`.

Visibility mirrors `message-composer.ui.tsx:471`: hidden while an edit session is
open (`root.controller.editingMessageId.length > 0`) and hidden when
`canCreatePoll` is false. `canCreatePoll` is exposed to QML as
`Q_PROPERTY(bool pollCreationAllowed READ canCreatePoll NOTIFY stateChanged)` so
the binding re-evaluates on selection/stream updates (the `Q_INVOKABLE` from #214
stays untouched). Clicking opens the dialog; the composer text/draft is untouched.

## Dialog geometry (NativePollDialog.qml)

Cloned from `AppDialog` + `create-poll-dialog.ui.tsx` + `app-dialog.ui.tsx`:

- Centered modal Popup (`Overlay.overlay` parent, dim `#80000000`), body
  `max-w-[600px]` -> `width: Math.min(root.width - 48, 600)`, vertical center
  (`top-1/2 -translate-y-1/2`).
- Title `pollForm.title` "Create poll", header/body horizontal padding 24
  (`px-6`), footer `border-t border-border-subtle bg-bg-elevated px-6 py-4`
  right-aligned `gap-2`: Cancel ghost button + Create primary button
  (`rounded-lg bg-accent px-4 py-2 text-on-accent`, disabled opacity 0.5).
- Colours are palette tokens only (`NativeTheme.qml`): `bg-elevated`,
  `text-primary`, `text-secondary`, `text-muted`, `border-subtle`, `accent`,
  `on-accent`, `danger` (blue-cold light: accent `#7087ff`, danger `#d92d20`).
- Scrollable body (`scrollBody`); errors render as `role=alert` danger lines
  above the form (`pollForm.sendFailed` / `validationFailed` /
  `creationUnavailable`).
- `PollToggle` (poll-form-controls.ui.tsx): 30x18 rounded track (`border-subtle`
  off / `accent` on) with a 14x14 white knob inset 2px, shifted +12px when
  checked; label `text-sm` semi-bold `text-primary`, hint `text-xs`
  `text-secondary`; clicking the label toggles the switch.

## Form rules (create-poll-dialog.ui.tsx)

- Opens with one question and one empty answer row (issue #215 acceptance; the
  React test file queries a single answer placeholder before the first input,
  see Deviations).
- Question text max 1000, answer text max 200 (`maxLength` on both inputs).
- Whenever every answer row of a question is non-empty, a fresh empty row is
  appended (auto-append, mirrors `options.push(newOption())`); removing the last
  non-empty row also keeps a trailing empty row.
- Extra question rows only (question count > 1) expose move up/down/remove; the
  answer rows expose move/remove and the "Mark as correct" checkbox only while
  the row is non-empty.
- "Add question" appends a question; "Poll settings" expands the settings block:
  toggles Multiple answers / Anonymous poll / Allow revoting / Allow adding
  options, the Results select (`after_vote` default, `always`), and the closing
  deadline block. All settings are dialog-wide (not per question).
- Create enabled == every question non-empty AND every question has >= 2
  non-empty answers AND (no deadline OR deadline is valid and in the future)
  AND not pending AND not creationUnavailable (submitDisabled line 171).
- Deadline: "Poll closing date" with Set a deadline / Remove deadline action.
  While enabled, an invalid (past/unparseable) value disables Create and shows
  `pollForm.futureDate`.

## Send semantics (chat-page-workspace.ui.tsx:1888-1908, create-poll.lib.ts)

The reference keeps a `pollAttemptRef`: while the submitted draft object is
unchanged the attempt (canonical message uuid + question/option uuids) is
reused; a changed draft rotates it. The native port mirrors this in the
controller (`m_pollAttempts` keyed by conversation key):

- `sendPoll` first validates (existing #214 guards), then serialises a draft
  signature (trimmed question/option texts + settings + closes_at). An existing
  attempt with the same signature is replayed with its stored message uuid and
  the same question/option uuids; otherwise a fresh attempt is generated.
- The POST `/messenger/messages/` body now carries `"uuid"` (the canonical
  client message uuid, same contract as the text-send body), and question rows
  carry `correct_option_uuids` when answers are marked correct
  (`preparePollSend` wire shape).
- A confirmed create clears the stored attempt; the next identical submit is a
  new attempt with new uuids.
- Error mapping in the dialog (submit catch, create-poll-dialog.ui.tsx:131-151):
  `http-409` -> `pollForm.creationUnavailable` and Create stays disabled;
  `http-400`/`http-422` -> `pollForm.validationFailed`, form remains editable
  so the user can correct and re-submit (new attempt uuids); any other failure
  (incl. `invalid-response`) -> `pollForm.sendFailed`, the form locks and
  pressing Create resends the identical attempt (identical uuids).
- Success or Cancel closes the dialog and resets all form state (draft cleared,
  `close(false)`/`reset()` semantics).

## i18n

All new `qsTr` strings land in `translations/workspace_ru.ts` using the
`pollForm.*` RU texts of the contract sha (en.json/ru.json). The locale guard
(`native-native-locale`) requires every extracted key translated with arguments
preserved (`Question %1`).

## Deviations

- Initial answer rows: the React `newQuestion()` at the contract sha opens two
  empty option rows, but its own test file and issue #215 acceptance ("1
  question + 1 empty option row") assume a single empty row before the first
  keystroke. Native opens with one row; the auto-append rule makes the visible
  behaviour converge after one keystroke either way.
- Deadline editor: the reference `PollDeadline` embeds `DateTimePicker`; the
  native preview has no calendar surface (`Main.qml` already states "Calendar is
  not available in this native preview"), so the deadline is a compact date +
  time pair of text fields (`yyyy-MM-dd`, `HH:mm`, local time) producing the
  same ISO `closes_at`. Validation semantics (future-only, blocks Create) match.
- Drag-and-drop answer reordering (HTML5 `draggable`) is represented by the
  reference's own move-up/move-down buttons only; native implements those.
- Reference results select is a custom keyboard Combobox; native uses the
  themed `ComboBox` with the same two options and `after_vote` default.
- Sections and answer rows are instantiated with `Instantiator` (QtQml), not
  `Repeater`: in this Qt 6.10.2 build the `Repeater` delegates inside this
  surface never incubate (the delegate model reports the right count but
  instantiates no items), while `Instantiator` behaves correctly. Behaviour is
  identical; the difference is tracked here so a future Qt bump can revisit it.
- `QQuickPopup` marks its close-event hook as non-overridable in this Qt, so
  the "cannot dismiss an in-flight create" rule is implemented by switching
  `closePolicy` to `NoAutoClose` while a create attempt is pending, which yields
  the same observable behaviour (Escape and outside-click ignored in-flight).
- The reference question caption template `Question {{number}}` is rendered as
  the literal `Question` label plus the 1-based number (RU `Вопрос` + number
  via the catalog), because QML `qsTr` argument substitution is per-call.
