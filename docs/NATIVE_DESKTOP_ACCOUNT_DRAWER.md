# Account entry opens the right-drawer Account page (issue #724)

Status: draft (lane `cassi/account-drawer-drawer`).
Design oracle note: the Figma API returns 403 for the configured token on file
`Q3BtN9qwfQV8rAsyBAgCy1` ("Figma token unavailable - mockups not consulted").
The oracle is therefore the reference source. Pointers for a later manual Figma
pass (frame ids from the 2026-11-13 tree snapshot, page `design макеты (desktop)`):
Messenger `Профиль, настройки` 3199:6015, `Профиль, личная информация` 3368:15407,
`Профиль, версия приложения/сборка` 3368:16497, `right menu` 12697:37968 and 12697:37361.

## Reference contract (workspace_ui e0194ffb667986b58a12256ce41ca5dc99b56b83)

Issue #724 shipped without a Reference contract block; the values below were
measured from source at that sha (gap noted on the issue).

### Entry point and store model

- Trigger: `packages/web/src/widgets/top-bar/top-bar-profile-trigger.ui.tsx:85,111` -
  the signed-in name/avatar calls `useRightDrawerStore().toggleUserMenu()`.
- `packages/web/src/widgets/right-panel/right-drawer.model.ts`:
  - `toggleUserMenu()` (model:200-207): if the top layer kind is `account`,
    `toggleVisibility()` (history kept, drawer hides); otherwise `openUserMenu()`.
  - `openUserMenu()` (model:290-293): `openLayer({kind:"account", screens:[ACCOUNT_ROOT_SCREEN]})`
    where `ACCOUNT_ROOT_SCREEN = {mode:"user-menu", accountScreen:"root"}` (model:87-90).
    `openLayer` promotes an existing layer of the same kind to the top (unique-layer rule).
  - `openAccountScreen(screen)` (model:207-217): non-root requests from a non-account
    top layer push the account layer rooted at `root` first, then the screen - so Back
    from any nested account screen returns to the Account root, not to chat-info.
  - `back()` pops one screen inside the top layer only when the layer has >1 screen;
    `closeCurrent()` pops the whole layer and keeps the drawer open iff layers remain.
  - Modes are `info | settings | user-menu | about | personal-info`; account sub-screens
    `root | settings | appearance | about | personal-info` (model:7-12).

### Account root screen (`user-menu` mode, `accountScreen: "root"`)

`packages/web/src/widgets/right-panel/right-panel-user-menu.ui.tsx` - a flat section
list, "no card chrome (Figma right menu)", dividers as 1px hairlines inset 16 px
(`SECTION_LIST_CLASS`, `[&>*+*]:before:inset-x-4 before:bg-border-subtle`), section
labels via `SectionLabel tone="muted"` with `px-4`. Rows are `PanelMenuRow`
(`packages/web/src/shared/ui/panel-menu-row/panel-menu-row.ui.tsx`): root rows
`px-4 py-1.5`, inner min-height 32 px (row height ≈ 44 px), optional icon box 32x32
with 22 px glyph (`iconSize = 22` default), gap 8 px, hover `bg-sidebar-hover`,
text `text-text-primary`, subtitle muted, trailing chevron `chevron-right` 16 px
`text-text-secondary` (rotated up when expanded).

Order and grouping at the reference root (measured top-to-bottom):

1. Section header `settings.sectionDescription` ("Description" / "Описание"):
   - Current-server block: header `auth.currentServer` ("Current server" /
     "Текущий сервер") plus login line.
   - `externalAccounts.title` ("External accounts" / "Внешние аккаунты") -
     collapsible list with chevron-up/down (`externalAccountsOpen`).
   - `settings.personalInfo` ("Personal info" / "Личная информация") -> `personal-info` screen.
   - `settings.status` ("Status" / "Статус") with presence subtitle - opens the
     in-place status dialog (kept local, not a drawer screen).
