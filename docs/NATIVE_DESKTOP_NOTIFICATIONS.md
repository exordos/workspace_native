# Native desktop notifications (release 0.6.0)

Issue: #3 (P2-high, milestone 0.6.0 — "Linux notifications: bounded,
deduplicated, open the exact message"). Reference contract measured from
`workspace_ui` at sha `61dacef78dab2f5d0be76f283bebf49fe29b84af` (scout sweep
comment on #3, matrix slice `shell-badge`).

## Reference pipeline (measured)

- `packages/web/src/widgets/layout/layout-workspace-notifications.hook.ts`
  consumes per-owner background candidates, resolves conversation metadata with
  `NOTIFICATION_METADATA_GRACE_MS = 30000` and a `NOTIFICATION_METADATA_RETRY_MS = 250`
  retry; after grace a generic notification fires with title key
  `notifications.genericNewMessage` (en "You have a new message", ru
  "У вас новое сообщение") and body `username · domain` (session profile, never a raw URL).
- `packages/web/src/shared/lib/workspace-desktop-notifications.lib.ts`
  `shouldWorkspaceDesktopNotify` classifies triggers `metadata_unavailable | dm |
  wildcard_mention | mention | followed_topic | stream` and blocks when:
  `notificationEligible === false`, own message, already read, muted
  (topic mode `mute` mutes; topic `unmute`/`follow` un-mutes; else stream mode
  `muted` mutes), the window is focused AND the conversation is on screen, or
  the trigger is not enabled (`followed_topic` requires topic mode `follow`,
  `stream` requires stream mode `all_messages`). Wildcard mentions match the
  pattern `(^|[\s([{"'.,!?;:])@(all|everyone|channel)\b` (case-insensitive).
- `packages/web/src/widgets/layout/notification-aggregate-registry.lib.ts`
  buckets: DM = `owner::streamConversationId`, channel = `owner::topicConversationId:author:authorUuid`;
  aggregate tag `bucket:<bucketKey>`, generic fallback tag `msg:owner::message:<uuid>`
  (`layout-notification-tag.lib.ts`). One entry per tag holding every pending
  message; the snapshot keeps count, the latest message by `createdAt` (uuid
  tiebreak) as body/click-route/title source. Read-close
  (`layout-notification-tags.lib.ts:closeReadMessageNotifications`): if the read
  set contains the bucket's latest message the whole tag closes, otherwise the
  read messages are dropped and the tag stays with a smaller count. Untracked
  uuids fall back to closing `msg:` tags.
- Dedup: `shared/lib/notification-dedup.lib.ts` keeps the recent
  `owner::messageUuid` set capped at `MAX_RECENT_KEYS = 200`.
- Titles (`layout-notification-title.lib.ts`, locales en.json 1300-1303):
  dm single = `senderName`; dm multi = "N message(s)" + ` · conversation`;
  channel = `sender · stream · topic` (topic omitted when null/default) and with
  count = "N message(s) from sender · stream · topic".
- Shell (`packages/electron/src/main.ts:1118-1201`): title/body truncated to
  `MAX_NOTIFICATION_LENGTH = 200`; empty title rejected; one active notification
  per tag (showing a new tag closes the previous one); `silent: true`; click
  routes the stored click route and also forwards `notifications:click(tag)`;
  `notifications:closeByTag` closes by tag.
- Attention (`main.ts:769-778` + focus handler at line 437): every shown
  notification calls `requestAttention()`; it returns when the window is
  focused, otherwise `flashFrame(true)` (Linux: taskbar flash via
  `_NET_WM_STATE_DEMANDS_ATTENTION`), and the flash clears on the window focus
  handler.
- Effects: notification itself is always silent; a separate sound may play per
  the `notificationSound` setting (settings surface not ported natively yet —
  see deviations). Click consumes the aggregate, closes the tag and navigates
  to the conversation route.

## Native design

New module `packages/native/src/native-notifications.{h,cpp}` inside
`workspace-native-core` (already links `Qt6::DBus`):

1. `notificationPolicy` (pure functions): trigger classification + mute rules +
   viewport block, mirroring `shouldWorkspaceDesktopNotify` value for value.
   Mention signal comes from the server DTO tri-state `Message.mentioned`
   (domain.cpp comment: the client never parses mention entities); wildcard
   detection ports the reference regex to `QRegularExpression` verbatim.
   Stream modes: `all_messages | mentions_only | muted` (native-controller.cpp
   `channelModes`); topic modes: `default | follow | mute`.
2. `NotificationAggregateRegistry`: ports the bucket/tag scheme above
   byte-for-byte (`bucket:<owner>::<conv>`, `bucket:<owner>::<topic>:author:<author>`,
   `msg:<owner>::message:<uuid>`), latest-by-(createdAt, uuid) snapshots,
   read-close semantics and owner drain prefix `bucket:<owner>::`.
3. `NotificationDedup`: recent `owner::messageUuid` LRU capped at 200.
4. `NotificationService` (QObject, owned by `NativeController`): receives
   background candidates from `applyRealtimeEvent` (message.created events that
   are not in the on-screen conversation), metadata resolution from the loaded
   `m_streams`/`m_topics` lists (a candidate is metadata-complete when the
   stream is known, or the topic is known, or channel rules already resolve —
   same missing-metadata rules as the hook); holds a grace timer
   (`metadataGraceMs = 30000`, `metadataRetryMs = 250`, both settable for
   tests) that falls back to the generic notification; on a positive decision
   upserts the aggregate, formats the title through `tr()` plural forms, asks
   the transport to show (closing any active notification with the same tag
   first — one active per tag), and requests attention when the window is not
   focused. `setWindowFocused(true)` clears the attention indication. Read and
   deletion events route through `onMessagesClosed(uuids)` implementing the
   close/re-render rule; clicking a tag consumes the aggregate, closes it and
   opens the conversation (`stream` + `topic` uuid) exactly like the reference
   click route.
5. Transport seam: `NotificationTransport` abstract interface
   (`show(tag,title,body,silent)`, `closeByTag`, `requestAttention`,
   `clearAttention`). Tests inject a fake (no session bus, no window). The
   production transport is `DBusNotificationTransport` —
   `org.freedesktop.Notifications.Notify` on the session bus with the tag
   carried through the notification id to `ActionInvoked` for click routing —
   and the attention adapter sends `_NET_WM_STATE` client messages
   (`_NET_WM_STATE_DEMANDS_ATTENTION`, add/remove) to the root window over
   xcb (the native window's own tool; linked only on Linux when xcb headers
   exist, attention otherwise degrades to a no-op without failing delivery).
   `MAX_NOTIFICATION_LENGTH = 200` truncation and the empty-title rejection live
   in the transport base, matching `main.ts:1118`.
6. Controller/window wiring (`main.cpp`): an event filter forwards
   `QEvent::ActivationChange` to `controller.setWindowFocused(window->active)`,
   mirroring Electron's focus/blur -> `window:activity` channel and the
   `flashFrame(false)` focus handler.

Owner key = `NativeController::creationOwnerKey()` so the tag schema stays
multi-owner-correct even though one window hosts one session.

Viewport rule: conversation on screen when the message's stream equals the
active stream and (no topic is selected OR the message's topic equals the
selected topic) — same predicate as `NativeController::inConversation`,
matching the reference `openConversationId === topic || stream` rule (stream
aggregate route suppresses the whole stream).

## Declared deviations (named per the design-fidelity rule)

- No permission banner. `shared/lib/notifications.ts` gates on a localStorage
  flag and proves delivery with a test notification because browsers require a
  user gesture for the web Notification API; a native Linux app talks to the
  desktop notification service without such a gate. Banner parity (and the
  `notificationSound` picker) is tracked as follow-up scope, not implemented
  here.
- Sound: the reference notification is always `silent: true` and any sound is a
  separate settings-driven playback; the native settings surface for
  `notificationSound` is not ported, so native shows silent notifications only.
- The generic-notification body is `username · host` derived from
  `Session::publicProfile()["username"]` and the API origin hostname
  (`ApiClient::server()`), mirroring `resolveGenericNotificationAccountLabel`
  (invalid origins produce an empty body, never a raw URL).

## Acceptance (all in the existing `native-native-controller` group)

1. Policy matrix fires/stays silent exactly per `shouldWorkspaceDesktopNotify`.
2. Two messages from one author in one topic produce ONE tagged notification
   titled `2 messages from X · Native channel · Native topic` (en locale).
3. Reading the newest message closes the tag; reading an older one keeps it
   with a smaller count.
4. Focused window with the conversation on screen suppresses delivery.
5. At most one active notification per tag (show closes the previous tag holder).
6. A candidate with unresolved conversation metadata falls back to the generic
   "You have a new message" after the metadata grace window.
7. Attention: an unfocused delivery triggers the attention indication once and
   it clears when the window gains focus; a focused delivery never triggers it.

## Transport hardening (review round, pre-merge)

- `Notify` is sent with the exact spec signature
  `(s app, u replaces_id, s icon, s summary, s body, as actions, a{sv} hints, i expire)`;
  the earlier seven-argument form was rejected by real daemons. The client
  declares the `default` action explicitly so `ActionInvoked` routing is
  guaranteed even on servers that do not synthesize it.
- Mute policy never runs on guessed metadata: topics outside the loaded
  catalog (background streams) are fetched on demand (`stream_topics/{uuid}`)
  and the candidate holds in the grace window until the detail resolves;
  failures are remembered per topic so a broken endpoint costs one request,
  not a retry storm.
- Sign-out closes every banner of the retiring owner before the session
  clears (`closeAllActiveMessageNotifications` parity): old message content
  can never outlive its account, and pending candidates of the old owner are
  dropped.
- Client-side stores are bounded: 2048 decided messages (FIFO), 64 pending
  candidates (oldest first), 128 live aggregate tags (coldest evicted).
- Plural wording follows the repository convention (singular/plural are two
  source strings, `tr("%n messages", "", count)`), because Qt's second `tr`
  argument is disambiguation, not a plural source.

New acceptance cases: 8. the session-bus `Notify` call matches the spec
signature and declares the default action (fake service on the bus);
9. a muted topic that is absent from the loaded catalog stays silent;
10. sign-out closes the owner's live banners; 11. decided/pending stores stay
bounded under message floods.

## Reviewer round 2 hardening

- Aggregate eviction now also closes the evicted banner on the transport.
  A coldest-evicted bucket previously lost its registry entry while the desktop
  banner stayed open untracked forever (no tag close, no future update).
- Topic metadata backfill is kept in a notification-only map instead of being
  appended to the active stream's topic catalog. `topics()` renders every
  catalog row, so the old append leaked background-stream topics into the
  conversation list UI. Active-catalog rows win over backfilled rows.
- The number of concurrent topic metadata backfill requests is capped at 8 per
  pass; the next grace tick retries whatever did not get a slot, so a flood of
  unknown topics cannot fan out into an unbounded request burst.
- Owner retirement additionally clears the backfill map and the in-flight set.

New acceptance cases: 12. eviction tag-closes the evicted aggregate's banner
(132 distinct-author buckets past the bound against the recording transport);
13. a backfilled background topic never reshapes the active catalog's topic
list (the pollution repro reverted to the old append fails this guard).

## Reviewer round 3 hardening

- An aggregate bucket retains at most 64 message rows; the coldest rows are
  dropped together with their message index entries when a long conversation
  exceeds the window. The displayed count keeps growing (and decrements when
  reads remove rows), so the banner wording stays honest while client memory
  stays bounded per bucket.
- Generic fallback banners (the msg: tag scheme shown when metadata never
  resolved) are tracked per owner outside the aggregate registry, closed on
  owner retirement together with their click routes, and pruned from tracking
  when a read closes them. Sign-out can no longer leave an untracked generic
  banner on the desktop.
- Account transitions: the controller session is constructor-injected and
  never reassigned, so logout is the only owner transition and the retirement
  sweep covers aggregates, pending candidates, decided entries, fallback
  banners and the notification topic backfill.

New acceptance cases: 14. per-bucket row bound with an honest display count;
15. sign-out closes generic fallback banners (RED observed: the generic banner
survived logout before the fix).

## Reviewer round 4 hardening

- The notification topic backfill map is bounded (256 entries, oldest evicted);
  it is a lookup cache, so dropping an entry only costs a later re-fetch.
- Generic fallback banner tracking is a FIFO with a hard bound: once 256
  generic banners are live, showing a new one closes the oldest. No shown
  banner can end up untracked, so sign-out closes every remaining one
  (regression case drives 300 generic banners; RED observed with the old
  capped-skip tracking - banners past the cap survived logout).
