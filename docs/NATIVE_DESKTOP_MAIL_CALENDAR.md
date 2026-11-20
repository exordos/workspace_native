# Mail and Calendar sections — native top-bar entry and section pages

This document is shared between the Mail section (#35) and the Calendar section
(#36, part of #32) as required by the issue notes. It covers both surfaces: the
Mail revision above, then the Calendar section appended below.

Reference oracle: workspace_ui at commit
`61dacef78dab2f5d0be76f283bebf49fe29b84af` (all measurements below were taken
from `git show 61dacef7:<path>` at that sha, not the working tree).

Figma: token available but rejected by the REST API (`403` on the file and
`401` on `/v1/me`) — Figma token unavailable, mockups not consulted. The Mail
mockup section is recorded in the cached page tree as node `3211:5470`
("Почта") on the page "design макеты (desktop)" (`151:2290`) of file
`Q3BtN9qwfQV8rAsyBAgCy1`; geometry below is therefore measured from the
reference React source, which is the accepted parity oracle.

## Scope decision

The reference `/mail` page (`packages/web/src/pages/mail/mail-page.ui.tsx`)
has two branches: a configured same-origin/allowlisted iframe embed and an
unavailable fallback card. The packaged desktop (Electron loads `dist` over
`file://`, `packages/electron/src/main.ts:452`) never resolves an embed URL,
so the shipping desktop product always shows the fallback card. QtWebEngine is
forbidden in the native client; per `docs/NATIVE_DESKTOP_PARITY.md` the
minimum accepted native scope is the fallback/unavailable state plus the
active top-bar entry. No embed, no spinner, no origin allowlist is migrated.

## Measured reference contract (61dacef7)

Top-bar section chips (`packages/web/src/widgets/top-bar/top-bar.lib.ts:60-96`):

- Mail is an always-visible section item: icon `mail`, label key `nav.mail`,
  `available: true`.
- One shared chip for idle/active/hover: 48x48 px, 12 px radius (`h-12 w-12
  rounded-2xl`), 32 px glyph (`TOP_BAR_SECTION_ICON_SIZE_PX`).
- Active chip: `border border-border-subtle bg-card-bg-active text-icon-active`.
- Idle chip: `text-text-muted` (hover adds border-subtle + card-bg-active).

Unavailable fallback card (`mail-page.ui.tsx` fallback branch):

- Container: flex column, centered on both axes, `gap-4` (16 px), `p-6`
  (24 px padding), `text-text-muted` inherited by glyph and body.
- Glyph: `Icon name="mail" size={64}` with `opacity-50`.
- Heading: `h2 text-xl font-medium text-text-primary`, text = `nav.mail`.
- Body: `p max-w-lg text-center text-sm`, 512 px max width, wrapped, text =
  `app.webModeUnavailable`.

i18n keys (reference `packages/web/src/i18n/locales/{en,ru}.json`):

| key | en | ru |
| --- | --- | --- |
| `nav.mail` | Mail | Почта |
| `app.webModeUnavailable` | This section is temporarily unavailable in web mode. | Раздел временно недоступен в веб-режиме. |

Default palette blue-cold (`packages/web/src/shared/lib/themes/blue-cold.ts`):

| token | light | dark |
| --- | --- | --- |
| bg | #e4ecf3 | #141517 |
| card-bg-active | #cde6ff | #2c3747 |
| text-primary | #1b1b1d | #ffffff |
| text-muted | #989898 | #707070 |
| border-subtle | #d8e4ef | #33384d |
| icon-active | #1b1b1d | #ffffff |

## Native mapping

- `NativeController` gains the section state so top-bar sections switch
  exclusively: `section` string property (`"chat"` default, `"mail"` once
  enabled; the Calendar lane will extend the accepted set) and
  `showSection(name)`, which for `"mail"` closes the drafts/mentions/inbox
  content pages first (they share the same slot via
  `closeOtherContentPages`, extended with a `Mail` member) and for `"chat"`
  returns to the conversation. Opening any of the chat content pages or
  selecting a stream/folder/topic returns the section to `"chat"` — the
  reference equivalent is the router mapping any conversation route to the
  chat section. Unknown section names are ignored so the Calendar lane can
  adopt the property without a behavioural surprise.
- `NativeIconButton` gains an opt-in `activeBorder` flag so an active section
  chip draws the reference `border-border-subtle` without changing any
  existing instance; the chip stays the shared 48x48/12 px-radius/32 px glyph
  geometry.
- The disabled Mail `NativeIconButton` in the `sectionButtons` row becomes
  active; Chat and Mail bind `selected` and glyph color to `section`
  (active `icon-active` on `card-bg-active` + `border-subtle`, idle
  `text-muted`).
- New `qml/NativeSectionUnavailablePage.qml` renders the fallback card:
  24 px padding box, centered column gap 16, 64 px `mail.svg` glyph tinted
  `text-muted` at opacity 0.5 (the codebase `MultiEffect` recolor pattern),
  20 px Medium heading in `text-primary`, 14 px wrapped centered body capped
  at 512 px in `text-muted`. Heading/body travel in as i18n strings so the
  same component serves the Calendar lane.
- The page mounts in the `Main.qml` page stack next to `NativeDraftsPage` and
  hides the conversation column while visible, exactly like the existing
  drafts/mentions/inbox pages.

## States and acceptance

Light + dark, en + ru (`packages/native/translations/workspace_ru.ts`).
The native window test group gets the reference-card render assertions for
all four locale/mode combinations, the button enable + active/idle chip
toggle, chat restore via the Chat button, and exclusive switching with the
chat content pages (controller group).

---

# Calendar section (issue #36)

Figma: token requested again for this lane and rejected by the REST API
(`401` on `/v1/me`) — Figma token unavailable, mockups not consulted; the
reference React source at the contract sha remains the oracle.

## Measured reference contract (61dacef7)

Top-bar section item (`packages/web/src/widgets/top-bar/top-bar.lib.ts`,
`getTopBarSectionNavItems`): `{ id: "calendar", icon: "calendar",
label: t("nav.calendar"), available: true }`, always visible, listed between
chat and mail — the same shared 48x48 px / 12 px-radius / 32 px-glyph chip and
the same idle/active/hover class strings documented above; no calendar-only
geometry exists.

`/calendar` page (`packages/web/src/pages/calendar/calendar-page.ui.tsx`):
structurally identical to `mail-page.ui.tsx`. Fallback branch: flex column
centered on both axes, `gap-4` (16 px), `p-6` (24 px), `text-text-muted`;
`Icon name="calendar" size={64}` at `opacity-50`; `h2 text-xl font-medium
text-text-primary` = `nav.calendar`; `p max-w-lg text-center text-sm`
(512 px) = `app.webModeUnavailable`. Embed branch differs only by env
`VITE_CALENDAR_EMBED_URL` and the `embeds/calendar-placeholder.html`
same-origin placeholder; wrapper `p-3`, frame `rounded-lg border
border-border-subtle bg-bg`. As with Mail, the packaged desktop (Electron
`file://` load, `packages/electron/src/main.ts:452`) never resolves an embed
URL, so the fallback card is the shipping desktop state and the native parity
scope; QtWebEngine stays out.

i18n keys (reference `packages/web/src/i18n/locales/{en,ru}.json`):

| key | en | ru |
| --- | --- | --- |
| `nav.calendar` | Calendar | Календарь |
| `app.webModeUnavailable` | This section is temporarily unavailable in web mode. | Раздел временно недоступен в веб-режиме. |

Default palette blue-cold: identical tokens/hexes to the Mail table above.

## Native mapping

- `NativeController::showSection()` accepts `"calendar"`: same exclusive
  switch as `"mail"` — `closeOtherContentPages(ContentPage::Calendar)` (new
  enum member) closes drafts/mentions/inbox and leaves the other section, then
  `section` becomes `"calendar"`. Returning to chat is unchanged: any chat
  page or conversation navigation runs `resetChatSection()` regardless of
  which section is active, and unknown sections (calls/services) stay ignored.
- The disabled Calendar `NativeIconButton` in the `sectionButtons` row becomes
  active with the shared geometry (48x48, radius 12, glyph 32) and the shared
  idle/active palette; `help` becomes the `nav.calendar` label ("Calendar").
- The already-shipped `qml/NativeSectionUnavailablePage.qml` is reused
  unchanged: the Calendar page instance passes `asset: "calendar.svg"` (the
  same `calendar.svg` the disabled chip already rendered), the `nav.calendar`
  heading and the `app.webModeUnavailable` body, mounted in the page stack as
  `calendarSectionPage` and visible while `section === "calendar"`. The
  conversation column already hides for any non-chat section.
- `workspace_ru.ts` (Main context): the obsolete "Calendar is not available in
  this native preview" chip tooltip entry is replaced by `Calendar` →
  `Календарь`, mirroring the Mail entry.

## States and acceptance

Light + dark, en + ru. `native-native-window` gains
`calendarSectionUnavailableCardMatchesReferenceContract` (all four
locale/mode rows: chip geometry and idle/active palette, `calendarSectionPage`
visible with the reference card geometry — 24 px padding, centered gap-16
column, 64 px glyph at opacity 0.5 tinted text-muted, 20 px Medium
text-primary heading "Calendar"/"Календарь", 14 px text-muted body wrapped at
max 512 px, conversation hidden; Chat-button restore, Mentions exclusivity
and sidebar-navigation return to chat), and
`native-native-controller` gains
`sectionStateSwitchesChatAndCalendarExclusively`.


The operating-system tray opens Mail and Calendar in the existing main window.
Both actions restore the window using the desktop activation path and select the
same project section as the corresponding top-bar button. When the signed-out
window is restored the login screen remains visible.
