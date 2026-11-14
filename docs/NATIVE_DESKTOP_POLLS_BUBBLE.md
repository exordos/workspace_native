# Poll bubble: reference card with immediate-click voting

Issue #216 (parent Epic #213). Builds on the merged polls foundation
(`docs/NATIVE_DESKTOP_POLLS_FOUNDATION.md`, #214): domain parse/model, the
controller REST actions (`sendPoll`, `votePollOption`, `closePoll`,
`updatePollSettings`, `addPollOption`, `loadPollVoters`) and the realtime
revision-guard merge. This issue ships the presentation: the poll card rendered
inside the message bubble, immediate-click voting, results, correct marks and
the inline add-option input. The composer creation surface is NOT part of this
issue (active lane #215 owns it).

The contract of record is the reference app at
`workspace_ui 230e02b8b0273dee556a406746d122180a68b3e7`:
`packages/web/src/features/workspace-message-poll/workspace-message-poll.ui.tsx`,
`workspace-message-poll-question.ui.tsx`,
`workspace-message-poll-connected.ui.tsx`, `workspace-message-poll.css`,
`workspace-poll-deadline.hook.ts`,
`packages/web/src/entities/messenger/messenger-poll-actions.lib.ts` and
`packages/web/src/widgets/workspace-message-list/workspace-poll-message-bubble.ui.tsx`
+ `workspace-message-bubble-presentation.ui.tsx:568-571`. This is parity work:
native mirrors those measurements and semantics; every deviation is named at
the bottom of this document.

## Bubble chrome (workspace-message-bubble-presentation.ui.tsx:568-571, workspace-message-poll.css:152-156)

| Property | Reference (file:line) | Native |
| --- | --- | --- |
| Width | `w-[360px] max-w-[88%]` (`…presentation.ui.tsx:570`) | fixed 360 px, capped by the same 88 % column bound |
| Padding | `px-3 py-2` = 12 x / 8 y (`:570`) | bubble keeps its 12/8 content margins |
| Corner | `rounded-lg` = 8 px (`:570`) | 8 px radius for poll bubbles (ordinary bubbles keep the 18/10/6 chat radii) |
| Background | `bg-bg-elevated`, `bg-card-bg-active` when the message is selected (`:568`) | `bg-elevated`; native has no multi-select state, so the selected tone never applies (deviation D4) |
| Author/topic labels | separated by a middle dot, `text-accent` | kept from the ordinary native bubble header (the reference poll bubble renders the same header row) |
| Message time | moves inline bottom-right; final line reserves `padding-right: 48px; min-height: 16px` (`workspace-message-poll.css:152-156`) | the existing `message-time` item anchors to the bubble's bottom-right corner and the card's last row reserves 48 px |
| Aggregate wrap | container < 300 px: aggregate wraps to its own line with 22 px indent (`…poll.css:134-143`) | poll bubbles are fixed 360 px; the wrap branch is unreachable in the native bubble (deviation D5) |

## Card geometry (workspace-message-poll.css)

| Property | Reference (file:line) | Native |
| --- | --- | --- |
| Container | 336 px (`:9-17`) = 360 − 2×12 padding | 336 px content column |
| Base font | Montserrat regular 12 px / 15 px line (`:12-14`, bundled OFL TTF) | app default font (deviation D1) |
| Question title | Helvetica Neue 14 px / 17 px, weight 500 (`:19-29`) | 14 px, weight 500, wrap anywhere |
| Question gap | `--poll-gap: 6px` (`:15`) | 6 px spacing |
| Option padding | 6 px vertical (`:16,44-47`) | 6 px vertical |
| Option row | gap 8 px, min-height 28 px, 14 px / 17 px (`:56-74`) | Row, min height 28 px |
| Option gap between options | 6 px column gap (`:37-42`) | 6 px |
| Result bar | height 2 px, `margin-top: 6px` (`:106-110`) | 2 px bar under the row |
| Checkbox marker | 16 px circle, 1 px accent border, filled accent + white check when checked, green tone for correct, 0.4 opacity when disabled (`shared/ui/checkbox/checkbox.css:1-35`, `checkbox.ui.tsx:9` default size 16) | same, drawn with Rectangle + canvas-free Text check |
| Colors (blue-cold) | accent `#7087ff`, indicator-green `#26c038`, text-secondary `#999999` dark / `#787878` light, danger `#d92d20` (`shared/lib/themes/blue-cold.ts:16-24,64,79-88,127`) | native theme keys `accent`, `indicator-green`, `text-secondary`, `danger` (palette mirrors per `NativeTheme.qml`) |
| compact-field-bg | `#303139` dark / `#e4edf5` light (`blue-cold.ts:13,77`) | introduced as a native theme key `compact-field-bg` (used by the add-option field, `create-poll-dialog.ui.tsx:252` is #215's surface) |

## Card structure (workspace-message-poll.ui.tsx)

- Subtitle on the first question only (`:34-44,91`): `Public poll` / `Anonymous
  poll` + `Choose one option` / `Choose multiple options` +
  `Questions: N` (only N > 1), joined by ` · `. When closed: just
  `Poll closed`.
- Closed state (`:24-25,45-49`): `closed = poll.closed || deadline reached`;
  the status line shows `Closed {date}` (closedAt else closesAt when expired),
  open polls show `Until {d MMMM, HH:mm}`.
- Per-question results (`workspace-message-poll-question.ui.tsx:265`):
  results show when `resultsVisibility == "always"` OR that question was
  answered OR the poll is closed.
- Bubble summary (`workspace-message-poll.ui.tsx:27-30,56-80,109-120`):
  visible when `always`, closed, or ALL questions answered;
  `Results after voting` while hidden; `N participant(s)` when visible;
  ` · You voted` suffix when a question with correct answers was answered
  (quiz); a `View votes` link when visible + open + not quiz (deviation D3).
- Correct-answer line (`workspace-message-poll-question.ui.tsx:292-306`,
  `workspace-message-poll.css:121-125`): only when the question has correct
  options; before the own answer the line stays EMPTY but reserves 15 px
  (soft rule: the marks are hidden until voting even though the API delivers
  them); after answering: `✓ Correct answer: X, Y`, color indicator-green.
- Status region (`:121-141`): date, `Read only`, card-level error line.

## Option rows (workspace-message-poll-question.ui.tsx:138-245)

- `aria-pressed = selected` (`:182`); circle indicator filled when
  `selected || correct` with the green tone for correct (`:199-202`).
- Selected text tone accent; correct text tone indicator-green (`:49-54`); a
  selected but wrong option loses the accent tone (`:154-165`).
- Right aggregate `P% · votes` when results visible (`:204-212`).
- Percent: `round(option.votes / question.voterCount * 100)`, 0 when
  `voterCount == 0` (`:160-161`); multiple mode can exceed 100 in sum.
- Result bar width `clamp(percent, 0, 100)` % of the option column
  (`:235-242`), accent when `correct || (selected && !wrong)` else
  text-secondary.
- Voter avatars to the right of the row (`:14,214-233`): deviation D3.

## Voting semantics (messenger-poll-actions.lib.ts:85-134, workspace-message-poll-connected.ui.tsx:76-140)

- Every click immediately POSTs `vote {question_uuid, option_uuid, selected}`
  with `selected = !selected` (desired state, never a local guess) — no submit
  button, no batching (`connected.ui:132-140`).
- Per-question pending: while a vote for a question is in flight, that
  question's option buttons are disabled (`question.ui:190-195` `busy`);
  a second click on the same question cannot reach the network
  (`connected.ui:84,88-90`). Other questions stay clickable.
- Per-question inline error on failure (`connected.ui:104-108`, text
  `pollBubble.actionError` = "Could not save the change. Please try again.");
  the card stays open, the buttons re-enable, and the server stays truth.
- The successful response's fresh `poll_state` replaces local state through
  the revision guard (#214 `applyPollMutation`).
- Selectability `canSelectPollOption` (`question.ui:32-40`): open questions
  are always clickable; with `allow_revote=false` single mode locks the whole
  question after the first answer; multi mode may add unselected options but
  never remove selected ones; closed/expired/read-only disables everything.
- Race with the deadline: the server answers 409; the inline error path is the
  only visible consequence.

## Deadline (workspace-poll-deadline.hook.ts:3-26)

Reference arms a `setTimeout` to `closes_at` (capped at 2^31-1 ms) and
re-checks on focus/visibility; the native card arms a single-shot timer for
the same (capped) remaining time and re-evaluates whenever the row snapshot
changes. Server remains truth.

## Inline add option (workspace-message-poll-question.ui.tsx:42-136,329-340, workspace-message-poll-connected.ui.tsx:141-152)

- Rendered when `allowAddingOptions && open && options.length < 20 && canAct`.
- Single-line input, `maxLength 200`, checkbox-style leading marker
  (secondary border, accent on focus, `poll.css:76-86`), placeholder
  `Add an answer option…`, screen-reader hint `Press Enter to add an answer…`.
- Plain Enter submits; Shift/Alt/Ctrl/Meta+Enter and IME composition
  (keyCode 229) do not (`question.ui:104-119`).
- Stable per-question retry uuid: a failed submit keeps `{uuid, text}` so a
  retry replays the same idempotent request; cleared on success
  (`connected.ui:143-150`).
- Failure shows red `Could not add the option. Try again.`; the input clears
  only on success.

## Missing poll_state / read-only

- The reference renders `Could not load the poll` when a poll message has no
  `poll_state` (`connected.ui:74`). The native DTO guard (#214) rejects such a
  row at parse time, so the native wire path cannot produce that state; the QML
  keeps the defensive branch (text `Could not load the poll`) for robustness.
- Read-only (encrypted/archived/non-native streams; `supportsMessengerPolls`):
  no vote controls, no add-option input, voter slots hidden, `Read only` line.
  Native derives it from `controller.canCreatePoll()` (current stream).

## Accessibility

Role group named by anonymity; regions `Vote counts` / `Poll status` /
`Answer status`; `aria-pressed`-style checked state; the native card exposes
`Accessible.role`/`name` on the same rows (Qt has no focus ring for divs;
keyboard focus keeps the native Button visuals).

## i18n

All user-visible strings go through `qsTr` and into
`packages/native/translations/workspace_ru.ts` (locale guard
`native-native-locale`). EN texts mirror the reference `pollBubble.*` values
(en.json/ru.json at the pinned SHA): Anonymous poll, Public poll,
Questions: %1, Choose one option, Choose multiple options, Poll closed,
Until %1, Closed %1, Read only, Results after voting, %n participant(s),
You voted, Could not save the change. Please try again., Saving answer…,
Correct answer: %1, Add an answer option…, Press Enter to add an answer…,
Could not add the option. Try again., Could not load the poll.

## Native structure

- `packages/native/qml/NativePollCard.qml` — the card (questions, options,
  summary, status, add-option input). Embedded from the `MessageContentRow`
  bubble in `packages/native/qml/Main.qml` when `modelData.kind == "poll"`.
  Receives the `modelData.poll` snapshot (#214 `pollRowSnapshot`), the
  anonymity/read-only flags, and forwards voting/add-option intents to the
  controller signals (`votePollOption`, `addPollOption`).
- In-flight and error state live in the card: the card keys pending votes by
  question and releases a lock when its own request completes -
  `controller.pollActionSucceeded(messageUuid, "vote"|"add_poll_option",
  questionUuid)` on success (round 3), `controller.pollActionFailed` on
  failure. A poll `state.revision` change from an unrelated update retires
  only stale error text, never an in-flight lock. This mirrors the reference
  per-question pending set without adding state to the controller. The
  add-option draft and its client option uuid live on the card keyed by
  question (not on the delegate), so re-incubations and unrelated revisions
  keep the unsubmitted text and the retry-replay uuid.
- Date label: `NativeController::formatPollDate(QString isoDateTime)` formats
  with the app locale (`QLocale()`, set by `applyLanguage`): en
  `12 December, 09:00`, ru `12 декабря, 09:00`.

## Deviations from the reference (each named per AGENTS.md)

- D1 Font: native substitutes its standard UI font for the bundled Montserrat
  12/15 (the TTF stays upstream-only; the row/column geometry above is kept).
  The reference itself declares `"Helvetica Neue", Helvetica, Arial, sans-serif`
  fallbacks for the question titles.
- D2 `View votes` link, voter avatar previews and the voters dialog are not
  shipped here; they belong with the voters-dialog surface (controller
  `loadPollVoters` already exists from #214). The summary still shows
  `N participants` and the per-question `Participants: N` line.
- D3 Voter slots (empty avatar column, `poll.css:93-104`) are omitted for the
  same reason as D2; the option row spans the full card width.
- D4 Selected-message background (`bg-card-bg-active`) has no native analogue
  (no message multi-select surface), so the poll bubble always uses
  `bg-elevated`.
- D5 The <300 px aggregate-wrap rule (`poll.css:134-143`) is unreachable with
  the fixed 360 px bubble; native does not implement it.
- D6 Date order: the reference renders JS `toLocaleDateString` locale defaults
  (`December 12` in en-US); native uses Qt `d MMMM, HH:mm` for every locale
  (ru output matches exactly, en day order differs). Font-derived tabular-nums
  is likewise not portable in Qt (already recorded in the app docs).
- D7 Screen-reader-only `Saving answer…` texts exist as `Accessible.name` on
  the disabled question (Qt has no `sr-only`); same for the region labels.
- D8 The reference `participantCount` plural uses ICU plural categories; the
  native catalog ships the three Qt numerus forms (one/few/many) like every
  other plural entry in `workspace_ru.ts` (the `%n event` pattern).
- D9 The #214 failure signal identifies the message, not the question, so a
  failed vote in native clears the in-flight flags of ALL pending questions of
  that message and marks them with the inline error. The reference isolates
  the error to the originating question; concurrent in-flight votes on two
  questions of one message are the only case where this differs visibly.
- D10 Success completion is per-request: `pollActionSucceeded` carries the
  request's question uuid (vote/add bodies identify it), so a lock releases
  only on its own response and an unrelated poll revision can never release
  an in-flight lock (round-3 review 4571525925).
- D11 `Qt.createUuid` is unavailable in shipped QML scopes, and retries must
  replay one client option uuid, so the card generates a v4-shaped uuid with
  `Math.random` and keeps it on the attempt (idempotent uuid+text replay);
  the controller still generates one when QML passes none.
