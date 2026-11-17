# Services page: planned services placeholder hub (issue #45)

Parity slice reproducing the reference static Services hub shipped at routes
`/services` and `/all-services`. The page is fully static: it issues no
backend requests and has no async states. Native reproduces it as-is; no
service functionality is invented that the reference does not ship.

Figma token unavailable - mockups not consulted. The reference implementation
in the pinned workspace_ui checkout is the sole geometry oracle; every value
below was measured at `61dacef78dab2f5d0be76f283bebf49fe29b84af`.

## Reference contract

- `packages/web/src/pages/services/services-page.ui.tsx` (whole file) and
  `services-page.types.ts`: static page, no backend calls; the page test
  (`services-page.test.tsx`) asserts the heading, a card title, a
  "Coming soon" chip, and that clicking "Chats & channels" navigates back
  (`services-page.test.tsx:31-36`).
- `packages/web/src/app/app-route-definitions.tsx:166-167` (`/services`,
  `/all-services`) and `:203-204` (the same two routes under
  `/org/:orgId`). Both map to `ServicesPage`.
- Entry points: the reference top bar hides the Services button behind a
  build flag (`packages/web/src/widgets/top-bar/top-bar.test.tsx:153`,
  "does not render calls or services nav buttons by default"). Default
  reachability is route/deeplink only, so **native adds no visible section
  button**; the whitelisted deeplink / route entry is the only surface, and
  the section state is reached without a top-bar chip.
- `openMessenger` (services-page.ui.tsx:74-82) navigates to the workspace
  messenger root route, with app root as fallback. The native counterpart
  switches back to the chat section (the section home), like the top-bar
  Chat button.

### Six cards, fixed order (services-page.ui.tsx:13-50)

| # | id | icon asset | title key | hint key |
|---|----|-----------|-----------|----------|
| 1 | knowledge-base | `files` -> files.svg | `settings.servicesPlaceholderKnowledgeBase` | `...KnowledgeBaseHint` |
| 2 | people-directory | `group` -> group.svg | `settings.servicesPlaceholderPeople` | `...PeopleHint` |
| 3 | approvals | `handshake` -> handshake.svg | `settings.servicesPlaceholderApprovals` | `...ApprovalsHint` |
| 4 | docs | `folder` -> folder.svg | `settings.servicesPlaceholderDocs` | `...DocsHint` |
| 5 | automation | `businessCenter` -> business_center.svg | `settings.servicesPlaceholderAutomation` | `...AutomationHint` |
| 6 | integrations | `links` -> links.svg | `settings.servicesPlaceholderIntegrations` | `...IntegrationsHint` |

(The native repo already ships all six SVGs in
`packages/native/../web/src/shared/assets/icons/`, all six are already in
the `native_icons` qrc list, and the reference `Icon` component maps the
`businessCenter` name to the `business_center.svg` asset.)

### Locale strings (`packages/web/src/i18n/locales/{en,ru}.json` at 61dacef)

Header (nav namespace, en.json:409 / ru.json:409, en.json:417 / ru.json:417):

- `nav.services`: "Services" / "Сервисы"
- `nav.chatsAndChannels`: "Chats & channels" / "Чаты и каналы"

Settings block (en.json:1096-1109, ru.json:1108-1121):

| key | EN | RU |
|-----|----|----|
| `settings.servicesPlannedHint` | Services will be available here. Integrations are being prepared. | Здесь появятся сервисы. Интеграции уже готовятся. |
| `settings.servicesComingSoon` | Coming soon | Скоро |
| `settings.servicesPlaceholderKnowledgeBase` | Knowledge base | База знаний |
| `settings.servicesPlaceholderKnowledgeBaseHint` | Internal docs and team playbooks. | Внутренние документы и командные инструкции. |
| `settings.servicesPlaceholderPeople` | People directory | Справочник сотрудников |
| `settings.servicesPlaceholderPeopleHint` | Team structure, contacts, and roles. | Структура команды, контакты и роли. |
| `settings.servicesPlaceholderApprovals` | Approvals | Согласования |
| `settings.servicesPlaceholderApprovalsHint` | Requests and approval workflows. | Заявки и процессы согласования. |
| `settings.servicesPlaceholderDocs` | Document vault | Документы |
| `settings.servicesPlaceholderDocsHint` | Contracts, templates, and shared files. | Договоры, шаблоны и общие файлы. |
| `settings.servicesPlaceholderAutomation` | Automation | Автоматизация |
| `settings.servicesPlaceholderAutomationHint` | Routine process automation and bots. | Автоматизация рутинных процессов и боты. |
| `settings.servicesPlaceholderIntegrations` | Integrations | Интеграции |
| `settings.servicesPlaceholderIntegrationsHint` | Connections with external SaaS systems. | Подключения к внешним SaaS-системам. |

## Measured geometry (Tailwind -> px)

Page root `flex h-full min-h-0 min-w-0 flex-1 flex-col overflow-hidden`
(full width, fills the content slot; services-page.ui.tsx:85):

