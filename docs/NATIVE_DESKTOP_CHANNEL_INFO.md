# Native channel info drawer content (issue #132)

Part of #121. This document specifies the channel (stream) content layer of the
right drawer: the big avatar header with participants/online summary, the
stream notification level switch, the description block, and the members list
with owner-only add dialog and self/owner remove. It mounts as the root screen
of the `chat-info` layer on the drawer shell built by #129
(`docs/NATIVE_DESKTOP_RIGHT_DRAWER.md`); `NativeRightDrawer.qml` is reused
unchanged as the shell. Issue #72 owns the chat-header entry point.

Design fidelity case: parity-preserving port of the existing Electron/React
surface, no redesign.

Reference contract: workspace_ui `61dacef78dab2f5d0be76f283bebf49fe29b84af`:

- `packages/web/src/widgets/right-panel/right-panel-workspace-info.ui.tsx`
  (header, notifications, description, members, add-members dialog),
- `packages/web/src/shared/ui/notification-level-switch/notification-level-switch.lib.ts`
  (segment sizes/icons/tones),
- `packages/web/src/entities/messenger/messenger-notification-mode.lib.ts`
  (mode <-> level mapping),
- `packages/web/src/shared/ui/workspace-member-card/workspace-member-card.ui.tsx`,
  `packages/web/src/shared/ui/avatar/avatar.tsx`,
  `packages/web/src/shared/ui/presence-indicator/presence-indicator.tsx`,
  `packages/web/src/shared/ui/user-picker-list/user-picker-list.ui.tsx`,
- endpoints: `POST /streams/{uuid}/actions/notifications/invoke`,
  `POST /streams/{uuid}/actions/add_users/invoke`,
  `DELETE /stream_bindings/{bindingUuid}`,
  `GET /stream_bindings/?stream_uuid={uuid}` (`messenger-streams.api.ts`),
- contracts: `right-panel-workspace-info.ui.test.tsx` (switch disabled while
  pending + error text, owner-only add button, picker excludes existing
  members, duplicate re-filter before POST, remove self vs other, unsupported
  fallback).

## Measured gaps versus the issue text

Measured from source at the pinned sha, which is the oracle:

