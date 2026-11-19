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
- Refresh trigger: a `Connections` on `selectedStreamChanged` (plus initial
  authentication and the catalog-ready transition) calls
  `controller.refreshChatHeaderMembers()`, keeping the projection
  selection-scoped and zero-cost for tests that never mount the header.

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
  stale counts never survive a selection change.
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
