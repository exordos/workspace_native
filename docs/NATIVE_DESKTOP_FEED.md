# Project Feed page: cross-chat "All messages" (native parity)

Status: approved for implementation.
Issue: #38 (part of #34).
Reference revision: `workspace_ui` `61dacef78dab2f5d0be76f283bebf49fe29b84af`
(measured in source, not screenshots).
Figma: token unavailable (401) - mockups not consulted; the reference-code
oracle is the only visual source.

## Purpose

The reference app has a project-scoped cross-chat "All messages" page (route
`project/:projectId/feed`) listing the newest messages of the current project
across all streams/topics, with marker-based pagination, open-in-chat and
forward actions, and scroll anchoring. The native client has no such page.

### Entry-point policy (parity decision)

The reference sidebar rail entry is hidden today: feed is present in
`MY_ACTIVITY` (`packages/web/src/widgets/sidebar/sidebar.lib.ts:80-87`) but
excluded from `VISIBLE_MY_ACTIVITY_KEYS` (lines 91-98). Parity here therefore
means the page and its route behaviour, NOT a new visible rail entry. The
native client adds no visible rail entry either. The page is reachable the
same way the reference page is reachable without the chip - through the route:
the external deep link `/org/<org>/project/<project>/feed` (already whitelisted
in `src/native-deeplink.cpp:39`) opens the feed page when org and project match
the active session, mirroring the existing `/inbox` route handling
(`NativeController::applyDeepLink`).

## Reference contract (measured)

Sources: `packages/web/src/pages/feed/feed-page.ui.tsx`,
`packages/web/src/pages/feed/feed-scroll.lib.ts`,
`packages/web/src/pages/feed/feed-page.ui.test.tsx`,
`packages/web/src/pages/feed/feed-scroll.lib.test.ts`,
`packages/web/src/entities/feed/feed.api.ts`,
`packages/web/src/entities/feed/feed.model.ts`,
`packages/web/src/shared/api/messenger-client.ts`,
`packages/web/src/shared/lib/datetime.lib.ts`,
`packages/web/src/i18n/locales/{ru,en}.json`.

### Server query

- `fetchFeedMessages` (feed.api.ts:43-78) calls `getMessagesPage`
  (messenger-client.ts:202-225): `GET /messages/` with `page_limit=50`
  (feed-page.ui.tsx:55) and `page_marker` only for an older page, plus the
  centrally added `project_id` (projectScopedParams, messenger-client.ts:96-119).
  No `sort_*`, `stream_uuid` or `topic_uuid` parameters are sent.
- Next cursor arrives in the `X-Pagination-Marker` response header;
  `hasMore = nextPageMarker != null` (feed.api.ts:60-64). `X-Pagination-Limit`
  is parsed but the page UI never reads it.
- `hydrateFeedMessagesFromCache` is a stub returning an empty list
  (feed.api.ts:83-88); no cache-first hydration is reproduced.
- The store keeps messages ordered oldest-first (sortUniqueMessages sorts
  `created_at` asc, uuid asc tie break, deduped by uuid, feed.model.ts:39-68);
  the initial page renders with the newest message at the list bottom and an
  older page is prepended above it (appendOlder merges and re-sorts).
- Behaviour pinned by feed-page.ui.test.tsx: loads after runtime ready (226),
  author names resolved from authorUuid only (251), row click routes to the
  topic route (263), summary never exposes raw file urls (280), forward opens
  with the message uuid (300), owner switch clears the feed (337), older page
  uses the page marker (363), stale older-page responses are ignored after an
  owner change (407), initial position is at the latest messages (534).

### Pagination and scroll model

- Fire an older-page request when `scrollTop <= 64` px
  (feed-scroll.lib.ts DEFAULT_TOP_THRESHOLD_PX; feed-page.ui.tsx:177-208).
  One request until re-armed past `scrollTop > 96` px
  (FEED_TOP_PAGINATION_REARM_THRESHOLD_PX, feed-page.ui.tsx:57); suppressed
  while a page is in flight, while refreshing, or without a marker
  (feed-scroll.lib.ts shouldRequestOlderFeedPage).
