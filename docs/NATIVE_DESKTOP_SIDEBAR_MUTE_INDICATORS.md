# Sidebar mute state: notification-mode indicators and the stream menu level switch (issue #97)

Reference: workspace_ui `61dacef78dab2f5d0be76f283bebf49fe29b84af` (read-only clone,
`packages/web/src/...`). Figma token unavailable - mockups not consulted; every measured
value below comes from the React source at the pinned sha.

Line references in issue #97 (2838, 3037, 3316, 2463) address the file as rendered by the
issue tracker; the same constructs in the pinned tree are at the line numbers recorded
here (`git show 61dacef7:<path>`).

## 1. Measured reference contract

### 1.1 Indicator component (`features/mute-chat/workspace-notification-mode-indicator.ui.tsx`)

One 20 x 20 slot (`flex h-5 w-5 shrink-0 items-center justify-center text-text-muted`)
holding a centered 16 px `Icon` (source comment: "16px on padded 28x28 frames ~ old 12px
cropped optical size"). `aria-label`/`title` carry the level label.

- `WorkspaceStreamNotificationModeIndicator` renders only when `mode === "muted"`
  (option `muted`: icon `bell_off`, label `channel.notificationMuted`). A `null` stream
  mode renders nothing (`sidebar-workspace.ui.tsx:555-559`).
- `WorkspaceTopicNotificationModeIndicator` renders whenever the topic mode differs from
  `"default"`, with the level from `mapWorkspaceTopicNotificationModeToLevel(mode,
  isDirectPrivate)` (`entities/messenger/messenger-notification-mode.lib.ts:88-105`):

| topic mode | direct-private | level -> icon |
| --- | --- | --- |
| `mute` | `mute` | muted -> `bell_off` |
| `unmute` | `followed` | unmuted -> `at` / direct-private -> `topic_follow` |
| `follow` | `follow` | followed -> `topic_follow` |
| `default` | `default` | inherit -> not rendered |

### 1.2 Row dimming (`sidebar-workspace.ui.tsx`)

Both rows append `opacity-70` (0.7 on the whole row surface):

- Stream row (line 493): `isWorkspaceStreamFullyMuted(stream.notificationMode,
  notificationTopics.map(t => t.notificationMode))` - the stream itself is `muted` and no
  topic carries an explicit `unmute`/`follow` (`messenger-notification-mode.lib.ts:22-29`).
  A muted stream that some topic un-muted keeps its indicator but not the dim.
- Topic row (lines 200, 214): `isWorkspaceTopicEffectivelyMuted(topic.notificationMode,
  stream.notificationMode)` - `mute`, or `default` under a muted stream (lib.ts:13-18). A
  `default` topic under a muted stream dims without an indicator; a `follow` topic keeps
  full opacity and shows `topic_follow`.

Meta top-line order (`sidebar-chat-row-meta.ui.tsx:99-105`, `gap-1` = 4 px): indicator
first, then the 12 px pin, the mention badge, the unread/chevron slot. The 70 percent dim
applies to the row surface *below* the meta layer, so badges and unread counters dim with
the row; the muted (`passive`) unread variant from issue #96 is untouched.

### 1.3 Stream context menu switch (`sidebar-workspace-context-menu.ui.tsx:466-488`)

`notificationPickerItem` is the first entry of the stream menu (`items =
[notificationPickerItem, ...]`, line 537): a `px-2 py-1` block with a heading
`text-[10px] font-medium text-text-muted` reading `channel.notifications`, then
`StreamNotificationLevelSwitch size="sm"`.

Segmented switch (`shared/ui/notification-level-switch/notification-level-switch.lib.ts`,
`stream-notification-level-switch.ui.tsx`; `role="radiogroup"`, aria-label
`channel.notifications`):

| part | classes | px |
| --- | --- | --- |
| container | `flex gap-0.5 rounded-md bg-bg p-0.5` | gap 2, radius 6, pad 2, bg `bg-bg` |
| segment | `h-7 min-w-7 flex-1 rounded-md` | 28 x >=28, radius 6, fill width |
| icon | `iconSize` for `sm` | 22 |
| selected | `bg-card-bg text-icon-active` | card background + bright glyph |
| idle | `text-text-muted hover:bg-sidebar-hover hover:text-text-primary` | muted glyph |
| disabled | `disabled:opacity-50` | 0.5 |

Stream levels (`STREAM_NOTIFICATION_LEVEL_OPTIONS`, order default, muted, subscribed) and
their wire modes (`messenger-notification-mode.lib.ts:66-86`):

| level | icon | label key | EN | RU | wire `notification_mode` |
| --- | --- | --- | --- | --- | --- |
| `default` | `at` | `channel.notificationDefault` | Mentions only | Только при упоминаниях | `mentions_only` |
| `muted` | `bell_off` | `channel.notificationMuted` | Muted | Без уведомлений | `muted` |
| `subscribed` | `bell` | `channel.notificationSubscribed` | All messages | Все сообщения | `all_messages` |

Section heading `channel.notifications`: EN `Notifications`, RU `Уведомления`.

Direct-private streams render `DIRECT_PRIVATE_STREAM_NOTIFICATION_OPTIONS`
(context menu lines 50-52): the same list with `default` removed, i.e. only `muted`
(`bell_off`) and `subscribed` (`bell`) - DMs already notify on every inbound message, so
the mentions-only level is not offered (lib.ts:66-72).

Selection semantics (switch `handleSelect` + `handleSetNotificationMode`): a disabled
segment or a click on the current level sends nothing; a change closes the menu first,
then fires one update; while pending the switch is disabled.

### 1.4 Persistence endpoint (oracle for the write path)

`updateStreamNotifications` (`shared/api/messenger-streams.api.ts:178-189`):
`POST /streams/{streamUuid}/actions/notifications/invoke` with body
`{ "notification_mode": "mentions_only" | "all_messages" | "muted" }`; the response is the
canonical stream DTO and the store value becomes the response value. The native
controller already implements exactly this contract for the chat-info panel
(`NativeController::setChannelInfoNotificationMode`, canonical-response admission,
per-stream in-flight token, no optimistic write). The sidebar menu reuses the same
endpoint, payload and guards; it must also work for streams that are not the selected one
(chat-info only answers for the currently open stream).

## 2. Native port

- `native-controller.cpp` `streams()` rows gain `notificationMode` (raw wire mode) and
  `fullyMuted` (the `streamGroupRank(...) == 1` port of `isWorkspaceStreamFullyMuted`).
  `topicRows()` rows gain `notificationMode` and `effectivelyMuted`
  (`isWorkspaceTopicEffectivelyMuted`, same predicate the row-rank lambda already uses).
- New `Q_INVOKABLE void setStreamNotificationMode(streamUuid, mode)`: same validation
  (known mode, known stream, changed level), same per-stream in-flight map/token, same
  `POST .../actions/notifications/invoke` request and canonical-DTO admission as the
  chat-info switch, but it updates the stored stream regardless of the selection. Failed
  or superseded responses release only their own stream's slot and keep the old level.
  `Q_INVOKABLE bool streamNotificationBusy(streamUuid)` exposes that slot (shared with
  `channelInfoNotificationBusy`).
- `Main.qml` stream delegate: `stream-<id>` opacity 0.7 while `fullyMuted`; a 20 x 20
  `streamNotif-<id>` slot (16 px `bell_off` glyph tinted `text-muted`, same
  `Image` + `MultiEffect` colorization idiom as the #96 pin glyph) before the pin in
  `streamMetaRow`, visible only when `notificationMode == "muted"`.
- `Main.qml` topic delegate: `topic-<id>` opacity 0.7 while `effectivelyMuted`;
  `topicNotif-<id>` slot on the unread meta line (glyph per the table in 1.1:
  `bell_off`/`at`/`topic_follow`, direct-private `unmute` uses `topic_follow`), title
  width reserve includes the slot. Unread/chevron stay as measured in #96.
- `Main.qml` `streamContextMenu`: a new first `MenuItem` with the `Notifications` heading
  and a size-`sm` switch (`streamNotificationSwitch`, segments `streamMenuNotifDefault`
  (`at`), `streamMenuNotifMuted` (`bell_off`), `streamMenuNotifSubscribed` (`bell`);
  direct-private excludes `streamMenuNotifDefault`). A segment click closes the menu then
  calls `setStreamNotificationMode(streamMenuTarget, mode)`; the controller drops
  same-level and in-flight repeats. While the menu's stream has a notification write in
  flight the whole switch renders disabled: the readonly root property
  `streamMenuNotificationBusy` (bound to `streamNotificationBusy(streamMenuTarget)` and
  re-evaluated on every controller `stateChanged`, review cid 4629685053) drives each
  segment's `enabled`, so a second selection cannot be silently rejected; another
  stream's menu stays interactive during the write. Selected segment `card-bg` + `icon-active`, idle
  `text-muted`, per the size table.
- Assets: `topic_follow.svg` joins `native_icons` in CMake (already bundled in
  `packages/web/src/shared/assets/icons`); `at.svg`, `bell.svg`, `bell_off.svg` were
  already shipped for #132.
- i18n: new `Main` context strings `Notifications`, `Mentions only`, `Muted`,
  `All messages` land in `translations/workspace_ru.ts` with the RU values of section 1.3
  (verified against `packages/web/src/i18n/locales/ru.json` at the pinned sha).

Glyph accessible rule: like the #96 pin/mention glyphs the row indicators are
`Accessible.ignored` decoration (row semantics unchanged); the menu segments are buttons
and carry their level label as `Accessible.name`.

## 3. Acceptance

Window group `native-window-test`:
`windowSidebarMuteIndicatorsAndRowDimming` asserts indicator visibility per mode on both
row kinds (stream muted indicator, topic `mute`/`follow` glyphs, no indicator for
`default`), the 0.7 dim for a fully-muted stream, the undimmed muted stream that a topic
follows, and the effectively-muted `default` topic under a muted stream.
`windowStreamContextMenuNotificationSwitch` opens the menu with a real right-click,
writes `{notification_mode: "muted"}` against `.../actions/notifications/invoke`, sees
the sidebar row flip to indicator + dim from the canonical response, sends nothing on a
same-level repeat, and checks the direct-private menu excludes the default level.
`windowStreamContextMenuNotificationSwitchDisabledWhileBusy` holds the first write with
the mock server's `defer` gate and asserts the menu's level switch is disabled for the
busy stream, an in-flight press posts no second request, another stream's menu stays
enabled, and the switch re-enables live once the held response settles.

Controller group `native-controller-test`: `streamNotificationModeWritePathTargetsAnyStream`
exercises the unselected-stream write (path, body, busy rejection while in flight,
same-mode no-op, `streams()` row `notificationMode`/`fullyMuted` updates) and the
`topics()` row `notificationMode`/`effectivelyMuted` projection.
