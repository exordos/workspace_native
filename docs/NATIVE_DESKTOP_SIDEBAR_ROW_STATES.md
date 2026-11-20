# Sidebar chat row: direct-chat status line, pin and mention badges, passive muted unread (issue #96)

Reference: workspace_ui `61dacef78dab2f5d0be76f283bebf49fe29b84af` (read-only clone,
`packages/web/src/widgets/sidebar/...`). Figma token unavailable - mockups not consulted;
every measured value below comes from the React source at the pinned sha.

## 1. Measured reference contract

### 1.1 Title line with status (`sidebar-chat-title-with-status.ui.tsx`)

Only `uiKind === "directPrivate"` stream rows receive a status
(`sidebar-workspace.ui.tsx:430-433`: `statusEmoji = stream.statusEmoji ?? null`,
`statusText = trim(stream.statusText)` or null when blank). The status facts come from the
partner user entity, not the stream DTO (`WorkspaceMessengerUserDto.status_emoji /
status_text`, `messenger.types.ts:303-304`); the sidebar row assembler injects them into the
stream row.

One flex line `gap-1` (4 px), `overflow-hidden`:

| item | classes | px (light defaults) |
| --- | --- | --- |
| name | `min-w-0 shrink truncate text-sm font-medium text-text-primary` | 14 px Medium, 4 px gap |
| status emoji | `shrink-0 text-xs` | 12 px, never shrinks |
| status text | `min-w-0 shrink-[9999] truncate text-xs text-text-muted` | 12 px muted |

Truncation priority is the point of the port: the status text absorbs almost all overflow
(`shrink-[9999]`) and truncates first; the name keeps its full width while the status can
still collapse, and only then elides itself.

Emoji display resolution (`entities/user/user-selectors.lib.ts:63-76`, ported exactly):

1. trim; empty -> hidden;
2. legacy symbol map: `speech_balloon`=💬, `house`=🏠, `palm_tree`=🌴,
   `plate_with_cutlery`=🍽️, `helmet_with_white_cross`=⛑️, `spiral_calendar_pad`=🗓️;
3. `^[a-z0-9_+-]+$` (case-insensitive) reads as a bare shortcode -> hidden;
4. otherwise the emoji string is shown.

### 1.2 Meta column (`sidebar-chat-row-meta.ui.tsx`)

Right column `min-w-10 justify-between gap-1` (compact: `min-w-8`, no time line - already
ported in an earlier lane). Top row `flex items-center gap-1` holds, in order:
notificationIndicator (native counterpart out of scope, absent natively today - unchanged),
then:

- **pin**: `Icon name="pin" size={12}` with `text-text-muted`, shown when the row's
  `pinnedAt != null`. `pinnedAt` is the folder-item `pinned_at` of the stream in the active
  folder view (`adaptMessengerFolderItem`, `messenger-adapters.lib.ts:251`; row assembler
  `messenger-sidebar.lib.ts:389`), so a pin renders for chats pinned in the current folder.
- **mention badge**: `MentionBadge size="default"` when `hasUnreadPersonalMention`
  (`sidebar-chat-row-meta.ui.tsx`, `shared/ui/badge/mention-badge.tsx`): rounded-full,
  `h-5 min-w-[20px] px-1.5 text-[11px]`, `bg-sidebar-unread text-badge-text`, literal glyph
  `@` (aria-hidden, `title="@"` - untranslated literal, no i18n key). `hasUnreadPersonalMention`
  is membership of the stream in the unread-mention index
  (`messenger-sidebar.lib.ts:389`). The badge renders before the unread badge.
- **unread slot**: existing 20 px slot. `Badge maxCount=1000` (`UNREAD_COUNT_DISPLAY_LIMIT`,
  `count-format.lib.ts`) with `variant = displayedUnread.passive ? "muted" : "unread"`;
  `muted` = `bg-notice-disable`, `unread` = `bg-sidebar-unread`, text `text-badge-text`
  (`shared/ui/badge/badge.tsx`). Size default: `h-5 min-w-[20px] px-1.5 text-[11px]` -
  identical to the native badge geometry already shipped, so only the color token switches.

Displayed counter (`resolveWorkspaceDisplayedUnread`,
`messenger-notification-mode.lib.ts:49-57`): `active_unread_count > 0` shows the active
count (not the total); otherwise a positive `passive_unread_count` shows the passive count
with `passive = true` (the gray variant); otherwise no badge.

