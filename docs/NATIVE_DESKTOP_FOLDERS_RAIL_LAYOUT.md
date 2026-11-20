# Folders rail layout options: vertical/horizontal layouts and system-folder visibility

Issue #85 (epic #82). Extends the folder rail from #83/#84
(`docs/NATIVE_DESKTOP_FOLDERS_RAIL.md`, `docs/NATIVE_DESKTOP_FOLDERS.md`)
with the two persisted rail display options toggled from the folder context
menu: the vertical/horizontal rail layout and the show/hide-system-folders
visibility.

Design fidelity case: parity-preserving port of the existing React surface;
no redesign, no new theme tokens. Figma token unavailable - mockups not
consulted (`FIGMA_TOKEN` returns HTTP 401 against `api.figma.com` since
2026-11-16); the pinned reference code is the oracle.

## Reference contract (measured)

Oracle: `workspace_ui` at sha `61dacef78dab2f5d0be76f283bebf49fe29b84af`
(`~/dev/workspace_ui`). All values below were read from the source at that
sha.

### Settings (`packages/web/src/features/settings/settings.model.ts`)

- `folderRailLayout: "vertical" | "horizontal"`, default `"vertical"`
  (`createDefaultSettings`); the load resolver is
  `value === "vertical" ? "vertical" : "horizontal"` - with a stored blob,
  anything that is not exactly `"vertical"` (including a missing or corrupt
  value) resolves to `"horizontal"`, while a virgin store keeps `"vertical"`.
- `showSystemFolders: boolean`, default `true`; the resolver accepts exactly
  `true`/`false` and falls back to the default for anything else.
- Both live in the one global settings blob (`persistSettings`), not a
  per-session scope. `setFolderRailLayout` / `setShowSystemFolders` write the
  store and persist immediately.
- `folder-rail.ui.tsx::handleToggleLayout` flips the layout
  (`horizontal -> vertical -> horizontal`); `handleToggleShowSystemFolders`
  flips the flag. Both changes are live bindings, so the rail re-renders
  without a reload.

### Horizontal view (`folder-rail-horizontal-view.ui.tsx`)

- Root: `flex h-11 w-full items-center gap-1 overflow-x-auto overflow-y-hidden
  px-2 py-1 select-none scrollbar-none`, `cursor-grab` while idle and
  `cursor-grabbing` while dragging. Measured: 44 px tall strip, 4 px row gap,
  8 px horizontal / 4 px vertical padding, full width, horizontally
  scrollable, vertically clipped.
- Items keep server order with All pinned first (the same
  `orderedIndexedFoldersForRail` used by the vertical view).
- Add-folder button is the last item: `h-8 w-8 rounded-lg`, add icon
  `size 24`, `aria-label` = `a11y.addFolder`, and it is excluded from the
  drag capture.
- Pointer-drag scrolling, all handlers on the root in capture phase:
  - `pointerdown` starts a drag only for mouse pointer type, left button,
    and only when the target is not inside `[data-folder-rail-action='add-folder']`.
  - `pointermove` marks the gesture *moved* once `|deltaX| >= 3` px and then
    sets `scrollLeft = startScrollLeft - deltaX`.
  - `pointerup`/`pointercancel` end the drag; if the gesture moved, the next
    click inside the strip is suppressed exactly once (`onClickCapture`
    calls `preventDefault`/`stopPropagation`), and the add-folder click is
    never suppressed.

### Horizontal item (`folder-rail-folder-items.ui.tsx:226-240`)

- `flex h-8 shrink-0 items-center gap-1.5 rounded-lg border border-transparent
  px-2.5 text-xs`: 32 px tall chip, 8 px radius, 10 px horizontal padding,
  6 px icon/label gap, 12 px font.
- Icon `size 18`; label `max-w-[112px] truncate` (12 px cap at 112 px).
- Unread badge: same shared badge as the vertical item, anchored
  `-right-1 -top-2.5` on the chip wrapper, rendered only when `badge` is set
  (`unreadCount > 0`).
- Visual rules (surface/icon/label tones) are the shared
  `getFolderItemVisualState` already ported for the vertical rail: hover
  surface only, selection carries on icon/label color, no selected fill.

### Rail mount (`sidebar-shell.ui.tsx`)

