# Native right drawer shell (issue #129)

Part of #121. This document specifies the right drawer shell infrastructure:
geometry, chrome, and the layer/screen navigation stack. Content layers
(partner/channel info, #72) mount on top of this shell later; the entry point
from the chat header is tracked by #72.

Reference contract: workspace_ui `61dacef78dab2f5d0be76f283bebf49fe29b84af`:

- `packages/web/src/widgets/right-panel/right-drawer.ui.tsx:11-84` (frame,
  close, back, content flush),
- `packages/web/src/widgets/right-panel/right-drawer.model.ts:90-408`
  (layers, promotion, dedup, chat scope reset),
- `packages/web/src/widgets/right-panel/right-drawer.types.ts:3-19`,
- width token `packages/web/tailwind.config.ts:84` (`w-panel-right` 315 px),
- contracts: `right-drawer.test.tsx` ("uses rounded shell geometry aligned
  with the left sidebar", "keeps close button when title is omitted"),
  `right-drawer.model.test.ts` ("collapses and expands without deleting layer
  or screen history", "X closes the active layer and reveals the previous
  one", "Back pops only internal screen history", "promotes an existing
  account layer instead of duplicating it", "retains navigation for the same
  stream and resets hidden chat screens on a stream/runtime change").

## Geometry (reference `right-drawer.ui.tsx`, `tailwind.config.ts:84`)

The reference aside is
`relative flex min-h-0 w-panel-right flex-shrink-0 flex-col overflow-hidden rounded-lg bg-bg-elevated px-2 pb-5 pt-2`:

| Reference class | Token | Native value |
| --- | --- | --- |
| `w-panel-right` | 315 px | fixed `width: 315` |
| `rounded-lg` | aligned with the left sidebar shell | shell `Rectangle.radius: 6` (same as the native left sidebar and `NativeAboutDrawer`) |
| `bg-bg-elevated` | dark `#222328`, light `#ffffff` | `colors["bg-elevated"]` |
| `px-2 pb-5 pt-2` | 8/20/8 px | content margins left/right 8, top 8, bottom 20 (identical to `NativeAboutDrawer`) |
| `overflow-hidden` | body clips | body slot `clip: true` |

The header row is `flex items-center gap-2 px-2 py-1`: a single row with
8 px spacing and 8/4 px padding, total height 40 px (as in `NativeAboutDrawer`).

Header children, in order:

1. Back chevron (`chevron-right.svg` rotated 180 deg): 32x32 hit box, 16 px
   glyph, visible only when the active layer has a deeper internal screen.
   `objectName: nativeRightDrawerBack`, label `common.back` (en "Back",
   ru "Назад").
2. Optional title: `min-w-0 flex-1`, truncated (`elide: Text.ElideRight`),
   `text-base font-semibold text-text-primary`. When absent the close button
   stays (reference "keeps close button when title is omitted").
3. Close (`close.svg`): always present, 32x32 hit box, 16 px glyph,
   `objectName: nativeRightDrawerClose`, label `common.close` (en "Close",
   ru "Закрыть"). Hover state `card-bg-active` (dark `#2c3747`, light
   `#cde6ff`), text `text-muted` -> `text-primary` on hover.

Body slot: `flex min-h-0 flex-1 flex-col overflow-hidden` with optional
`contentFlush` cancelling the shell `px-2` for edge-to-edge children
(`-mx-2` equivalent: left/right margin -8 on the body slot).

The aside gets `Accessible.role: Accessible.Complementary` and
`Accessible.name: qsTr("Info panel")` (reference `a11y.infoPanel`,
en "Info panel", ru "Панель информации").

## Shell mounting

New `packages/native/qml/NativeRightDrawer.qml`, modelled on
`NativeAboutDrawer.qml`, mounted in `Main.qml` beside the existing
`aboutDrawer` (Main.qml:61). It docks right under the top bar with the same
anchors as `aboutDrawer`; the workspace row gains the same right margin it
already gives `aboutDrawer` so the drawer docks beside the chat instead of
overlaying it. The drawer is a single app-scoped instance that stays mounted
when hidden (`visible: drawerOpen`). Escape closes the top layer and is
accepted so no other handler consumes it.

## Layer model (reference `right-drawer.model.ts:90-408`)

The stack lives QML-side inside `NativeRightDrawer.qml` (the issue allows
NativeController or a small QML-side stack; the controller file is kept
untouched because the entry points land later with #72).

State mirrors the reference store:

- `drawerOpen: bool` — visibility is independent from navigation history so
  collapse never deletes it.
- `layers: [ { kind: "chat-info" | "account" | "about", screens: [...] } ]`
  where a screen is `{ mode, accountScreen?, chatScreen?, profileOverride? }`
  exactly as in `right-drawer.types.ts`:
  - modes `info | settings | user-menu | about | personal-info`;
  - account screens `root | settings | appearance | about | personal-info`;
  - chat screens carry a scope `{ ownerKey, runtimeGeneration, streamUuid }`
    plus `screen` and optional `keyUuid`;
  - profile override is `legacy-user-id` (int) or `workspace-user-uuid`
    (uuid string).

Semantics ported 1:1 from `right-drawer.model.ts`:

- `setOpen(open)`: opening with an empty stack seeds one `chat-info` root
  layer; closing only flips visibility.
- `toggleVisibility()`: empty stack opens `chat-info`; otherwise flips
  visibility and keeps all layers and screens.
- `toggleChatInfo()`: collapses when the top layer is already `chat-info`,
  otherwise opens it (chat header affordance contract).
- `openLayer(kind)` (promotion): if a layer of the kind exists it is moved to
  the top instead of appended, preserving its internal screens; pushing the
  top kind again replaces the top with the requested root screens (so
  `openInfo` resets nested profile screens without touching older layers).
- `openScreen(screen)`: appends onto the top layer, but is a no-op when the
  screen equals the current top. Equality (`sameScreen`) compares mode,
  account screen, chat scope + chat screen + key uuid, and profile override
  kind+id, as in the reference `sameScreen`/`sameProfileOverride`.
- `back()`: pops only the top layer's internal screens; with a single screen
  it is a no-op (layers are closed with X, never with Back).
- `closeCurrent()` (X): removes the top layer and reveals the previous one;
  removing the last layer hides the drawer and leaves an empty stack.
- `openAccountScreen(screen)` / `openChatScreen(chatScreen)`: ensure the
  matching layer kind is on top first, then `openScreen`. `openAccountScreen`
  always applies the screen request after promoting an existing account layer
  (so a `root` request lands on the account root instead of a stale nested
  screen; the dedup in `openScreen` keeps fresh creation a single root
  screen). `openChatScreen`
  additionally reuses an already-listed screen instead of adding a Back loop
  (reference "keeps local-key UUIDs distinct and returns to the existing list
  without a Back loop") and drops a `local-key` push without a `keyUuid`.
- `resetChatScreens(scope)`: drops screens whose chat scope differs from the
  current `{ownerKey, runtimeGeneration, streamUuid}` (selected conversation
  or runtime generation changed); a layer that loses all chat screens keeps
  only its `chat-info` root screen.
- `clearAll()`: hides the drawer and deletes all history (logout/reset).

Computed read-only projections (used by the shell chrome and the CTest):
`topLayerKind`, `topScreenCount`, `canGoBack` (`topScreenCount > 1`),
`currentMode`, `currentAccountScreen`, `currentChatScreen`,
`currentChatKeyUuid`, `currentProfileOverride`.

## Native chat scope wiring

`Main.qml` derives the current scope as
`{ ownerKey: controller.ownerKey, runtimeGeneration: <login generation>,
streamUuid: controller.selectedStream }` and calls `resetChatScreens(scope)`
whenever the owner, the login generation, or the selected conversation
changes. The login generation is a QML counter that increments on each new
authenticated session, mirroring the reference runtime generation: switching
conversations or re-login drops hidden chat screens and reverts the chat-info
layer to its root, while non-chat layers (account) keep their screens.

## Test plan (native-native-window group)

Extended `tests/native-window-test.cpp` (window group; reference file names
are cited inside the geometry assertions):

1. Shell geometry: drawer opens at 315 px (`w-panel-right`), right-docked,
   shell radius 6 aligned with the left sidebar, shell paints
   `bg-elevated`, body slot clips (reference `right-drawer.ui.tsx` aside).
2. Header: close keeps a 32x32 hit box with a 16 px glyph and stays visible
   when the title is empty; back only appears while a deeper screen exists
   and is 32x32 (reference back/close blocks); en + ru labels.
3. Promotion: opening `chat-info` again promotes instead of duplicating
   (single `chat-info` layer); closing layers reveals the previous one and
   the last close hides the drawer.
4. Back pops nested screens only (account and chat flows), three-screen chat
   flow walks back without loops.
5. Dedup: pushing the identical top screen (account screen, chat scope,
   profile override) is a no-op.
6. Collapse: hiding the drawer (toggle) preserves layer and screen history;
   re-opening restores the nested screen; `toggleChatInfo` opens then closes.
7. Chat scope reset: a real conversation switch (mock server with two
   channels) drops nested chat screens of the previous conversation and
   keeps the chat-info root; a runtime generation change does the same;
   the same conversation keeps its navigation.
8. Escape collapses: first Escape removes the top layer revealing the
   previous; second Escape hides the drawer.

i18n: new `NativeRightDrawer` context strings `Close`, `Back`, `Info panel`
are added to `packages/native/translations/workspace_ru.ts` (locale guard).
