# Activity Marked (Starred) messages page (native parity)

Status: approved for implementation.
Issue: #41 (part of #40 Activity parity epic).
Reference revision: `workspace_ui` `61dacef78dab2f5d0be76f283bebf49fe29b84af` (measured in source, not screenshots).

## Purpose

The shipping desktop app exposes a "Marked messages" activity page: a cross-chat
list of the messages the current user starred, newest first, paged from the
Workspace messenger messages endpoint with `starred=true`. The native client
renders the `activity-starred` sidebar chip disabled
(`qml/Main.qml` sidebarActivity Repeater - only `drafts`, `mentions` and
`inbox` are enabled) even though per-message star/unstar already works
(`NativeController::starMessage`). This feature enables the chip and ships the
page with the measured reference behaviour.

## Reference contract (measured)

Sources: `packages/web/src/pages/activity/activity-page.ui.tsx`,
`packages/web/src/pages/activity/activity-message-card.ui.tsx`,
`packages/web/src/pages/activity/activity-message-list.ui.tsx`,
`packages/web/src/entities/activity/activity-workspace-starred.api.ts`,
behaviour locked by `activity-page.test.tsx` (lines 283, 315, 336, 764, 952, 1066).

### Server query

`fetchWorkspaceStarredMessages` (activity-workspace-starred.api.ts:50-90):
`GET /api/workspace/v1/messenger/messages/?starred=true&sort_key=created_at&sort_dir=desc&page_limit=50[&page_marker=…]`
(`page_limit=ACTIVITY_PAGE_SIZE=50`, activity-page.ui.tsx:44). The next
cursor arrives in the `X-Pagination-Marker` response header
(`ApiResult::nextMarker`, same as chat history and mentions). There is no
second index walk like the mentions unread index - starred is one collection.

### Collection and state (activity-page.ui.tsx:111-365)

- One collection keyed `starred` (the `collectionKey` is `ownerKey:activity:starred`).
- Mount / re-open: if the collection already has rows the refetch is a
  refresh that keeps the rows and shows a loading overlay
  (`isRefreshing`); otherwise it is the initial loading state
  (`isInitialLoading`). A response or error only applies while its
  `requestVersion` is current and the runtime context is still the same -
  an A-to-B-to-A owner switch must discard the stale page (test 952).
- Order: dedupe by uuid, newest first (`created_at` desc, uuid desc tie
  break) via `sortUniqueActivityMessages` (activity-page.ui.tsx:91-101).
  Page appends merge into the same deduped, sorted set (test 764).
- Refresh error with rows present: inline
  `activity.messagesRefreshError` retry link; empty-list load error: error
  box `activity.messagesLoadError` + `activity.retryLoad`; pagination
  error: inline `activity.loadMoreError` retry link that re-fires the
  load-more request (activity-page.ui.tsx:601-615).
- Empty (no error, not loading): `chat.noMessages` ("No messages here yet...").

### Pagination (newest-first list, activity-page.ui.tsx:518-555)

- Distance from bottom = `contentHeight - viewportHeight - contentY`.
- Fire `loadMore` once when distance <= 64 px
  (`ACTIVITY_BOTTOM_PAGINATION_THRESHOLD_PX`, line 47), re-arm only after
  distance > 96 px (`…_REARM_THRESHOLD_PX`, line 48); suppressed while
  initial loading / refreshing / loading more.
- Initial scroll position: top of the list (`scrollTop = 0`, line 367-378,
  test 1066 "initializes Workspace starred list at the latest messages").

### Card behaviour (activity-message-card.ui.tsx)

- Card: `p-3` (12 px), `rounded-lg` (8 px), `gap-2` (8 px), hover
  `bg-card-bg`. Starred rows never show the unread dot
  (`showUnreadMarker={getIsUnread != null}` - starred passes no
  `getIsUnread`, so no slot at all) and never the unread background.
- Meta row: time top-left 11 px `text-text-muted`; `stream · topic`
  (or `Private · name` for direct streams) top-right, 11 px
  `text-text-muted`, truncated.
- Sender: `mt-0.5` 12 px `text-sidebar-sender`.
- Summary: `mt-1` 14 px `text-text-primary`, clamped to 2 lines; markdown
  summarized with max 80 chars incl. media/attachment labels and quote
  prefix (`ACTIVITY_MESSAGE_SUMMARY_OPTIONS`, card lines 15-20);
  non-markdown payloads render the encrypted kind label.
- Hover actions (`opacity-0 group-hover:opacity-100`, icons 16 px):
  `newWindow` = open in chat, `send` = forward, forward disabled for
  non-markdown payloads (card lines 116-128). Clicking the card or the
  open button navigates the message route (jump in chat, test 315);
  forward opens the forward flow with `[uuid]` (test 336).
