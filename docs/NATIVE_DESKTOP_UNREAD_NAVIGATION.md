# Unread conversation navigation

## Contract and existing behavior

Issue #1 tracks the unread experience from the pinned `workspace_ui` revision
`61dacef78dab2f5d0be76f283bebf49fe29b84af`. The current native controller already
batches visible message boundaries, confirms `read_up_to` through Messenger and
refreshes topic/stream active and passive counters. Its window suppresses reads
while inactive, hidden, minimized, stale, loading or covered by a modal surface.
These mechanisms remain authoritative; navigation must not invent read state.

The missing opening behavior was separate from this existing read pipeline:
`messageList.replaceMessages` gave every new route tail-follow intent.
A conversation with an unread message above the tail therefore opened below it.
The reference centers the first unread message, keeps a stable divider at its
author-group start, and supplies a floating tail button with a loaded-unread
badge. An already-read conversation still opens at the tail.

Reference sources are `workspace-message-list-scroll.hook.ts` (initial placement,
divider retirement and saved anchor), `workspace-message-list.ui.tsx` (stable
first-unread identity and author-group divider),
`floating-scroll-to-bottom-button.tsx` (40px control at 16px bottom/right), and
`messenger-messages-loader.lib.ts` (server unread query and around-window load).

## Implementation plan

1. Reproduce the current visible opening with a real Quick window and synthetic
   HTTP history. Check both an unread row above the tail and the all-read control.
2. Keep initial placement in the existing measured viewport restoration path.
   Resolve an initial unread UUID only from a verified conversation snapshot;
   cached/provisional rows must not consume the one-time opening intent. Preserve
   user scrolling and explicit message-link navigation over automatic placement.
3. Keep a stable first-unread UUID for the mounted conversation even after its
   read receipt is confirmed. Render its divider before the containing author
   group, then dismiss it only after it passes above the viewport. A route/owner
   change retires this state.
4. Reuse the existing exact read boundaries and server counter refresh. The
   floating tail action uses the established history loading mechanism when the
   current window does not contain the server tail; failed loading retains the
   existing window and offers a retry.
5. When the first window has older pages, resolve the server's earliest unread
   with `read=false`, ascending chronology and page limit one. Reuse the existing
   owned around-anchor loader if that message is outside the window. A complete
   history can resolve the same boundary directly from its canonical read flags.
   One retry covers a read event overtaking the boundary lookup.
6. Keep the reference's maximum twelve remembered positions in memory, scoped to
   owner and runtime generation. Store an anchor UUID plus measured viewport
   offset, or the previous tail UUID. Re-entry restores an anchor, while a saved
   tail with new unread messages opens on the first new unread. An out-of-window
   saved anchor uses an exact message lookup and the same around-anchor loader.
   Explicit bulk-read events invalidate affected topic and stream positions.
7. Full issue completion requires the complete native-window CTest group on the
   reference Linux environment. Existing unrelated failures must be reproduced
   and recorded separately; a focused pass is not the full acceptance gate.

The stale topic counter with all loaded rows already read is tracked separately
by #194. It needs its own reproduced bottom-anchor case; it is not a reason to
change ordinary read admission speculatively here.

## Verification boundaries

Tests use temporary preferences and loopback HTTP fixtures in the visible Linux
Quick window. Focus, actual rendered message geometry, outgoing read boundaries
and canonical server counter refresh are separate witnesses. English/Russian
and light/dark variants cover new text and controls. Controller fixture checks
cover any new server requests, malformed responses and owner/route retirement.
Windows, macOS and a live Workspace account require separate acceptance.

No release version or README change is part of this implementation.

## Reproduction record

The first visible-window fixture enabled realtime before requesting a snapshot
without attaching a realtime synchronizer. That left the synchronization barrier
closed even though history rows were rendered. Those early failures are not the
acceptance witness for initial placement. The corrected fixture completes an
ordinary verified bootstrap before disabling background polling, explicitly
asserts `messageHistoryReady`, and repeats the regression against the original
QML opening behavior before testing the candidate.

