# Native drafts

Persistent ordinary drafts are implemented for 0.3.0. The wire parser, revision transport and
durable storage backend, bounded storage worker, local composer integration and
conditional API synchronization are implemented. Main constructs the store/model and binds the existing composer
to the verified account and ordinary conversation. The clean composer cache now
has bounded residency. Acceptance requirements and their scope are listed below.
This document describes the contract and acceptance targets, not complete parity.

## Existing contract

The pinned Source [draft API](https://github.com/exordos/workspace_ui/blob/a8cb5c990cedd57bba19085ff96007e744d2d16e/packages/web/src/entities/draft/draft.api.ts)
uses stable client UUIDs and the existing Messenger resource. Its earlier
[chat synchronization](https://github.com/exordos/workspace_ui/blob/a8cb5c990cedd57bba19085ff96007e744d2d16e/packages/web/src/entities/draft/draft-chat-sync.lib.ts)
and [hydration](https://github.com/exordos/workspace_ui/blob/a8cb5c990cedd57bba19085ff96007e744d2d16e/packages/web/src/pages/chat/chat-page-draft-sync.hook.ts)
are historical references; the composer entity below is the editor reference.

| Operation | Contract |
| --- | --- |
| GET `/drafts/` | Owned project/user snapshots; `updated_at` ordering, 100-row pages and opaque pagination marker. |
| GET `/drafts/{uuid}` | Exact owned snapshot with a strong revision ETag. |
| POST `/drafts/` | Stable UUID, exact stream/topic and Markdown payload; freeze the first create intent across uncertain responses. |
| PUT `/drafts/{uuid}` | Payload only, with the current strong `If-Match`. |
| DELETE `/drafts/{uuid}` | Exact `If-Match`; 204 confirms removal, 404 confirms absence. |

The [backend contract](https://github.com/exordos/workspace_backend/blob/32d21ab35fe136c49882c85b3d5e9c49ecc5b0d4/docs/workspace_api.md#drafts)
limits non-empty canonical Markdown to 40,000 Unicode code points. A stale
revision returns 412 with an owned DTO in the `current` envelope and its ETag.
Native accepts that envelope and the older direct DTO shape, subject to the same
project/user/route and strong-revision checks. No draft CRUD emits ordinary
messages or realtime events. Navigation therefore uses hydrated local state;
other-client changes require a reload or explicit refetch. Native draft parsing
verifies the expected project/user, all resource identities, numeric revision,
timestamps, Unicode bounds and an exact supplied strong ETag. Missing collection
ETags derive from each row's revision. Transport retains the ETag on canonical
HTTP errors but never on cancelled, redirected or stale-session responses.

The pinned [composer draft entity](https://github.com/exordos/workspace_ui/blob/95495b10b828f853c9e770b47216073eb973fa95/packages/web/src/entities/composer-draft/composer-draft.model.ts)
defines local snapshot identities, reply tabs, consumed/delete records and
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
pending/conflict bodies. The composer retains those private edits in memory and
marks an earlier durable public snapshot for removal rather than saving the
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

## Local composer

`NativeDrafts` coalesces ordinary input with a 400 ms timer and uses the existing
worker for all SQLite access. Initial reads cannot replace newer input; clearing
before initial hydration also disposes of the prior selected disk row. Account
changes retain captured writes while the foreground editor follows only the
verified current owner. Route switches force earlier input through the worker
without forcing each new keystroke. Exact local whitespace is preserved.
Whitespace-only input remains in memory so users can type an initial indent;
it does not become a durable empty body.

The controller captures a snapshot when creating a send intent and consumes only
that snapshot after canonical message read-back. A later edit remains, including
changing A to B and back to A while confirmation is pending. The same revision
rule also protects the existing in-memory editor. Attachment completions update
their captured ordinary conversation. Encrypted conversation drafts retain the
existing memory boundary.

Main calls the shared storage-shutdown helper for Quit and updater restart. It
flushes the model's coalesced input and per-write receipts before stopping the
draft and history workers. A failed flush leaves both workers available, retains
input and restores the window with a persistence notice; it does not report
successful shutdown or start the successor. This wiring and its component/window
checks do not establish installed-process or live-service acceptance.

## Server synchronization and the Drafts page

The controller binds synchronization only after verifying the account and stream
catalog. The verified session token scopes requests to the exact project; each
returned DTO is checked against that project and user. The draft endpoint does
not accept project_id as a query filter. One HTTP operation at a time
serializes each UUID; collection import uses 100-row pages and removes absent
Saved records only after a complete successful collection. It preserves local,
failed, deleting and conflicted work. Ordinary navigation reads local storage;
startup and explicit Drafts refresh fetch remote changes. Refresh waits behind
an in-flight mutation instead of cancelling a save.

Remote saves coalesce for one second. The first create body and UUID reach disk
before POST, and uncertain retries retain both. Conditional PUT/DELETE use the
accepted strong revision. Consumed records reach disk before DELETE. A later
edit, including the same text with a different snapshot, remains editable.
Transient failures back off; rejected writes remain visible until another edit
or explicit user action. Private promotion retains only deletion metadata and
uses exact GET to reconcile an uncertain prior create without replaying plaintext.

The Drafts activity page shows at most 100 previews ordered by local update time
and UUID, with Previous/Next navigation. Each preview contains at most 160 UTF-16
units plus an ellipsis; full bodies are read on the worker only when needed.
Cards open their exact UUID through verified topic resolution. Conflicts offer
server/local/delete choices; a failed consumed deletion offers retry. Actions
are owner/snapshot fenced and cannot overwrite input typed after the click.
Only the Drafts activity is enabled by this feature. Native quotes remain part
of the saved Markdown. The format preserves reply-session JSON, but Source reply
tabs and inline-edit sessions remain unfinished. Encrypted draft text retains
its existing memory-only boundary.

Authoritative access changes remove inaccessible local records on the worker,
independently of the draft endpoint being available. This removal is scoped to
the captured owner. A still-accessible stream becoming encrypted stops ordinary
synchronization and retains its prior ordinary record; newly typed encrypted
input stays in controller memory. The schema and decimal 10 GB history-cache default remain
unchanged; draft work is stored separately from the evictable history cache.

## Composer residency

The GUI model keeps at most 64 evictable clean records, 4 MiB of their serialized
payloads and 128 evictable conversation-metadata entries across accounts. Current
input, uncommitted writes, private/invalid input and actual pending operations
are retained separately. These are clean-cache limits, not a universal RSS cap.
Eviction removes only RAM state; the existing owner-bound SQLite data is retained.
An evicted conversation reloads from disk without a draft API navigation request.

Send tokens share an opaque residency lifetime. Queued and in-flight attachment
operations retain the same kind of lifetime, including when the conversation was
empty. The model holds weak references; releasing the last operation copy queues
trimming without a periodic timer. Dirty writes and failed saves remain protected.
The controller keeps one current ordinary-text fallback instead of retaining every
saved body. Encrypted/unbound unsent editor work keeps its existing RAM behavior.

Record pressure, byte pressure and empty-conversation metadata each have retained
failing regressions from the unbounded implementation. Tests also cover idle
same-empty edits, exact send snapshots under pressure, last-copy release,
attachment captions, failed writes across owners and model destruction. The
controller regression runs its prior source with only a diagnostics accessor and
reproduces retained text growing across 80 conversations.

The separately registered `native-native-draft-workload` exercises actual
Main.qml, the controller, draft workers and a working history cache against local
synthetic HTTP. It enters large texts in 160 conversations, checks the clean
budgets and one-text controller fallback, returns to an evicted conversation and
requires a history-cache hit. Linux `/proc/self/status` samples observe process
RSS; they are workload measurements, not cross-platform or installed-account
acceptance. The report is written under the build's `Testing` directory. The
history-cache default remains decimal 10 GB.

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
Local Store32 and Service16 results pass. The model tests cover coalescing,
hydration/clear ordering, account/route changes, distinct-draft revisit, exact send
snapshots, private input, write failures and the actual Main shutdown helper.
Four retained failing model checks precede the corresponding fixes; a separate
controller regression reproduces the prior same-text send-clearing defect.
The real Main.qml field is also exercised across two window/controller/worker
lifetimes, including initial spaces entered as native key events. The development
executable also opened its anonymous Wayland form and exited through real Ctrl+Q
with code 0; its isolated profile was removed. Live API synchronization and
installed-process checks remain separate
gates.

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
7. Repeat resource/load acceptance with the installed primary-account package.
   Clean-residency and synthetic Main.qml workload checks cover the local path;
   the cache limits alone do not prove a general process-memory bound.
