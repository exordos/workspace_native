# Folders rail: system folders, badges and folder-scoped chat list

Issue #83 (epic #82). Replaces the static `folderRail` placeholder in
`packages/native/qml/Main.qml` (a hard-coded Repeater with "All chats" enabled
and Personal/Channels disabled, plus a disabled add-folder button) with an
API-backed folder rail and a folder-scoped sidebar stream list.

## Reference contract (measured)

Oracle: `workspace_ui` at sha `61dacef78dab2f5d0be76f283bebf49fe29b84af`
(`~/dev/workspace_ui`). All values below were read from the source at that sha.

### API (`packages/web/src/shared/api/messenger-folders.api.ts`)

- `GET /folders/` (under `/api/workspace/v1/messenger`, paginated) returns an
  array of `WorkspaceMessengerFolderDto`
  (`packages/web/src/shared/api/messenger.types.ts:284`):
  `uuid`, `title`, `background_color_value` (int 0xRRGGBB, may be null),
  `unread_count` (int), `system_type` (`all` | `personal` | `channels` |
  `created`, may be absent), `folder_items[]` (each carries `stream_uuid`,
  `chat_type`, `folder_uuid`).
- The store maps them to camelCase `backgroundColorValue` / `unreadCount` /
  `systemType` (`sidebar-shell.ui.tsx:225-230`).

### Fixed system UUIDs (`entities/messenger/messenger-folder-system-type.lib.ts`)

- All chats `00000000-0000-0000-0000-000000000000`, Personal
  `00000000-0000-0000-0000-000000000001`, Channels
  `00000000-0000-0000-0000-000000000002`.
- `resolveMessengerFolderSystemType` forces the system type from these UUIDs;
  a reported `system_type: "all"` on any other UUID is discarded (treated as
  `created`).

### Rail geometry (`widgets/folder-rail/*`)

`folder-rail-vertical-view.ui.tsx`:

- Rail: `w-[72px]`, `py-2` (8 px), column `gap-0.5` (2 px), centered items.
- The "All" entry (`systemType === "all"`, legacy index 0; `id` normalized to
  `system:all` in `sidebar-folder.constants.ts`) is pinned above a scrollable
  group (`mt-0.5 px-0.5 flex-1`); the rest keep server order
  (`folder-rail.lib.ts::orderedIndexedFoldersForRail` pins All first).
- Quick-list button + dropdown appear only when the scroll content overflows
  (`content.scrollHeight > root.clientHeight`).

`folder-rail-folder-items.ui.tsx::VerticalFolderItem`:

- Item cell `h-[72px] w-[56px]`, `p-1`, centered.
- Inner card `h-[64px] w-[48px]`, `origin-center`, `scale-110` while selected,
  `scale-100` otherwise (150 ms ease-out).
- Icon button: `absolute left-1/2 -translate-x-1/2 top-[10px] h-8 w-8
  rounded-lg border border-transparent`, icon `size 32`.
- Unread badge: `absolute right-1 top-0`, rendered only when
  `badge !== undefined` (i.e. `unreadCount > 0`).
- Label: `absolute left-1/2 -translate-x-1/2 top-[42px] w-[52px] text-xs
  leading-4 text-center truncate` (12 px font, 16 px line height); click,
  hover and keyboard-select behave like the icon.

Add-folder button (last scroll item): `mt-2 h-8 w-8 rounded-lg`, add icon
`size 28`, `aria-label` = `a11y.addFolder`.

### Visual rules (`folder-rail-visual.lib.ts`, `folder-colors.lib.ts`)

- System icons: `folders` / `profile` / `channels`. Custom folders: `folder`,
  `folder_open` while selected. Custom icon color is always the folder hex
  (`folderColorValueToCssHex`: `#RRGGBB` of `value & 0x00ffffff`).
- System icon/label tone: `text-text-muted` normally, `text-text-primary`
  while selected or hovered (vertical item: `verticalToneClass`).
