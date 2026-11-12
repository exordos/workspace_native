# Older-history prefetch before the loaded top reaches the viewport

Issue #156. The reference contract was measured by this lane (no reference
contract block was posted on the issue). Reference app: local checkout
`/home/ubuntu/dev/workspace_ui` at revision
`61dacef78dab2f5d0be76f283bebf49fe29b84af`, files read in full:
`packages/web/src/widgets/workspace-message-list/workspace-message-list-scroll.hook.ts`,
`packages/web/src/widgets/workspace-message-list/workspace-message-list-scroll-anchor.lib.ts`,
`packages/web/src/widgets/workspace-message-list/workspace-message-list.ui.tsx`,
and `docs/WORKSPACE_MESSENGER_CACHE.md`.

## Reference contract (measured, sha 61dacef78dab2f5d0be76f283bebf49fe29b84af)

`workspace-message-list-scroll.hook.ts`:

- `SCROLL_AT_BOTTOM_THRESHOLD = 80` px (:21) and
  `LOAD_MORE_THRESHOLD = 100` px (:22). `scrollTop` is the distance already
  scrolled from the top of the loaded window; at the very top it is 0.
- Older-page trigger (:1185-1197): on a scroll event with
  `userScrollSeen`, no programmatic scroll, `hasOlderMessages`,
  `!isLoadingOlder`, an `onLoadOlder` callback, the trigger armed, and
  `scrollTop <= 100`, the hook captures the pre-prepend snapshot
  (`capturePrependScrollSnapshot`, :312-321), disarms and calls `onLoadOlder`.
- Re-arm (:1172-1174): any scroll event with `scrollTop >= 100` re-arms.
  Initial state is armed (:180). Both comparisons are inclusive; an event at
  exactly 100 px re-arms and may fire.
- In-flight coalescing: the trigger requires `!isLoadingOlder`; while a page is
  in flight and the user keeps scrolling, the pending prepend snapshot is
  re-captured (:1177-1183) so the restore follows the gesture's current
  position instead of the position at request time.
- Prepend anchor compensation (:899-954): after the page lands, the visually
  first visible render-anchor node (`data-message-render-key`, resolved with
  its pixel offset from the container top,
  `workspace-message-list-scroll-anchor.lib.ts:57-91`) is restored to the same
  offset (`computeWorkspaceScrollTopFromRenderAnchor`, lib :100-120); if that
  node is gone, the fallback is the scroll-height delta
  (`computeWorkspaceScrollTopAfterPrepend`, lib :141-148).
- Wheel (`handleWheel`, :1235-1269) and touch move (`handleTouchMove`,
  :1271-1285) mark the user-scroll intent; an upward wheel immediately clears
  bottom-pinning so a layout pass cannot steal the position.
- Hidden-page auto-advance (`workspace-message-list.ui.tsx:996-1038`): when
  every loaded row is filtered out of the view, the widget loads the next
  window with no scroll surface, newer direction first, deduplicated per
  (user, conversation, direction, first/last row key). Shipped natively under
  #106 (`NATIVE_DESKTOP_AUTO_PAGING.md`); unchanged here.
- `docs/WORKSPACE_MESSENGER_CACHE.md` records no scroll/prefetch pixel
  constants; it constrains window bounds and `page_marker` opacity (markers are
  opaque tokens, not sort keys) and realtime batching limits.

