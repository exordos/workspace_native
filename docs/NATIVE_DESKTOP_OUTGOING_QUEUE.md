# Durable outgoing messages

Issue #9 adds restart recovery for composer text sends, including serialized
quotes, mentions and attachment references. Poll creation and realtime cursors
retain their separate workflows. The pinned React outbox is not persistent.

## Storage and admission

Reuse the owner-scoped `NativeDraftStore` SQLite database and the bounded
`NativeDraftService` worker. A separate outgoing table contains immutable client
UUID, exact stream/topic, payload, captured draft identity and creation time.
There is at most one unresolved send per conversation, with explicit record/count
and aggregate byte limits. Writes commit with the existing FULL/WAL transaction
before any POST. A storage failure leaves the input and sends no request.
The queue is independent of evictable history cache settings.

Ordinary bodies and captured public draft text have the same private filesystem
boundary as durable drafts. Encrypted entries contain the already encrypted wire
payload and public key-generation identifiers only: no plaintext, group keys or
private key material. The existing key vault pending-request table concerns key
requests, not message sends; it is not an outgoing-message store.

## Identity and recovery

The current backend `V2Store.create_message` derives the visible placement UUID
with UUIDv5(topic UUID, client UUID) and deduplicates the immutable client UUID
under the authenticated owner. Message GET accepts the placement ID and legacy
public aliases; a new client UUID is not a GET address.
Keep both the client ID and confirmed visible ID. After a cold start, load only
the verified server/project/user owner's records and reconcile by canonical GET.
No startup recovery POST is automatic. An explicit Retry repeats GET and may
POST the original immutable body and UUID only after an authoritative absence.
Conflicting identity/body, transport failure, missing authority and rejected
requests stay visible and recoverable; none creates a fresh retry UUID.

The composer shows a pending status and Check/Retry controls. One unresolved
entry blocks a new send to that conversation while leaving draft editing usable.
Sending the unchanged ordinary draft again performs the same checked Retry.
Other conversations can send independently. Queue state is published only after
owner, transport, catalog, destination and cache lifecycle checks. An encrypted
entry can be checked while keys bootstrap, but Retry requires the original key
and its unchanged deletion revision in the current vault lease. It never
re-encrypts under a replacement key or silently converts to plaintext.

A confirmed server message is distinct from external-provider delivery. Canonical
readback retires the client queue and never creates a new message to retry an
external provider. The backend worker's one-hour new-send expiry does not expire
a client's UUID. The current v2 message model exposes no provider-delivery status
field, so this change does not infer one from elapsed time or transport errors;
a separate provider-delivery UI requires that server contract.

## Completion and lifecycle

Only exact own canonical message verification completes an entry. Consume only
the captured draft snapshot: edits made during a send or after restart survive,
including editing away and back to the same text. Persist queue retirement;
if retirement fails, retain an unresolved entry and reconcile again, never POST
blindly. Logout/account switch stop publication and preserve the owning queue;
explicitly forgetting a saved account discards that owner's queued sends before
removing its credentials. Access loss prevents both replay and body publication.
Discard stays busy until its owner-scoped deletion commits, including during
cache replacement or key deletion; a failed delete keeps the entry available for
an explicit retry.

Schema migration is additive and transactional. Older versions must fail closed
on the newer schema rather than replaying or rewriting queue records. Returning
to an older build does not remove data; upgrading reopens it. No server schema or
API change is required.

## Required validation

Before product changes, reproduce a committed send with a lost acknowledgement,
process restart and duplicate retry on the unmodified controller. Then cover
pre-POST durable admission, original UUID/body recovery, placement identity mapping,
malformed/conflicting readback, 404-only replay, storage failure/capacity,
owner switching, destination revocation, edited drafts, account forgetting,
encrypted cold-start bootstrap and original-key deletion. Run storage/controller
regressions and actual visible Qt Quick status/retry scenarios in both locales
and themes. Linux fixtures establish client behavior; live provider delivery,
real OS credential-store restart, Windows and macOS remain separate acceptance.
