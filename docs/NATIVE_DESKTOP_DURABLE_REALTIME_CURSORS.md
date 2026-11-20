# Durable realtime cursors

Issue #15 follows the durable outgoing queue (issue #9, release 0.12.0,
[NATIVE_DESKTOP_OUTGOING_QUEUE.md](NATIVE_DESKTOP_OUTGOING_QUEUE.md)). The
issue text carried no measured contract block; this document records the
design and the reference pointers it was derived from.

## What cursor state exists today

`RealtimeClient` keeps one in-memory cursor per owner:

- `m_cursors` maps an owner string `server + '/' + project + '/' + user`
  (`src/realtime-client.cpp`, owner construction in `resume()`) to the pair
  `(epochGeneration, epoch)` (`src/realtime-client.h`, `m_cursors`).
- The cursor advances only after confirmed application: `acknowledge()` after
  the controller finishes one dispatched event and `acknowledgeSnapshot()`
  after the snapshot reload completes (`src/realtime-client.cpp`).
- The cursor is consumed in `resume()` (a saved owner cursor continues through
  `catchUp()` REST paging; no saved cursor takes `baseline()` plus a snapshot)
  and it is sent verbatim on the socket in `connectSocket()` as
  `last_epoch_version` + `epoch_generation` through
  `ApiClient::openRealtimeSocket()` (`src/api-client.cpp`).
- Expiry reconciliation: REST `410 EventsCursorExpiredError`, a socket
  `EventsCursorExpiredError` frame and close code `4410` all route to
  `cursorExpired()`, which calls `resynchronize()`: the in-memory owner cursor
  is removed and the client re-baselines through the epoch endpoint plus a
  fresh snapshot (`src/realtime-client.cpp`).
- Sign-out (`Session::signedOut`) clears the in-memory cursor map and owner.

Everything is process-lifetime memory. A restart always pays a full
`baseline()` + snapshot round-trip even when the last acknowledged epoch was
known seconds earlier, and a crash mid-catch-up re-sends the whole snapshot
cycle. There is one cursor per owner, not per stream/topic: the Workspace
events epoch is project-wide, so per-stream state would not be a smaller or
safer key (same ownership as the reference runtime,
`workspace-runtime.types.ts` ownership notes in
`~/dev/workspace_ui/docs/WORKSPACE_MESSENGER_CACHE.md`).

## Reference behavior (read-only pointers, `~/dev/workspace_ui`)

- Durable cursor storage contract:
  `packages/web/src/shared/lib/workspace-realtime/workspace-realtime-cursor.lib.ts`.
  Owner key covers account/instance/organization/project/user and excludes the
  runtime generation so a cursor survives reloads (lines 63-79). Stored values
  are validated on read and invalid or old-protocol rows are discarded
  (lines 34-61, 88-93). Writes never move a same-generation cursor backwards
  (lines 96-107).
- Transport use:
  `packages/web/src/shared/lib/workspace-realtime/workspace-realtime-runtime.lib.ts`.
  The transport reads `lastCursor ?? cursorStorage.read(owner)` when catching
  up or opening the socket (lines 553, 578, 923, 1044), writes the durable
  cursor only after appliers complete for the current owner (lines 537, 806),
  clears the durable row when the server reports the cursor pruned
  (`cursorExpired` handling, lines 604-617) and restores a replacement
  owner's own persisted cursor on owner replacement (lines 1133-1138).
- Cache doctrine: `docs/WORKSPACE_MESSENGER_CACHE.md` - "Transport advances
  its durable cursor only after every applier completes and the
  owner/generation is still current"; "Retention and logout/account cleanup
  are owner-scoped".

## Durable store design

New module `packages/native/src/native-realtime-cursor-store.{h,cpp}`: a small
synchronous SQLite store `RealtimeCursorStore` living beside the existing
durable queue storage.

- File and directory: default directory is
  `QStandardPaths::AppLocalDataLocation + "/drafts"`, exactly the directory
  `NativeDraftStore` already uses for the per-owner queue databases
  (`src/native-draft-store.cpp`, constructor default). The cursor database is
  a separate file `realtime_cursors.sqlite3` in that directory.
- Table: `realtime_cursors(owner_key TEXT PRIMARY KEY NOT NULL, epoch_generation TEXT
  NOT NULL, epoch_version INTEGER NOT NULL)` with `PRAGMA user_version=1`.
  Open-time hygiene mirrors `NativeDraftStore::open()`: WAL, `synchronous=FULL`,
  `secure_delete=ON`, 0600 file/owner guards, symlink rejection and a
  `QLockFile` so a second process cannot double-write.