- The unsupported-fallback label key is `composer.actionUnsupported`
  ("This action is not available in this conversation yet." / "Это действие
  пока недоступно в этом чате."); the issue cites the older
  `workspaceMessenger.actionUnsupported` key, which does not exist at this sha.
- The member role label is `text-[10px] text-text-secondary` in
  `workspace-member-card.ui.tsx` (the issue text says `text-xs`).
- Reference i18n `channel.participants` / `channel.online` are pluralized
  (`{{count}} member(s)`, `{{count}} online`); the sub line joins them with
  ", ". Qt uses `tr("%1 member", "%1 members", n)` with Russian
  one/few/many forms in the catalog.
- Reference `participantsCount` counts member bindings and `onlineCount`
  counts member users whose presence is active
  (`entities/messenger/messenger-chat-header.lib.ts:84-139`); native mirrors
  that: members from `stream_bindings` and online from the user directory
  presence (`active`). Users with unknown status (missing/invalid `status`)
  are participants but never counted online, and their row secondary text
  falls back to the email exactly like `resolveWorkspaceStatusLabel(status) ??
  member.email`.

## Geometry and colors (Tailwind tokens resolved to px; cite: file above)

All colors come from the reviewed `blue-cold` palettes already in
`NativeTheme.qml` (`bg`, `bg-elevated`, `card-bg`, `card-bg-active`,
`text-primary`, `text-secondary`, `text-muted`, `border-subtle`,
`notice-base`, `icon-active`, `indicator-green`, `indicator-orange`,
`accent`). Text sizes: `text-sm`=14, `text-xs`=12, `text-[11px]`=11,
`text-[10px]`=10, `text-lg`=18.

Header (right-panel-workspace-info.ui.tsx:672-685):
`mx-2 border-b border-border-subtle px-2 pb-3 pt-1`, row `gap-4` (16 px);
avatar size lg = 48x48, `text-lg` semibold initial,
`backgroundColor resolveAvatarBackgroundColor(info.color)` (RGB integer ->
#rrggbb, absent -> `bg` + border-border-subtle like the shared Avatar);
title `text-sm font-medium` truncated; sub line
`text-[11px] text-text-secondary`, "N members, M online".

Notifications block (688-715): heading `text-sm font-medium`, `mb-3 px-2`,
muted suffix " (For entire chat)" in `text-text-muted`; the switch size lg
(notification-level-switch.lib.ts: `lg`): container `flex gap-2 rounded-lg
bg-bg p-1` (gap 8, radius 8, padding 4, bg token `bg`), segments
`h-9 min-w-0 flex-1 rounded-lg` (height 36), icons 28 px
(at / bell_off / bell), selected `bg-card-bg` + `text-icon-active`, idle
`text-text-muted`, disabled `opacity-0.5`. Mode mapping:
`mentions_only->default(at)`, `muted->muted(bell_off)`,
`all_messages->subscribed(bell)` (messenger-notification-mode.lib.ts).
Error line `text-xs text-notice-base` (`app.error`). When the stream
notification mode is null/empty an unsupported box replaces the switch:
`rounded-lg bg-bg-elevated px-2 py-2 text-sm text-text-muted`.

Description block (730-739): heading `text-sm font-medium mb-3 px-2`
(`chatInfo.description`); value block `rounded-lg bg-bg-elevated px-3 py-2
text-sm text-text-primary`; rendered only when the description is non-empty.

Members block (748-812): heading row `h-8 justify-between px-2 text-sm
font-medium` + add button `h-8 w-8 rounded`, `Icon person_add size 24`,
`hover:bg-card-bg-active`, text `text-secondary` -> `text-primary` on hover;
visible only to the stream owner (`isCurrentWorkspaceStreamOwner` ==
selected stream `role == "owner"` for the current user). Rows are the shared
`WorkspaceMemberCard`: `rounded-lg bg-card-bg px-3 py-2 gap-3`, hover
`bg-card-bg-active`, list spacing `space-y-1` (4 px); name `text-sm`,
role `text-[10px] text-text-secondary` gap 6 px, secondary `text-[11px]
text-text-secondary`; avatar size sm = 32x32 `text-xs` with `bg-bg-elevated`
background and a presence dot (`presence-indicator.tsx`: sm = 8 px, green
`indicator-green` active, orange `indicator-orange` idle, `text-muted`
offline, ring `ring-2 ring-bg` approximated by a 2 px border in the row bg);
rows are intentionally not buttons. Remove button: `h-6 w-6 rounded
text-text-muted hover:bg-bg hover:text-notice-base`, `opacity-0` until row
hover (desktop breakpoint), disabled `opacity-40` while a removal is in
flight; aria `a11y.removeMemberFromChannel` ("Remove from channel: %1").
Empty list: `px-2 py-3 text-center text-sm text-text-muted`
("No members found"). Remove rules (member.canRemove selector, comment at
569-570): any member can remove themselves; only the stream owner sees the
button for other members.

Add-members dialog (99-246): `max-w-md` (448) `max-h-[70vh] p-0`; header
`px-4 py-3 border-b border-border-subtle` with title `text-sm font-semibold`
("Add members"), subtitle = stream name `text-xs text-text-secondary`, close
X aria `common.close`; body `p-4`: search input `w-full rounded-lg
border border-border-subtle bg-bg px-3 py-2 text-sm focus:border-accent`,
list `h-96`(384) `rounded-lg border border-border-subtle` with multi-select
rows (presence dot + name `font-medium` truncate + status label or email
`text-[11px] text-text-secondary`, selected rows show the `check` glyph);
candidates come from the same verified member directory as channel creation
(`controller.channelMemberChoices`) with existing members excluded
(`buildWorkspaceUserOptions` `excludedUserIds`); error line `text-xs
text-notice-base`; footer Cancel (disabled while submitting) + primary Add
(disabled while submitting or when nothing is selected). Submit re-filters
already-existing members before the POST (reference lines 509-523).

## Native wiring

- New `packages/native/qml/NativeChannelInfo.qml` mounted in the
  `NativeRightDrawer` body slot in `Main.qml`, visible when the top layer is
  `chat-info` with the root info screen (no `chatScreen`, no profile
  override). Layer promotion/dedup/scope reset stay 1:1 with the shell doc.
- `NativeController` gains a channel-info projection for the selected stream,
  re-requested by the page via `refreshChannelInfo()` on mount and on stream
  change: `channelInfoMembers` (binding rows joined with the directory),
  `channelInfoMemberCount`, `channelInfoOnlineCount`, `channelInfoIsOwner`,
  `channelInfoNotificationMode` (store value wins over any stale info, per
  the reference store rule), `channelInfoNotificationBusy/Error`,
  `channelInfoRemovingUsers`, `channelInfoMemberError`,
  `channelInfoAddBusy/Error/SettledTick`, and the invokables
  `setChannelInfoNotificationMode`, `removeChannelInfoMember`,
  `addChannelInfoMembers`. All mutations are guarded by the owner key, API
  generation and the selected stream; binding pages are validated like the
  channel-creation `stream_bindings` pages (unique binding/user ids, project,
  stream, role and mode enumerations, canonical dates) and a rejected page
  keeps the previous list. `addChannelInfoMembers` re-filters members that
  joined meanwhile and closes without a POST when nothing new remains, so a
  parallel refresh cannot send duplicates.
- Icons `at.svg`, `bell.svg`, `bell_off.svg`, `person_add.svg` are added to
  the pinned reference icon resource set (already vendored under
  `packages/web/src/shared/assets/icons`).
- i18n: new strings land in `translations/workspace_ru.ts` under the
  `NativeChannelInfo` QML context and `workspace::NativeController` (the
  shared "Something went wrong" error), natural Russian preserving `%1`.

## Test plan

`native-native-window` (window fixture with the mock server):

1. Header: given a mocked stream with N members and M online, opening the
   drawer root screen shows "N members, M online", the initial avatar painted
   with the stream color, the title and the description block only when the
   stream carries a description.
2. Notifications: toggling the bell-off segment issues the notifications
   invoke, the switch is disabled while the request is in flight, the store
   mode persists afterwards; a failing update shows "Something went wrong"
   and the switch settles enabled again; an empty mode renders the unsupported
   box instead of the switch.
3. Members: rows show role and presence/email secondary text; removing self
   succeeds through the real DELETE and the row disappears.
4. Owner gate: the stream owner sees the add button and the dialog excludes
   existing members; submitting posts only newly selected users and the new
   rows appear; a non-owner sees no add button and no remove button for
   others (own row keeps the self remove).