- Near-bottom threshold 80 px (FEED_BOTTOM_THRESHOLD_PX, line 56) drives the
  floating scroll-to-bottom button (hidden while within 80 px of bottom) and
  refresh stickiness (a refresh keeps the bottom only if the user was near it).
- Initial load scrolls to the bottom (feed-page.ui.tsx:161-172). Prepend
  restores scroll so visible content does not jump:
  `scrollTop + (newScrollHeight - oldScrollHeight)` (feed-scroll.lib.ts:16-31
  via shared/lib/scroll-prepend-anchor.lib.ts).
- FloatingLoadingOverlay while loading more or refreshing; refresh dedupe via
  runInFlightDeduped keyed on owner+marker.

### Row layout (feed-page.ui.tsx:363-424) and geometry

List container: `px-3 pb-3 pt-2` = 12 px left/right, 12 px bottom, 8 px top;
`space-y-2` = 8 px row gap; `overscroll-behavior: contain`.
Row: `rounded-xl` 12 px radius, 1 px `border-subtle`, `bg-elevated` at 60 %,
`p-2.5` = 10 px padding.
- Top line: time pill `rounded-full bg-bg px-2 py-0.5` (9999 radius, 8x2 px
  padding), 10 px font-medium `text-muted`; time from
  `formatMessageTimeWithDate` (datetime.lib.ts:86-96): same local day ->
  24 h HH:mm, otherwise locale dated style (2-digit day, short month, numeric
  year) + " " + HH:mm. Context pill "stream name · topic name", truncate,
  same pill geometry, 10 px font-medium `text-secondary`; stream/topic fall
  back to their uuid when unknown (feed-page.ui.tsx:369-372).
- Sender line: `mt-1` 4 px, 12 px font, `sidebar-sender` colour; display name
  resolved from authorUuid only, fallback to the uuid.
- Summary block: `mt-1.5` 6 px, `rounded-lg` 8 px, `bg-bg` 70 %,
  `px-2.5 py-2` = 10x8 px, 14 px font leading-snug, `text-primary`,
  `line-clamp-2`. Summary = summarizeWorkspaceMessageMarkdown with
  maxLength 80 plus media label, attachment label and quote prefix
  (feed-page.ui.tsx:64-69); non-markdown payloads render the encrypted label.
- Row action cluster: `bg-bg` 60 %, `rounded-lg` 8 px, `p-1` 4 px,
  opacity 0.7 -> 1.0 on row hover; two 16 px icon buttons (open-in-chat
  `newWindow`, forward `send`), each `rounded-md p-1.5`, hover `card-bg-active`.
- State cards (loading/error/empty): `m-3` 12 px margin, `rounded-xl`,
  `border-subtle` border, `bg-elevated` 50 %, `px-4 py-3` = 16x12 px, 14 px
  font. Loading and empty use `text-muted`; error uses `notice-base`.

### Palette (blue-cold, shared/lib/themes/blue-cold.ts)

| token | light | dark |
| --- | --- | --- |
| bg-elevated | #ffffff | #222328 |
| border-subtle | #d8e4ef | #33384d |
| text-primary | #1b1b1d | #ffffff |
| text-secondary | #787878 | #999999 |
| text-muted | #989898 | #707070 |
| sidebar-sender | #7087ff | #7087ff |
| notice-base | #7087ff | #3d5eff |
| card-bg-active | #cde6ff | #2c3747 |

### Labels (ru.json / en.json, measured at the contract sha)

| key | ru | en |
| --- | --- | --- |
| feed.title | Все сообщения | All messages |
| feed.noMessages | Пока нет сообщений | No messages yet |
| feed.loadError | Не удалось загрузить сообщения | Failed to load messages |
| app.loading | Загрузка… | Loading… |
| message.openInChat | Открыть в чате | Open in chat |
| message.forward | Переслать | Forward |

## Native design

