# Chat list: compact density mode and open-latest-topic click mode (issue #102)

Issue #102 (P3-normal, parity, Feature): the sidebar chat list reads two persisted
settings in the reference client: `chatListDensity` (row density) and
`chatClickAction` (what a row-body click opens). This document records the
measured reference contract at the pinned source state and its native mapping.

The Figma token is unavailable (HTTP 401), so no mockups were consulted; the
oracle for every number below is the reference code at
`workspace_ui` sha `61dacef78dab2f5d0be76f283bebf49fe29b84af`:

- `packages/web/src/widgets/sidebar/sidebar-chat-row-layout.lib.ts`
- `packages/web/src/widgets/sidebar/sidebar-chat-row-meta.ui.tsx`
- `packages/web/src/widgets/sidebar/sidebar-workspace.ui.tsx`
- `packages/web/src/features/settings/settings.model.ts`
- `packages/web/src/shared/ui/avatar/avatar.tsx`
- `packages/web/src/i18n/locales/en.json` / `ru.json`

Settings-page UI switches are out of scope (filed with the settings sweep);
this issue implements only the persisted-value behavior.

## 1. Measured reference contract

Tailwind spacing reference: `0.5`=2 px, `1`=4 px, `1.5`=6 px, `2`=8 px, `3`=12 px.

### 1.1 Density (`chatListDensity`)

`sidebar-chat-row-layout.lib.ts`:

- `sidebarChatRowLinkClass(compact)` (stream row):
  - compact: `flex items-start gap-2 rounded-md px-2 py-1.5` -> gap 8, x-pad 8, y-pad 6
  - normal: `flex items-stretch gap-3 rounded-lg px-2.5 py-2.5` -> gap 12, pad 10
- `sidebarTopicRowLinkClass(compact)` (topic row):
  - compact: `gap-2 rounded-md py-1.5 pl-9 pr-2` -> gap 8, y-pad 6, left 36, right 8
  - normal: `gap-3 rounded-lg py-2 pl-[38px] pr-2` -> gap 12, y-pad 8, left 38, right 8
- `sidebarTopicShowMoreButtonClass(compact)` (show-more row):
  - compact: `gap-2 py-1.5 pl-6 pr-2`; normal: `gap-3 py-2 pl-6 pr-2` (left inset 24)
- `SIDEBAR_TOPIC_LIST_CLASS = "mt-0.5 space-y-0.5"` -> the 2 px stream/topic gap is
  density-independent (native `streamDelegate` `spacing: 2` stays).

`sidebar-chat-row-meta.ui.tsx` (right meta column):

- `showTime = !compact && time != null && time !== ""` -> the time line is
  suppressed in compact mode (both stream and topic rows).
- column: compact `min-w-8 gap-0.5` (32 px min width, 2 px gap) vs
  normal `min-w-10 justify-between gap-1` (40 px, 4 px).

`sidebar-workspace.ui.tsx`:

- `const compact = useSettingsStore((s) => s.chatListDensity === "compact")` (line 652).
- topic row: `{!compact && <WorkspaceSidebarMessagePreview .../>}` (line 231) ->
  preview line suppressed in compact mode.
- stream row body: `{!compact && stream.preview?.route != null && ...}` (line 536)
  -> preview line suppressed in compact mode.
- `avatarSize = compact ? "sm" : "md"` (line 417); `avatar.tsx`:
  `sm: w-8 h-8` (32 px), `md: w-10 h-10` (40 px).
- `showAvatar: !compact` in the `general` click config (line 452) -> the avatar is
  hidden in compact mode unless `chatClickAction` forces it (section 1.2).

Derived row box ladder (content lines: title `text-sm leading-5` = 20 px,
preview line 20 px, meta action slot `h-5` = 20 px):

| row | normal | compact |
| --- | --- | --- |
| stream (general click) | 10 + 40 + 10 = **60** | 6 + 20 + 6 = **32** |
| stream (latestTopic click, avatar sm) | 10 + 40 + 10 = **60** | 6 + 32 + 6 = **44** |
| topic | 8 + 20 + 20 + 8 = **56** | 6 + 20 + 6 = **32** |
| show-more | 8 + 20 + 8 = **36** | 6 + 20 + 6 = **32** |

Derived x insets (bar `w-[3px]`, spacer twin for show-more):

| column | normal | compact |
| --- | --- | --- |
| topic bar x | 38 (`pl-[38px]`) | 36 (`pl-9`) |
| topic title x | 38 + 3 + 12 = **53** | 36 + 3 + 8 = **47** |
| show-more label x | 24 + 3 + 12 = **39** | 24 + 3 + 8 = **35** |

These match the accepted native normal ladder (stream rows 60 px, topic rows
56 px, bar x 38, title x 53, show-more x 39) fixed by issues #2/#16.

### 1.2 Click action (`chatClickAction`)

`sidebar-workspace.ui.tsx:407-462`:

- `const chatClickAction = useSettingsStore((s) => s.chatClickAction);`
  `const opensLatestTopic = chatClickAction === "latestTopic";`
- `general` config (default): body link `to: stream.route` (opens the stream
  "General Chat"), `showAvatar: !compact`, no avatar aria-label, no overlay.
