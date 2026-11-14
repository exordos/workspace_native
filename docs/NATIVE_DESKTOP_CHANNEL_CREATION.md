# Native ordinary channel creation

This slice creates an unencrypted native channel and preserves its selected
initial members. It opens the verified stream aggregate after success. The
aggregate has the existing choose-topic prompt; it does not acquire a default
topic composer. Channel directory discovery and invitation management
remain separate features; the Protected channel (E2EE) creation path is
specified in the sibling
[encrypted channel creation slice](NATIVE_DESKTOP_ENCRYPTED_CHANNEL_CREATION.md),
with key requests, grants and rotation governed by
[the crypto acceptance doc](NATIVE_DESKTOP_CRYPTO.md).
The sibling [ordinary direct-chat slice](NATIVE_DESKTOP_DIRECT_CHATS.md)
uses the initial Start chat tab and shares the creation write gate.

## Contract and user intent

`NativeController::createChannel(name, description, inviteOnly, announce,
memberUuids)` freezes one request and one generated stream UUID. Name and
description use ECMAScript-compatible trim and Unicode code point bounds of
1–255 and 0–10000. The two flags are independent. Selected UUIDs are normalized,
deduplicated, and exclude the current user. Nonempty selections require the
current verified directory; an empty selection does not wait for that directory.
The directory exposes only `id`, `displayName`, `username`, and `email`.

The source ordinary path is in
`packages/web/src/features/create-chat/create-chat-dialog.hook.ts` and
`packages/web/src/entities/messenger/messenger-create-chat-actions.lib.ts`.
The source dialog's caller supplies four tabs. This C++ workflow covers the
ordinary channel path, including initial members, rather than claiming those
four workflows are all implemented.

The backend transaction is in
`workspace/messenger_api/api/v2_store.py`, `_create_stream` (lines 392–615 in
the audited backend snapshot). It creates the owner binding and default topic.
The backend's same-UUID comparison does not compare every creation flag;
therefore the native client never replays a stream POST after an unknown result.
The designated default UUID is server-owned, not inferred from the topic name.
The backend can select another existing default or delete the current default
without deleting its stream.

## Requests and phases

The initial path sends one stream POST, a canonical stream GET, the complete
target topic catalog, an optional member POST, and the complete stream catalog.
Publication reuses the verified target topics, then navigates to the target
aggregate. Its ordinary history GET is necessary. There is no unconditional
users GET, old-conversation history GET, or global snapshot reload.

The stream body is:

```json
{
  "uuid": "one immutable attempt UUID",
  "name": "trimmed name",
  "description": "trimmed description",
  "source_name": "native",
  "invite_only": false,
  "announce": false,
  "encryption": false,
  "current_encryption_key_uuid": null
}
```

The member action is
`POST /api/workspace/v1/messenger/streams/<same UUID>/actions/add_users/invoke`
with `{"member":["frozen selected UUIDs"]}`. Existing active bindings may have
a promoted role or a different original `who_uuid`; the native validator accepts
the actual role enum and does not invent an actor-only or member-only rule.
The action itself preserves those bindings in the backend.

| Phase | Meaning and permitted recovery |
| --- | --- |
| `idle` | No admitted request, including local validation errors. |
| `creating` | The one stream POST is pending. |
| `verifying-stream` | Canonical GET for the same UUID. |
| `verifying-default` | Complete target topic catalog is being checked. |
| `adding-members` | One explicitly authorized member POST is pending. |
| `checking-members` | Complete active binding catalog is being checked. |
| `refreshing-catalogs` | Complete stream catalog is being checked. |
| `uncertain` | GET-only **Check status**; no hidden mutation retry. |
| `members-required` | Channel exists; explicit **Add members** for the frozen set. |
| `members-failed` | Channel exists; explicit **Retry members** for the frozen set. |
| `failed` | Definite initial failure or authoritatively deleted created stream; an explicit new request may use a new UUID. |
| `ready` | Verified publication and matching target navigation completed. |

`checkChannelCreation()` performs only reads. After an ambiguous stream POST it
can discover the stream and its default, but it never silently adds the frozen
members. `retryChannelMembers()` first verifies the same stream, default, and
current bindings. It skips the member POST when the selected set is already
complete. Otherwise, one explicit authorization consumes one member POST; a
subsequent read reconciliation cannot replay it.

`channelCreationForm` retains `attemptId`, the frozen input, `memberUuids`,
`confirmedStreamId`, `defaultTopicId`, `membersAttempted`, and the captured
`contextKey`. `confirmedStreamId` is exposed only after a valid canonical GET.
Closing the UI dismisses the form rather than cancelling a submitted request.
Unresolved same-owner forms can be reopened and explicitly checked in a new
current route context. Intent retention is in process only: it is not a durable
outbox or restart recovery journal. Unresolved owner slots are capped at 32 and
retained forms at a bounded compact representation budget; unresolved identities
are never evicted to admit another mutation.

## Authority and realtime

Admission requires authenticated, verified Session ownership and authoritative
catalog trust. API generation, owner/project, route selection, catalog barrier,
cache service/lease, and active target authority fence every asynchronous stage.
Cached display rows never authorize creation. Canonical stream, default topic,
binding, and complete catalog DTOs are validated before publication.

`channelCreationContextKey` describes ownership/API/route/cache/trust lifetime.
Ordinary unread counters and user display/presence changes do not close a form
or discard selected UUIDs. Actual active-target ACL loss or deletion changes its
lifetime. Per-target mutation revisions provide a separate fence, including an
A→B→A change while an older response is delayed. The saved target topic catalog
has its own watermark: a newer request's captured revision cannot make that
older snapshot fresh again between membership and stream-catalog stages.

Matching own `stream.created`, `topic.created`, and `stream_bindings.created`
events may reconcile a delayed read automatically. The binding-batch payload's
`uuid` is its stream UUID, and `items` contains the bindings. At most three
read-only reconciliations are permitted per explicit operation, shared across
its stages. Exhaustion leaves an honest `uncertain` state and an explicit
GET-only check. Unsafe updates, ACL loss, deletions, and older captured revisions
cannot be laundered by a later matching own event. There is no automatic write
retry.

An authoritatively deleted created stream is terminal; it permits a new explicit
intent, never an automatic replacement. Deletion of the default topic retains the
confirmed stream and allows GET-only recovery if another authorized caller later
selects an existing replacement default. Completed old intents do not change the
lifetime of an unrelated new form.

Each collection traversal is bounded by 500 HTTP requests, 100000 accumulated
rows, and 32 MiB of accumulated compact JSON, including discarded pages on
read-only reconciliation. Empty pages with a next marker are accepted; repeated
markers are rejected. ApiClient's independent response bound is 16 MiB. The
32 MiB number is not a raw-wire traffic limit.

## Local verification

`native-controller-test.cpp` exercises actual local HTTP and, for the own-event
ordering cases, RealtimeClient with a real WebSocket server. It covers exact
request bodies/counts, Unicode and directory validation, ambiguous and definite
errors, explicit same-stream member recovery, typed identity rejection, bounded
pagination, delayed owner/project/route/session/cache/snapshot changes, unsafe
revision ABA, own-event reconciliation, deletion, subscriber lifetime, cold
saved-catalog hydration, and warm navigation without redundant catalog GETs.

These checks do not establish deployed backend acceptance, a release artifact,
or full desktop parity. Visible dialog behavior and packaging are verified
separately from this C++ transport/state-machine slice.
