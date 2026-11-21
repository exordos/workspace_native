# Sidebar chat search keyboard access (issue #628)

Design fidelity: parity-preserving. This reproduces the pinned reference
behaviour; it proposes no restyle of existing features.

Reference: workspace_ui `75bd21f37de94728e2ebe100acfea24ac9c1a4e1` (read-only
clone), `packages/web/src/widgets/layout/layout-shortcuts.hook.ts`,
`layout-escape-navigation.hook.ts`, `widgets/sidebar/sidebar-workspace-search.lib.ts`,
`sidebar-search-header.ui.tsx`, `sidebar-workspace.css`, and the behaviour
contract in `e2e/sidebar.spec.ts` (+199 lines in this commit).
Figma token unavailable - mockups not consulted (the token endpoint returned a
403 "Token expired" at implementation time).

## 1. Measured reference contract

### 1.1 Chat-search hotkey (`layout-shortcuts.hook.ts`)

`useAppHotkey("navigation.chatSearch", ...)` with default binding `Mod+K`
(`shared/lib/hotkeys/hotkeys.lib.ts:15`). The handler
(`layout-shortcuts.hook.ts:37-50`):

1. If the sidebar is closed, mount it first (`flushSync(() => setSidebarOpen(true))`).
2. Then call `sidebarSearchRef.current?.toggle({ openOnly: !sidebarOpen })` -
   opening from the collapsed state is open-only (never toggles off).
3. The hotkey stays alive while the composer input has focus and while the
   search input itself has focus (`allowInputTarget`, `:46-48`:
   `isSearchInput(element) || element.closest('[data-focus-zone="composer"]')`);
   other text inputs keep their own keys.

### 1.2 Toggle and cancel (`sidebar-search-header.ui.tsx:17-46`)

- `toggle({openOnly})` (`:37-45`): if not openOnly and `cancel()` returns true,
stop (hotkey acts as an off-switch). Otherwise remember
`previousFocusRef = document.activeElement` and focus the input.
- `cancel()` (`:18-32`): returns false when there is no input, or when the
  input is unfocused and the query is empty (empty query falls through).
  Otherwise clears the query (`onSearchQueryChange("")`); if the input held
  focus it blurs and restores `previousFocusRef.current?.focus({ preventScroll: true })`.
  `previousFocusRef` is reset to null after every successful cancel, including
  the unfocused path, so a later session can never restore a stale target
  (PR #639 review finding: native hoists the reset out of the focused branch
  to match this contract).

### 1.3 Escape precedence (`layout-escape-navigation.hook.ts:25-49`)

Escape (not repeated, not composing, no modal open) first calls
`sidebarSearchRef.current?.cancel()` at `:29`; a true result consumes the
event before every other escape handler (the inbox-navigation fallback). A
second Escape with empty query and unfocused input returns false and falls
through to that fallback.

### 1.4 Arrow navigation (`sidebar-workspace-search.lib.ts:7-33`)

