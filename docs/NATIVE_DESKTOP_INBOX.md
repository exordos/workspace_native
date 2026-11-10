# Inbox page (native parity)

Status: approved for implementation.
Issue: #44.
Reference revision: `workspace_ui` `61dacef78dab2f5d0be76f283bebf49fe29b84af` (measured in source, not screenshots).

## Purpose

The shipping desktop app has an Inbox page: unread direct conversations and
unread channel topics grouped into two sections, with per-stream cards,
unread-count badges and loading/error/empty states. The native client renders
the `activity-inbox` sidebar chip disabled (`qml/Main.qml:2317`). This feature
enables the chip and ships the page from the existing local catalog projection.

## Reference contract (measured)

Sources: `packages/web/src/pages/inbox/inbox-page.ui.tsx` (310 lines, whole
file), `packages/web/src/entities/messenger/messenger-notification-mode.lib.ts:31-47`,
`packages/web/src/shared/lib/count-format.lib.ts`, behaviour contract
`packages/web/src/pages/inbox/inbox-page.ui.test.tsx`.

### Grouping rules (inbox-page.ui.tsx:54-95, 225-241)

- Streams come from the sidebar catalog (`selectMessengerSidebarStreams`), in
  sidebar order; topics per stream in sidebar topic order (recency-based).
- A stream appears iff `hasUnread` = stream active unread > 0 OR any of its
  topics has active unread > 0 (line 54-61). Passive (muted) traffic is
  excluded entirely: the resolver is `resolveWorkspaceActiveUnreadCount`
  (`activeUnreadCount ?? unreadCount`, notification-mode lib:37-41) — muted
  streams have `activeUnreadCount = 0` regardless of `unreadCount`.
- Sections: Direct messages (`uiKind === "directPrivate"`) first, then
  Channels (line 229-236); a section renders only when it has groups.
- Inside a stream (buildInboxStreamGroup, line 86-95): rows for unread topics
  only (active unread > 0). Row title = `"<Stream> · <Topic>"`; an empty or
  whitespace topic name renders `inbox.allMessages` ("All messages"). A stream
  with stream-level active unread but no unread topics renders ONE fallback row
  titled with the stream title, routing to the stream route.
- Row click navigates to the topic (or stream) route; time label =
  `formatMessageTimeShort(updatedAt)` — same compact rule locked for the
  mentions page (same-day `HH:mm`, `Yesterday HH:mm`, compact calendar date;
  invalid timestamps render no time line).

### Badge counter (count-format.lib.ts)

`UNREAD_COUNT_DISPLAY_LIMIT = 1000`; `formatCount(count, 1000)`:
`count > 1000` renders `floor(count/1000)` + `"k+"`. So 1000 → `"1000"`,
1001 → `"1k+"`. Card header badge = stream active unread (line 158); row badge
= topic (or fallback stream) active unread (line 122).

### Geometry (measured, Tailwind → px; tokens = palette names)

Scroll container: px 12, pb 12, pt 8. Section head: px 4 / py 4, 11 px,
font-semibold, uppercase, letter-spacing 0.08 em (≈0.88 px), `text-muted`,
gap 8 to a 1 px hairline `flex-1` `border-subtle` at 60 % alpha. Channels
section offset mt 16; section card lists gap 10 (`space-y-2.5`); stream cards
gap 8 inside a section head area.

Stream card: background `bg-elevated` at 45 % alpha, radius 12
(`rounded-xl`), 1 px border `border-subtle`, padding 8. Card head (mb 6,
px 4 / py 4, gap 8): icon circle 28 px `bg-bg` `text-muted` with 16 px icon
(`profile` for DMs, `channels` for channels), title 14 px semibold truncated,
badge on the right. Rows list gap 6 (`space-y-1.5`).

Row: radius 12, 1 px border `border-subtle`, background `bg-elevated` at
50 % alpha, padding 10, gap 8, hover border `accent-soft` at 40 % and
background `card-bg`. Icon circle 32 px `bg-bg` with 18 px icon; title 14 px
medium truncated; time mt 2, 11 px `text-muted`.

Badge (both levels): height 20, min-width 20, rounded-full (radius 10),
1 px border `border-subtle`, background `badge-bg`, padding-x 4, 11 px
medium, `badge-text`.

State cards (loading / error / empty): margin 12, radius 12, 1 px border
`border-subtle`, background `bg-elevated` at 50 % alpha, padding x 16 / y 12,
text 14 px; error text `notice-base`, loading/empty `text-muted`.