- List container: `p-2` (8 px) `space-y-1` (4 px) + `FloatingLoadingOverlay`
  while refreshing/loading more (activity-message-list.ui.tsx).

### States

initial loading (`app.loading`); error box with retry when the list is empty;
slim refresh-error and pagination-error retry links when rows are present;
empty state `chat.noMessages`. Loading overlay over the list on refresh.

### i18n (both locales required)

| key | en | ru |
| --- | --- | --- |
| activity.starred | Marked messages | Отмеченные сообщения |
| activity.messagesLoadError | Could not load activity messages. | Не удалось загрузить сообщения активности. |
| activity.messagesRefreshError | Could not refresh activity messages. | Не удалось обновить сообщения активности. |
| activity.loadMoreError | Could not load older messages. | Не удалось загрузить более старые сообщения. |
| activity.retryLoad | Retry | Повторить |
| chat.noMessages | No messages here yet... | Здесь пока нет сообщений... |
| app.loading | Loading… | Загрузка… |
| message.openInChat | Open in chat | Открыть в чате |
| message.forward | Forward | Переслать |

## Native architecture

Follows the Mentions page precedent end to end (chip -> controller visibility
flag -> sibling page that hides the conversation column -> controller-enriched
`QVariantMap` page payload).

- `src/native-controller.h/.cpp`:
  - `Q_PROPERTY` `starredVisible`, `starredPage` (enriched rows like
    `mentionsPage()`: `author` via `userDisplayName`, `time`,
    `streamName`/`topicName`, `direct`, `preview`, `markdown`),
    invokables `showStarred()`, `closeStarred()`, `loadMoreStarred()`,
    `retryStarred()`, `openStarredMessage(id)` (closes the page, then
    `openMessageInChat(id)` - the message-route jump precedent).
  - single `StarredCollection` state `{rows, marker, version, inflight,
    loading, error, paginationError, hasMore}` (starred has one collection;
    the version counter is the collection-key guard, `resetStarred()` on
    logout/owner switch is the runtime-owner guard).
  - fetch `GET /api/workspace/v1/messenger/messages/` with
    `starred=true&sort_key=created_at&sort_dir=desc&page_limit=50[&page_marker=…]`;
    every continuation re-checks the version, stale responses are dropped.
  - `ContentPage::Starred` added; `closeOtherContentPages`, `selectStream`,
    `selectTopic` and the logout reset path close/reset it like Mentions.
- `qml/NativeStarredPage.qml` - new page (FocusScope, Escape closes, header
  height 54 `bg-elevated`, `starredCloseButton`), ListView `starredList`
  with delegate ids `starred-card-<uuid>`, hover action buttons
  `starred-open-<uuid>` (always available through the controller; shown on
  hover like the reference) and `starred-forward-<uuid>` (enabled iff the
  payload is markdown; emits `forwardRequested(uuid)`), bottom-edge
  pagination (64 fire / 96 rearm), states `starredErrorBox`,
  `starredRetryButton`, `starredEmptyText`, `starredLoadingText`,
  `starredRefreshError`, `starredPaginationError`.
- `qml/Main.qml` - chip `activity-starred` enabled -> `showStarred()`;
  `selected` gates on `starredVisible`; `conversation.visible` also gates on
  `!starredVisible`; page added as a sibling next to `NativeMentionsPage`;
  `NativeForwardDialog.sourceIds` uses the page-requested single uuid while a
  starred forward is open (the reference forward store is independent from
  the chat selection, workspace-forward-message.model.ts).
- `translations/workspace_ru.ts` - new `NativeStarredPage` context with the
  ru strings above (locale catalog guard).

## Testing plan (TDD, existing CTest groups — no new groups)

`native-native-controller` (tests/native-controller-test.cpp):

1. `activityStarredPageIssuesServerFilteredQuery` - opening the page issues
   `starred=true&sort_key=created_at&sort_dir=desc&page_limit=50`, rows
   newest-first, preview/meta enrichment, `canLoadMore` from the marker.
2. `activityStarredPaginationFollowsMarkerNewestFirst` - `X-Pagination-Marker`
   walk appends older rows, deduped by uuid, newest-first; re-open refetches
   from the first page.
3. `activityStarredErrorStatesAndRetry` - load error -> `error` state, retry
   refetches and clears; pagination error -> `paginationError`, retry
   re-fires with the same marker.
4. `activityStarredStaleOwnerSwitchDiscardsPage` - deferred first page;
   logout; the late response must not repopulate the collection.

`native-native-window` (tests/native-window-test.cpp):

