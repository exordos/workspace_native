# Topic context menu actions

Issue #636: notification-level switch, mark as read, rename dialog and
done/not-done toggle on topic rows (sidebar topic list and the channel
information drawer topic list).

Figma token unavailable - mockups not consulted (token expired at fetch time,
HTTP 403 from the Figma API). This issue is parity-preserving against the
reference contract below, so no restyle decisions are introduced.

## Reference contract (workspace_ui 75bd21f37de94728e2ebe100acfea24ac9c1a4e1)

- `packages/web/src/features/workspace-topic-actions/workspace-topic-context-menu.ui.tsx`
  - trigger: right-click capture + keyboard Menu (dropdown-context-menu anchor);
    item class `px-2 py-2 text-sm text-text-primary hover:bg-sidebar-hover` (:24);
  - menu: `contentVariant="narrow"` (:301), context `sideOffset: 4`,
    `align: "start"` (:306-307);
  - item order: notifications custom block first, then Mark as read only when
    `topic.unreadCount > 0` (:242), Rename topic, Done toggle whose label is
    `markTopicAsNotDone` when done else `markTopicAsDone` (:265);
  - rename dialog: title `channel.renameTopicTitle` (:69), description = stream
    title, submit label `common.save` disabled while the trimmed name is empty
    or the write is pending (:75-78), Enter submits (:91), an unchanged name
    closes without a request (:175);
  - actions `runWorkspaceTopicRenameRequest` (:1083), `runWorkspaceTopicDoneToggle`
    (:1096), `runWorkspaceTopicNotificationUpdate` (:1107) and
    `runWorkspaceTopicRead` in `entities/messenger/messenger-sidebar-actions.lib.ts`,
    each with a per-action pending guard and `reportUnexpectedError` on failure.
- `packages/web/src/features/workspace-topic-actions/workspace-topic-notification-switch.ui.tsx`
  - `role="radiogroup"` named by `channel.topicNotifications` (:53-54), each
    segment `role="radio"` with the level label (:90-92); clicking the selected
    level sends nothing.
- `packages/web/src/shared/ui/notification-level-switch/notification-level-switch.lib.ts`
  - levels: muted `bell_off`, inherit `bell`, unmuted `at`, followed
    `topic_follow`; sm size: icon 22, container `flex gap-0.5 rounded-md bg-bg
    p-0.5`, segment `h-7 min-w-7 flex-1 rounded-md` (:44-46);
  - tones: selected `bg-card-bg text-icon-active`, idle `text-text-muted
    hover:bg-sidebar-hover` (:70-71);
  - `getTopicVisibilityLevelOptions` order: `muted, inherit[, unmuted],
    followed`; the unmuted option shows only when `allowUnmuted &&
    (streamMuted || topicExplicitlyUnmuted)` (:132-136). Direct-private chats
    pass `allowUnmuted=false`.
- `packages/web/src/entities/messenger/messenger-notification-mode.lib.ts`
  - `mapWorkspaceTopicNotificationModeToLevel` (:92): `default`->inherit,
    `unmute`->unmuted (`followed` when direct-private), `follow`->followed,
    `mute`->muted; `mapTopicVisibilityLevelToWorkspaceMode` (:113) is its
    inverse (`inherit`->default, `unmuted`->unmute, `followed`->follow,
    `muted`->mute).
- i18n (ru/en, `packages/web/src/i18n/locales/{ru,en}.json`): channel
  `.topicNotifications` "Уведомления темы"/"Topic notifications" (ru:904,
  en:896), `topicVisibilityMuted/Default/Unmuted/Followed` "Заглушить|По
  умолчанию|Включить|Отслеживать" / "Mute|Default|Unmute|Follow" (ru:909-912,
  en:901-904), `markTopicAsDone`/`markTopicAsNotDone` (ru:930-931,
  en:922-923), `renameTopic`/`renameTopicTitle` "Переименовать тему"/"Rename
  topic" (ru:932-933), `sidebar.markAsRead` "Отметить как прочитанное"/"Mark
  as read" (ru:1446, en:1436).

