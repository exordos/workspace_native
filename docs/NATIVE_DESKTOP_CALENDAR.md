# Native Calendar section: page shell, views and agenda (UI phase)

Issue: #881 (Part of #851). Status: draft, lane `cassi/calendar-shell-881`.

## Why this is new functionality, not a pixel clone

The packaged reference exposes NO functional calendar screen. Measured at
the contract sha `e0194ff` (also still current on `origin/master`, last
touch `8d5f9eb`):

- `packages/web/src/pages/calendar/calendar-page.ui.tsx` (58 lines) renders
  an `EmbedFrame` iframe pointed at `env.CALENDAR_EMBED_URL`, falling back to
  `embeds/calendar-placeholder.html`, and when the URL is missing or its
  origin is not in `VITE_EMBED_ALLOWED_ORIGINS` it renders the
  `app.webModeUnavailable` fallback card (`nav.calendar` heading).
- `calendar-page.test.tsx` asserts exactly those three cases; no event,
  date or view behaviour exists in the reference.

The native client forbids WebView, so issue #36 shipped the fallback card as
`calendarSectionPage` (`NativeSectionUnavailablePage`). #851 decomposes the
Figma-driven calendar build; per Evgeny's design rule, brand-new
functionality is built from the Figma mockups. This page is therefore a
Figma build, not a deviation from the reference source.

## Design oracle status (honest record)

Figma token returned HTTP 403 on 2026-11-26 (`figma-token` helper hands back a
token but `api.figma.com` rejects it) - **mockups not consulted in full**.
The cached frame tree (`~/dev/reference_state/figma/tree.json`, 2026-11-13)
confirms the cited frames exist and name them:

- `18693:32760` "Календарь / день" (Day, agenda, and event details)
- `18693:32323` "Календарь / неделя" (Week grid and upcoming events)
- `18693:33825` "Календарь / месяц" (Month grid and agenda)
- (`18693:33117` "Календарь / создание встречи" belongs to #882)

Until the token is restored, geometry and palette follow the established
native token conventions (`root.colors` token set, section-page layout
patterns from `NativeFeedPage`/`NativeInboxPage`) and every such value is
marked `pending-Figma` below. When the token is restored, the PENDING lines
must be measured against the three nodes and updated in a follow-up on this
issue before Testing.

## Layout (from cached frame names + existing native section patterns)

`NativeCalendarPage.qml` replaces the `calendarSectionPage` placeholder slot
in `Main.qml` (the `section === "calendar"` branch). The objectName
`calendarSectionPage` is KEPT on the new page root so existing layout pins
(`tests/native-window-test.cpp` route-gate cases that click
`sectionCalendarButton` and measure the page fill) stay green.

- Header strip (48 px, section-page pattern): view tabs Day | Week | Month
  (`viewTabDay/viewTabWeek/viewTabMonth`), date navigation
  `calendarPrev`, `calendarNext`, `calendarToday`, selected-date title
  `calendarDateTitle`.
- Body row: left rail column (mini-calendar `calendarMini`, visibility
  filters `calendarFilters`), main pane bound to the active view:
  - Day: agenda list `calendarAgenda` for the selected date, event rows
    (time pill, title, colour chip per calendar).
  - Week: 7-column grid `calendarWeekGrid` with event chips per day cell.
  - Month: 6x7 day grid `calendarMonthGrid` with day numbers, today mark,
    event dots, plus the agenda pane for the selected day.
- States (same card pattern as the feed page, `m-3 rounded-xl ...` tokens):
  `calendarLoading`, `calendarEmpty`, `calendarError` + `calendarRetry`.

All strings are new user-visible `qsTr` keys; they must be added to
`packages/native/translations/workspace_ru.ts` in the same PR (catalog guard
`native-native-locale`).

## Data: deterministic mock adapter (UI phase)

Live API is #852; this phase is fixtures only (issue scope). The controller
is filled from `GET /calendar/events?from&to` responses and exposes
`calendarEvents` as a `QVariantList` projection with `stateChanged`
announcements (the feed-page pattern of #38, `native-controller.cpp`).
Amended during slice 2: a QAbstractListModel would only serve the agenda
ListView - the week columns and month cells need random per-day access and
would force a second index in JS anyway, so the list shape buys nothing in
this phase. Late answers of a retired window are fenced by a monotonic
version + owner guard exactly like `FeedState` (issue #356); a re-show of an
already-loaded window is not re-fetched, so section re-entry is traffic-free
and fixtures that never navigate to calendar see zero new requests. The
window-test mock server serves fixed fixtures for a frozen reference date so
cells are assertable. Events:
`{uuid, day (YYYY-MM-DD), start, end, title, calendar (work|personal),
colorToken}`. Loading/error drive the state cards; retry re-fetches the
visible window.

`pending-Figma`: header heights, chip metrics, mini-calendar cell size.

## Acceptance (issue #881)

New ctest group `native-native-calendar` (window layer):
1. `sectionCalendarButton` opens the page; placeholder gone.
2. Day/Week/Month each render the fixture events in the correct cells for
   the fixed reference date.
3. prev/next/today move the selection and refetch the window.
4. Mini-calendar selection and visibility filters react.
5. loading/empty/error render; `calendarRetry` re-fetches.
Bucket registration via `gen-test-buckets.py` with measured timings.

## Event detail (#907, part of #882)

Figma oracle for this slice: `18693:33117`-adjacent event-details frames
inside `18693:32760` (day view). The token was still 401 when this section
was written, so metrics stay `pending-Figma` and follow native token
conventions, exactly as the shell section above.

Data contract additions on each event row from `GET /calendar/events`
(parsed in `NativeController::requestCalendarWindow`): `location` (string),
`all_day` (bool), `start_utc` / `end_utc` (ISO-8601 UTC instants), and
`participants` (list of display names). Absent fields degrade: no `start_utc`
falls back to the day + `start`/`end` strings; no location shows the
`No location specified` placeholder.

Rendering: `NativeCalendarEventDetail.qml`, a centered card overlay inside
`NativeCalendarPage` (`page.detailEvent` selects the row; day and month agenda rows and
week chips open it, `calendarDetailClose` clears it). Fields: title
(`(untitled)` fallback, elided with `Layout.minimumWidth: 0` per #879),
time line, location block, participants (first three + `+%1 more` via
`calendarDetailParticipantsOverflow`).

Time-zone rule: stored instants are UTC; the card converts with the JS Date
pipeline (`new Date(isoWithZ)` + `Qt.format*`, i.e. the host local zone), so
DST boundaries print correctly with no offset math. All-day events render the
local date only, no time. Window tests derive expectations from
`QDateTime::fromString(instant).toLocalTime()` - never hardcoded day/weekday
names (TZ-pinned flake rule).

Acceptance (window layer, calendar cases in `native-window-test.cpp`):
clicking a fixture row in either the day or month agenda opens the detail with the fixture title,
time range, location and participants; an all-day fixture shows the date
only; five participants render three rows + `+2 more`; the DST fixture
instant renders the locally derived day/time; `calendarDetailClose` hides
the card. New `qsTr` strings ship in `workspace_ru.ts` (context
`NativeCalendarEventDetail`) in the same PR.
