# Unread conversation navigation

## Contract and existing behavior

Issue #1 tracks the unread experience from the pinned `workspace_ui` revision
`61dacef78dab2f5d0be76f283bebf49fe29b84af`. The current native controller already
batches visible message boundaries, confirms `read_up_to` through Messenger and
refreshes topic/stream active and passive counters. Its window suppresses reads
while inactive, hidden, minimized, stale, loading or covered by a modal surface.
These mechanisms remain authoritative; navigation must not invent read state.

The missing opening behavior is separate from this existing read pipeline:
`messageList.replaceMessages` currently gives every new route tail-follow intent.
A conversation with an unread message above the tail therefore opens below it.
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