## Native design

Menu infrastructure mirrors the accepted `streamMenu` (`streamContextMenu`,
Main.qml): a root-level Popup with `width: 180`, `topPadding/bottomPadding: 5`
and `leftPadding/rightPadding: 1` (the geometry the issue cites as the
streamMenu contract), `modal: false`, `dim: false`, radius-6
`bg-elevated`/`border-subtle` background, and the same guarded open
(bookmark owner/stream/topic + anchor; stale target closes the menu). One
shared `topicContextMenu` Popup instance serves both entry points so rows never
instantiate menus inside item delegates (documented pitfall: items
materializing inside the stream menu break pre-layout collapse pins in
unrelated sidebar-menu cases).

Entry points:

1. Sidebar topic rows (`topic-<uuid>` NativeButtons in `sidebarTopicRepeater`):
   `Keys.onMenuPressed` and a right-button `MouseArea` open the menu.
2. Channel information drawer topic rows (`channelTopicRow-<uuid>` in
   `NativeChannelTopics.qml`): the existing Menu is replaced by the shared
   root menu; the existing `channelTopicMuteAction` object name moves onto the
   muted segment button so the pinned drawer cases keep their seam.

Menu content (mirrors the reference item list, narrow variant):

- `topicNotificationItem`: heading "Topic notifications" (10 px medium
  `text-muted`) + `topicNotificationSwitch` radiogroup (Accessible group) with
  segments `topicNotifSegment-<level>` (`role radio`, object names kept),
  accessible names = level labels, segment height 28 (h-7), min width 28
  (min-w-7), icon 22, container height 32 with 2 px gaps/padding
  (gap-0.5/p-0.5), radius 6; selected tone `card-bg` + `icon-active`, idle
  `text-muted`. Levels and order follow `getTopicVisibilityLevelOptions`; the
  unmuted segment is hidden unless the stream is muted or the topic is already
  unmuted, and hidden entirely for direct-private chats. Clicking the current
  level or acting while busy sends nothing; while the topic has a notification
  write in flight the whole switch is disabled (mirrors `disabled={notificationPending}`).
- `topicMarkReadAction` "Mark as read": visible only when the topic's unread
  count > 0; disabled while the read write is pending.
- `topicRenameAction` "Rename topic": opens `topicRenameDialog` prefilled with
  the topic title; title "Rename topic", stream title as description, field
  label "Topic name"; Save disabled while the trimmed text is empty or the
  write is pending; Enter submits; unchanged name closes without a request;
  errors surface as "Could not rename the topic. Please try again."
- `topicDoneAction`: label "Mark topic as done" / "Mark topic as not done"
  (toggles with `isDone`; the sidebar row strikeout follows `done`).

Controller contract (`NativeController`):

- `Q_INVOKABLE void applyTopicAction(stream, topic, action, value)` with actions
  `notifications` (value in mute|unmute|default|follow), `read`, `rename`
  (value = trimmed name), `toggle_done`. Transport matches the reference
  endpoints already used by `applyChannelTopicAction`:
  `POST messenger/stream_topics/<uuid>/actions/{notifications,read,toggle_done}/invoke`,
  `PUT messenger/stream_topics/<uuid>` for rename. `applyChannelTopicAction`
  delegates to it for the selected stream, preserving its
  `channelTopicActionFinished` signal so the drawer keeps its existing seam.
- Pending guards: one in-flight action per topic (any action, matching the
  existing `m_channelTopicRequests` arbitration that pinned cases rely on:
  a competing action on the same topic is rejected as finished-false without a
  second request; a different topic is never blocked). `topicActionBusy(topic)`
  and `topicActionState(stream, topic)` expose the guard for the menu
  (`<action>Pending`, `error`), emitted via `topicActionsChanged`.