Header: `ChatChannelHeader` with the page title, topic and participants
controls hidden, plus the search entry. The native client reproduces the
established native activity-page header (height 54, `bg-elevated`, 14 px
DemiBold title + close button, same as the drafts and mentions precedents); the
header search entry is deferred with the blocked server-side global search
(#10) — same call as the Mentions page, recorded here so it is a named
deviation, not a silent one.

### States (inbox-page.ui.tsx:196-207, 231-238)

- `loading = store.isLoading || (!catalogLoaded && error == null)`;
  loading card `app.loading` when there are no groups;
- error card `inbox.loadError` when the catalog errored and there are no
  groups (the fixed string renders, not the store message);
- empty card `inbox.noUnread` when loaded, no error, no groups.

### i18n (both locales required)

| key | en | ru |
| --- | --- | --- |
| inbox.title | Inbox | Входящие |
| inbox.dm | Direct messages | Личные сообщения |
| inbox.channels | Channels | Каналы |
| inbox.allMessages | All messages | Все сообщения |
| inbox.noUnread | No unread messages | Нет непрочитанных сообщений |
| inbox.loadError | Failed to load inbox | Не удалось загрузить входящие |
| app.loading | Loading… | Загрузка… |

## Native architecture

The native catalog already carries everything this page projects: `Stream`
and `Topic` hold `activeUnread`/`passiveUnread` (`src/domain.h:57-58, 74-75`),
persisted in the cache columns `active_unread_count` / `passive_unread_count`
(`src/native-cache.cpp:325, 340`), and realtime `stream.*`/`topic.*` events
already replace those rows in place. No new endpoint is introduced; the page
is a local projection that updates live with the existing catalog pipeline.

Resolver parity note: the reference resolves `activeUnreadCount ?? unreadCount`.
The native DTO parser (`domain.cpp counter()`) normalizes a missing
`active_unread_count` to 0, and the Workspace API always ships the full
counter triple alongside read state (enforced by `completeReadCounterSnapshot`,
`src/native-controller.cpp:8049`); therefore the native resolver uses
`activeUnread` directly. Falling back to `unread` client-side would let
muted/passive streams leak into the inbox — the exact case the reference tests
guard (fallback semantics identical against a spec-compliant server).

Topic hydration model note (measured gap + adaptation): the reference hydrates
the full stream+topic catalog at login, so topic-level unread is available for
every stream; the native client hydrates topic lists lazily per stream
(`loadTopics`, `src/native-controller.cpp:7130`, keyed to the selected stream
so the sidebar vector `m_topics` stays single-stream). To reproduce the
reference observable result without polluting the sidebar vector, the inbox
keeps its own snapshot `QHash<QString /*stream*/, QVector<Topic>>
m_inboxTopics`: on `showInbox()` every non-archived stream with active unread
whose slot is empty triggers the SAME cached collection the sidebar uses
(`/stream_topics/?stream_uuid=…`), parsed into the slot. While a slot is
pending, the stream renders the reference fallback row (stream title +
stream-level badge); when the response lands the rows refine to the reference
topic rows — a transient difference, identical to React re-rendering after
store hydration. Realtime `topic.created/updated/read/deleted` events update a
filled slot for that stream; `stream.read` zeroes the slot's counters; logout
and owner switch clear the slots.

- `src/native-controller.h/.cpp`
  - `Q_PROPERTY(bool inboxVisible ...)`, `Q_PROPERTY(QVariantMap inboxPage ...)`
    (both `NOTIFY stateChanged`); invokables `showInbox()`, `closeInbox()`,
    `openInboxRow(stream, topic)`; per-stream topic snapshot slots
    `m_inboxTopics` filled by `hydrateInboxTopics()` via the same
    `cachedCollection({Topics, stream})` path the sidebar uses.
  - `inboxPage()` builds `{direct, channels, loading, error}`: per stream with
    unread (stream OR any topic active unread > 0, archived streams skipped,
    sidebar catalog order) a card `{id, title, direct, badge, rows}`; rows for
    unread topics in recency order (`updatedMicros` desc, uuid desc tie — the
    native sidebar topic ordering oracle, `topics()` :6291) plus the fallback
    row rule above. Enriched fields: `title` (`"Stream · Topic"` /
    `All messages`), `badge` (formatted here, `countText` mirrors
    `count-format.lib.ts` exactly), `time` (compact formatter shared with the
    mentions page — `mentionTime` generalised), `stream`, `topic` (empty for
    the fallback row), `direct` (same expression as `streams()` `isDirect`).
    Stream display title = `peerDisplayName` for DMs, `name` otherwise (uuid
    fallback for a blank name, matching `mentionRows`).
  - `loading = authenticated && !m_catalogVerified && error().isEmpty()`,
    `error = authenticated && !m_catalogVerified && !error().isEmpty()` —
    `m_catalogVerified` is the existing catalog-loaded flag (set at :2882,
    :3423, :6973) mirroring the reference `catalogLoaded`.
  - `openInboxRow` closes the page then hands the inbox slot's owned DTO to
    `beginTopicOpening` (the owned-canonical path; `openTopic(topic, stream)`
    is the fallback when no slot row exists); the fallback row selects the
    stream via `selectStream(stream)`. `selectTopic` also closes the inbox,
    mirroring its `closeMentions()` call.
  - `applyRealtimeEvent`: in the `stream.*` and `topic.*` branches, emit
    `stateChanged()` while `m_inboxVisible` so the bound page recomputes with
    the replaced counters; the `m_inboxTopics` slots follow the topic/stream
    events per the hydration note above. Logout/owner switch resets
    `m_inboxVisible` and the slots.
  - Deeplink: `applyDeepLink` maps `/org/<org>/project/<project>/inbox` to
    `showInbox()` behind the same org/project guard as `applyMessageRoute`;
    the route is already allowlisted (`src/native-deeplink.cpp:39`), where it
    previously only activated the window.
- `qml/NativeInboxPage.qml` — new page (FocusScope, Escape closes, header per
  the drafts/mentions precedent), Flickable + sections/cards/rows at the
  measured geometry; objectNames: `nativeInboxPage`, `inboxHeader`,
  `inboxCloseButton`, `inboxSectionDirect`/`inboxSectionChannels`,
  `inboxSectionTitleDirect`/`inboxSectionTitleChannels`, `inbox-card-<uuid>`,
  `inbox-card-title-<uuid>`, `inbox-card-badge-<uuid>`, `inbox-row-<uuid>`,
  `inbox-row-title-<uuid>`, `inbox-row-time-<uuid>`, `inbox-row-badge-<uuid>`,
  `inboxLoadingText`, `inboxErrorBox`, `inboxEmptyText`.
- `qml/Main.qml` — chip `activity-inbox` enabled → `showInbox()` with
  `selected: controller.inboxVisible`; `conversation.visible` also gates on
  `!inboxVisible` (:2564); page instance next to `NativeMentionsPage` (:5090).
- `translations/workspace_ru.ts` — new `NativeInboxPage` context (ru column
  above) plus the `NativeController` tr entry for `All messages` (locale
  catalog guard).

## Testing plan (TDD, existing CTest groups — no new groups)

`native-native-controller` (tests/native-controller-test.cpp):

1. `inboxGroupsDirectAndChannelsExcludingPassiveTraffic` — DM + channel
   fixtures mixing active/passive counters; passive-only stream excluded;
   section order Direct then Channels; row titles and badge strings; topic
   order by recency; unread-topic-only rows; `inboxVisible` toggles.
2. `inboxFallbackStreamRowWhenNoUnreadTopics` — stream-level active unread
   with all topics read renders the single fallback row (empty topic target).
3. `inboxBadgeFormattingMatchesReferenceLimit` — 1000 → "1000", 1001 → "1k+".
4. `inboxRowOpensTargetAndClosesPage` — topic row → `openTopic` + page hides;
   fallback row → `selectStream`; Escape/close equivalent via `closeInbox()`.
5. `inboxRealtimeCounterUpdateRecomputesPage` — a `stream.updated` realtime
   event with new counters moves the stream between included/excluded.
6. `inboxDeeplinkOpensTheInboxPage` — `ew://…/inbox` opens the page for the
   matching org/project; a foreign project route leaves the page closed.
7. `inboxEmptyAndErrorStates` — fresh catalog with no unread → no groups;
   `/streams/` failing keeps the catalog unverified and flips `error` on.

`native-native-window` (tests/native-window-test.cpp):

8. `inboxPageChipGroupsBadgesAndNavigation` — enable `activity-inbox`, open
   the page, assert section heads, card/row titles, badge texts (incl. "1k+"),
   badge height 20 and row icon circle 32 (measured from inbox-page.ui.tsx),
   row click opens the topic and reveals the composer, close returns.
9. `inboxStateCardsInBothLocales` — empty card and error card strings for
   `en` and `ru` (preferences locale), Escape closes.

## Out of scope (named)

- Header search entry: deferred with the blocked server-side global search
  (#10), consistent with the Mentions page decision (#42).
- Mark-all-as-read control (`inbox.markAllRead`): the reference page renders
  no such control in the measured revision; the strings exist for other
  surfaces. Not part of this slice.
- Sidebar chip unread dot / tooltip counter: belongs to roadmap #1 (on hold).
- Inbox deeplink beyond `/inbox` (per-message anchors inside the page): the
  reference has none.

## Version and release

Patch release in the 0.3.x chain (policy per #16): CMake version → `0.3.10`,
tag `0.3.10`, section appended to `docs/NATIVE_DESKTOP_RELEASE_NOTES.md`.
