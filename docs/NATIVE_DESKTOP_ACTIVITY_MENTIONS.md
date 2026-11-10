# Activity Mentions page (native parity)

Status: approved for implementation.
Issue: #42 (part of #40 Activity parity epic).
Reference revision: `workspace_ui` `61dacef78dab2f5d0be76f283bebf49fe29b84af` (measured in source, not screenshots).

## Purpose

The shipping desktop app exposes a Mentions activity page: a cross-chat list of
messages that mention the current user, with an All/Unread segmented filter,
realtime merging of arriving mentions without refetching the open page, an
unread-mention index that drives unread dots, and per-filter empty states.
The native client renders the `activity-mentions` sidebar chip disabled
(`qml/Main.qml:2315` — only `drafts` is enabled). This feature enables the chip
and ships the page with the measured reference behaviour.

## Reference contract (measured)

Sources: `packages/web/src/pages/activity/activity-page.ui.tsx`,
`packages/web/src/pages/activity/activity-message-card.ui.tsx`,
`packages/web/src/entities/activity/activity-mentions.api.ts`,
`packages/web/src/entities/activity/activity-realtime-applier.lib.ts`,
`packages/web/src/entities/activity/activity.model.ts`,
behaviour locked by `activity-page.test.tsx` (lines 370, 406, 489, 556, 590, 633, 664, 673, 685, 704).

### Server queries

- List page (`fetchMyMentionsPage`, activity-mentions.api.ts:60-100):
  `GET /api/workspace/v1/messenger/messages/?mentioned=true[&read=false]&sort_key=created_at&sort_dir=desc&page_limit=50[&page_marker=…]`
  (`mentioned=true` always; `read=false` only for the Unread collection).
  Next cursor arrives in the `X-Pagination-Marker` response header.
- Unread index (`fetchUnreadMentions`, activity-mentions.api.ts:102-134):
  same endpoint with `mentioned=true&read=false&page_limit=100`, walking every
  page to the end; the index is the set of returned message uuids.
- All/Unread are separate collections: switching filters resets to a fresh
  first page and the cursor of one filter never feeds the other
  (activity-page.ui.tsx:178-247, collectionKey = `mentions:<filter>`).
- Own messages that mention the current user are included — this is purely
  server-side (`mentioned=true`), test line 370.

### Live behaviour (realtime applier, activity-realtime-applier.lib.ts)

Classification is server-driven: the message DTO carries a `mentioned`
boolean. The client never parses mention entities.

- `message.created` (or untagged `message`) with `mentioned === true` →
  upsert into the unread index and into a live-mention message map. Live rows
  merge into the open page without any new page request (test line 489).
- `message.read`, `message` with `read: true`, `message` with
  `mentioned === false`, `message.deleted`, and batch `messages.read`
  (exact uuid list) → remove uuids from the unread index (dots off, the
  message stays visible in the All collection).
- `topic.read` / `topic.deleted` → drop index entries for that stream+topic;
  `stream.read` / `stream.deleted` → drop index entries for that stream.
- Mutations are owner-scoped and epoch-guarded (`epoch_version` monotonic per
  user, enforced by the existing realtime ingestion; runtime-generation guard
  = request generation check, same as the cache-context guards already used by
  drafts).
- Unread state rule (activity-page.ui.tsx:226-247, 626-633): while the index
  is not ready, fall back to `!message.read` (test line 556); once ready, a
  row is unread iff its uuid is in the index (tests 590, 633).
- Ordering: dedupe by uuid, newest first (`created_at` desc, uuid desc tie
  break), same as `sortUniqueActivityMessages` reversed.

### Pagination (newest-first list, activity-page.ui.tsx:518-555)

- Distance from bottom = `contentHeight - viewportHeight - contentY`.
- Fire `loadMore` once when distance <= 64 px (`ACTIVITY_BOTTOM_PAGINATION_THRESHOLD_PX`, line 47),
  re-arm only after distance > 96 px (`…_REARM_THRESHOLD_PX`, line 48),
  suppressed while initial loading / refreshing / loading more.

### States

