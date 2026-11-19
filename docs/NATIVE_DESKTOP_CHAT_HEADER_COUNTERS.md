# Chat header: member and online counters (issue #89)

Figma token unavailable - mockups not consulted (the `figma-token` helper
returns a token that the Figma API rejects with HTTP 401 as of 2026-11-16;
the React source oracle is authoritative for this issue).

## Scope

The channel/topic header shows a member + online caption under the title.
The ordinary native header rendered no caption at all and the encrypted
header kept an empty `encryptedParticipantsPlaceholder`. This issue projects
the counts in `NativeController` and renders the caption in both header
variants. The "topic - stream" heading itself is already covered.

## Reference contract (workspace_ui 61dacef78dab2f5d0be76f283bebf49fe29b84af)

- `packages/web/src/entities/messenger/messenger-chat-header.lib.ts`
  - `selectWorkspaceChatHeaderView`: `participantsCount` counts the stream's
    bound members (lines 94-101); `onlineCount = selectOnlineUserCount`
    over the member users (line 130) - a member counts as online when the
    directory `status` is exactly `active`; unknown status is never online;
    `hideTopic` is true unless the route is a topic.
  - Direct chats resolve to the `directPrivate` view: no counts at all.
- `packages/web/src/widgets/chat-view/chat-header-channel.ui.tsx`
  - Caption: `t("channel.participants", {count}) + ", " + t("channel.online",
    {count})`, class `text-xs text-text-muted`, `mt-0.5` under the 14px
    title (encrypted variant: `mt-1 leading-[18px]`). Hidden via
    `hideParticipants`.
- Locales (`packages/web/src/i18n/locales/{en,ru}.json`, `channel.*`):
  - en: `participants_one` "{{count}} member", `participants_other`
    "{{count}} members"; `online_one`/`online_other` "{{count}} online".
  - ru: one/few/many: "{{count}} участник / участника / участников";
    "{{count}} в сети" for all three forms.

## Native design

### Controller projection (`packages/native/src/native-controller.{h,cpp}`)

Selection-scoped members projection, mirroring the channel-info member
loader contract (`refreshChannelInfo` / `loadChannelInfoPage`):

