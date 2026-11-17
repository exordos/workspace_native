# Activity panel: compact rail badges and expanded list

Issue #101 (part of #40, Activity parity). The compact My Activity rail existed
with live navigation for inbox/mentions/drafts/starred, but the expand chevron
was a disabled placeholder and no chip carried a count. This change makes the
chevron toggle an expanded "My Activity" list and renders unread count badges
on the compact rail and in the expanded rows, with the open/closed state
persisted per workspace session.

## Reference contract

Measured from `workspace_ui` at sha
`61dacef78dab2f5d0be76f283bebf49fe29b84af`:

- `packages/web/src/widgets/sidebar/sidebar-activity-view.ui.tsx` — compact
  rail and expanded list.
- `packages/web/src/widgets/sidebar/sidebar.lib.ts` — `MY_ACTIVITY` item
  metadata (labels, glyphs, chip colors) and `VISIBLE_MY_ACTIVITY_KEYS`
  (inbox, favorites, markedMessages, mentions, drafts).
- `packages/web/src/widgets/sidebar/sidebar-workspace-activity.ui.tsx` — the
  only live counts are inbox and unread mentions; drafts and markedMessages
  are passed as `null`.
- `packages/web/src/entities/messenger/messenger-sidebar.lib.ts:757-785` —
  `selectMessengerSidebarActivityCounts`: inbox count is the `unreadCount` of
  the system `all` folder; `null` when no all-folder exists.
- `packages/web/src/shared/lib/count-format.lib.ts` —
  `UNREAD_COUNT_DISPLAY_LIMIT = 1000`; `formatCount` renders
  `floor(count / 1000) + "k+"` when the count is strictly above 1000 (1001 →
  `1k+`, 2002 → `2k+`; exactly 1000 renders `1000`).
- `packages/web/src/widgets/sidebar/sidebar-config.model.ts` — `activityOpen`
  persists per workspace-session storage scope, default `false`.
- `packages/web/src/shared/ui/badge/badge.tsx` — `unread` variant fill is
  `bg-sidebar-unread`; `sm` size is 16 px high, min-width 16, `px-1`, 10 px
  text; default size is 20 px high, min-width 20, `px-1.5`, 11 px text.

Figma token unavailable - mockups not consulted.

## Compact rail (collapsed)

- Row: five cards sharing the leftover width (`flex-1`, gap 8) plus a fixed
  32 px chevron slot (`w-8 shrink-0`). Each card is 32 px high, `rounded-lg`
  (8), `p-1`, glyph 18–20 px; idle `bg-card-bg`, hover `bg-card-bg-active`,
  active route `bg-card-bg-active`; the chevron card stays muted
  (`text-text-muted`).
- Badges render only for inbox and mentions, only when the count is non-null
  and greater than zero (badges hide at zero). A badge sits at the card's
  top-right corner (`absolute right-0 top-0`), size `sm`, unread variant,
  text tone `primary`, capped at `UNREAD_COUNT_DISPLAY_LIMIT` = 1000.
- The chevron toggles the panel (`aria-label nav.activity`); it is no longer
  a "not implemented" placeholder.

## Expanded list

- Header button: full width, `px-1 py-1 rounded-lg`, label `t("nav.activity")`
  (`text-base` 16 px, `font-semibold`), 16 px `chevron-up` in a `p-1` muted
  slot; clicking collapses.
- List: `mt-2` (8), rows spaced 4. Each row is `rounded-lg bg-card-bg p-2`
  (8) `gap-3` (12), text 14 px; hover `bg-card-bg-active`; the active route
  row uses fill `bg-bg-elevated/60`; disabled items render at opacity 0.7
  with the "not implemented" reason and no route.
- Icon chip: 30x30 circle (28 px in compact density, which the native client
  does not model) with a 24 px white glyph. Chip fills from
  `sidebar.lib.ts MY_ACTIVITY` — `bg-indicator-purple` (inbox),
  `bg-indicator-blue` (favorites), `bg-indicator-red` (marked messages),
  `bg-indicator-yellow` (mentions), `bg-indicator-pink` (drafts).
