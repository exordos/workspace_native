# Licenses page: open source packages browser (issue #46)

Part of the browser umbrella (issue #46). This slice ports the reference Open
Source Licenses browser (`/licenses`, `/org/:orgId/licenses`) to a native
section page. The dataset is new functionality: the native client's own
third-party dependencies, generated into a Qt resource at build time.

Figma token unavailable - mockups not consulted (the token endpoint answered
401). The reference implementation in the pinned workspace_ui checkout is the
sole geometry oracle; every value below was measured at
`61dacef78dab2f5d0be76f283bebf49fe29b84af`.

## Reference contract

- `packages/web/src/pages/licenses/licenses-page.ui.tsx` (162 lines)
  - Page root `flex h-full min-h-0 w-full flex-1 flex-col overflow-hidden
    px-6 py-4`: full width, 24 px left/right, 16 px top/bottom padding; no
    `max-w-narrow-page` cap.
  - Header row `mb-4 flex items-start justify-between gap-3` (16 px bottom
    margin, 12 px gap): title `h1 text-xl font-semibold text-text-primary`
    (20 px, DemiBold) with i18n key `licenses.title`; subtitle
    `mt-1 text-sm text-text-muted` (4 px top margin, 14 px) with key
    `licenses.subtitle` interpolated with the package count.
  - Close button `grid h-9 w-9 shrink-0 place-items-center rounded-lg
    text-text-muted transition-colors hover:bg-card-bg-active
    hover:text-text-primary` with `Icon name="close" size={18}` and
    `aria-label` = `common.close`. 36 x 36 px, 8 px radius.
    Close behaviour (test lines 47 and 56): history back when the history
    stack is non-empty; otherwise the resolved messenger route - session
    instance/project fallback `/project/<projectId>/messenger`, and `/` when
    no Workspace session is selected.
  - License-type filter row `mb-4 flex flex-wrap gap-2` (8 px gap, wraps):
    the top 8 types by package count (`licenseTypes.slice(0, 8)`; count
    descending, first-seen order breaks ties because the `Map` iteration is
    insertion-ordered and `Array.sort` is stable). Chip
    `rounded-full px-3 py-1 text-xs font-medium` (20 px radius, 12 px/4 px
    padding, 12 px text, Medium): selected `bg-accent text-on-accent`,
    unselected `bg-card-bg text-text-secondary hover:bg-card-bg-active`.
    Chip label is `{type} ({count})`. Clicking the selected chip toggles the
    filter off. While a filter is active a `Reset` chip
    (`rounded-full bg-card-bg px-3 py-1 text-xs text-text-muted
    hover:bg-card-bg-active`, key `licenses.reset`) clears it.
  - Search input `mb-4 w-full max-w-md rounded-lg bg-text-field-bg px-4
    py-2 text-sm text-text-primary outline-none placeholder:text-text-muted`
    (max width 448 px, 8 px radius, 16 px/8 px padding, 14 px text),
    placeholder key `licenses.searchPlaceholder`. A query matches when the
    lower-cased trimmed query is a case-insensitive substring of the entry
    `name` OR the entry `publisher`.
  - Counter line `mb-2 text-xs text-text-muted` (8 px bottom margin, 12 px):
    key `licenses.packagesOf` = "{filtered} of {total} packages". The empty
    filter result renders no table rows and the "0 of N packages" counter.
  - Table `w-full text-left text-sm` inside a scroll area
    (`min-h-0 flex-1 overflow-y-auto`); `thead sticky top-0 bg-bg text-xs
    text-text-muted`; 4 columns with keys `licenses.package`,
    `licenses.version`, `licenses.license`, `licenses.author` (th padding
    `pb-2 pr-4`: 8 px bottom, 16 px right). Rows `border-t
    border-border-subtle` (1 px top border); cells `py-2 pr-4` (8 px).
    Package cell: external link `text-accent hover:underline`
    (`target="_blank" rel="noopener noreferrer"`) when `repository` is
    non-empty, otherwise plain `text-text-primary` text. Version and author
    cells are `text-text-muted`. License cell chip `rounded bg-card-bg
    px-2 py-0.5 text-xs text-text-secondary` (4 px radius, 8 px/2 px
    padding, 12 px text).
- `packages/web/src/pages/licenses/licenses-page.types.ts`: entry shape
  `{name, version, license, repository, publisher}` (all strings).
- `packages/web/src/generated/licenses.json` is the npm-app dataset; it is an
  example shape only and must not be copied (issue #46 contract).
- Close-route helpers: `shared/lib/last-messenger-route.lib.ts`
  (instance/project fallback) and `shared/lib/navigation-history`.
- i18n keys (`packages/web/src/i18n/locales/{en,ru}.json`):
  - `licenses.title` = "Open Source Licenses" / "Open Source лицензии"
  - `licenses.subtitle` = "Workspace uses {count} open source packages" /
    "Workspace использует {count} open source пакетов"
  - `licenses.reset` = "Reset" / "Сбросить"
  - `licenses.searchPlaceholder` = "Search packages…" / "Поиск пакетов…"
  - `licenses.packagesOf` = "{filtered} of {total} packages" /
    "{filtered} из {total} пакетов"
  - `licenses.package` = "Package" / "Пакет", `licenses.version` =
    "Version" / "Версия", `licenses.license` = "License" / "Лицензия",
    `licenses.author` = "Author" / "Автор"
  - `common.close` = "Close" / "Закрыть"
- Routes: `app-route-definitions.tsx` maps `/licenses` and
  `/org/:orgId/licenses` to this page. The native deeplink allowlist already
  accepts both shapes (`src/native-deeplink.cpp:73-78`); nothing rendered
  them until this slice.

## Palette (blue-cold, `packages/web/src/shared/lib/themes/blue-cold.ts` +
`tokens.ts`; mirrored 1:1 by `qml/NativeTheme.qml`)

| Token | Light | Dark |
| --- | --- | --- |
| `bg` | `#e4ecf3` | `#141517` |
| `bg-elevated` | `#ffffff` | `#222328` |
| `card-bg` | `#eef5fb` | `#282a32` |
| `card-bg-active` | `#cde6ff` | `#2c3747` |
| `text-field-bg` | `#eef5fb` | `#222328` |
| `text-primary` | `#1b1b1d` | `#ffffff` |
| `text-secondary` | `#787878` | `#999999` |
| `text-muted` | `#989898` | `#707070` |
| `accent` | `#7087ff` | `#7087ff` |
| `on-accent` | `#1b1b1d` | `#141517` |
| `border-subtle` | `#d8e4ef` | `#33384d` |

Tailwind sizes used by the page: `text-xl` = 20 px, `text-sm` = 14 px,
`text-xs` = 12 px, `font-semibold` = DemiBold (600), `font-medium` = Medium
(500), `rounded-lg` = 8 px, `rounded` = 4 px, `rounded-full` = capsule,
`h-9`/`w-9` = 36 px, `gap-2` = 8 px, `mb-4` = 16 px, `mb-2` = 8 px,
`px-6`/`py-4` = 24 px/16 px, `px-4` = 16 px, `py-2` = 8 px, `px-3`/`py-1` =
12 px/4 px, `px-2`/`py-0.5` = 8 px/2 px, `max-w-md` = 448 px.

## Native design

- New `qml/NativeLicensesPage.qml`: presentation-only section page
  (precedents: `NativeCallsPage` for #37, `NativeInboxPage`). Root
  `FocusScope` object name `nativeLicensesPage`, `required property var
  colors`. Children: `licensesTitle`, `licensesSubtitle`, `licensesClose`,
  `licensesChipRow` (Flow of `licensesChip-<type>` buttons plus
  `licensesResetChip`), `licensesSearch`, `licensesCounter`,
  `licensesHeaderRow`, `licensesList` (ListView of `licensesRow-<index>`),
  row cells `licensesName-<index>`, `licensesVersion-<index>`,
  `licensesLicense-<index>`, `licensesAuthor-<index>`.
- The packaged Linux page reads the catalog next to the bundled third-party
  notices. Packaging generates one row per source package and version in
  `bundled-libraries.json`, so the version includes the distribution
  revision. Known components use the established display names, publishers
  and license labels. Other packages link to their recorded Ubuntu source
  page and refer to the bundled notice. Qt WebEngine and Chromium require
  corresponding inventory evidence.
- For unpackaged builds CMake embeds the versions of the libraries it found.
  Window tests use a fixed catalog to exercise filtering and chip ordering.
- Entry surfaces:
  - `NativeAboutDrawer.qml` gains an "Open Source Licenses" entry link
    (`nativeAboutLicensesLink`) below the update card emitting
    `licensesRequested()`; `qml/Main.qml` closes the drawer and calls
    `controller.showSection("licenses")`.
  - `NativeController::applyDeepLink` maps `(?:/org/<org>)?/licenses` to
    `showSection("licenses")` with the same org guard as the `/calls` route.
  - `showSection` learns the `licenses` section (`ContentPage::Licenses`);
    leaving to chat or another section closes it like the other pages.
- Close handoff: the page exposes `property bool canGoBack` plus signals
  `backRequested()` / `messengerHomeRequested()`; `close()` picks per the
  reference tests (history back when available, else the resolved messenger
  home). `qml/Main.qml` records the section active before `licenses` opened:
  a non-chat predecessor makes `canGoBack` true and back restores that
  section; otherwise close resolves to the chat section (the native
  messenger home).
- External links: a package link click emits `externalLinkRequested(url)` and
  `qml/Main.qml` hands the URL to `Qt.openUrlExternally` (the QDesktopServices
  equivalent already used by `NativeMarkdown::openExternal` and
  `Main.qml:1316`). Tests stub the handoff through
  `QDesktopServices::setUrlHandler` on the same path.
- Electron: n/a.
- Filtering: top 8 license types by count (count descending, first-seen
  order on ties), identical `name`/`publisher` case-insensitive substring
  match, `{filtered} of {total}` counter, toggle-off and Reset chip. The
  empty result keeps the table area with no rows and the "0 of N" line.

## States and tests

- States: light + dark, ru + en, populated list, filtered list, and the
  empty-filter-result state ("0 of N packages", no rows).
- CTest `native-native-window` (`tests/native-window-test.cpp`): page states
  (title/subtitle fonts and colors, counter, chip counts/top-8 cap, chip
  toggle + reset, search by name and by publisher, empty result), close
  behaviour (back vs messenger fallback) per reference tests, and the stubbed
  external-link handoff, across both locales.
- CTest `native-about-policy`: the drawer entry link exists, is labeled with
  `licenses.title`, and emits `licensesRequested()`.
- Tooling (direct `python3 packages/native/tests/licenses-dataset-test.py`,
  not registered in CTest so the CTest group count stays stable): generator
  determinism (two runs byte-identical), entry shape, required native
  dependencies present (Qt6, md4c, SQLite, keychain), sort order, and a
  license-type census that exercises the top-8 cap.
- i18n: every new `qsTr` string is added to
  `packages/native/translations/workspace_ru.ts` (contexts
  `NativeLicensesPage` and `NativeAboutDrawer`), keeping the
  `native-native-locale` catalog guard green.