5. `activityStarredChipOpensPageAndCloseFlow` - chip enabled (updates the
   older gating test that asserted it disabled), page renders rows newest
   first, close restores the conversation, card click jumps and closes.
6. `activityStarredScrollPaginationFollowsMarkerAndRearm` covers an overflowing
   first page. Opening or resizing it can briefly give the list unfinished
   dimensions. The page keeps its marker until the settled view scrolls within
   64 px of the bottom and rearms beyond 96 px. Closing the view cancels
   its pending pagination check.
7. `activityStarredShortFirstPageAutoLoadsNextMarker` covers a first page that
   fits after layout. The list follows its marker automatically.
8. `activityStarredErrorAndEmptyStates` checks the English and Russian strings.

## Out of scope (named)

- Live insertion/removal of rows on realtime star/unstar events: the
  reference refetches on remount; per-message star UI already works.
- Reactions page (#43), favorites chip, and the activity expand button.
- Unread markers: the reference renders none for starred.

## Version and release

Patch release inside the 0.3.x chain (per #16 policy): no version bump in
this lane - the release train owner bumps with the milestone batch.

---

# Addendum: Activity reactions collection - unsupported state (issue #43)

Status: approved for implementation.
Reference revision: `workspace_ui`
`61dacef78dab2f5d0be76f283bebf49fe29b84af` (measured in source, not screenshots).
Figma token unavailable - mockups not consulted.

## Scope

The `reactions` activity collection has no data source in the reference: the
page renders an explicit unsupported state instead of a message list. This
addendum records the measured state, its tokens, both locale strings and the
deeplink route policy. Native message reactions
(`qml/NativeReactionPicker.qml`) are a different surface and are not touched.

## Reference contract (measured)

`packages/web/src/pages/activity/activity-page.ui.tsx`:

- `:74-76` - `getUnsupportedMessage()` returns
  `t("workspaceMessenger.reactionsUnsupported")`.
- `:103-109` - `ActivityUnsupportedState()` is
  `<div className="flex min-h-0 flex-1 items-start p-4 text-sm text-text-muted">`
  around that single string.
- `:569-571` - `renderActivityContent()` short-circuits
  `validFilter === "reactions"` to `<ActivityUnsupportedState />`, before the
  runtime-context, loading, error and empty branches - so no collection
  request is issued for `reactions`.
- `:563, 648-653` - the page still renders `ChatChannelHeader` with
  `channelName={getActivityTitle("reactions")}` (`activity.reactions`) and
  `hideTopic`; `ChatChannelHeader` paints the name as
  `text-sm ... font-semibold text-text-primary`
  (`packages/web/src/widgets/chat-view/chat-header-channel.ui.tsx:66, 82`),
  which the native pages already render as the 54 px `bg-elevated` header with
  a 14 px DemiBold title and a 16 px close button (`NativeStarredPage.qml`).

Measured geometry (Tailwind scale, reference sheet `p-4` = 16 px):

| box | measurement |
| --- | --- |
| content padding (`p-4`) | 16 px on all four sides |
| content alignment (`items-start`) | top-aligned, left-aligned, single line, wraps |
| content font (`text-sm`) | 14 px (line-height 20 px) |
| content colour (`text-text-muted`) | see tokens below |
| header height | 54 px (native header precedent above) |
| header title | 14 px, semibold, `text-text-primary` |

Behaviour contract: `packages/web/src/pages/activity/activity-page.test.tsx:360`
("shows an explicit unsupported state for reactions") asserts the rendered text
and that `fetchWorkspaceStarredMessages` / `fetchMyMentionsPage` are never
called for this filter.

## Sidebar: no reactions entry

`packages/web/src/widgets/sidebar/sidebar.lib.ts:90-96` -
`VISIBLE_MY_ACTIVITY_KEYS` is exactly `inbox`, `favorites`, `markedMessages`,
`mentions`, `drafts`; the comment reads "Only items with live Workspace routes
are shown in the rail", and `:98-100` keeps hidden items available for their
routes. So the reactions state is reachable by route only, and the native
sidebar must keep exactly the 5 visible chips
(`qml/Main.qml` `sidebarActivity` Repeater) with no reactions chip added.

## Tokens: `text-text-muted` in light and dark

Default palette is `blue-cold` (`packages/web/src/shared/lib/themes/registry.ts:9`
`defaultPaletteId = "blue-cold"`); `engine.ts:17` picks `palette.light` or
`palette.dark` per resolved mode.

| token | light | dark | reference source |
| --- | --- | --- | --- |
| `text-muted` | `#989898` | `#707070` | `themes/blue-cold.ts:79` (light), `:16` (dark) |

The native theme already carries both values in the default palette
(`qml/NativeTheme.qml:18` light `#989898`, `:65` dark `#707070`), so the page
consumes `colors["text-muted"]` and needs no new token.

## i18n (both locales required)

| key | en | ru |
| --- | --- | --- |
| `workspaceMessenger.reactionsUnsupported` | Reactions are not connected to Workspace messaging yet. | Реакции пока не подключены к Workspace-мессенджеру. |
| `activity.reactions` (header title) | Reactions | Реакции |

(`packages/web/src/i18n/locales/en.json:441, 516`,
`packages/web/src/i18n/locales/ru.json:441, 518`.) Native renders both through
`qsTr` in the new page and ships the ru forms in
`packages/native/translations/workspace_ru.ts` (locale catalog guard).

## Deeplink route policy

The reference registers the collection under the project scope only:
`packages/web/src/app/app-route-definitions.tsx:180` declares
`project/:projectId/activity/:filter` (`:176` is the `activity/favorites`
special case), i.e. the absolute route is
`/org/{org}/project/{project}/activity/{filter}`. There is no top-level
`/activity/:filter` route.

Native already whitelists that shape:
`src/native-deeplink.cpp:39` accepts
`/org/[^/]+/project/[^/]+/.../activity/[^/]+`, and `applyDeepLink` already
dispatches sibling project routes (`/inbox`, `/feed`) behind the same
org/project guard (`src/native-controller.cpp:14882-14898`). This lane adds the
matching `activity/<collection>` branch:

- `workspace::parseActivityRoute()` extracts `org`, `project` and `collection`
  from the accepted route (same shape as `parseMessageRoute`).
- the route resolves to the unsupported-state view only for the `reactions`
  collection; every other collection keeps today's behaviour (the whitelisted
  route only activates the window) - this lane does not add routes for other
  collections.
- the org and project segments must match the signed-in origin and project,
  exactly like `/inbox` and `/feed`; a foreign org or project is a no-op.
- a bare `ew://open/activity/reactions` stays rejected: it is not a reference
  route and the whitelist keeps its documented closure (the shorthand in the
  issue text resolves to the canonical project-scoped route above).

## Native architecture

- `src/native-deeplink.h/.cpp` - `ActivityRoute{org, project, collection}` and
  `parseActivityRoute()`.
- `src/native-controller.h/.cpp` - `reactionsVisible` property,
  `showReactions()`/`closeReactions()`, `ContentPage::Reactions` added to
  `closeOtherContentPages` and to the logout/session-reset path (the state
  holds no data, so reset only clears visibility).
- `qml/Main.qml` - `NativeActivityUnsupportedPage` as a sibling of the other
  activity pages, `visible: controller.reactionsVisible`, and
  `conversation.visible` additionally gated on `!reactionsVisible` (issue #355
  single-visible-content-page rule); the `sidebarActivity` Repeater is
  unchanged at 5 chips.
- `qml/NativeActivityUnsupportedPage.qml` - 54 px header (`title: Reactions`,
  close button), then the muted 14 px line with 16 px padding, top-aligned;
  Escape closes like the sibling pages.
- `translations/workspace_ru.ts` - the two ru strings above.

## Testing plan (TDD, existing CTest groups - no new groups)

`native-native-desktop` (`tests/native-desktop-test.cpp`):

1. `activityRouteExtractionResolvesTheReactionsCollection` - the accepted
   project-scoped `.../activity/reactions` deeplink extracts
   `collection == "reactions"` (so the route resolves to the unsupported-state
   view rather than falling through to the default activity view), other
   collections extract their own name, and a route without a collection
   segment extracts nothing.

`native-native-window` (`tests/native-window-test.cpp`, `NativeWindowTest`):

2. `activityReactionsDeeplinkRendersUnsupportedState_data/-()` - en and ru: the
   deeplink renders the unsupported text, the header title, and no message list
   (`messageList` not visible); the starred/mentions endpoints are never hit.
3. `activityReactionsSidebarKeepsFiveVisibleChips` - `sidebarActivity` exposes
   exactly `inbox`, `favorites`, `starred`, `mentions`, `drafts` and no
   `activity-reactions` object exists anywhere in the window.

## Out of scope (named)

- Any reactions data source, and the `favorites` chip / activity expand button.
- Route-to-page wiring for `activity` collections other than `reactions`.
- `qml/NativeReactionPicker.qml` and `qml/NativeQuickReactions.qml` (message
  reactions) are untouched.


When the user clicks another channel's latest topic from Starred, the page stays
visible while the native client looks up the topic. A successful route change
reveals the new conversation. A failed or superseded lookup leaves the selected
page in place.
