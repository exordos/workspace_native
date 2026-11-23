# Disk-cache retention and background prefetch (#729)

Status: draft (design step of issue #729). Reference: issue #729 body,
`docs/NATIVE_DESKTOP_DEVELOPMENT.md` (disk retention and budget section),
`docs/NATIVE_DESKTOP_CACHE_BENCHMARK.md`, #718 topic-preload sweep.

## Goal

Ordinary navigation and history browsing render from the on-disk cache before
any network reply finishes, background work warms likely-next routes without
displacing useful data, and inline image previews survive chat switches and
application restarts. The cache keeps the configured global default of
10 GB and honours the user's configured limit; nothing here changes budget
policy, only which data is kept warm and which requests are avoided.

## What already ships (verified against this tree)

- Retention vs freshness: expiry marks pages for revalidation and leaves saved
  rows on disk; in-memory fresh-page bookkeeping is capped at 4096 keys without
  deleting disk pages. Storage-level proof already in `native-cache-test`:
  `defaultRetentionExceedsTheOldPageAndDatabaseCeilings`,
  `messageBodyIsOwnedTypedSingleRecordAndSurvivesReopen`,
  `scopedFreshnessDoesNotReadUnrelatedDiskPages`,
  `archivedTimestampSurvivesRestartWithoutChangingCatalogKind`.
- Pressure-only eviction: LRU eviction runs to fit a write within the physical
  budget or a lowered limit, including the DB/journal accounting
  (`loweringThePhysicalBudgetEvictsLruAndReclaimsDatabaseAndJournal`,
  `messageBodySharesLruEntryAndPhysicalDiskBudgets`,
  `compactingEvictionCanCancelAfterADeletionAndResumeWithTheNewestPage`).
  Acceptance items on restart-below-quota retention and LRU near quota are
  therefore already reproduced green; this issue must not re-implement them.
- Foreground history is cache-first with background revalidation
  (`packages/native/src/native-controller.cpp`, history lookup ~L17200 area),
  and older pages prefetch ahead of scroll via the armed scroll reserve
  (`noteScrollRearm`, `m_olderPrefetchArmed`, #212).
- Background topic warm exists from #718: `scheduleTopicPreload` /
  `runTopicPreloadBatch` / `requestTopicPreload` with a session-scoped done
  ledger (`m_topicPreloadDone`), a transient-failure skip set re-armed at the
  verified-catalog seam, in-flight cap 2 and one serial batcher.

## Decisions

1. **Warm policy stays strictly behind foreground traffic.** Every prefetch
   path (topic windows, media previews, cross-route warm) is dispatched only
   while the controller is idle of user-attributed work, never preempting a
   selection-change fetch, and obeys the same token-generation fence as
   foreground requests (a reply from a retired generation is discarded whole).
2. **One budget ledger.** New data kinds (persisted preview variants) are
   admitted through the existing `NativeCacheService` storage API so the
   10 GB default, per-owner scoping and global LRU stay a single accounting
   domain; no side-car directory with its own eviction rules.
3. **Previews are derived data.** A persisted thumbnail variant is regenerable:
   it may always be evicted first regardless of recency, and a cache miss
   degrades to today's download-and-scale path, never to a broken bubble.
4. **Freshness semantics unchanged.** Serving a stale-but-eligible page while
   revalidating keeps the current contract (unchanged reply does not repaint or
   clear the viewport; confirmed deletes and access changes still invalidate
   every projection, including previews).

## Phases

### Phase 1 - persistent preview media cache (`message-images.cpp`)

`MessageImages::drain()` currently calls `m_files->downloadBytes(uuid, ...)`
unconditionally for every visible image; the scaled preview lives only in the
in-memory manager and the original bytes are never retained. Add:

- A disk lookup keyed by `(owner scope, attachment uuid)` consulted before
  `downloadBytes` in `drain()`. A hit feeds the existing decode/scale path
  (`preview()` stays the single scaling authority) and settles the entry
  without a transfer; generation and scope fences around the async callback
  follow the `downloaded()` pattern.
- A store on the miss path after a successful download: the encoded original
  is handed to the cache service as a derived-blob kind, admitted under the
  same physical budget and LRU as message rows (derived class evict-first per
  decision 3). Storage failures must leave the visible pipeline untouched.
- Copy-image and the full-window viewer keep using original-resolution pixels
  (existing contract from the media-viewer docs); the preview cache must never
  be substituted for the clipboard path.

Tests (`native-media-images-test`, extended): first display stores once; a
fresh manager instance over the same store shows the image with zero transfer
counter increments; a scope switch serves nothing cross-scope; eviction of the
derived kind re-enables the transfer path; owner logout clears derived rows
(access invalidation).

### Phase 2 - cross-route warm queue (controller)

Extend the #718 sweep beyond topics of the selected stream:

- Candidate order: current selection first, then unread chats (newest
  activity), then recently opened chats (recency list fed by selection
  commits, bounded at 64 entries), each contributing at most one first page;
  the existing done ledger and skip-set semantics carry over unchanged,
  per-session.
- Coalescing: a warm request for a topic that gains a foreground request while
  in flight is not duplicated; the foreground generation fence retires the
  warm slot (reuse `m_topicPreloadInFlight` keyed by topic uuid).
- Yield and backoff: the batcher pauses on `busy()`/selection churn and
  doubles its idle delay on 429/5xx (bounded), re-arming at the verified
  catalog commit seam exactly like the transient skip set.
- Traffic-pinned window cases keep the per-case disable
  (`controller.setTopicPreloadEnabledForTest(false)`); warm stays default-on
  in all ctest groups (proven pattern from #718).

Tests (`native-controller-test`): warm candidate order and per-chat page cap;
zero warm requests while busy; coalescing under a deferred mock reply;
backoff on 429 then recovery; a late warm reply cannot overwrite a newer
foreground page (generation fence).

### Phase 3 - measurement evidence

Extend `docs/NATIVE_DESKTOP_CACHE_BENCHMARK.md` with cold vs prepared
navigation (time-to-first-populated-frame, foreground history request count,
disk usage) and the media acceptance matrix from the issue (thumbnail cached
across restart, offline render, no repeated original transfer). Numbers go in
the benchmark doc; window-layer proof for "saved content visible while the
connection is delayed" uses the mock-server `defer` seam, never sleeps.

## Out of scope

- Server-side message search, media thumbnails generated server-side (no
  endpoint), and any change to the budget configuration surface.
- Re-implementing retention/LRU storage semantics (already shipped, listed
  above); this issue schedules around them.

## Acceptance mapping

| Issue acceptance | Where proven |
| --- | --- |
| retention across freshness + restart below quota | existing native-cache rows (above) |
| LRU eviction near quota / after limit lowering | existing native-cache rows |
| prefetch before opening a route; delayed connection shows saved content | Phase 2 tests + Phase 3 window run |
| no repaint on unchanged revalidation | existing cache-first contract + Phase 3 |
| rapid switches and scrolling during background work | Phase 2 coalescing tests + full-gate window groups |
| thumbnail reuse after restart, offline preview, original pixels for copy | Phase 1 tests + viewer rows |