`moveWorkspaceSidebarSearchFocus` handles ArrowDown/ArrowUp only, ignoring
ctrl/meta/alt/shift and composing events (`:9-18`). It resolves the nearest
`[data-focus-zone="sidebar"]`, its `[data-sidebar-search-results]` container,
and the flat list of `[data-sidebar-search-item]` nodes inside it (`:20-25`) -
only the filtered, rendered results are in this list. Step is +1/-1
(`:28`). With the focus on the search input itself the index is -1, so
ArrowDown enters at `items[0]` and ArrowUp enters at `items.at(-1)` (`:29`).
At the first/last item the reference sibling lookup yields `undefined` and
focus stays (`:29-32`); the native ring instead wraps the index at both ends
(issue #628 acceptance requires cycling and the PR contract states
wrap-around; PR #639 review pinned this as the deliberate native deviation).
The event is always consumed while a result container exists.
`Enter` on an item opens it and keeps the query (`e2e/sidebar.spec.ts:302-311`).

### 1.5 Focus ring (`sidebar-workspace.css:1-12`)

A conversation row with keyboard focus (self or contained search item) gets
`background-color: var(--color-sidebar-item-hover)`, an inset ring
`outline: 2px solid var(--color-accent); outline-offset: -2px`, and
`transition: none` (`:1-9`); the inner search link drops its own outline (`:11-12`).
`e2e/sidebar.spec.ts:16-46` pins outline `solid` 2px at offset -2px with the
accent colour and the hover fill, no animations.

### 1.6 e2e behaviour pins (`e2e/sidebar.spec.ts:161-325`)

- From the composer with a hidden sidebar: hotkey expands and focuses the
  search (`:171-178`); a second hotkey press cancels - clears the query and
  returns focus to the composer draft (`:182-186`); hotkey again focuses,
  Escape restores the composer focus (`:188-191`); Escape while focus sits on
  another sidebar control clears the query and leaves that control focused
  (`:193-201`).
- Arrows walk stream rows and their matching topic rows in visual order and
  never land on filtered-out rows (`:260-301`); ArrowDown from the input
  enters at the first item, ArrowUp at the last (`:263-264`, `:316-317`).
- The focus ring is asserted in both palettes (`:265-268`, `:314-321`).
- Escape with results open clears the query and does not navigate (`:322-324`).

## 2. Native design

States: sidebar collapsed expands first; empty query falls through on Escape;
light+dark and ru+en unchanged; no new user-visible strings.

- Hotkey registry: add `navigation.chatSearch` with default `Mod+K` to the
  catalog in `src/native-hotkeys.cpp` (mirrors `hotkeys.lib.ts:15`). The chord
  passes the existing reservation rules (K is outside the reserved single-key
  set) and collides with no default. `docs/NATIVE_DESKTOP_HOTKEYS.md` and the
  catalog inventory counts in `tests/native-hotkeys-test.cpp` (19 actions,
  nine navigation rows) grow with the catalog by design; no behaviour
  expectation is relaxed.
- `qml/Main.qml` `handleAppShortcut`: the input-focus gate (`composer focus`
  block) gains the reference `allowInputTarget` exception for
  `navigation.chatSearch` (composer or sidebar search focused). The action
  branch mirrors the reference handler: expand `shortcutSidebarVisible` when
  collapsed (openOnly), then `toggleSidebarSearch(openOnly)`.
- `toggleSidebarSearch(openOnly)` and `cancelSidebarSearch()` port
  `sidebar-search-header.ui.tsx:17-46` 1:1: `previousFocus` is
  `root.activeFocusItem` captured at toggle time and restored with
  `forceActiveFocus()`; `cancel` clears `root.sidebarQuery`, discards the
  saved target on every successful cancel and only claims the event when the
  field has focus or the query is non-empty.
- Escape: `handleChatEscape` (the existing inbox fallback on the workspace
  RowLayout) calls `cancelSidebarSearch()` first, matching
  `layout-escape-navigation.hook.ts:29`; a second Escape falls through to the
  unchanged inbox fallback.
- Arrows: `sidebarSearchTargets()` (issue #648 fix round) builds the ring from
  the filtered model itself - `controller.sidebarStreams` rows and each row's
  `searchTopics` in delegate order - so every filtered chat/topic participates
  even when its delegate is below the fold or not incubated (the reference
  `querySelectorAll` likewise sees every `[data-sidebar-search-item]` node
  regardless of the viewport). `focusSidebarSearchTarget` scrolls the target's
  stream delegate with `streamList.positionViewAtIndex(row, ListView.Contain)`
  plus an item-level `contentY` nudge for topic rows spilling out of a tall
  delegate, then focuses it (retrying while the delegate incubates).
  `moveSidebarSearchFocus(from, step)` ports
  `sidebar-workspace-search.lib.ts:27-32` including the index -1 entry points
  (input: Down -> first row, Up -> last row); at the item edges native wraps
  the index at both ends instead of the reference no-op, per the issue
  acceptance "cycle" and the PR wrap-around contract (PR #639 review).
  Handlers are attached to `sidebarSearch` and both row button kinds via
  `Keys.onUpPressed` / `Keys.onDownPressed`, gated on a non-empty
  `normalizedSidebarQuery` (the
  native equivalent of the `data-sidebar-search-results` container) and on the
  ctrl/meta/alt/shift filter at `:10-18`.
- Focus ring: the rows already render the house keyboard-focus indicator
  (accent border bound to `visualFocus`, per the pattern pinned at
  `native-window-test.cpp` `servicesChatButtonShowsFocusIndicatorOnKeyboardFocus`
  and used by `streamButton`/`topicButton` today). Native keeps that house
  indicator instead of porting the DOM outline geometry; the behavioural
  contract (a visible accent ring on the keyboard-focused result row) holds.

## 3. Verification plan

Window group (native-native-window) QtTest cases added to `native-window-test.cpp`
before any production change, each watched failing:

1. `sidebarSearchKeyboardHotkeyFocusesAndExpandsSidebar`: the registered
   `Mod+K` focuses `sidebarSearch` (also from composer focus), and from the
   collapsed sidebar it expands the sidebar first, then focuses; a second
   press cancels back to the previous focus.
2. `sidebarSearchKeyboardArrowsCycleFilteredResults`: with a query that
   matches only some rows, ArrowDown/ArrowUp walk exactly the visible
   filtered rows (streams and their matching topics) in visual order, wrap-in
   from the input on both ends, and never focus a hidden row.
3. `sidebarSearchKeyboardEscapeClearsQueryAndRestoresFocus`: Escape clears
   the query and restores the previous focus; a second Escape falls through
   to the existing inbox fallback.
4. `sidebarSearchKeyboardEscapeWithQueryElsewhereClearsOnly`: Escape while a
   result row holds the focus clears the query and keeps that row focused
   (`e2e/sidebar.spec.ts:193-201`).

Catalog inventory updates in `native-hotkeys-test.cpp` accompany the new
`navigation.chatSearch` row. Existing pinned expectations are otherwise
untouched. Synthetic local HTTP/UI checks do not establish live Workspace,
installed-package, Wayland or Windows/macOS acceptance.

## Retry invalidation (PR #639 review finding 4652076279)

`focusSidebarSearchTarget` walks rows that may still incubate and retries
focus on `Qt.callLater`. Every navigation captures a
`sidebarSearchFocusSession` token into its target object; a retry whose token
no longer matches the current session drops itself before touching focus.
The session is bumped by: any query change, a successful `cancelSidebarSearch`
(including the empty-query case), `controller.sidebarStreams` changing, and
any change of the window's `activeFocusItem`. Pinned by
`sidebarSearchPendingFocusRetryInvalidatedByQueryChange`.