- Results: success updates the stored row (current stream via `m_topics`,
  expanded sidebar streams via the inbox topics cache), emits
  `topicsChanged`/`sidebarExpansionChanged`, and reports `error-report`-style
  failures through `topicActionState.error` (reference: `reportUnexpectedError`).
- Every transition of the guard emits `topicActionsChanged` (pitfall: new
  NOTIFY must fire on every state transition).

States: rows stay enabled for other topics while one topic has a write in
flight; light+dark, ru+en.

## Session-renewal fencing and action lifetime (issue #645)

- Same-account token renewal bumps the API generation, and the request
  wrapper never lets a retired-generation callback settle a topic action.
  The generation-change handler therefore calls `interruptTopicActions()`:
  every ledger entry (`<stream>|<topic>|<action>` -> dispatching owner)
  settles as a retryable interrupted outcome (`<action>Pending` cleared,
  `<action>Error` = "...was interrupted by a session refresh. Try again."),
  `topicActionFinished`/`channelTopicActionFinished` fire with `ok=false`,
  and the current stream's catalog is reconciled by a re-read because the
  interrupted write may have committed server-side (the interrupted name is
  never presented as a definitive success; the saved truth comes from the
  GET). Sign-out and account switches are left to `reset()`. Late replies
  stay fenced by their captured generation and token-keyed arbitration
  entry, so they can neither settle a newer action nor touch the newly
  current route. This mirrors the accepted folder precedent
  (`interruptFolderMutation`, issue #423).
- A route move must not strand a guard: when the reply arrives after
  another stream became current, the action settles against its own
  `stream|topic` state without touching the new route's catalog; a
  collapsed sidebar stream settles its guard the same way.
- A successful write on the selected stream mirrors the committed row into
  the sidebar inbox catalog (`sidebarTopicsChanged`) so sidebar titles and
  badges stay true immediately after rename/read/done/notification writes.
- Rename dialog close policy stays `NoAutoClose` only while a rename is
  actually pending; the interrupted settle re-enables Cancel/Save/Esc so
  the form can be retried or dismissed without reopening the workspace.

## Trigger coordinates (issue #647)

- Right-click entry uses the pointer handler's row-local `point.position`
  (already control-local). `mapFromScene` is not invokable on a
  QQC2 Button in this runtime; calling it threw a TypeError before the
  menu opened (reproduced under Qt 6.10.2, window case
  `topicContextMenuOpensFromRightClickAndMenuKey` fails RED without this).
  `open()` receives the click point relative to the anchor and maps it
  into the menu parent; the keyboard Menu-key entry keeps its anchor-based
  point.

## Tests

- `native-controller-topics-test.cpp` (native-native-controller group):
  notification update posts the mapped mode and updates the cached row; read
  clears the topic unread; rename PUTs the trimmed name and rejects empty;
  done toggle flips `is_done`; pending guard rejects a second same-topic action
  without a second request and allows other topics; failures report an error
  state; sidebar-scoped (non-selected stream) actions update the expanded rows.
- `native-window-test.cpp` (native-native-window group): right-click and Menu
  key open `topicContextMenu` on a sidebar topic row; narrow geometry 180/5/1
  (streamMenu contract above); Mark-as-read visibility with unread>0;
  notification switch segments, radiogroup name, gating when the stream is
  muted and on direct-private chats; clicking the selected level sends nothing
  and pending disables the switch; rename dialog prefill/submit rules; done
  toggle flips the label and strikes the row. Existing pinned cases
  (`channelInfoTopicRowsNavigateAndCollapse`,
  `channelTopicMenuAndSummarySaveUseTheSelectedTopic`,
  `openingAnotherTopicMenuDuringMuteAllowsFurtherActions`,
  `channelTopicSettingsCanRetryWhileAnotherTopicActionWasPending`) keep their
  seams and must stay green, plus the stream-menu pins.