- Custom label takes the folder color while selected or hovered.
- Hover surface (never while selected): custom = folder color at alpha 0.10
  background, border at alpha 0.22; system = `bg-sidebar-hover` with
  `border-border-subtle`. Selection carries on icon/label color + scale,
  there is **no** selected surface fill.
- Badge: shared `UnreadCountBadge`; the rail uses it unchanged at
  `right-1 top-0`.

### Selection semantics (`widgets/sidebar/sidebar-shell.ui.tsx:100-254`)

- `workspaceRailFolders` maps store folders to
  `{id, label: title, backgroundColor: backgroundColorValue ?? 0,
  badge: unreadCount > 0 ? unreadCount : undefined, systemType}`.
- Effective folder: the selected folder if still present, else the All folder,
  else the first folder (hidden selection falls back to All).
- Selecting a *different* folder clears the sidebar search query
  (`handleSelectWorkspaceFolder`: `if (folderId !== effective) setSearchQuery("")`).
- The sidebar stream list selector receives `selectedFolderUuid: effective`;
  membership comes from the server `folder_items`. While a search is active on
  the All folder the list keeps the folder-filtered set (no extra widening in
  the vertical rail path used here).

### Overflow quick list (`folder-rail-quick-list.ui.tsx/.lib.ts`)

Only rendered on overflow. Opens via the app hotkey `navigation.folderList`
("Folder list"). SearchInput auto-focuses and selects on open; substring
filter on lowercased label; before arrow keys the selected folder is
highlighted (first match), ArrowUp/Down clamp without wrap, Enter selects and
closes, selection resets query + highlight on close. Item background:
active `bg-accent/20`, selected `bg-accent/10`, else hover.

### i18n (en + `ru` both present)

| key | en | ru |
| --- | --- | --- |
| `folder.allChats` | All chats | Все |
| `folder.personal` | Personal | Личные |
| `folder.channels` | Channels | Каналы |
| `a11y.addFolder` | Add folder | Добавить папку |
| `folder.emptyFolder` | Folder is empty | Папка пуста |
| `folder.emptyFolderHint` | Add chats from a chat menu to populate this folder. | Добавьте чаты через меню чата, чтобы заполнить папку. |

## Native design

### Model on `NativeController`

- `QVariantList folders` (Q_PROPERTY, `foldersChanged`): ordered rail rows:
  `{key, id, title, systemType ("all"|"personal"|"channels"|"created"),
  color (uint 0xRRGGBB), unreadCount (int), streamIds (QStringList),
  synthesized (bool)}`. `key` is `all`/`personal`/`channels` for system slots
  and the UUID for created folders; it is the stable QML objectName key.
- Load path: `reloadSnapshot()` fires `loadFolders()` - a `GET
  /api/workspace/v1/messenger/folders/` through the existing API client with
  the same catalog-epoch guard as the other bootstrap collections. It is *not*
  part of the snapshot part counter: the rail must render from a degraded
  backend exactly like the stream catalog does (system slots always exist).
- Parsing accepts the snake_case DTO fields (`background_color_value`,
  `unread_count`, `system_type`, `folder_items[].stream_uuid`) and also the
  camelCase aliases. System type resolution mirrors the reference: the three
  fixed UUIDs force their type; reported `all` on another UUID is `created`.
- System folder synthesis: missing All / Personal / Channels rows are
  synthesized with the fixed UUIDs and localized titles. Synthesized rows are
  marked `synthesized: true` and scope the sidebar to all streams (they carry
  no server membership); server-provided rows always scope strictly by their
  `folder_items`. On request failure the synthesized trio remains, so the rail
  never goes empty and the sidebar is never filtered by a phantom folder.
- Order: All first (`orderedIndexedFoldersForRail` port), server order kept
  for the rest; synthesized Personal/Channels slots are inserted directly
  after All, above the server's custom rows, so the system group stays
  together in the degraded case exactly like the old static rail showed it.

### Selection and sidebar scoping