Native shipped state (#106, master): the trigger runs in `Main.qml`
`messageList.onContentYChanged` at the reference geometry
(`distanceFromTop = contentY - originY + topMargin`, threshold 100 px),
coalescing is the view's `topPaginationArmed` flag plus the controller's
`m_loading`/`canLoadOlder` guards, and anchor preservation across prepends is
the `captureViewport()`/`restoreViewport()` pair in `Main.qml` (visually first
read-boundary row + screen offset, re-captured at commit time - the native
analogue of the reference re-capture at :1177-1183).

## The gap #156 closes

The reference trigger fires at 100 px from the loaded top. Under native fetch
+ parse + layout latency a request started 100 px early usually lands after the
user has already hit the top: the wheel stalls, waits, then jumps. #156 moves
the decision earlier: the request must begin while content still remains above
the viewport (more than the 100 px boundary), so the page is normally ready
before the boundary is reached.

## Native design

### Reserve policy in the controller (testable, conversation-safe)

`NativeController` gains one invokable, no new properties:

- `Q_INVOKABLE void noteScrollReserve(qreal distanceFromTopPx, qreal viewportHeightPx)`:
  the view calls it from `messageList.onContentYChanged` for user-attributed
  scroll passes only (same gates as before: `userScrollSeen && !positioning &&
  !anchorJumpPending`). `distanceFromTopPx` is the reference `scrollTop`
  analogue; `viewportHeightPx` is the list height.
- Policy: `reserve = max(100 px, viewportHeightPx)`. The 100 px floor is the
  measured reference `LOAD_MORE_THRESHOLD`; the one-viewport headroom is the
  chosen prefetch buffer. Below or at `reserve` and armed -> start the older
  request (which is the existing `loadOlder()` path, so all existing guards:
  `m_loading`, snapshot, cache verification - still coalesce the request). At
  or above `reserve` -> arm. Comparisons mirror the reference exactly
  (`>=` arm, `<=` trigger), including the exact-boundary event that arms and
  fires.
- Why one viewport + the reference floor (tradeoff): a wheel notch natively
  scrolls hundreds of pixels (tests drive 1200 px steps), so the trigger to
  boundary gap must scale with the viewport, not with a fixed small constant.
  One viewport keeps at most one page in flight as reserve: it bounds request
  volume (never eager: no request without a user-attributed scroll pass, at
  most one page per scroll pass below the reserve, one page per
  `page_limit=50` rows) while giving representative network latency a full
  viewport of scroll time to land. Larger leads would prefetch pages the user
  may never reach; the reference floor guarantees the reference boundary
  trigger still fires even with a degenerate viewport.
- Arming (`m_olderPrefetchArmed`, initial true) is consumed only when a request
  actually starts. `!canLoadOlder()` (end of history, in flight, snapshot,
  cache-verification) returns without consuming it, so end of history neither
  loops nor storms: further notes are no-ops while `m_before` is empty.
- Re-arm sites: any note at or above the reserve; a fresh window commit
  (`!older` success in `historyPage`); `reset()`; a failed history request
  (the `!result.ok()` branch), so a failed prefetch becomes retryable on the
  next user scroll instead of silently disarming forever; and the settled
  viewport restore - `restoreViewport()` ends with a re-arm-only
  `noteScrollRearm(distanceFromTop, height)` call because the `positioning`
  gate suppressed every note during the swap and the controller would
  otherwise keep a consumed arm above the restored reserve. `noteScrollRearm`
  can only arm (at or above the reserve) and can never start a request, so
  programmatic movement keeps its lack of user attribution (PR #212 review).
- Non-finite inputs are ignored (defensive against mid-layout NaN geometry).

This placement makes the acceptance-critical trigger semantics (fires while
reserve remains, coalescing, end-of-history, deferred-response races)
controller-testable without a window, which the view-side #106 trigger could
not express in the controller group.

### View wiring (`Main.qml`)

`onContentYChanged` keeps the same user-attribution gates and now forwards
`noteScrollReserve(distanceFromTop, height)`; the view-local
`topPaginationArmed`/`loadMoreThreshold` trigger flags are removed because the
controller owns arming now. The manual `Load older messages` button stays as
the fallback. The zero-visible-window auto-advance (#106) is untouched.

### Anchor preservation across page insertion

No new mechanism: `replaceMessages()` captures the viewport before mutating
the model (`captureViewport()`: visually first read-boundary row + its y
offset, or tail-follow), and `restoreViewport()` re-anchors with
`positionViewAtIndex(..., Beginning)` plus a residual offset correction, with
a second settle pass after async row geometry. Because the capture happens at
commit time - even mid-gesture, with the page arriving while the user is still
wheeling - the restored position preserves the *current* visible row and
screen offset, which is exactly the reference re-capture semantics
(:1177-1183). `positioning` suppresses prefetch notes during the swap, so
programmatic motion never feeds the policy (reference `programmaticScrollRef`).

### Failure and slow pages

A slow (held) page keeps the loaded window and position usable: the view is
fully interactive while `m_loading` is true; notes while in flight are
no-ops (coalesced). A failed page commits nothing, reports through the
existing notice path, keeps `m_before` (retryable), re-arms the prefetch, and
leaves the viewport where it was.

## Acceptance mapping

| Contract line | Where proven |
| --- | --- |
| Request begins while content remains above viewport | controller `historyPrefetchFiresWhileReserveRemains` (fires at `reserve - epsilon` with > 100 px still above; silent above) |
| Next page normally ready before the boundary under representative latency | controller deferred-race + window `prefetchLandsMidGestureKeepingAnchor` (page held, user keeps wheeling, page lands, gesture continues with no stall) |
| Anchor preserved (short/multiline/date-separator rows) | window `prefetchLandsMidGestureKeepingAnchor` on a mixed fixture (single-line, multi-line variable-height, two-day date separators): first visible UUID + offset within 1 px after insertion |
| Wheel/touchpad continuous across boundaries, direction changes | same window test: wheel keeps moving after insertion, direction reversal re-arms per policy |
| No duplicate/cursor-storm/end-of-history loop | controller `historyPrefetchCoalescesInFlight` (deferred response), `historyPrefetchStopsAtEndOfHistory`, `historyPrefetchFailedPageStaysRetryable` |
| Slow/failed page keeps chat usable, position retained | controller failed-page case + window held-page case (messages intact, not busy, anchor kept) |
| Bound memory/request volume, no eager full-conversation fetch | policy has no timer and no scroll-less request: at most one in-flight page, only after user-attributed scroll, stopped by `canLoadOlder`; existing `historyPage` page/row/byte caps unchanged |

## Recorded deviations

- The #106 view-local trigger constant (`loadMoreThreshold: 100` in
  `messageList`) is replaced by the controller reserve; the 100 px reference
  boundary survives as the policy floor (a request is always started by the
  time the viewport is within 100 px of the top). The existing window test
  `scrollNearTopAutoLoadsOlderPages` asserts the old exact boundary; it is
  updated to the reserve boundary, keeping its no-fire-before-user-scroll,
  coalescing, re-arm and end-of-history assertions intact.
- Scrollbar drags remain unattributable (recorded under #106); the reserve
  trigger is driven by the same wheel/drag/fling attribution as before.