- Why a separate file and not a table inside each per-owner queue database:
  the queue database is bound to one `NativeDraftService` lease and worker
  thread (`src/native-draft-service.cpp`, worker `DraftWorker`), while the
  realtime cursor belongs to the `RealtimeClient` lifetime on the GUI thread
  (`src/main.cpp` constructs it next to the controller). A separate file
  keeps the queue fail-closed admission semantics untouched, lets cursors
  persist independently of draft-store capacity or lease state, and avoids
  cross-thread lease plumbing for what is replay-safe best-effort state.
- Owner key: the durable row key is the sha256 hex of the compact JSON array
  `[server origin, project, user]` - the identical identity derivation as
  `NativeController::creationOwnerKey()` (`src/native-controller.cpp`) and
  the account-forgetting purge (`src/native-outgoing.cpp`,
  `forgetSavedAccountWithFollowUp`). Cursors therefore can never cross owners
  even inside the shared file, and the same key lets a future account-forget
  follow-up clear cursors with the queue. This mirrors the reference owner
  key rule (owner-scoped, generation excluded).
- Value contract (both read and write): generation is a non-empty string within
  1024 bytes; epoch is an integer in `[0, 9007199254740991]` - the same
  JavaScript-safe range `src/realtime-client.cpp` already enforces on
  incoming epoch values. Any row failing validation, a missing row, a locked
  or unavailable store reads back as "no cursor". Invalid rows are deleted on
  read, matching the reference discarding of old-protocol cursors.
- `save()` is monotonic within a generation: a write for the same generation
  with an epoch not greater than the stored one is a no-op; a different
  generation replaces (a pruned generation is cleared before re-baselining
  anyway). This reproduces "cursor only grows" so a delayed write from a
  replaced socket cannot roll catch-up back
  (`workspace-realtime-cursor.lib.ts` lines 96-107).
- Threading: single-thread use, created, used and destroyed on the GUI thread
  - unlike the queue worker, every operation is one bounded one-row
  transaction, which is why the throttle below exists.
- Wiring is a one-line seam in `src/main.cpp` via
  `RealtimeClient::useCursorStore()` (non-owning pointer). `native-controller.cpp`
  is untouched.

## Write cadence

- On every cursor advance (`acknowledge()`, `acknowledgeSnapshot()`) the client
  marks the cursor dirty and (re)starts a single-shot 500 ms coalescing timer;
  when it fires it flushes once for the still-current owner.
- Immediate boundary flushes: `suspend()` (covers every retry/teardown path)
  and `stop()`. `resynchronize()` discards the pending write and deletes the
  durable row instead of writing it. A failed persist keeps the cursor dirty
  and re-arms the coalescing window, and `resume()` re-arms it again when a
  dirty cursor survived a suspend with no further events arriving, so a
  transient storage outage self-heals without waiting for the next event.