- `selectedFolderUuid` (Q_PROPERTY): defaults to the All UUID.
  `selectFolder(key)` resolves the row by `key` or UUID; selecting the
  effective folder is a no-op (no search clear); selecting another folder
  updates the property and emits `folderSelected()` which the QML uses to
  clear `sidebarQuery` (mirrors `handleSelectWorkspaceFolder`).
- `effectiveFolderUuid`: selected row if present in the current folder list,
  else All, else first row (hidden-folder fallback).
- `QVariantList sidebarStreams` (Q_PROPERTY): the existing `streams()`
  projection filtered to the effective folder's `streamIds` (all rows for the
  All slot and for synthesized slots). The sidebar ListView binds to it; every
  other consumer (topic nav, headers, archives) keeps the full `streams()`.

### QML rail

- `folderRail` keeps its 72 px column. Inside: the All slot is pinned, then a
  ListView (positional model over `controller.folders.length`, delegates read
  live state by index) renders the scrollable group plus the trailing
  add-folder button (`objectName: folderAddButton`, emits only - the creation
  flow is issue #84).
- Delegates reproduce the measured cell/card/icon/label geometry and the
  visual rules above with existing theme tokens: system hover uses
  `sidebar-item-hover` + `border-subtle`; custom hover uses the folder color
  at 0.10/0.22 alpha; selection uses scale 1.1 + colored icon/label, no fill.
- objectNames: `folder-<key>` (clickable cell), `folderBadge-<key>`,
  `folderAddButton`.
- Unread badge: same `badge-bg`/`badge-text` pill treatment as the sidebar row
  badge, placed at card `right-1 top-0`, visible only when `unreadCount > 0`.

## States covered

- Folders with and without unread badges; system and custom slots; selected
  scale/color states; search-clear-on-select; empty membership (custom folder
  without items shows an empty scoped list); fallback when the selected folder
  disappears from a refresh.
- Design fidelity case: parity-preserving port of the existing React surface;
  no redesign, no new theme tokens. Figma mockups were not consulted because
  no new chrome tokens were introduced (reference code is the primary oracle).

## Test acceptance

- Window CTest (`native-native-window`): mock server seeds All + Personal +
  Channels + one custom folder (color `0x2563eb`, `unreadCount` 7) whose
  `folder_items` hold one of two seeded streams. Asserts: rail renders All
  pinned first with all four cells; badge `7` visible on the custom folder;
  selecting the custom folder filters the sidebar list to the seeded stream
  and clears an active sidebar search; static disabled placeholders
  (`This filter is not implemented yet` tooltips) are gone.
- Controller CTest (`native-native-controller`): API parsing (snake_case +
  camelCase, fixed-UUID type forcing, `all` forcing on foreign UUID), badge
  passthrough, synthesis on empty/failed responses, order pinning, selection
  no-op vs clear signal, hidden-selection All fallback, `sidebarStreams`
  scoping for All/custom/synthesized slots.
- Locale guard: new `Add folder` string gets its `ru` entry in
  `packages/native/translations/workspace_ru.ts`.

## Deviations and follow-ups

- Overflow quick list (search + hotkey `navigation.folderList`) is not part of
  this slice: the native rail renders the full scrollable list without an
  overflow button, so no quick-list surface exists yet to trigger. Tracked as
  a follow-up (same issue or a child of #82) together with the context menu
  (rename/delete/layout, reference `folder-rail-context-menu.lib`).
- Folder creation stays disabled by contract: the add-folder button emits
  `folderAddRequested()` only; the creation flow is issue #84.
- Realtime folder mutation events (badge updates without reload) are not wired
  yet; `refresh()`/snapshot reload re-reads `/folders/`.
- The reference "search widens across All folder items while search is active"
  path (`workspaceAllStreams` in `sidebar-shell.ui.tsx:150-165`) applies to
  its cross-folder search model; the native sidebar keeps the simple
  folder-scoped filter plus the search clear on folder change, matching the
  selection semantics above.
