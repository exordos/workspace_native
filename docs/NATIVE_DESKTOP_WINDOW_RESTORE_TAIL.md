# Window restore keeps the conversation tail and the reading anchor

Issue #253 (`Restoring the window after maximizing loses the conversation
tail`). Design fidelity case: parity port of the current web client's
resize-time scroll preservation; no redesign.

## Observed failure (measured in the reporter's environment)

Normal size 1440x900, maximized 2493x1010. With the latest message fully
visible, one maximize/restore cycle moved the latest 186px row from
y=638 to y=1028..1214 while the viewport was y=135..844: fully below the
fold, 3 out of 3 cycles. While maximized the same row re-wrapped to 126px
and remained visible, so the loss happens when the narrower width makes the
wrapped rows grow again.

## Reference contract (workspace_ui workspace-message-list-scroll.hook.ts)

The web feed keeps its position through viewport changes with a
`ResizeObserver` effect (effect at `:956-1016` in the pinned revision
`61dacef78dab2f5d0be76f283bebf49fe29b84af`):

- A persistent `wasAtBottomRef` / `userScrolledAwayFromBottomRef` pair -
  not the instantaneous geometry - decides whether the reader was following
  the tail before the resize.
- If the reader was following the tail: `pinTailToBottom(root)` re-pins the
  latest message after the reflow, with a second pass on the next animation
  frame for the measured text height.
- Otherwise: `computeWorkspaceScrollTopFromRenderAnchor(root, contentScrollAnchorRef)`
  returns the `scrollTop` that keeps the remembered visible message at the
  same offset, and the observer applies it when it differs by at least 1 px.
- The observer runs on both directions of the resize (`clientHeight` grew
  or a content child resized), so the anchor is preserved while maximized
  as well as after restoring.
- Gates: skip while `anchorHandoffPending`, while a prepend restore is
  pending, or before the initial position is applied. After the correction
  the anchor memory is refreshed from the current visible row.

## Native mapping

The native list already owns the capture/restore lifecycle used by message
swaps: `captureViewport()` (anchor readBoundary + offset + followTail
heuristic), `restoreViewport(ticket)` (tail pin with a measured final pass,
or anchor reposition to the captured offset), `positioning` and
`layoutTicket`. Only `onWidthChanged`/`onHeightChanged` stayed outside that
lifecycle: they scheduled visible-read/service inspection only, so a
resize-driven delegate reflow left `contentY` untouched while the rows
above the viewport grew, exactly the drift the report measured.

The corrective change hooks the same two lifecycle points into the size
handlers, mirroring the reference observer:

- A persistent `tailFollow` flag on the message list is the analogue of
  `wasAtBottomRef && !userScrolledAwayFromBottomRef`. It is set by the tail
  branch of `restoreViewport` and by `followSentMessage`, cleared by the
  anchor branch, and re-derived from the actual distance to the bottom on
  every non-programmatic `contentY` change. It is never a user-scroll
  attribution: the #156 wheel observer / `movementStarted` remains the only
  source of `userScrollSeen`, and the size-driven restore keeps
  `positioning = true` so its programmatic movement stays unattributed and
  only re-arms the #156 prefetch reserve through the existing
  `noteScrollRearm` call.
- `resizeViewport()` (on `onWidthChanged`/`onHeightChanged`): mirrors the
  reference observer gates - skip while a capture/restore is already in
  flight (`positioning`), during an anchor highlight jump
  (`anchorJumpPending`), before the list has a route/layout or an empty
  model. It captures the current viewport through `captureViewport()` and
  overrides `followTail` from the persistent `tailFollow` flag: the
  instantaneous heuristic is unreliable exactly during the resize (the
  viewport height is already new while `contentHeight` still reflects the
  pre-reflow rows, so the tail-side distance reads above the 80 px threshold
  even though the reader was at the tail). The restore then runs through the
  existing `restoreViewport` ticket chain, whose two-pass
  `forceMeasuredLayout` is the analogue of the reference's
  `requestAnimationFrame` second pin.
- `replaceMessages` keeps consuming the `pendingViewport` snapshot the
  resize staged, so a model swap arriving mid-resize restores once from the
  captured state instead of capturing the transient clamped geometry, and
  the #141 layout-freeze safeguards (single restore chain, virtual
  delegates, `settleGeometry` measurement passes) are unchanged.
- The reference `pinTailToBottom` second pass returns when
  `userScrolledAwayFromBottomRef` fired while its `requestAnimationFrame`
  pass was pending. The native restore applies the same hand-off gate: a
  snapshot captures the wheel-observer state (`userScrollSeen`) when the
  chain is scheduled, and if the observer fires before a queued tail-pinning
  pass runs, the pass abandons the restore instead of overwriting the user's
  movement, re-deriving `tailFollow` from where the user landed.

The #141 event-loop safeguards stay intact: no new timers or spin loops are
added; the restore stays on `Qt.callLater` and only measures instantiated
rows.

## Regression coverage

`native-window-test.cpp` gains two visible-window cases with synthetic
variable-height markdown rows and a long wrapped latest message:

- `windowRestoreKeepsTheConversationTailVisible`: three
  1440x900 -> 2400x1050 -> 1440x900 cycles; after each state the settled
  latest row must be fully inside the message viewport, with no cumulative
  drift between cycles.
- `windowRestoreKeepsAnOlderHistoryReaderAnchored`: the reader is parked in
  older history with real wheel input far enough from the tail; across the
  same cycles the first visible message must stay visible at approximately
  its captured offset in both window sizes, and the view must never jump to
  the tail.

Both cases failed before the fix because the reflow left `contentY` behind
(tail fully below the fold; anchor offset displaced), and pass after it.