- `latestTopic` config:
  - body link `to: stream.preview?.route ?? stream.route` -> opens the topic of
    the last message when one is known, otherwise falls back to the stream route;
  - the avatar stays visible in every density (`showAvatar: true`) as a separate
    link `to={stream.route}` raised above the overlay (`relative z-10`), with
    `aria-label` and `title` `` `${t("chat.generalChat")}: ${title}` `` (lines 458, 499);
  - body link carries `after:absolute after:inset-0 after:rounded-[inherit]` ->
    one overlay across the whole card except the avatar.

`settings.model.ts`:

- `chatListDensity` default `"standard"`; resolver `value === "compact" ? "compact"
  : "standard"` (line 124).
- `chatClickAction` default `"general"`; resolver `parsed.chatClickAction ===
  "latestTopic" ? "latestTopic" : "general"` (line 173).

i18n: `chat.generalChat` = EN `"General Chat"` (`en.json:466`),
RU `"Общий чат"` (`ru.json:467`).

## 2. Native mapping

- `NativePreferences` gains two persisted string properties, loaded strictly with
  the same fallback discipline as the reference resolvers:
  - `chatListDensity`: `"compact"` when persisted `"compact"`, otherwise `"general"`
    (default `"general"`).
  - `chatClickAction`: `"latestTopic"` when persisted `"latestTopic"`, otherwise
    `"stream"` (default `"stream"`).
- Persistence: global QSettings keys `settings/app/chatListDensity` and
  `settings/app/chatClickAction` (see deviation D2). Written through the setter
  with an immediate `sync()`, mirroring `setAuthIdleTimeout`.
- `NativeController::streams()` rows gain `previewTopic`: the topic UUID of the
  stream's last message, derived with the last-message pointer already used for
  the preview text (extended `RowPreview`), so it lands on the same hydration and
  invalidation passes. Empty when the last message is unknown (reference:
  `preview?.route == null`).
- `Main.qml` `streamDelegate`:
  - density ladder per section 1.1 (row heights 60/32/44, 56/32, 36/32;
    paddings 10/6+8; RowLayout spacing 12/8; meta column min width 40/32);
  - compact hides the stream preview line, the topic preview line, the stream and
    topic time labels, and (in `stream` click mode) the row avatar;
  - `chatClickAction == "latestTopic"`: the row button click routes to
    `controller.openTopic(previewTopic, stream)` when `previewTopic` is non-empty
    and otherwise `controller.selectStream(stream)` (reference `?? stream.route`);
    the avatar gets its own click surface that calls `controller.selectStream`
    and the accessible name `qsTr("General Chat: %1")` (ru catalog:
    `Общий чат: %1`), matching the reference aria-label format.

## 3. Deviations

- **D1 - enum spelling.** The reference persists `chatListDensity:
  "standard" | "compact"` and `chatClickAction: "general" | "latestTopic"`.
  Issue #102 fixes the native persisted enums as `chatListDensity:
  "general" | "compact"` and `chatClickAction: "stream" | "latestTopic"`
  ("general" density = the roomy rows; "stream" click = open the stream). The
  resolver behavior (strict equality, fallback to the default) is identical.
- **D2 - global settings scope.** `authIdleTimeout` (#165) is persisted per owner
  under `settings/owners/<owner>`. The reference persists both settings in one
  global settings blob (`settings.model.ts` `persistSettings`), not per account,
  so the native keys are global `settings/app/...`. This also makes the values
  apply before the owner scope resolves (startup/login) exactly like the web
  client.
- **D3 - overlay equivalence.** The reference achieves "whole card is a link
  except the avatar" with an `after:absolute inset-0` pseudo overlay on the body
  link. Native rows already handle the whole-card click on the row button; the
  raised avatar MouseArea reproduces the same hit map without an extra item.
- **D4 - avatar `title` tooltip.** The reference sets both `aria-label` and the
  HTML `title` attribute on the avatar link. Native ports the accessible name;
  the hover tooltip is not ported (QML hover tooltips for this surface are not
  part of the accepted contract).
- **D5 - Figma unavailable.** The Figma token returned 401; mockups were not
  consulted. All numbers come from the reference code at the pinned sha above.

## 4. Verification plan

- `native-native-preferences`: defaults, strict fallback sanitization, and
  persistence across reopen for both keys.
- `native-native-window` (window group): the compact ladder (row heights,
  paddings, spacing, suppressed preview/time/avatar, topic row insets), the
  unchanged normal ladder, and both click modes - `latestTopic` body click opens
  the last-message topic, the avatar opens the stream with accessible name
  `General Chat: Product team` (en) and the ru catalog string.
- `native-native-locale`: catalog guard for the new `General Chat: %1` string.
- Live server acceptance per `docs/LIVE_ACCEPTANCE.md` flow with the seeded
  `settings/app` keys, recorded in the PR `## Live verification` section.
  In the QSettings INI (`~/.config/Exordos/Workspace Native Preview/preferences.ini`)
  the two-level key is the section `[settings]` with the single-backslash escape
  `app\chatListDensity=compact` / `app\chatClickAction=latestTopic`; a doubled
  backslash parses as a literal key and is silently ignored by QSettings.