- `layout === "vertical"` (default): the rail sits beside the sidebar frame
  (native: the 72 px `folderRail` column in the `workspace` row).
- `layout === "horizontal"`: the sidebar frame renders alone and the rail
  mounts inside the sidebar scroll area through `activityPanelBottomSlot`
  (`sidebar-workspace.ui.tsx:819-825`): directly below the activity panel,
  followed by a divider block `my-2` (8 px + 1 px `border-subtle/70` line +
  8 px), above the state cards and the stream rows.

### Context menu (`folder-rail-context-menu.lib.ts`)

Item order: Rename, toggle-layout, toggle-system-folders, Delete. The two
toggles are always enabled (also on system folders); the label states the
action the click performs:

| key | en | ru |
| --- | --- | --- |
| `folder.displayVertical` | Display vertically | Отобразить вертикально |
| `folder.displayHorizontal` | Display horizontally | Отобразить горизонтально |
| `folder.showSystemFolders` | Show system folders | Показать системные папки |
| `folder.hideSystemFolders` | Hide system folders | Скрыть системные папки |

While `folderRailLayout === "horizontal"` the menu offers
`displayVertical` and vice versa; while `showSystemFolders` it offers
`hideSystemFolders` and vice versa.

### System-folder visibility

At the oracle sha the flag is persisted, the menu label flips and the value
is plumbed into both rail views, but no view filters rows on it. The issue
contract fixes the native behavior: hiding system folders removes the
Personal and Channels slots from the rail while the All chats slot stays
pinned and the effective selection still falls back to All chats for the
chat list (the #83 `effectiveFolderUuid` rule is untouched: the filter is
display-only, selection and sidebar scoping never change).

## Native design

### Persistence (`src/native-preferences.cpp/.h`)

`folderRailLayout` and `showSystemFolders` join the global chat-list layout
keys from #102 (`settings/app/chatListDensity`, `chatClickAction`) in the
one global settings scope - the native mirror of the reference global
settings blob:

- `settings/app/folderRailLayout`: absent -> `"vertical"` (virgin store);
  stored value strictly `"vertical"` -> vertical, any other stored value ->
  `"horizontal"` (the reference resolver, corrupt-input default included).
- `settings/app/showSystemFolders`: stored `"true"`/`"false"` strictly;
  absent or malformed -> `true`.
- Both expose Q_PROPERTY with WRITE setters that sanitize through the same
  resolver, write, `sync()` and emit on change; read live by QML through
  `root.preferences` like `chatListDensity`.

### Rail views (`qml/Main.qml`)

- `root.folderRailHorizontal: preferences.folderRailLayout === "horizontal"`.
- The vertical `folderRail` column gets `visible`/`Layout.preferredWidth`
  gated on `!folderRailHorizontal` (0 px when horizontal, so the workspace
  row loses the 72 px gutter exactly like the reference remount).
- New `folderRailHorizontal` Item inside the sidebar (objectName
  `folderRailHorizontal`), `visible` only in horizontal mode: 44 px high,
  full sidebar width, anchored below the activity panel at
  `(query ? 60 : 104) + activityExpandedDelta` and followed by the divider
  (8 px + 1 px `border-subtle` + 8 px). `streamList` and `sidebarStateBlock`
  add a `sidebar.folderRailInset` (61 px) top offset while it is visible.
- Inside: a non-interactive `Flickable` (`contentWidth = row + 16`,
  `boundsBehavior: StopAtBounds`, no visible scrollbar - the reference hides
  it) holding the `Row` of chips with the measured spacing (4 px gap, 8 px
  side padding, chips vertically centered in the 44 px strip).
- Chip delegate `folder-<key>`: 32 px tall, radius 8, 10 px padding, 6 px
  gap, icon 18 px, `maximumWidth: 112` elided label, badge
  `folderBadge-<key>` at the measured `-right-1 -top-2.5` anchors. It has
  no click handler of its own: the rail-level `MouseArea` owns input so a
  drag can start on a chip exactly like the React capture-phase handlers.