Controller checks independently cover an earliest unread outside the tail,
remembered anchors, fresh-tail loading and failure retention, rejected malformed
or foreign responses, cancellation, credential generation changes, and a read
receipt overtaking either the earliest-unread query or the around-window pages.
The latter two asynchronous cases exposed candidate defects before their fixes.

The corrected original-QML regression failed for a loaded first unread, an
unread outside the latest page, and return-position preservation. The candidate
then exposed two separate visible failures: returning after a settled bulk-read
restored the obsolete position, and a large real wheel jump recycled the divider
before its geometry could retire it. Their tests now wait for the actual rendered
frame, so neither a transient layout nor immediate engine destruction can count
as a navigation result. The bulk-read route lease is re-armed only by fresh user
movement; the divider also uses the rendered row order when its delegate has been
recycled.

The existing visible-read window fixture also enabled realtime without a
synchronizer. Its old `reloadSnapshot()` kept the verification barrier closed.
The focused regression reproduced zero outgoing boundaries after activation;
the fixture now completes ordinary bootstrap before stopping background polling.
The production readiness gate remains intact. The test still checks inactive,
minimized, hidden and modal windows, the exact visible boundary, and confirmed
counter refresh.

Existing image pixel and transfer-count checks caught a compatibility regression
in the candidate: using the saved-position key as the displayed route disabled
media admission, and the intermediate opening layout restarted image metadata
requests. The displayed route retains its existing service-scope identity; only
the position map uses the owner/runtime key. Verified-history image admission
waits for the final opening viewport. Provisional cached images keep their prior
admission behavior while canonical history is unavailable.

The existing scroll-preservation fixture initially returned its fifty-row read
history for every GET, including the new one-row unread lookup. A new message at
the tail reproduced zero read receipts because opening correctly rejected that
invalid response. The fixture now serves the requested earliest unread (or an
empty array), preserving its original scroll and read-boundary assertions.

A read/history event during saved-anchor paging reproduced a failed restoration.
Saved-position retries now repeat the same owned UUID lookup and around-window
load once, independently of unread-boundary retries. Only a confirmed missing
saved UUID falls back to unread; repeated history churn retains a bounded failure
instead of silently moving the reader to the tail.

The full-window run also reproduced an explicit-link regression when the target
is an account-irrelevant service message. Taking navigation ownership cancelled
the queued layout, so its empty-window paging pass never ran. The existing
anchor regression passed on the base and failed on the candidate; explicit
anchor hand-off now invokes that same bounded paging pass. Ordinary anchors
still do not count as user scrolling or request older pages.

Two older pagination fixtures now answer the added unread-only lookup with the
correct empty array for their fully read histories. The aggregate visible-read
fixture completes an ordinary verified bootstrap and explicitly takes manual
navigation ownership before loading its 400-row viewport. It still verifies
pagination, bounded delegates, two exact topic read boundaries and inactive /
minimized suppression. Automatic opening is covered separately.

The later full-window run exposed the same incomplete synthetic bootstrap in
both media-viewer read-isolation rows. The old base passed because it did not
require opening readiness; the candidate correctly withheld reads. The fixture
now completes the verified bootstrap before enabling synthetic realtime. All
eight media isolation rows pass with the real viewer and unchanged read gates.

The reply-tab selection control also failed on the unmodified base. Its short
author produced a 63 px pill: rounding the whole-pill centre selected the exact
boundary between the author hit area and the 32 px close control. The test now
clicks the author region explicitly, while retaining the separate real close
button click, tab-count checks, selected message and preserved draft assertions.

The profile and attachment-picker teardown failures were independently reproduced
on the base. Qt reports a parent active while its transient PiP or popup remains
active, even after the parent has received FocusOut and is hidden. The test helper
now closes only that window's transient family, deepest first, before its existing
focus-before-surface checks. The original two-second acknowledgement limit and
incubation/frame assertions remain intact. A separate root/tool/unrelated-window
regression fails with the old helper and verifies both ordered teardown and
preservation of the unrelated window. The three original failing scenes and
existing lifecycle controls also pass with this helper.
