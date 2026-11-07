# Native stream aggregate

The native core treats a selected stream without a selected topic as a distinct
aggregate conversation. It requests `/api/workspace/v1/messenger/messages/` with
`stream_uuid` and **omits** `topic_uuid`. Every returned message still has its
actual validated topic, author and placement UUID. Topic history supplies both
query UUIDs. The two routes have independent pagination and draft scopes.

This ordinary, unencrypted aggregate route is a feed with a choose-topic prompt.
Sending, attachment upload and quote-to-composer are disabled there; selecting a
real topic restores its composer. No implicit General/default-topic send target
is introduced. The existing independent native Edit dialog is retained as a
deliberate UI difference; its canonical confirmation checks the message's actual
topic and author. Encrypted-provider composition remains separate work.

The source caller is `chat-page-workspace.ui.tsx` at `showStreamTopicPrompt` and
its footer branch, not the generic `resolveSendTarget` default-topic helper. The
web caller enables topic labels and dividers for stream selection. Labels use
the real topic catalog; blank/unknown names do not become invented General
labels. Native rows expose `authorId`, `topicId`, `topicName` and the existing
validated `createdAt`. UI grouping uses author UUID, topic UUID and local day;
topic-run dividers occur only when the preceding topic changes. Closed topics
remain in the choose-topic rail. Topic ordering uses active, muted and closed
groups, then descending last loaded message timestamp or validated topic update
timestamp, without a special default-first rule.

## Read boundaries and asynchronous safety

`markVisibleMessagesRead(ids)` accepts at most 256 loaded visible UUIDs. It
coalesces the latest boundary separately for each actual topic and dispatches a
bounded serial queue. Duplicate viewport callbacks do not add duplicate intents.
One failed topic finishes its bounded attempts before the next queued topic;
leaving the route, changing account/project or replacing the API generation
invalidates pending work. Confirmation updates captured read flags only, without
replacing concurrent message content or marking another topic's rows. Independent
topic counter responses do not cancel each other, and older counter responses
cannot overwrite a newer catalog/realtime revision.
Topic counter GETs remain independent; the stream counter GET is deferred until
the visible-read queue drains and is sent once for that batch. A successful
earlier topic still triggers that stream refresh when the last topic fails.
Route/session changes discard the deferred refresh.

The endpoint `POST messages/<anchor>/actions/read_up_to/invoke` marks only the
anchor's actual topic through `(created_at, UUID)`. Viewport visibility never
substitutes the explicit whole-stream read action. The UI must pass all loaded
rows that actually intersect the active, visible viewport; a single bottom row
of an interleaved aggregate is insufficient. No aggregate tail is implicitly
marked read.

History retains microsecond/UUID ordering, reconciliation through the oldest
loaded boundary, and one recovery from a deleted cursor. A refresh traversal is
bounded to 500 HTTP pages, 100,000 rows and 32 MiB of accumulated compact JSON,
including pages with no rows and a unique next cursor. The transport's existing
per-response bound applies separately. The compact JSON budget is not a claim
of a 32 MiB wire-traffic limit.

## Shared response-cache projection

Schema version 3 preserves existing version-2 topic pages and their indexes.
Older clients reject the new semantic version. `Kind::History` with an empty
**exact key topic** stores an aggregate response; the stream UUID remains
mandatory and each row must contain a real topic UUID. Empty topic in an
invalidation selector retains its existing wildcard meaning.

A topic-scoped change affects that topic's pages and all aggregate pages for the
same stream. Read confirmation patches indexed matching IDs in both families.
Canonical payload/star/reaction projections preserve their field masks and may
insert a confirmed new row only into an already existing bounded head. Raw
ciphertext/control payloads, saved ages and pagination markers are retained;
projected pages stay stale. Topic/message deletion hard-invalidates both page
families and dominates queued aggregate refills. Other topic pages and other
owners retain their independent state. API, route, service, lease and per-field
revision fences remain authoritative; a saved response cannot grant mutation
permission.

A `topic.deleted` event removes that topic's loaded rows and catalog entry
without an unsolicited history refill. This follows
`messenger-realtime-applier.lib.ts:529–551`; the earlier planning assumption of an
immediate refill was corrected after inspecting the effective reducer. Existing
pagination remains available, including deleted-marker recovery on the next
explicit load.

The cache uses its existing single asynchronous worker and bounded queue. No
second database, full-history reconstruction, raw credential persistence or new
physical-cache maintenance mechanism is added. Cold reopening can hydrate both
families after messenger failures while mutations remain blocked. Settled warm
stream/topic navigation reuses fresh responses; authentication, realtime traffic
and visible read-marker requests are separate from redundant history GETs.

## Local regression coverage

The storage/service tests cover mixed-topic round trips, schema migration,
flag/field projection into both families, preservation of encoded payloads and
age/cursors, scoped deletion and queued refill dominance. Controller tests use
actual local HTTP and WebSocket servers for strict mixed-topic history,
pagination and cursor deletion, canonical identity, read failures and races,
counter independence, cold restart after a confirmed read, and warm request
counts. A warm fixture observes one aggregate plus one topic history GET during
initial loading and no further history GET over three settled round trips.

Visible UI and packaged-runtime acceptance are separate from these core tests;
see the corresponding versioned UI/package evidence. This slice does not
implement channel creation, an encrypted aggregate composer or full desktop
feature parity.
