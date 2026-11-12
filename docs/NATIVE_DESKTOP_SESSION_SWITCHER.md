# Top bar workspace session switcher (native parity)

Status: approved for implementation.
Issue: #144.
Reference revision: `workspace_ui` `61dacef78dab2f5d0be76f283bebf49fe29b84af` (measured in source, not screenshots).

## Purpose

The reference top bar left slot is a workspace session switcher: quick logo
buttons for up to three remembered sessions, a per-session unread dot, an
overflow `+N` chip, and a dropdown with one row per remembered account
(login + `host · projectId`), a per-org logout with confirmation, and an
"Add server" row. The native client currently shows one organization button
that opens the account menu (`qml/Main.qml:1377-1423`): no remembered-session
list in the shell, no switching UI, no per-session unread projection and no
per-org logout. This feature ports the switcher into the shell.

## Reference contract (measured)

Sources: `packages/web/src/widgets/top-bar/top-bar.ui.tsx:110-124` (left slot),
`packages/web/src/widgets/top-bar/top-bar-workspace-session-switcher.ui.tsx`
(whole file), `packages/web/src/widgets/top-bar/top-bar-workspace-session-unread.lib.ts:21-72`,
`packages/web/src/shared/lib/count-format.lib.ts:1-8`,
`packages/web/src/shared/ui/badge/badge.tsx`,
`packages/web/src/entities/workspace-auth/workspace-post-logout-route.lib.ts`,
behaviour contract `packages/web/src/widgets/top-bar/top-bar-workspace-session-switcher.ui.test.ts`.

### Left slot and quick buttons (switcher :34, :93-153, :307-356)

- Left slot: toolbar row padding 8 plus `pl-5` (20) => content starts at x=28
  (matches the existing native left margin); inner scroller `max-w-xs`
  (320px) `overflow-x-auto`, wrapper `py-1 pr-0.5`. The switcher root is a
  row with `gap-1.5` (6px) and `aria-label` `auth.selectServer`.
- `MAX_VISIBLE_SESSIONS = 3`: quick buttons render `sessions.slice(0, 3)`.
- Quick button: 40x40, `rounded-lg` (8px radius), transparent background;
  the current session uses `bg-card-bg-active`. Inner logo frame: 36x36,
  `rounded-md` (6px), `bg` token background, and for the current session a
  2px inset ring `ring-border-subtle` (`#33384d` dark / `#d8e4ef` light in
  the blue-cold palette; `bg` `#141517`/`#e4ecf3`, `card-bg-active`
  `#2c3747`/`#cde6ff`).
- Unread dot (button-level): when the session's projection unread > 0, a
  10x10 (`h-2.5 w-2.5`) circle at the top-right of the button,
  `bg-badge-bg` (`#3d5eff` dark / `#7087ff` light) with a 1px
  `border-bg-elevated` ring.
- Tooltip content is the account display name (profile first+last name,
  username fallback); the accessible name is
  `auth.currentServer + ": " + label` for the current button, `label`
  otherwise, where `label` = `"<primary> · <host> · <projectId>"`.
- Overflow chip: when more than 3 sessions are remembered, a
  `+<hiddenCount>` chip, `rounded-md` (6px), `bg-bg-elevated`, `px-1.5 py-0.5`
  (6/2), `text-xs` (12px) `text-text-muted`.
- Dropdown trigger: 32x32 (`h-8 w-8`) `rounded-lg` button with the
  `chevron-down` icon at 14, `aria-label` `auth.selectServer`.
- Labels: primary = trimmed login, fallback user uuid; secondary =
  `"<host> · <projectId>"` where host is the organization origin hostname
  (origin text fallback); title = `"<primary> · <secondary>"`.

### Dropdown (switcher :201-305, :338-354)

- `contentVariant="wide"` (min width 220px, `tailwind.config.ts:98`),
  `sideOffset: 6`, `align: "start"`, vertical content padding `py-1` (4px).
- One row per remembered session (all, not only the visible three):
  40x40 logo frame (`rounded-lg`, `bg` token, current row adds the same 2px
  inset `border-subtle` ring) + unread Badge at its top-right when
  projection unread > 0 (badge `maxCount = UNREAD_COUNT_DISPLAY_LIMIT =
  1000`), then a text column with `gap-0.5` (2px): primary label
  (`font-medium`, truncating) and secondary label (`text-xs`,
  `text-text-muted`). The current row is `rounded-lg bg-card-bg-active`.
