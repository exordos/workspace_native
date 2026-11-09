# Native drafts

Persistent drafts are planned for 0.3.0. The wire parser, revision transport and
durable storage backend and bounded storage worker are implemented; the running
application's controller still holds drafts in memory. The composer coordinator,
API synchronization,
actual Main integration and packaged acceptance remain required before release.
This document describes the contract and acceptance targets, not complete parity.

## Existing contract

The current committed Source [draft API](https://github.com/exordos/workspace_ui/blob/a8cb5c990cedd57bba19085ff96007e744d2d16e/packages/web/src/entities/draft/draft.api.ts),
[chat synchronization](https://github.com/exordos/workspace_ui/blob/a8cb5c990cedd57bba19085ff96007e744d2d16e/packages/web/src/entities/draft/draft-chat-sync.lib.ts)
and [hydration](https://github.com/exordos/workspace_ui/blob/a8cb5c990cedd57bba19085ff96007e744d2d16e/packages/web/src/pages/chat/chat-page-draft-sync.hook.ts)
use stable client UUIDs and the existing Messenger resource:

| Operation | Contract |
| --- | --- |
| GET `/drafts/` | Owned project/user snapshots; `updated_at` ordering, 100-row pages and opaque pagination marker. |
| GET `/drafts/{uuid}` | Exact owned snapshot with a strong revision ETag. |
| POST `/drafts/` | Stable UUID, exact stream/topic and Markdown payload; freeze the first create intent across uncertain responses. |
| PUT `/drafts/{uuid}` | Payload only, with the current strong `If-Match`. |
| DELETE `/drafts/{uuid}` | Exact `If-Match`; 204 confirms removal, 404 confirms absence. |

The [backend contract](https://github.com/exordos/workspace_backend/blob/32d21ab35fe136c49882c85b3d5e9c49ecc5b0d4/docs/workspace_api.md#drafts)
limits non-empty canonical Markdown to 40,000 Unicode code points. A stale
revision returns 412 with the current body and ETag. No draft CRUD emits ordinary
messages or realtime events. Navigation therefore uses hydrated local state;
other-client changes require a reload or explicit refetch. Native draft parsing
verifies the expected project/user, all resource identities, numeric revision,
timestamps, Unicode bounds and an exact supplied strong ETag. Missing collection
ETags derive from each row's revision. Transport retains the ETag on canonical
HTTP errors but never on cancelled, redirected or stale-session responses.

The separately pinned [composer draft entity](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/entities/composer-draft/composer-draft.model.ts)
also defines local snapshot identities, reply tabs, consumed/delete records and
400 ms disk / 1000 ms remote debouncing. These fields are local editor metadata;
the wire API accepts only Markdown text. Pending uncommitted Source work is not
part of this implementation's reference contract.

## Local backend

`NativeDraftStore` stores unsent work in a separate owner-bound SQLite database
under `AppLocalDataLocation/drafts`. The existing owner hash separates server,
project and user; its value is checked inside the database as well as its filename.
Each owner has an exclusive process lock. Database initialization and batch edits
are transactions, with WAL and FULL synchronization. Files are private to the
user. Unknown schemas, corruption, invalid input, locked owners and storage
failures remain explicit failures; they do not masquerade as empty successful reads.

Records preserve exact text, reply-session JSON, a snapshot identity, current
revision, a frozen pending create body, conflict alternatives and consumed/delete
state. Private `:::` editor fragments are rejected in text, reply context and
pending/conflict bodies. The future coordinator must retain those private edits
in memory and remove an earlier durable public snapshot rather than saving the
protected plaintext.

`NativeDraftService` constructs, uses and closes SQLite on its single private
worker. Owner/generation leases fence late reads. Accepted writes keep their
captured owner across account changes. Destroying a callback receiver suppresses
its receipt while the accepted write still completes. Admission allows at most
eight data jobs and 32 MiB of serialized record input, plus two small control
jobs and one reserved shutdown barrier. Flush is a FIFO completion barrier;
each Apply receipt reports actual durable success or failure. Shutdown rejects
new work and drains accepted writes; destructor fallback also drains them
without depending on GUI callbacks.

Metadata pages contain at most 100 small
summaries; individual record reads are capped at 4 MiB, and one atomic batch at
100 records / 8 MiB. The SQLite page cache targets 2 MiB. These bounds describe
component admission, not whole-application memory measurements. Drafts are unsent
user work, so the history cache's LRU must not evict them. The configurable 10 GB
history-cache default and existing cache schema remain unchanged.

## Verification and remaining gates

The registered domain, API and draft-store suites cover owner/route admission,
strong ETags on real conditional HTTP responses, cancelled sessions, Unicode
limits, exact restart restoration, owner separation, locks, bounded pagination,
atomic rollback under an injected SQL failure, conflict/delete metadata, private
fragment rejection and corruption. A real child process commits a draft and exits
without destructors; a new store instance then reopens its committed WAL. The
default-null-cursor regression must enumerate the same rows as an explicitly
empty cursor. Linux packaging CI runs these suites before producing an artifact.
The CI job uses `C.UTF-8` for both LANG and LC_ALL so Qt's startup locale
diagnostic does not invalidate the subprocess test's strict empty-stderr check.
The child check retains that assertion and reports its captured synthetic
diagnostics on failure; it does not filter or suppress Qt messages.

The service suite exercises the real worker, SQLite writer contention with a
main-loop pulse, old-owner reads, accepted writes across owner changes, job and
serialized-byte saturation, receiver destruction, observer-triggered teardown,
destructor draining and shutdown behind full queues. Indexed conversation lookup
returns the newest restorable UUID with a deterministic tie-break; malformed
consumed metadata is an explicit corruption result rather than an empty draft.
Local Store32 and Service16 results pass. Application composition and API
synchronization remain separate gates.

Before publishing 0.3.0, complete and verify:

1. Debounced/coalesced composer state using the implemented bounded storage
   service, owner/shutdown flush and visible per-write persistence failures.
2. Bootstrap remote hydration, per-UUID serialization, fixed create retries,
   conditional updates/deletes, explicit conflict choices and pending recovery.
3. Exact selected/newest draft restoration in the actual composer, keeping newer
   input across late hydration, route switches and successful sends.
4. Consumed-record durability before server deletion, while a changed draft stays
   editable; no automatic resend or duplicate ordinary message.
5. Account/project/API replacement, private and encrypted editor boundaries,
   local-only reply/edit context and removal of inaccessible conversation data.
6. Real Source/Native matched screenshots, primary-account process restart,
   cross-client synchronization and the installed update into the public package.