### 1.3 Palette tokens

All required tokens already exist in `qml/NativeTheme.qml`: `badge-bg` (the native
port of `sidebar-unread` used by the shipped unread badge), `notice-disable`,
`badge-text`, `text-muted`. No theme changes. Icon asset: `pin.svg` is shipped in
`packages/web/src/shared/assets/icons/`; it is added to the `native_icons` CMake list.

## 2. Native data plumbing

`NativeController::streams()` gains four derived row keys (sidebar projection copies them):

- `statusEmoji`, `statusText` - direct rows only, from `m_names` (partner `UserPresentation`,
  already parsed from `status_emoji` / `status_text`), trimmed and filtered through a port of
  `resolveWorkspaceStatusEmojiDisplay` (`statusEmojiDisplay` helper). A partner’s
  status update refreshes the visible direct row whenever its displayed text or emoji changes.
- `displayedUnread`, `unreadPassive` - the `resolveWorkspaceDisplayedUnread` port over the
  existing `activeUnread` / `passiveUnread` stream counters.
- `hasMention` - stream present in `m_unreadMentions` (the native unread-mention index).
  The index keeps its existing lazy contract: it walks when the mentions surface opens
  (the activity-rail tests pin that no walk happens at bootstrap). The mention-index
  mutation sites now also emit `streamsChanged` so rows re-bind (same rule as
  `announceStreamRowPreviews`: any row-derived state must announce itself).
- `pinned` - the `FolderRow` learns which stream ids carry a `pinned_at` folder item;
  `sidebarStreams()` marks `pinned` when the selected folder's item is pinned. All /
  synthesized folders (no server items) resolve to false. No sort or pin-action behavior -
  pin management stays out of scope.

No new user-visible `tr()` strings (`@` is a literal), so the locale suite is untouched.

## 3. QML (`Main.qml` streamButton delegate)

- Title: the single title `Text` becomes a `Row` (spacing 4) of `streamTitleName-<id>`
  (14 px Medium elided), `streamTitleEmoji-<id>` (12 px) and `streamTitleStatus-<id>`
  (12 px `text-muted`, elided). Widths are computed with the flex shrink priority: the
  status text gets `max(0, min(implicit, avail - name - emoji - gaps))` and the name takes
  what the status gave back, reproducing `shrink-[9999]` before the name elides.
- Meta: the existing top slot Item hosts a trailing `Row` (spacing 4) with the pin image
  (`streamPin-<id>`, 12 px `MultiEffect` colorized `text-muted`), the mention badge
  (`streamMention-<id>`, 20 px pill, `badge-bg` / `badge-text`), and the untouched action
  slot (unread rectangle + expand chevron keep their internals and `anchors.right` there).
  The unread rectangle (`streamUnreadBadge-<id>`) colors `notice-disable` when
  `unreadPassive`, else the existing `badge-bg`, and shows `countText(displayedUnread)`
  (the existing 1000 cap formatter).
- Rows keep the measured 60 px / 32 px heights: the status line rides the existing 20 px
  title line box; the meta badges ride the existing 20 px slot.

## 4. Tests

Controller (`native-controller-test.cpp`, offscreen): fixture streams cover a direct chat
whose partner carries `status_emoji: "palm_tree"` + long `status_text` (legacy emoji
mapping + trim), a pinned folder item, a mention message in the unread-mention index, and
a passive-only counter set; assertions on the derived row keys, including that a
non-direct row never carries status fields and active+passive shows the active count.

Window (`native-window-test.cpp`, `:33` xcb, inside `NativeWindowTest`):
`sidebarDirectRowStatusPinMentionAndPassiveBadge` seeds via the `WindowChannelFixture`
handler wrapper - the direct row renders emoji (legacy name mapped to its symbol) + status text (elided before
the name keeps full width), pin shows only on the folder-pinned chat, the mention badge
`@` appears after the mentions surface walks the index and closes, passive-only unread
renders `#989898` (blue-cold light `notice-disable`) while the active-unread channel keeps
`#7087ff` (`badge-bg`).
Both locales boot the window with the scene (ru fixture run asserts no translated strings
are introduced on the row).

## 5. Non-goals

Pin folder management and pin-first ordering (other issues), the notification-mode
indicator (`bell_off` slot) of the meta row, compact density changes (the time column is
already suppressed there and the new badges share the same slot in both densities).