- Per-row logout button: 24x24 (`h-6 w-6`), `rounded-md`, `text-notice-base`
  on `bg-notice-base/10` with a 1px `border-notice-base/40` border,
  `logout` icon at 14, `aria-label` `auth.logoutFromOrg`. Activation asks
  `auth.logoutFromOrgConfirm` ("Log out from {{server}}?" with the row title)
  before removing the session. Removing the current session then activates
  the next remembered inbox (`resolveWorkspacePostLogoutRoute`: first
  remaining session's inbox, or the sign-in route when none remain).
- Separator, then the "Add server" row (`add` icon at 16 + `auth.addServer`)
  which opens the sign-in route (the reference keeps remembered sessions).

### Per-session unread (unread lib :19-72, count-format.lib.ts:1-8)

`getBackgroundProjectionUnreadCount(projection)` fallback ladder:

1. `unreadByFolderId[ALL_CHATS_FOLDER_UUID]` (all-folder count) when present.
2. Sum over folder items whose topology places them in the all-chats folder.
3. Sum of `unreadByFolderId` (negative counts clamped to 0).
4. Sum of `unreadByFolderItemId`.
5. Sum over non-archived stream snapshots of `activeUnreadCount ??
   unreadCount` (clamped), plus 1 when a notification candidate references an
   unknown stream and is itself active (not own, not read, and its current
   message snapshot is missing or unread-and-not-deleted).
6. Otherwise 1 when any active candidate exists, else 0. No projection => 0
   ("loading = no projection yet -> no dot").

The menu Badge renders `formatCount(count, 1000)`. The issue #144 acceptance
text pins the native dropdown rendering for 1500 as `"1000+"` (an exact cap
at the display limit); the native badge therefore renders the plain count up
to 1000 and `"1000+"` above it. The pinned reference compacts the same value
into `"1k+"`; this single label difference is intentional per the acceptance
text, everything else mirrors the reference.

### i18n (locales en/ru)

`auth.selectServer` "Select account" / "Выбрать аккаунт";
`auth.currentServer` "Current server" / "Текущий сервер";
`auth.logoutFromOrg` "Logout from organization" / "Выйти из организации";
`auth.logoutFromOrgConfirm` "Log out from {{server}}?" /
"Выйти из организации {{server}}?"; `auth.addServer` "Add server" /
"Добавить сервер".

## Native design

New `qml/SessionSwitcher.qml` replaces the left-slot Row of `qml/Main.qml`
(the old single organization button). It renders quick buttons, the overflow
chip, the chevron trigger, the session dropdown Popup and the logout
confirmation Popup. The existing right-side profile control and its account
menu stay unchanged (issue #137 territory).

Controller additions in `src/native-controller.h/.cpp`:

- `QVariantList sessionSwitcherSessions` property: one row per remembered
  account in vault order with `id`, `primaryLabel`, `secondaryLabel`, `title`,
  `displayName`, `unread` and `isCurrent`. `isCurrent` compares against the
  session's current saved-account id (new `Session::currentSavedAccountId()`).
  `displayName` uses the verified viewer display name for the current row and
  the login (user fallback) for inactive rows, because the native client does
  not yet store per-account profiles.
- `publishSessionProjection(accountId, projection)` + internal registry:
  the per-owner background projection facts (folder/folder-item/stream
  snapshots and notification candidates in the reference wire shape). Rows
  compute `unread` with `NativeController::backgroundProjectionUnread`, a
  direct port of the fallback ladder above. No projection => unread 0 => no
  dot, matching "no projection yet".
- `NativeController::unreadBadgeLabel(unread)`: `"37"` for 37, `"1000"` at
  the limit and `"1000+"` above it (see the deviation note above).
- `selectWorkspaceSession(id)`: no-op for the current id; otherwise
  `Session::detach()` (new: clears only in-memory session state and keeps the
  vault entry, emitting `signedOut` so the existing controller `reset()`
  teardown runs), then `resumeSavedAccount(id)`. Switching does not erase any
  cache partition: identities stay remembered.
- `logoutWorkspaceSession(id)`: for a non-current row this is the existing
  remembered-account removal (durable cache cleanup first). For the current
  row it detaches the live session, removes the remembered account, and then
  resumes the next remembered account (the native counterpart of
  `resolveWorkspacePostLogoutRoute`), or stays on the sign-in step when none
  remain. The successor is chosen before the removal because the vault list
  drops the entry synchronously while its secure-store cleanup is async.
- `openAddServer()`: detaches the live session (remembered accounts survive)
  so the sign-in step opens, mirroring "Add server navigates to /login".

`sessionSwitcherSessions` also synthesizes the live session as the first
row when it is not (yet) a remembered account, mirroring
`accountsFromApiSession` in the reference store, and deduplicates it against
the vault entry with the same identity. Until a background sync exists, per
remembered account the native client only renders unread state fed through
`publishSessionProjection` (the hook the background sync will call); no
projection means no dot, as in the reference. The row logo uses the static
`organization-fallback.svg` brand asset: the native client has no per-server
logo store yet, matching the pre-feature top bar behavior.

QML rows slice the first three entries for quick buttons and show `+N` for
the rest, mirroring `MAX_VISIBLE_SESSIONS`. The reference's horizontal scroll
area is unnecessary in the native port because the row count is bounded to
three buttons plus the chip; that is the only structural deviation.

Note: appearance (palette/mode) is owner-scoped and re-read when the window
wiring binds the authenticated owner, so automated tests that pin dark-palette
tokens must call `setMode("dark")` after the window is wired, not before.

## Acceptance (CTest)

Controller group (`native-controller-test`): row list content/order and
current marker from saved accounts; the ladder port covers the reference
test cases (all-folder, all-folder item sum, folder sum, item sum, stream sum
with `activeUnread` preference and the +1 pending unknown-stream candidate,
archived exclusion, candidate presence, no projection); badge label cap;
select/logout-then-next/add-server transitions at controller level.

Window group (`native-window-test`): with five remembered sessions the top
bar shows exactly three 40x40 quick buttons (inner 36x36 frame) plus a `+2`
chip; the current button paints a 2px inset ring `#33384d` (blue-cold dark);
projection unread 37 renders a 10x10 `#3d5eff` dot with a 1px `bg` ring border;
the dropdown shows all five rows and renders the 1500 badge as `1000+`; the
24x24 logout of the current session asks "Log out from <title>?" and, after
confirmation, activates the next remembered inbox; the last dropdown row
"Add server" opens the sign-in step with remembered accounts preserved.
Geometry oracles are fixed pixel values measured from the reference component
(font-independent), per the issue #16 rule.