- A failed durable delete keeps its owner key fenced out of restore and
  pending for retry; every owed key retries independently (a second failing
  clear never replaces the first one's retry), and a successful save only
  satisfies a pending clear when the key verifiably holds the replacement
  cursor - a same-generation monotonic no-op save leaves the invalidated row
  untouched, so the fence and the delete obligation stay in force until the
  row is actually gone or replaced. While the fence is held the replacement
  cursor also stays dirty with the coalescing timer re-armed: a no-op save
  wrote nothing, so if the dirty flag were cleared the later successful
  delete would remove the stale row and the replacement would never reach
  disk; the flush after the delete lands writes it to the now-empty key.
- Justification: catch-up pages arrive in batches of up to 100 events
  (`page_limit=100` in `catchUp()`), so an fsync per acknowledge would
  serialize the GUI thread during exactly the bursts where responsiveness
  matters. The reference persists the cursor per event, but only because
  IndexedDB writes are asynchronous and the main thread never blocks on them
  (`docs/WORKSPACE_MESSENGER_CACHE.md`, "Realtime memory bounds"). A full
  SQLite transaction on the GUI thread must amortize instead.
- Safety direction of the throttle: under-writing a cursor is always safe,
  over-writing never is. `catchUp()` requests events with
  `epoch_version>cursor`, so losing the last un-flushed batch replays at most
  the last 500 ms of already applied events - at-least-once delivery, which
  the existing idempotent applier path already tolerates. Persisting an epoch
  the client did not acknowledge is structurally impossible: only acknowledged
  values are ever queued for persistence, and flush only runs for the current
  owner while active.

## Restore-on-connect rules

In `resume()`, before deciding between baseline and catch-up: if the current
owner has no in-memory cursor and a store is attached, read the durable row.

- Valid row: install it into the owner cursor map as the transport position.
  A restore that came from the durable store (never from the in-memory map)
  additionally routes through the snapshot gate exactly like `baseline()`
  does - see "Transport position vs application-model hydration (issue #376)"
  below. The catch-up then pages REST from `epoch_version>` the persisted
  epoch and hands `last_epoch_version` + `epoch_generation` to
  `ApiClient::openRealtimeSocket()` on connect (`src/api-client.cpp`). Only
  the epoch endpoint round-trip is skipped; no epoch fetch is invented for a
  restored cursor.
- Missing, invalid, locked or unavailable row: take the untouched `baseline()`
  path (epoch endpoint, snapshot required, fresh cursor). This is also exactly
  the pre-issue behavior, so a corrupt durable file is a no-regression case,
  not an error state.
- The store is consulted for the exact owner key computed at connect time; a
  row for any other owner is invisible by construction.

## Transport position vs application-model hydration (issue #376)

Issue #15 shipped with the premise that a restored durable cursor can "take
the existing catch-up path - same behavior as the current in-memory
reconnect, now across process exit". That premise conflated two different
pieces of state and produced a P1: a fresh process that restored a cursor
authenticated successfully, went straight to catch-up, and never emitted
`snapshotRequired`, so `NativeController::reloadSnapshot` - the only path
that requests `/streams/` and `/users/` - never ran. The chat list stayed
empty until a manual refresh. `openCache()` deliberately does not load the
catalogs; only the snapshot path publishes them.

The corrected doctrine separates the two state machines:

- TRANSPORT POSITION: the durable `(epoch_generation, epoch_version)` pair.
  It answers "from which server event may replay continue?" It survives
  process exit and is owned by `RealtimeClient` plus
  `RealtimeCursorStore`.
- APPLICATION MODEL HYDRATION: the controller's authorized catalogs
  (streams, users, and the conversation projection). It answers "has this
  process ever published a valid authorized snapshot?". It is owned by
  `NativeController` and is established only through the
  `snapshotRequired` -> `reloadSnapshot` -> `snapshotReloaded` ->
  `acknowledgeSnapshot` cycle (issue #15's own write-cadence rule: a cursor
  is only persisted after an acknowledged snapshot or acknowledged event, so
  a persisted row proves the previous process was hydrated - never this one).

A durable row restored from the store therefore proves the former process
hydrated, and this process nothing: every fresh process must complete one
snapshot cycle even when the transport position is known. The
`m_cursors` in-memory map retains the old contract (a same-process reconnect
is an already-hydrated consumer and keeps skipping the snapshot).

Policy applied in `resume()` when the durable row restores:

1. Install the restored position as the epoch/`last_epoch_version` the socket
   and REST catch-up will use (transport benefit of issue #15 retained: no
   epoch endpoint round-trip).
2. Enter the same `m_waitingSnapshot` gate the baseline path uses and emit
   `snapshotRequired`. The consumer's `acknowledgeSnapshot` releases the
   gate and catch-up resumes from the restored position. A consumer that
   never acknowledges (a standalone transport with no controller) stalls at
   the gate on purpose - holding events back is always safer than delivering
   them to an unhydrated model, and it matches the pre-existing baseline
   behavior where no socket opens without a snapshot ack.

Catch-up around the initialization is reconciled, never lost:

- While the snapshot is in flight the gate blocks `catchUp()`, `nudge()` and
  `connectSocket()`, so no event can overtake the catalogs.
- Catch-up requests events with `epoch_version>` the restored epoch, so
  events that arrived (server-side) during the initial loading are
  re-delivered after the acknowledgement and applied on top of the fresh
  catalog - at-least-once delivery onto a current snapshot, which the
  idempotent applier path already tolerates (same safety direction as the
  write-throttle rule in "Write cadence").
- Owner/session fencing is untouched: restore still consults only the exact
  durable owner key, pending-clear fences still exclude invalidated rows,
  and a stale restored generation is still pruned through `410`/`4410` into
  `resynchronize()` (which deletes the row and re-baselines with a fresh
  snapshot). The controller's authentication path continues to drive
  `resynchronizeRequested` -> `resynchronize()` before any restore happens,
  and a failed snapshot still retries through `snapshotFailed` ->
  `retrySnapshot()` while the gate stays closed.

This holds with an empty, evicted or disabled history cache: the snapshot
requests are authoritative network reads (`cachedCollection` with
`authoritative=true`), and the cursor database is a separate file that
cache eviction never touches - which is exactly the user scenario (restart
after a cursor was persisted, history cache cleared, catalogs unrecoverable
from replayed events alone).

Regression coverage lives in
`packages/native/tests/realtime-client-test.cpp` (`native-realtime-client`
group): `restoredDurableCursorBootstrapsFreshControllerModels`,
`restoredDurableCursorReconcilesEventsArrivingDuringInitialLoad`,
`restoredDurableCursorBootstrapsWithCacheDisabled`,
`restoredDurableCursorBootstrapsAfterCacheEviction` and
`sameProcessStopStartKeepsInMemoryCursorWithoutSnapshot` bind
NativeController + NativeCacheService + RealtimeCursorStore + RealtimeClient
in `src/main.cpp` order against a mock gateway; the transport cases
(`persistedCursorAdvancesAndFreshClientResumesFromIt`,
`persistedRestoreRequiresSnapshotWithoutEpochRoundTrip`,
`cursorExpiredAfterRestartResynchronizesAndReplacesPersistedCursor`,
`noopSaveKeepsInvalidationFenceAndStaleRowNeverRestored`) now acknowledge
the restore-time snapshot as the production controller does. No new `tr()`
strings are introduced.

## Reconcile semantics (cursor expired/unknown)

The existing path stays authoritative and the persisted row follows it:

1. Server reports the cursor pruned/unknown (REST `410`, socket frame or close
   `4410`) -> `cursorExpired()` -> `resynchronize()`.
2. `resynchronize()` removes the in-memory cursor and also deletes the durable
   row (after discarding any pending throttled write). A restart between the
   expiry and a completed resync therefore finds no row and connects clean.
3. The client re-baselines: epoch endpoint, fresh generation, snapshot reload.
4. The post-resync cursor is persisted only when the snapshot is acknowledged
   (`acknowledgeSnapshot()`) or the first event after it is acknowledged -
   the pruned cursor is replaced after a successful resync, never re-sent
   blindly. This matches the reference `clear()` on pruned plus
   write-after-apply (`workspace-realtime-runtime.lib.ts` lines 604-617, 537).

## Sign-out and owner-switch wipe rules

- `Session::signedOut`: the client first flushes/stops pending durable writes
  (via `suspend()`), then clears the durable row for the owner key captured
  while the session was still live. If the delete fails, the running client
  keeps the key pending and skips the stale row on the next login. It retries
  the delete and persists the new baseline when storage accepts writes again.
- Owner replacement inside a live client (`resume()` sees a different owner):
  the previous owner's durable row is cleared before the new owner is bound.
  The incoming owner may restore its own row (reference owner-replacement
  restore, `workspace-realtime-runtime.lib.ts` lines 1133-1138), but no owner
  can ever resume from another owner's row - by key equality and because the
  departed owner's row is deleted.
- After any wipe the next connect sends no cursor: `baseline()` plus snapshot,
  so a signed-out owner's stream position cannot be resumed by anyone.

## Crash consistency

- Every durable change is a single-row transaction under WAL + `synchronous=FULL`
  (same guarantees the queue DB relies on, `src/native-draft-store.cpp`), so a
  torn write cannot exist: after a crash the row is either the previous or the
  new complete value, never a mix.
- Absent, unreadable or schema-mismatched cursor DB -> store unavailable ->
  read returns "no cursor" -> clean baseline connect. The client never
  degrades to a guessed, partial or cross-owner cursor; the only fallback is
  the pre-issue cursor-less path.
- A crash inside the throttle window loses at most the last 500 ms of
  acknowledged advances and replays them at-least-once (see "Write cadence").

## Required validation

Regression coverage lives in the existing `realtime-client` CTest group
(`packages/native/tests/realtime-client-test.cpp`, `packages/native/CMakeLists.txt`
test list), using the existing mock gateway/WebSocket fixtures:

- `persistedCursorAdvancesAndFreshClientResumesFromIt` - acknowledge advances
  the durable row; a fresh `RealtimeClient` instance restores it into
  `openRealtimeSocket` on next connect.
- `sameOwnerReconnectResumesFromPersistedCursorWithoutBaseline` - no epoch
  endpoint call, no snapshot, socket resumes at the persisted epoch.
- `cursorExpiredAfterRestartResynchronizesAndReplacesPersistedCursor` - the
  stale restored cursor is pruned, resynchronize runs (deleting the durable
  row before the fresh baseline), and the post-resync generation/epoch
  replaces the durable row after the snapshot acknowledgement.
- `signOutClearsPersistedCursorAndNextConnectStartsClean` - sign-out wipes the
  row; the next connect re-baselines and cannot resume the old position.
- `durableCursorStoreIsOwnerScopedMonotonicAndValidating` - store-level owner
  fencing (rows never cross owner keys, wipes are owner-scoped), the
  same-generation monotonicity rule and malformed key/value rejection.
- `missingOrCorruptDurableCursorFallsBackToCleanConnect` - absent row,
  type-tampered row and a garbage database file all fall back to the
  untouched baseline path with no connection regression.

No new `tr()` strings are introduced (internal transport state only), so no
`workspace_ru.ts` changes are required. No server API or schema change is
needed.