- Row badge: default size (20 px high, min-width 20, 11 px text), unread
  variant, same 1000 cap and hide-at-zero rule; only inbox and mentions have
  counts.

## Palette tokens

The indicator hexes are identical in light and dark in every reference
palette (e.g. `packages/web/src/shared/lib/themes/blue-cold.ts:60-66` and
`122-128`, `packages/web/src/app/app.styles.css:336-342`):

| Token | Hex | Chip |
| --- | --- | --- |
| `indicator-purple` | `#8d6dff` | inbox |
| `indicator-blue` | `#4b98e6` | favorites |
| `indicator-red` | `#f04c4c` | marked messages (starred) |
| `indicator-yellow` | `#ffd633` | mentions |
| `indicator-pink` | `#f458d2` | drafts |

The native palette in `qml/NativeTheme.qml` already ships the same
`indicator-*` keys, and the badge uses the existing `sidebar-unread` fill
with the compact label tone from the reference (compact) or `badge-text`
(expanded).

## Labels

- `nav.activity`: EN "My Activity" / RU "Моя активность".
- `activity.favorites`: EN "Favorites" / RU "Избранное".
- `activity.starred`: EN "Marked messages" / RU "Отмеченные сообщения".
- Inbox / Mentions / Drafts reuse the existing catalog entries.

## Native mapping

- `qml/Main.qml`: the `sidebarActivity` RowLayout is wrapped in a
  `sidebarActivityPanel` column. Closed, it renders the previous compact
  rail (same `activity-<id>` object names) plus the now-enabled
  `activityExpandButton`; opened, it renders `activityHeader`
  (`activityCollapseButton` chevron-up) and `sidebarActivityList` with
  `activityRow-<id>` rows (`activityChip-<id>`, label, optional
  `activityRowBadge-<id>`), and the chat list top margin grows by the panel
  delta so the list is never overlapped. Compact badges are
  `activityBadge-inbox` / `activityBadge-mentions`.
- `src/native-controller.h/.cpp`: `activityOpen` (bool, persisted),
  `activityInboxCount` (unread count of the `all` folder row, `-1` = not
  known yet → no badge), `activityMentionsCount` (size of the unread-mentions
  index once it is loaded, `-1` = not known yet, mirroring the reference
  owner-key gate) and `activityBadgeLabel(int)` implementing the 1000 cap as
  `floor(count / 1000)k+` per `count-format.lib.ts`.
- `src/native-preferences.h/.cpp`: `loadActivityOpen(owner)` /
  `recordActivityOpen(owner, open)` store
  `sidebar/owners/<owner>/activityOpen` per workspace session (same
  64-hex owner validation and write pattern as `presence/owners/.../manualAway`
  and `expandedStreams`); default closed, matching `sidebar-config.model.ts`.
  The controller loads the value when the session owner changes
  (`prepareSidebarExpansion`) and stores it on every toggle.
- Expanded glyphs (`mail_activity.svg`, `home_filled.svg`, `marker.svg`,
  `drafts.svg`) plus `chevron-up.svg` join the shipped icon resource list.
- favorites keeps the disabled "not implemented" state until #70 lands;
  starred routes to the merged #41 page. Inbox/mentions/drafts keep their
  existing pages.

## Acceptance coverage

- Window QtTest (`native-window-test`): toggle geometry (32 px rail,
  46 px rows, 30 px chips), chip colors, active-route fill, badge visibility
  at 0/>0/1001, favorites disabled, collapse, and open-state persistence
  across two logins sharing a preferences file.
- Controller QtTest (`native-controller-test`): `activityBadgeLabel` cap
  rules, `activityInboxCount` following the all-folder unread,
  `activityMentionsCount` gated on index readiness, and the
  record/load persistence round trip per owner.
