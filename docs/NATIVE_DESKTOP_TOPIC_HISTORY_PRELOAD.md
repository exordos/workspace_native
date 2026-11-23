# Topic history preload + swap-on-ready (issue #718, slice 2)

Status: implemented on `cassi/topic-history-sidebar-stability`. Companion to
`docs/NATIVE_DESKTOP_HISTORY_PREFETCH.md` (older-page prefetch) and the slice-1
sidebar sort-key fix.

## Contract (issue #718 acceptance)

- A bounded background preload warms a small message-count slice for every
  accessible populated topic (message COUNT selection, so a months-old topic
  stays warm; freshness never excludes a topic).
- Switching to a prepared topic shows its own rows on the first visible frame
  (no blank frame), while the verified page for the new selection loads.
- Existing history pagination and NativeCache budgets are unchanged.

## Design decisions (fix round, after the slice-2 CI regression)

1. Swap frame is a PREVIEW. `selectTopic` may publish the prepared slice rows
   (and the older-page marker the slice legitimately carries), but it does NOT
   set `historyVerified`. Verification stays owned by the page commits of the
   current selection, so:
   - the stale-cache verdict still fires when a saved page expired and the
     revalidation fails (a swap frame must never mask `cacheStale()`);
   - cache-protected mutations keep waiting for the verified page.
2. The reload after a swap runs the normal `loadMessages()` path (saved-page
   shortcut included), not a forced latest-page request. A `m_preparedSwapActive`
   flag lets the first verified page of the selection overwrite the swap frame
   (`fresh` gate and cached-hydration gate). This preserves the pre-existing
   contracts:
   - warm navigation over verified saved pages issues no extra HTTP;
   - the older-page marker of the fresh/hydrated page is what `canLoadOlder()`
     and auto-paging observe (a boundary-free latest reload had dropped it and
     stalled older-page loading, `scrollNearTopAutoLoadsOlderPages` family).
3. Slice lifetime equals saved-page lifetime, never longer. The prepared slice
   is a cached history page, so `invalidateCache`/`staleCache` for the History
   kind drop it with the same (stream, topic) scoping (`dropTopicSlices`).
   Without this, reopening a topic after a confirmed delete resurrected the
   deleted row from the stale slice (red `deletedPeerMessage...`).
4. A trimmed slice never carries the older-page marker. The marker describes
   what lies beyond the FULL window; `rememberTopicSlice` keeps it only while
   the slice holds every row of the window, otherwise older reads would
   silently skip the dropped rows.
5. Sweep scope and determinism. The sweep is scheduled only from the verified
   topic-catalog commit (never from `stateChanged`), concurrency 2, page size 5,
   map budget 512 topics, and each topic is fetched at most once per session
   (skipped set on empty/error). It ships enabled (product default), but the
   exact-request pins in the test harnesses make it non-deterministic where
   they count `/messages/` traffic, so:
   - `support/native-test-harness.h` turns the sweep off for controller
     fixtures (`setTopicPreloadEnabledForTest`); the two sidebar sweep cases
     opt back in;
   - window cases that pin exact page traffic disable it on their local
     controller.
6. Tests that used "rows appeared" as the wait for the verified page must now
   await the async verdict itself (`historyCommitRevision`, `cacheStale()`),
   because the swap frame shows rows earlier than the commit that used to be
   the earliest observable moment. Expectation values were not loosened, only
   the wait points moved onto the commit/stale signals.

## Seams (test-only)

- `hasPreparedTopicSlice(topicUuid)` - slice warmed for the topic.
- `setTopicPreloadIntervalMsForTest(ms)` - compress the sweep pacing.
- `setTopicPreloadEnabledForTest(bool)` - deterministic fixtures (default ON).