- Controller state in `src/native-controller.cpp` (`ContentPage::Feed`):
  `feedVisible`, `feedPage` (rows/loading/empty/error/canLoadMore/
  loadingMore), `showFeed/closeFeed/loadMoreFeed/retryFeed`. The fetch reuses
  the existing `GET messenger + "/messages/"` request path
  (native-controller.cpp:901, :7100) extended with `page_limit=50`,
  `page_marker` and the explicit `project_id`, mirroring
  `NativeController::loadMentionCollection` (the closest shipped precedent).
  The `X-Pagination-Marker` header already reaches the caller through
  `ApiResult::nextMarker` (src/api-client.cpp:497).
- Rows are stored oldest-first (createdAt asc, uuid asc, deduped by uuid),
  identical to the reference store; `loadMoreFeed` merges the older page in
  front. Version + owner guards discard stale pages: a response whose owner
  key or request version no longer matches mutates nothing (feed-page.ui.test
  lines 337, 407).
- Row preview (`feedRowPreview`) runs a markdown summary before the 80-char
  clamp so a link target or attachment urn is never visible in a row and can
  never be truncated into a half-URL. Measured against
  `packages/web/src/pages/feed/feed-page.ui.tsx:64-69`
  (`FEED_MESSAGE_SUMMARY_OPTIONS`: maxLength 80, media/attachment labels and
  quote prefix on) and `workspace-message-summary.lib.ts`: media inlines
  render as the media label (`summarizeFileInline`, lines 74-95; unsupported
  media keeps the media label, `workspace-message-parse.lib.ts:745`), file
  references as `Файл: name`/`Файл`, markdown links prefer the readable label
  over the target (`summarizeLinkInline`, lines 97-112), mentions render as
  `@label`, and a block quote renders as `Цитата: <summary>`
  (summarizeBlock, lines 206-213). The pinned oracle
  `packages/web/src/pages/feed/feed-page.ui.test.tsx:279-297` proves the
  visible contract: `![screen.png](urn:image:...?name=screen.png) Вот скрин`
  renders as `Изображение: Вот скрин` with neither the urn nor the file uuid
  present; with the ru catalog the native preview produces exactly that
  string (`Image`/`Video`/`File`/`Quote` labels come from the same
  translations the message renderer uses). Deviations kept deliberately:
  block decorations without a security role (code/list prefixes) follow the
  shipped single-flow precedent documented in
  docs/NATIVE_DESKTOP_ACTIVITY_MENTIONS.md.
- Feed previews render as plain text. The reference puts the summary string
  into a React text node (feed-page.ui.tsx:398) - it is never interpreted as
  markup; the native `Text` therefore pins `textFormat: Text.PlainText`
  (also for sender and pills) so attacker-controlled content can never load
  a remote image through automatic format detection.
- Time pill: `formatFeedRowTime` implements formatMessageTimeWithDate
  (same day HH:mm, else locale dated + HH:mm) against the preferences locale,
  same locale accessor as `rowPreviewFields`.
- QML: new `qml/NativeFeedPage.qml` rendered in the Main.qml page stack next to
  NativeDraftsPage/NativeMentionsPage/NativeInboxPage (precedent). ListView
  fires `loadMoreFeed` at contentY <= 64 px, re-arms past 96 px, never while a
  page is in flight; a prepended page restores `contentY += delta(contentHeight)`
  so visible content does not jump; the initial load anchors at the bottom;
  the floating scroll-to-bottom button appears beyond 80 px from the bottom.
- Open-in-chat reuses the existing topic navigation path: like
  `openInboxRow`, the row (and its open-in-chat button) closes the page and
  opens the message's stream/topic through the existing topic-opening
  machinery - the same route the reference row click uses (feed-page.ui.test:263).
- Forward: no forward flow exists on master yet. Per the issue the button is
  rendered (layout parity) but disabled, pending roadmap issue #7 (0.10.0
  multiple forward). Its accessible name stays "Forward"/"Переслать".

## Test groups (recorded for group accounting)

No new CTest group. Feed fetch/pagination/state cases extend the existing
`native-controller` group (native-controller-test.cpp) and rendering/behaviour
cases extend `native-window` (native-window-test.cpp), keeping the local suite
group accounting unchanged.

## States

initial loading card; error card (with retry action); empty card; list;
loading-more overlay; scroll-to-bottom button - light + dark, ru + en, with
the geometry and colours measured above.