- Rail `MouseArea` (left + right button): on left press it records the chip
  under the cursor and the scroll offset; movement `>= 3` px marks the drag
  and sets `contentX = startContentX - deltaX`; on release it selects the
  pressed folder (after `closeDrafts()`, like the vertical cell) only when
  the gesture never crossed the 3 px threshold - that is the port of "one
  click suppressed after a drag". Right press opens the existing
  `folderContextMenu`. Cursor is `OpenHandCursor`/`ClosedHandCursor` per
  the grab state; a plain wheel's horizontal component scrolls the strip.
- The add-folder button stays a real `NativeIconButton` (`folderAddButton`)
  as the last row item, icon 24 px: because it consumes its own press the
  rail drag never starts on it and its click never gets suppressed - the
  same two guarantees as the reference exclusion, reached through the
  button's own grab.
- Rail rows: both views iterate a new `root.folderRailRows` projection of
  `controller.folders` that drops the `personal`/`channels` rows while
  `preferences.showSystemFolders === false` (All and created folders stay;
  All remains the pinned first slot).

### Context menu

`folderContextMenu` gains the two reference items between Rename and Delete:

- `folderMenuToggleLayout`: text `Display vertically` while horizontal else
  `Display horizontally`, icon `folders.svg`, writes the flipped
  `preferences.folderRailLayout`.
- `folderMenuToggleSystemFolders`: text `Hide system folders` while shown
  else `Show system folders`, icon `folder.svg`, flips
  `preferences.showSystemFolders`.

Both are always enabled; rename/delete keep their #84 system-folder gating.

### Quick-list hotkey

The reference overflow quick list binds hot action `navigation.folderList`,
whose default sequence is `null` (`hotkeys.lib.ts:23` - unassigned until the
user assigns one), and the native client has no quick-list surface yet at
all (#83 follow-up). There is no native binding to keep working in
horizontal mode, so the requirement is satisfied by introducing no shortcut
here; when the quick list lands it must register in both modes.

## States covered

- Vertical (default) and horizontal rendering; toggling from the folder
  context menu in either direction; the choice reloaded by a recreated
  preferences object and a fresh window (restart parity).
- Pointer-drag scrolling of the horizontal strip past the 3 px threshold,
  the trailing click suppressed after a drag, and an unmoved press still
  selecting the folder; the add-folder button clickable. The test waits for the rebuilt chips to be positioned in a presented frame before pressing their centers.
- System folders hidden: Personal/Channels gone from both rail views, All
  and custom folders stay, the sidebar keeps the All-chats effective
  selection (stream list unchanged), and the flag persists.
- Long folder lists (overflow drag), light + dark, en + ru.

## Test acceptance

- `native-native-preferences`: round-trip of both keys in the
  `settings/app/` scope across recreated `NativePreferences`, with the
  strict reference resolvers (non-`"vertical"` stored layout -> horizontal,
  non-boolean stored visibility -> show).
- `native-native-window` (extends the #83/#84 folder cases): toggling
  Display horizontally from the folder context menu re-renders the 44 px
  strip (geometry oracle at scale 1), the choice survives a rebuilt window
  from the same settings file; dragging the strip scrolls it and the click
  at the drag end does not change the selection; hiding system folders
  removes Personal/Channels while the sidebar keeps both seeded streams
  under the All-chats fallback, and showing them again restores the slots.
- Locale guard: the four new `qsTr` strings carry `ru` entries in
  `packages/native/translations/workspace_ru.ts`.

## Deviations and follow-ups

- The horizontal strip uses a Qt `Flickable` + rail-level `MouseArea`
  instead of DOM capture-phase pointer handlers; the observable contract
  (3 px threshold, one suppressed trailing click, add-folder exclusion) is
  ported 1:1, while inertial touch scrolling is Qt-default and the browser
  scroll-behavior is not reproduced.
- The reference `overflow-x-auto` scrollbar is hidden there
  (`scrollbar-none`); the native strip has no scrollbar and no keyboard
  scrolling, only drag and the wheel's horizontal component.
- The divider line under the horizontal rail uses the opaque
  `border-subtle` token (the reference applies `/70` alpha at that one
  spot); no new token was introduced.
- Folder *creation* remains the only rail surface not draggable-adjacent:
  #84's `folderAddButton` semantics are unchanged in the horizontal strip.
- Realtime folder list changes keep re-reading `/folders/` on `refresh()`
  (#83 behavior); neither toggle touches the network.
