# Calls page: recent Jitsi calls empty state (issue #37)

Part of the Calls umbrella (issue #33). This slice reproduces the standalone
recent-calls page in its only shipped state: the "Recent Jitsi calls" empty
state. Live Jitsi conferencing is explicitly out of scope
([parity matrix](NATIVE_DESKTOP_PARITY.md): "Calls require an explicit
integration decision"); the `feature:jitsi-call` and `shell-jitsi` matrix
slices file it separately.

Figma token unavailable - mockups not consulted. The reference implementation
in the pinned workspace_ui checkout is the sole geometry oracle; every value
below was measured at `61dacef78dab2f5d0be76f283bebf49fe29b84af`.

## Reference contract

- `packages/web/src/pages/calls/calls-page.ui.tsx`
  - Page root `flex max-h-full min-h-0 w-full min-w-0 flex-1 flex-col
    overflow-hidden`: full width, no `max-w-narrow-page` cap (asserted in
    `calls-page.test.tsx:22-36`).
  - Header row `flex items-center gap-2 px-4 pb-2 pt-3`: 16 px left/right
    padding, 12 px top, 8 px bottom, 8 px gap; `Icon "phone" size={18}` in
    `text-text-muted`; `h2 text-sm font-semibold text-text-primary` with
    i18n key `call.recentCalls`.
  - State card `m-3 rounded-xl border border-border-subtle
    bg-bg-elevated/50 px-4 py-3 text-sm` in `text-text-muted` with i18n key
    `call.noRecentCalls`: 12 px outer margin, 12 px radius, 1 px border,
    bg-elevated at 50 % alpha, 16 px/12 px padding, 14 px text.
- `packages/web/src/pages/calls/calls-page.test.tsx`: heading and empty text
  render; no loading state (`:49` asserts no "loading" text); no navigation
  on mount; full-width container without `max-w-narrow-page`.
- `packages/web/src/app/app-route-definitions.tsx:161-162,198-199`: both
  `/call` and `/calls` map to this page.
- `packages/web/src/widgets/top-bar/top-bar.lib.ts:89-91` and
  `top-bar.ui.tsx:57`: the top-bar Calls entry (label key `nav.calls`, icon
  `phone`) renders only when the build sets `TOP_BAR_CALLS_NAV`
  (`VITE_TOP_BAR_CALLS_NAV`). It is hidden by default, so the route itself is
  the entry surface.
- i18n keys (`packages/web/src/i18n/locales/{en,ru}.json`):
  - `call.recentCalls` = "Recent Jitsi calls" / "Последние звонки Jitsi"
  - `call.noRecentCalls` = "No recent Jitsi calls yet" /
    "Пока нет недавних звонков Jitsi"
  - `nav.calls` = "Calls" / "Звонки" (unused natively: no chip is rendered)

## Native design

- New `qml/NativeCallsPage.qml`: presentation-only section page (precedent:
  `NativeSectionUnavailablePage` for #35/#36, `NativeFeedPage` for the card
  geometry). Root object name `nativeCallsPage`; children
  `callsHeaderRow`, `callsHeaderGlyph`, `callsHeading`, `callsEmptyCard`,
  `callsEmptyText`.
- Controller section state: `showSection("calls")` becomes a rendered
  section (`section() == "calls"`), following the mail/calendar pattern.
  `closeOtherContentPages()` keeps exclusivity: opening a chat page or
  selecting a conversation returns the section to `chat`; mail, calendar and
  calls hold the same slot and each takes it from the others.
- No top-bar Calls chip is added. The reference hides it behind the
  `TOP_BAR_CALLS_NAV` build gate and the native build settings do not
  introduce that gate, so the button "stays hidden" by not being rendered at
  all - the strongest reading of the issue requirement.
- Deep link: the native whitelist in `src/native-deeplink.cpp` already
  accepts `/calls` (and `/org/<id>/calls`); `applyDeepLink` now maps it to
  `showSection("calls")`, mirroring how the routes of `/inbox` and `/feed`
  are the entry surface while their chrome is hidden. The `/call` alias from
  `app-route-definitions.tsx` is NOT added to the native deep-link
  whitelist: that would widen the deep-link contract beyond this page and
  the whitelist was frozen with `/calls` only. Note this as accepted delta.
- Conversation surfaces: the shared `conversationPanel` visibility already
  gates on `section === "chat"`, so the calls page exclusively occupies the
  content slot exactly like mail/calendar.

## Geometry (blue-cold palette, light / dark)

| Reference class | px | Token | hex light | hex dark |
| --- | --- | --- | --- | --- |
| header `px-4` | 16 | - | - | - |
| header `pt-3` / `pb-2` | 12 / 8 | - | - | - |
| header `gap-2` | 8 | - | - | - |
| Icon phone | 18x18 | text-muted | `#989898` | `#707070` |
| h2 `text-sm font-semibold` | 14, DemiBold | text-primary | `#1b1b1d` | `#ffffff` |
| card `m-3` | 12 | - | - | - |
| card `rounded-xl` | 12 | - | - | - |
| card border 1 | 1 | border-subtle | `#d8e4ef` | `#33384d` |
| card fill 50 % alpha | - | bg-elevated | `#ffffff` | `#222328` |
| card `px-4 py-3` | 16 / 12 | - | - | - |
| card text `text-sm` | 14 | text-muted | `#989898` | `#707070` |

Font-derived values (heading height, line boxes) are derived at runtime per
the portability rule in `NATIVE_DESKTOP_DEVELOPMENT.md` (issue #16); only
fixed pixel sizes, spacing and radii are pinned. The card follows the
established native convention from `NativeFeedPage` state cards: content is
anchored 16/12 inside the border box (the 1 px border is not added to the
padding).

## i18n

New `qsTr` sources live in `NativeCallsPage.qml` and travel in the reviewed
catalog `packages/native/translations/workspace_ru.ts`:

| source | ru |
| --- | --- |
| Recent Jitsi calls | Последние звонки Jitsi |
| No recent Jitsi calls yet | Пока нет недавних звонков Jitsi |

The `native-locale` suite regenerates the catalog with `lupdate` and fails
on any unreviewed source, so the catalog entry is mandatory.

## States

The empty state is the only state: no loading state, no error state, no
history request is issued when the page opens (`calls-page.test.tsx`
counterpart). Opening the page performs no HTTP at all.

## Tests

- `native-controller-test`
  - `sectionStateRendersRecentCallsSection`: exclusive section slot
    (chat pages / stream selection return to chat; calls vs mail slot
    hand-off), and `ew://open/calls` deep link opens the section.
  - The two pre-existing expectations that `showSection("calls")` is ignored
    (written for mail #35 / calendar #36) move to the still-unrendered
    `services` section.
- `native-window-test` `callsRecentEmptyStateMatchesReferenceContract`
  (en/ru x light/dark): geometry rows above by objectName, en/ru strings,
  full-width page footprint equal to the conversation slot (no narrow-page
  cap, measured like the Feed content-area check), conversation surfaces
  hidden while the page is open, no `loading` text, request counters
  unchanged across open (no history request), selection unchanged (no
  navigation on mount), Chat chip and stream row return to the chat section.
- Every new case was watched failing before the implementation.

## Live verification

Recorded in the PR. The sign-in and Calls-page screenshots through the
built client against the live server, plus health/send latencies, are in the
`## Live verification` section of the #37 PR.