2. Section header `settings.settings` ("Settings" / "Настройки"):
   - `settings.settings` row, subtitle `settings.notificationSound,
     settings.language, settings.authIdleTimeout` ("Notification sound, Language,
     Auto sign-out") -> `settings` screen.
   - `settings.appearance` ("Appearance" / "Внешний вид"), subtitle
     `settings.themeSettings, settings.folderLayout, settings.chatListDensity`
     ("Theme settings, Folder layout, Chat list density") -> `appearance` screen.
3. Section header `settings.appVersion` ("App version" / "Версия приложения"):
   - `settings.appVersion` row -> `about` screen (via the shell as accountScreen
     `about`, right-panel-shell.ui.tsx:47).
   - `settings.connectionDiagnostics` row ("Diagnostics" / "Диагностика") when the
     diagnostics flag is enabled.
   - `settings.clearCache` row, subtitle `settings.clearCacheHint`.
   - `auth.logout` row ("Logout" / "Выйти", aria `auth.logoutFromOrg`), danger tone.

Theme mode and palette controls appear only inside the `appearance` screen
(`right-panel-user-settings.ui.tsx` `RightPanelUserAppearance`) - once, not on the root.

i18n keys were dumped for both locales at the sha and are quoted inline above
(`packages/web/src/i18n/locales/en.json`, `ru.json`).

## Current native surface being replaced

- `qml/Main.qml`: `accountMenuButton` (~:3156) opens a floating `accountPopup`
  (:3815+) containing the current-server block, `NativeExternalAccountBlock`,
  Status, Personal info, Settings, Appearance, App version, Diagnostics, plus
  inline theme-mode/palette buttons, Language and disk-cache widgets; footer
  `signOutButton`. Settings/Appearance route through `openPreferencePanel(screen)`
  into `rightDrawer.openAccountScreen(screen)`; the About drawer's back handler
  re-opens the popup.
- `qml/NativeRightDrawer.qml` already ports the reference store 1:1 (layers/screens,
  `toggleUserMenu`, `openAccountScreen`, `back`, `closeCurrent`) - the `account`
  layer root simply has no content screen yet. The #666/#679 route gate keeps the
  drawer visible in non-chat sections while the account layer is on top, which is
  exactly what acceptance criterion 3 (Calendar/Mail) needs.

## Design decision

Port the reference user menu as a drawer content screen instead of a popup:

1. New `qml/NativeAccountMenu.qml`: the account root screen. Flat section list
   mirroring the reference order/grouping above with `NativeButton` rows using the
   established drawer row metrics; hairlines inset 16 px; section labels muted.
   It hosts the existing `NativeExternalAccountBlock`, the current-server labels,
   the disk-cache widget block, and all rows. objectNames are carried over from the
   popup unchanged (`currentServerLabel`, `currentServerLogin`, `ownStatusAction`,
   `personalInfoAction`, `accountSettingsAction`, `accountAppearanceAction`,
   `aboutAction`, `diagnosticsAction`, `signOutButton`, `languageSetting`,
   `language-en`, `language-ru`, scroll handle `accountMenuScroll`) so existing
   test seams and translations keep working. The inline theme-mode/palette buttons
   are removed from the root (criterion 2: available through Appearance once).
2. `Main.qml`: `accountMenuButton.onClicked` -> `rightDrawer.toggleUserMenu()`;
   `accountPopup` is deleted together with `preferencesFromAccountMenu` and the
   popup-restore `Connections` block (Back from Settings now pops to the account
   root inside the drawer, reference behaviour). The About drawer's back handler
   returns to the account layer root (`rightDrawer.openAccountScreen("root")`)
   instead of the popup. App version row routes to `rightDrawer.openAccountScreen("about")`.
3. Root subtitle deviations (native has no folder-layout / chat-density rows in this
   drawer yet): the Appearance row subtitle names what the native Appearance screen
   actually contains ("Theme settings"); recorded deviation, not a restyle.
4. Status behaviour keeps its existing dialog (reference parity: the status editor
   is a dialog, not a drawer screen).

## Test plan (same PR)

Window cases affected (grep of `accountMenuButton`/`ownStatusAction`/popup handles
in `tests/native-window-test.cpp`, ~30 call sites): most click-through cases keep
passing unchanged because every handle keeps its objectName and remains reachable
after one click on `accountMenuButton`; cases that pin popup-specific structure
(`accountPopup` QQmlExpression at :4658, press-outside close, popup scroll bounds
at :199) and the theme-control cases (:13875, :17125, :17193 - must now open the
Appearance row first) get updated expectations in this lane. New RED-first cases:

- `accountButtonOpensAccountRootInDrawer` - click opens the drawer with
  `topLayerKind == "account"`, `currentAccountScreen == "root"`, sections ordered
  as above, theme controls NOT on the root.
- `accountButtonTogglesAccountLayer` - second click hides the layer keeping history.
- `accountLayerSurvivesSectionSwitch` - account root visible from calendar and mail.
- `accountNestedBackRestoresRootAndFocus` - open Settings, Back -> root, focus on root.
- `accountAppearanceExposesThemeControlsOnce` - Appearance row reveals mode/palette.

New cases go through `scripts/gen-test-buckets.py` with timings of the new cases only.

## Live verification plan

`live_qa.py native-login` on the lane display, then xdotool: open the account
button in Chat, screenshot vs reference contract; switch to Calendar and Mail and
re-open; exit nested Appearance and Back. Evidence under `~/dev/qa/evidence/724/`.