- Header `border-b border-border-subtle bg-card-bg px-4 py-4` (:86):
  full width, 16 px horizontal and vertical padding, 1 px bottom border in
  `border-subtle`, background `card-bg`.
  - `h1 text-lg font-semibold text-text-primary` (:87): 18 px, weight 600.
  - Sub `mt-1 text-sm text-text-muted` (:88): 4 px top margin, 14 px.
- Body section `flex min-h-0 flex-1 flex-col gap-3 overflow-hidden p-4`
  (:91): 16 px padding all sides, 12 px gap between grid and button row.
- Card list `grid grid-cols-1 gap-3 overflow-auto pb-1 min-h-0 flex-1`
  (:92): CSS grid, 12 px gaps both axes, 4 px bottom padding, scrolls.
- Card `flex min-h-[170px] flex-col justify-between rounded-xl border
  border-border-subtle bg-card-bg p-4` (:54): min height 170 px, 12 px
  radius, 1 px border, background `card-bg`, 16 px padding; the icon/text
  block sits at the top and the chip at the bottom (justify-between).
  - Inner block `space-y-3` (:55): 12 px between the icon tile and the text.
  - Icon tile `inline-flex h-11 w-11 items-center justify-center rounded-lg
    border border-border-subtle bg-bg` (:56): 44 x 44 px, 8 px radius,
    1 px border, background `bg`; `Icon size={22}` in `text-text-muted`
    (:57).
  - Title `h2 text-sm font-medium text-text-primary` (:60): 14 px, weight 500.
  - Hint `p mt-1 text-sm text-text-muted` (:61): 4 px top margin, 14 px.
  - Chip `span mt-4 inline-flex w-fit rounded-md border border-border-subtle
    bg-bg px-2.5 py-1 text-xs text-text-muted` (:64): 16 px top margin
    (bottom-anchored by justify-between), content-hugging width, 6 px
    radius, 1 px border, background `bg`, 10 px/4 px padding, 12 px text.
- Button row `flex justify-end` (:98); button `rounded-lg bg-accent px-3
  py-1.5 text-sm text-on-accent hover:opacity-90` (:99-105): 8 px radius,
  background `accent`, 12 px/6 px padding, 14 px text in `on-accent`,
  90 % opacity on hover (hover state is pointer-only; not asserted).

## Grid / column policy for native window widths

The reference grid is responsive by viewport width: `grid-cols-1`,
`md:grid-cols-2` (Tailwind md = viewport width >= 768 px),
`xl:grid-cols-3` (xl = viewport width >= 1280 px). The native window is the
viewport analog: the column count is derived from the window width with the
same bands, not from the content-slot width (the sidebar never changes the
reference breakpoint behaviour either):

| native window width | columns |
|---------------------|---------|
| < 768 (min window 700) | 1 |
| 768 .. 1279 | 2 |
| >= 1280 (default 1440) | 3 |

Card width is `(gridWidth - (columns - 1) * 12) / columns` where
`gridWidth = page width - 32` (the p-4 section padding); the minimum card
height is 170 px. At the default 1440 x 900 window the content slot is
~1069 px (1440 - 72 nav rail - 299 sidebar) giving three ~337 px columns;
at the 700 px minimum window it is a single column. The window tests resize
the window to 700 / 1100 / 1440 and assert 1 / 2 / 3 columns at card-row
positions.

## Palette tokens (default palette `blue-cold`, `packages/web/src/shared/lib/themes/registry.ts:9`, `blue-cold.ts`)

The native `qml/NativeTheme.qml` already mirrors these values 1:1 (verified
at lane time for every token below).

| token | light | dark |
|-------|-------|------|
| `bg` | `#e4ecf3` | `#141517` |
| `card-bg` | `#eef5fb` | `#282a32` |
| `text-primary` | `#1b1b1d` | `#ffffff` |
| `text-muted` | `#989898` | `#707070` |
| `accent` | `#7087ff` | `#7087ff` |
| `on-accent` | `#1b1b1d` | `#141517` |
| `border-subtle` | `#d8e4ef` | `#33384d` |

(light: blue-cold.ts:72,74,79,81,83,85,88; dark: blue-cold.ts:8,10,15,17,19,21,24)

## Native design

- New static page `qml/NativeServicesPage.qml` mounted in the `Main.qml`
  page stack (sibling of the Calls/Mail/Calendar pages), visible while
  `controller.section === "services"`. It receives `colors` and the window
  `viewportWidth` for the column policy and exposes a `goToChats()` signal
  wired to `controller.showSection("chat")`.
- `src/native-deeplink.cpp:73-79`: `services` and `all-services` join the
  whitelisted section segments (bare and `/org/<id>/` forms).
- `NativeController::applyDeepLink`: `/services`, `/all-services` and the
  org-scoped forms call `showSection("services")` (origin-guarded like the
  calls route); `showSection` gains the `services` branch
  (`ContentPage::Services`).
- No top-bar / section button is added (reference build flag stays absent,
  mirroring the Calls slice precedent, docs/NATIVE_DESKTOP_CALLS_RECENT_EMPTY.md).
- All `qsTr()` sources ship with Russian translations in
  `translations/workspace_ru.ts` (context `NativeServicesPage`) to satisfy
  the native-native-locale catalog guard.