initial loading; error-with-retry box when list is empty
(`activity.messagesLoadError` + `activity.retryLoad`); slim refresh-error and
pagination-error retry buttons when rows are present; per-filter empty states
(`activity.mentionsEmpty` / `activity.unreadMentionsEmpty`).

### Geometry (measured, Tailwind → px; tokens = palette names)

Filter bar (activity-page.ui.tsx:655-682):

- bar: padding x 12 / y 8, 1 px bottom border `border-subtle`;
- pill container: gap 2, radius 8, background `bg`, padding 2;
- buttons: height 28, padding-x 12, font 12 (medium weight), radius 6;
  selected background `card-bg` + `text-primary`; unselected `text-muted`,
  hover background `card-bg-active`; role=group with aria-label
  (`activity.mentionsFilter`), aria-pressed per button.

Mention card (activity-message-card.ui.tsx):

- card: padding 12, radius 8, gap 8, unread row background `card-bg`;
- unread dot: slot margin-top 6, dot 8x8 rounded-full `sidebar-unread`,
  accessible name `activity.unreadMention`; empty slot reserved always;
- time: 11 px `text-muted`, top-left; `stream · topic` (or
  `Private · name` for direct streams) top-right, 11 px `text-muted`,
  truncated;
- sender: margin-top 2, 12 px `sidebar-sender`, `font-medium` when unread;
- preview: margin-top 4, 14 px `text-primary`, max 2 lines, `font-medium`
  when unread; markdown summary capped at 80 chars with media/attachment
  label substitution; encrypted kinds render the locked label.
- item time: same-day `HH:mm`, yesterday `Yesterday HH:mm`, else compact
  calendar date (shared/lib/datetime.lib.ts:98-112).

### i18n (both locales required)

| key | en | ru |
| --- | --- | --- |
| activity.mentions | Mentions | Упоминания |
| activity.mentionsFilter | Filter mentions | Фильтр упоминаний |
| activity.allMentions | All | Все |
| activity.unreadMentions | Unread | Непрочитанные |
| activity.unreadMention | Unread mention | Непрочитанное упоминание |
| activity.mentionsEmpty | No mentions yet | Упоминаний пока нет |
| activity.unreadMentionsEmpty | No unread mentions | Непрочитанных упоминаний нет |
| activity.messagesLoadError | Could not load activity messages. | Не удалось загрузить сообщения активности. |
| activity.messagesRefreshError | Could not refresh activity messages. | Не удалось обновить сообщения активности. |
| activity.loadMoreError | Could not load older messages. | Не удалось загрузить более старые сообщения. |
| activity.retryLoad | Retry | Повторить |

## Native architecture

Follows the drafts page precedent end to end (chip → controller visibility
flag → sibling page that hides the conversation column → controller-enriched
`QVariantMap` page payload).

- `src/native-mentions.h/.cpp` — new `NativeMentions` QObject module:
  - `useRemote(api, project, user)` / `reset()` mirroring
    `NativeDrafts::useRemote`; owner key = project + user.
  - per-filter collection state `{rows, cursor, hasMore, loading, error,
    paginationError, requestVersion}`; filters `all` / `unread`; switching
    filters clears the incoming collection and refetches (reference
    collectionKey semantics, one live slot keyed by `mentions:<filter>`);
  - unread index `QSet<QString>` + ready flag + owner key; full walk
    (`page_limit=100`) started when the page is first shown for the owner;
  - live-mention map fed by realtime events; visible rows = dedup(live + fetched),
    `createdMicros` desc, uuid desc;
  - fetches use `GET /api/workspace/v1/messenger/messages/` with
    `mentioned=true` (+`read=false`), `sort_key=created_at`,
    `sort_dir=desc`, `page_limit=50`; cursor via `X-Pagination-Marker`
    (`ApiResult::nextMarker`), same as chat history
    (`native-controller.cpp:7092-7100`);
  - every async continuation re-checks owner + `ApiClient::generation()`
    (stale-on-owner-switch guard);
  - realtime entry point `applyRealtime(kind, message, readIds)`.
- `src/domain.h/.cpp` — `Message` gains `mentionedKnown`/`mentioned`, parsed
  from the message DTO when present (tri-state like the reference: missing →
  no index mutation). Unknown-field tolerance unchanged.