- `Q_INVOKABLE void refreshChatHeaderMembers()` (re)loads the bindings of
  the currently selected stream. Query: `GET /messenger/stream_bindings/`
  with `stream_uuid` only (the same chat-bootstrap query shape; the
  `page_limit=200` marker stays exclusive to the channel-info pages so the
  #197 fetch discipline tests keep their meaning). Pages are walked via
  `page_marker`; rows are validated with the shared `creationBindings`
  gate (unique binding/user uuids, project, stream, role enum).
- Properties `chatHeaderMemberCount` and `chatHeaderOnlineCount`
  (NOTIFY `stateChanged`): 0 unless the projection belongs to the current
  owner, authentication and selected stream. Online follows
  `selectOnlineUserCount`: a member counts when its verified-directory
  `status` is `active` (same rule as `channelInfoOnlineCount`).
- Reset on selection change: `refreshChatHeaderMembers()` for a different
  stream (or a direct chat, which never counts) clears the stale member set
  first, so the caption never shows the previous channel's numbers.
- A rejected reload keeps the previous member set (same rule as the channel
  info page).
- Presence half: the realtime `user.updated` handler now emits
  `stateChanged()` when the user's status actually changed, so the header
  caption re-renders when presence projection changes (the directory
  refresh path already emits `stateChanged()`).
- Realtime binding events (`stream_bindings.created` / `stream_binding.deleted`
  on the selected stream) adjust the loaded member projection in place -
  membership add/remove only, with a `stateChanged()` re-notify reserved for
  a real change of the open conversation's member set. They never reload
  `stream_bindings`: the E2EE read path pins exactly one binding GET per
  authority refresh (issue #434), and a `stream_binding.updated` role change
  never moves membership or online, so it never notifies at all. The same
  change-gate applies to a completed page load that re-serves the identical
  member set.
- Round 3 (PR #432 review finding 4628746243): a membership event that lands
  while a member page load is in flight is additionally remembered in a
  per-load pending event log; the final page replays those deltas onto its
  (possibly older) snapshot before the change-gate, so completion never
  drops an event that the in-place update already applied. The log is reset
  at every refresh start and at every load end; the replay is idempotent
  when the snapshot already reflects the event, so the round-2 properties
  (no extra fetch, no re-notify on an unchanged set) are unchanged.

### QML (`packages/native/qml/Main.qml`)

- Caption text helper mirrors `NativeChannelInfo.qml` (`memberCountLabel` /
  `onlineCountLabel`): `%n member` / `%n members` / `%n online`, joined by
  `", "` - identical source strings so the reviewed ru catalog plural forms
  apply (three Qt forms = one/few/many).
- Ordinary header: new `Text` `chatHeaderCounters` at `x:20, y:30`
  (title line ends at y=28; mt-0.5 = 2px), `font.pixelSize: 12` (text-xs),
  `color: colors["text-muted"]`, visible with the title RowLayout
  (`!isDirectConversation && !encryptedConversation && !favoritesVisible`)
  and only while `chatHeaderMemberCount > 0` (no misleading "0 members"
  flash before the projection arrives; a bound creator means a real stream
  always has >= 1 member).
- Encrypted header: the same caption fills `encryptedParticipantsPlaceholder`
  (already at y:24/height 18, matching mt-1 leading-[18px] under the 16px
  title line), objectName `encryptedChatHeaderCounters`.
- Direct chats render `directConversationHeader` instead; both counter
  items are gated off and the controller never counts for direct streams.
- Refresh trigger (issue #438 fix): the header publishes its on-screen state
  through the `chatHeaderVisible` property (a `Binding` on the header item's
  `visible`); the controller fetches a member page only when the selected
  stream actually changes (`activateStream`'s commit with a different stream)
  while the header is on screen and favorites is closed, and only when that
  owner+stream has no projection yet. The original trigger - a `Connections`
  with `onSelectedStreamChanged`/`onAuthenticatedChanged` handlers - resolved
  to those properties' NOTIFY, which is the shared `stateChanged`, so every
  state change re-fetched (see the #438 section below).

### Translations

New `Main` context entries `%n member` / `%n members` / `%n online` in
`packages/native/translations/workspace_ru.ts` with the three ru plural
forms (participant forms as in the reference locale; "в сети" for all
forms). The locale catalog guard (`native-locale-test`) regenerates the
extraction and fails on any unreviewed source.

## Tests

- Controller (`native-controller-test.cpp`): stream with 2 members / 1
  active -> counts 2/1; single-member stream -> 1/1; `user.updated` status
  change flips the online half; direct stream keeps both counters at 0;
  stale counts never survive a selection change. Binding events on the
  selected stream move the counts in place; role-only events neither fetch
  nor re-notify; an event that lands while a member page load is in flight
  is reconciled against the completed page (deferred-response race,
  finding 4628746243).
- Window (`native-window-test.cpp`, native-native-window group): channel
  with 2 members / 1 online renders "2 members, 1 online"; the en singular
  branch renders "1 member, 1 online"; a presence event updates the
  caption; direct chats show no caption.

## Known deltas vs the reference

- Reference `text-xs` is 12px in this app's scale (`font.pixelSize: 12`);
  the channel-info subline uses the same wording at 11px - the header
  caption keeps the header contract (12px), not the drawer's.
- Qt catalogs carry three ru plural forms (one/few/many), which matches the
  reference categories exactly; `other` only exists for other languages.

## Issue #438: fetch scoping fix (release 0.39.0 blocker)

PR #432's automatic trigger broke two gui ctest groups at tip `096110d`:

- The `Connections` handlers were named `onSelectedStreamChanged` /
  `onAuthenticatedChanged`, but `selectedStream` and `authenticated` are
  `Q_PROPERTY`s whose NOTIFY is the shared `stateChanged`. QML resolves an
  `on<Property>Changed` handler to that NOTIFY, so every state change in the
  app (draft edits, presence ticks, busy flips) called
  `refreshChatHeaderMembers()` - which re-fetched unconditionally. Window
  fixtures that pin exact request traffic (`messageTopicReferenceUsesThe
  CanonicalRoute`, `selectedDeletedTopicKeepsAnExplicitMainRouteUntil
  CanonicalOpening`, `globalChannelCreationUsesVerifiedHttp`, the
  aggregate-prepend scroll row) saw repeated `stream_bindings` GETs, and the
  #434-pinned e2ee request budgets (exactly one binding GET per authority
  refresh) broke for the same reason.
- One such late completion raced a fixture teardown: a member-page response
  was delivered into a `WindowProfileFixture` mock handler whose fallback
  chain had already gone away, invoking an empty `std::function`
  (`std::bad_function_call` SIGABRT after
  `windowProfileShareLinkFetchesAndReportsFailure`).

Fix (same scoping discipline as #434's NOTIFY gate, applied to the fetch):

- `NativeController::ensureChatHeaderMembers()` is the only automatic
  trigger. It runs on selection edges (an `activateStream` commit whose
  stream differs from the previous one) and on the `authenticated` signal.
  A member page loads only while `chatHeaderVisible` is set (a `Binding`
  fed by the header item's effective `visible`) and favorites is closed,
  and only when the current owner+stream has no projection yet - so
  mounting a window, topic switches, feed/inbox overlays and hidden
  controllers produce zero member traffic. Empty or direct selections clear
  the projection without a request.
- `setChatHeaderVisible` stays silent on `stateChanged`; its QML value
  expression reads only the item's `visible` (reading any controller
  property would loop, since they all notify through `stateChanged`).
- The forced reloads for channel-info member mutations and the explicit
  `refreshChatHeaderMembers()` QML entry point keep their current behavior.
- Test migrations (the reference genuinely reads the member page when the
  visible header opens a different conversation, exactly as the #364
  migration treated lagging master cases): `WindowTopicLinkFixture` now
  counts the routed `stream_bindings` GET (`routeBindingGets`/
  `routeBindingStreams`) and pins it - one read for the destination stream
  in `messageTopicReferenceUsesTheCanonicalRoute(cross-canonical)` and
  `selectedDeletedTopic...(selected-deletion-opens-owned-canonical-topic)`,
  zero elsewhere; `globalChannelCreationUsesVerifiedHttp` expects the one
  read of the created channel (`bindingGets - initialBindings == 1`). The
  pinned `native-native-e2ee-ui` budgets were not touched.
- Regression coverage: `chatHeaderCountersFetchFollowsVisibleSelectionEdges`
  (controller) pins headless silence, mount-is-not-an-edge, one page per new
  visible selection, no refetch on re-activation, direct clear without a
  request, and the ticket fence for a page that completes after its
  selection was replaced (the deferred-response race behind the abort).
