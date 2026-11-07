# Native ordinary Start chat

This slice opens or creates an **active unencrypted native direct conversation**.
It uses the real stream API and the existing stream aggregate route. Selecting a
person opens the aggregate feed and the real choose-topic prompt; selecting a
returned topic opens its composer. No empty-topic or invented General send target
is introduced. UI and installed-runtime acceptance are recorded separately from
core HTTP tests.

## Contract and identity

The reference is web tag `0.9.11` (`95495b10b828f853c9e770b47216073eb973fa95`):
`features/create-chat/create-chat-dialog.hook.ts:508–527`,
`entities/messenger/messenger-create-chat-actions.lib.ts:484–520`,
`shared/api/messenger-streams.api.ts:71–110`, and the effective stream prompt in
`pages/chat/chat-page-workspace.ui.tsx:4105–4129,4722–4783`. The ordinary source
picker excludes self and has no encryption toggle.

Backend source audited at `5ef555366fbc69b99d0919611350914c6279b6fa` defines
`DIRECT_STREAM_NAMESPACE` and the canonical pair algorithm in
`workspace/messenger_api/dm/helpers.py:914–924`. It is UUIDv5 with namespace
`26f42131-4503-5f59-8aa6-32b20f551751` and UTF-8 name
`projectUUID:sortedCanonicalUserUUID[0]:sortedCanonicalUserUUID[1]`.
The same project/pair yields the same UUID for either viewer. This UUID is not a
random client attempt token. The server remains authoritative.

`api/controllers.py:743–763` assigns private identity, and
`api/v2_store.py:392–440,476–615` checks project membership, locks the canonical
identity and atomically creates the stream/default/bindings or reuses it. Both
participants have owner bindings. The global creator may be the peer;
`direct_user_uuid` is projected relative to the current viewer by migration
0152, so the returned peer must equal the selected person. Existing names,
descriptions and ordinary flags need not equal their initial defaults.

The native path is:

1. Admit only a verified current session, fresh catalog and a non-self person
   from the verified current directory. Malformed, foreign and stale-only
   directory entries cannot authorize mutation.
2. GET `/api/workspace/v1/messenger/streams/<canonicalUUID>` first. A valid
   existing pair needs no stream POST. This lookup is a deliberate API-economy
   improvement over the pinned source's unconditional ensure POST.
3. Only an admitted fresh 404 can consume the explicit create authorization:
   POST `/api/workspace/v1/messenger/streams/` with exactly
   `{"name":"Direct","description":"Private workspace","source_name":"native","direct_user_uuid":peer}`.
   There is no random stream UUID, member POST, key POST or default-topic POST.
4. Verify the canonical stream with GET, then read the complete target
   `/stream_topics/?stream_uuid=<id>&page_limit=200`. The designated default UUID
   must match one actual `is_default` topic. Do not infer it from its name.
5. Read the complete Streams catalog as the authoritative publication/cache
   barrier, save validated raw Streams/Topics, and activate the aggregate.
   Opening its new aggregate history is the normal target-route GET. No extra
   IAM/users/old-history/global-snapshot reload is requested by this workflow.

The strict stream check includes current viewer/project, canonical UUID,
private=true, selected peer, native source with no provider identity, an owner
inside the pair, owner role, typed flags/counters/dates and encryption/key parity.
401/403, malformed DTOs and wrong identities are never treated as absence.

## Recovery and lifetime

Public `directChatPhase` is `idle`, `looking-up`, `creating`, `verifying-stream`,
`verifying-default`, `refreshing-catalogs`, `uncertain`, `unsupported`, `failed`
or `ready`. The public form contains attemptId, peerUuid, expectedStreamId,
confirmedStreamId, defaultTopicId and contextKey. The attempt ID only matches the
UI lifecycle; confirmedStreamId appears only after a verified canonical GET.

`checkDirectChat()` always reads only. Clicking the same uncertain person also
checks/retains that same attempt and never grants another hidden POST.
`retryDirectChat()` is a separate explicit authorization: it first performs a
fresh canonical GET, and can send at most one identical pair POST only on 404
with a still-verified directory peer and no current access-loss block. A
malformed/wrong-identity or denied response grants no create authority; a later
explicit check can establish a new current status. Deterministic identity and
backend locking prevent a replacement channel when a previous request arrives
late. There is no automatic mutation retry.

A busy DM and channel creation block each other's mutation admission. When not
busy, an explicit different-person choice may replace the owner's last intent;
the earlier pair remains recoverable from its deterministic identity. At most
32 verified owners retain one intent each in memory. These pending intents are
not persisted across application restarts.

Every stage checks owner/project, API generation, route/selection, catalog/trust,
cache service/lease and attempt/ticket before publishing. Actual target authority
loss changes the form lifetime key; ordinary directory display updates and
unread counters do not. Closing a form does not cancel a request that might have
reached the server. Reentrant subscribers cannot stamp an older success onto a
new person's intent, nor emit success for another owner or route.

Source-valid matching own stream/default/binding events share at most three
GET-only read reconciliations per explicit operation. Unsafe target changes,
deletions and ABA changes remain fenced. Saved Topics carry a separate topic
snapshot watermark: later read events cannot publish earlier counters, while
stream/binding events do not falsely pretend to modify the topic snapshot.
Bound exhaustion leaves honest uncertainty and an explicit status check.

All paginated default/catalog reads together are bounded per explicit operation
by 500 page requests, 100,000 returned rows and 32 MiB accumulated **compact JSON**,
including discarded pages and reconciliation reads. Empty pages with a next
marker are allowed; repeated markers fail. ApiClient independently limits each
response to 16 MiB. These are client resource bounds, not backend limits or a
claim about summed wire bytes.

## Presentation and cache

Logical conversation kinds remain none/stream/topic. Direct presentation is
separate: streams expose isDirect/peerId/displayName; isDirectConversation and
directPeer expose safe directory labels for both aggregate and topic routes.
Raw stream names are preserved. No IAM identity rewrite, avatars, remote images,
presence inference or extra private-member API is introduced.

Domain appends a typed optional `privateStream` field. Streams cache preserves
`private` as an optional boolean; legacy cache rows without it still parse as
false, without changing schema or physical table layout. Cold cached labels and
topics may be displayed while updating, but cannot authorize Start chat/send.
After settled authoritative publication, warm navigation reuses existing fresh
catalog/history pages. Read-marker and realtime traffic remain independent;
zero redundant navigation GET is not a claim of zero total network traffic.

## Boundaries

* Existing encrypted pairs report an explicit unsupported status with zero
  plain ensure POST and no downgrade/key replacement. The ordinary source action
  itself requires no E2EE. Crypto primitives and VaultV17 storage do not amount to
  completed encrypted-session/bootstrap/rotation support.
* Existing archived pairs report an explicit unsupported status with zero POST
  and no automatic unarchive. Source-equivalent archived-target navigation is a
  separate remaining gap.
* This entry rejects self like the pinned picker; the backend supports singleton
  self notes, which belong to the separate Favorites/self flow.
* Legacy/provider peer inference from two bindings is not implemented here.
  Native canonical identity uses explicit verified private/peer metadata.
* Local TCP/WebSocket tests verify client protocol, counts and lifetime fences.
  They do not prove PostgreSQL lock behavior. True simultaneous A→B/B→A backend
  acceptance needs two database sessions on an explicitly authorized stand.

Core RED/GREEN, full regular/sanitizer results and source hashes are retained in
the task-run handoff. UI, live backend, packaging and cross-platform acceptance
are separate evidence; this document does not declare full desktop parity.