- `NativeController` — new `Q_PROPERTY`s `mentionsVisible`, `mentionsFilter`,
  `mentionsPage` (enriched `rows` like `draftPage()`: `author` via
  `userDisplayName`, `time`, `streamName`/`topicName`, `direct`, `preview`,
  `unread`), invokables `showMentions()`, `closeMentions()`,
  `selectMentionsFilter(id)`, `loadMoreMentions()`, `openMention(stream, topic)`;
  wires `NativeMentions::applyRealtime` from `applyRealtimeEvent` for
  `message.created/updated/read/deleted`, `messages.read`,
  `topic.read/deleted`, `stream.read/deleted` (all owner+epoch checked before
  the hook); logout/owner switch calls `reset()`.
- `qml/NativeMentionsPage.qml` — new page (FocusScope, Escape closes, header
  height 54 `bg-elevated` matching `draftsHeader`, `mentionsCloseButton`),
  filter bar + pills at measured geometry, `ListView` `mentionList` with
  delegate ids `mention-card-<uuid>` / `mention-unread-dot-<uuid>`, bottom-edge
  pagination (64 fire / 96 rearm), error/empty states with objectNames
  `mentionsErrorBox`, `mentionsRetryButton`, `mentionsEmptyText`,
  `mentionsLoadingText`.
- `qml/Main.qml` — chip `activity-mentions` enabled → `showMentions()`;
  `conversation.visible` also gates on `!mentionsVisible`; page added as the
  sibling next to `NativeDraftsPage` (Main.qml:5079 pattern).
- `translations/workspace_ru.ts` — new `NativeMentionsPage` context with the
  ru strings above (locale catalog guard).

## Testing plan (TDD, existing CTest groups — no new groups)

`native-native-controller` (tests/native-controller-test.cpp):

1. `activityMentionCollectionsIssueServerFilteredQueries` — opening the page
   issues `mentioned=true` (no `read` param); the Unread filter issues
   `mentioned=true&read=false`; cursors do not leak between filters.
2. `activityMentionPaginationFollowsMarkerNewestFirst` — `X-Pagination-Marker`
   walk appends older rows, deduped by uuid, newest-first order.
3. `activityMentionRealtimeMergesWithoutRefetch` — request counter frozen; a
   `message.created` with `mentioned=true` appears; `mentioned=false`,
   `message.read`, batch `messages.read`, `message.deleted`, `topic.read`,
   `stream.read` update the unread index / row removal per the table above;
   own mention rows included.
4. `activityMentionStaleOwnerSwitchDiscardsResponse` — deferred first page;
   re-login as another project/user; late response must not touch the page.
5. `activityMentionIndexFallbackToReadFlag` — dots use `!read` before the
   index walk completes, index membership after.

`native-native-window` (tests/native-window-test.cpp):

6. `activityMentionsChipOpensPageAndFilterSwitchesCollections` — click
   `activity-mentions`, page visible with measured geometry (filter button
   height 28, dot 8x8, values measured from
   activity-page.ui.tsx:670 / activity-message-card.ui.tsx:71-75), unread dot
   rendered iff unread; click Unread pill → unread rows only.
7. `activityMentionsEmptyAndRetryStates` — per-filter empty strings for
   `en` + `ru` data rows; error box + retry reissues the request; Escape closes.

Fixture: extend the controller test `Fixture` via its overridable
`messages` route handler (mock-server.h `X-Pagination-Marker` pattern);
window tests get a `WindowMentionsFixture` cloning `WindowDraftFixture`'s
skeleton with a mentioned-aware `/messages/` route. Message rows gain
`"mentioned": true` in the JSON used by these tests only.

## Out of scope (named)

- Forward button on cards: deferred to the multiple-forward roadmap (#7); the
  reference hides it for encrypted payloads anyway.
- Starred collection and Reactions-unsupported state: separate issue #41/#43.
- Marking a mention read by clicking through: read state changes come from the
  existing server read events, no new client call.

## Version and release

Patch release between milestone epochs (0.3.x chain, per #16 policy): CMake
version → `0.3.9`, tag `0.3.9`, release notes section appended to
`docs/NATIVE_DESKTOP_RELEASE_NOTES.md`.
