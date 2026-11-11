# Message list auto-paging while scrolling

Issue #106 (part of the #105 message-area epic). Reference measurements were
taken from `exordos/workspace_ui` at the pinned parity revision
`61dacef78dab2f5d0be76f283bebf49fe29b84af` (the sha recorded in
`NATIVE_DESKTOP_PARITY.md`), by reading the full source of the scroll hook and
the message-list widget, not from screenshots.

## Reference contract (measured)

`packages/web/src/widgets/workspace-message-list/workspace-message-list-scroll.hook.ts`:

- `LOAD_MORE_THRESHOLD = 100` px (:22). The DOM scroll container's `scrollTop`
  is the distance already scrolled from the top of the loaded window.
- Older-page auto-load (:1185-1197): fires when a scroll event reports
  `scrollTop <= 100` while `hasOlderMessages`, no older fetch is in flight, an
  `onLoadOlder` callback exists, a user scroll was ever seen
  (`userScrollSeenRef`, :175, set by `handleWheel` (:1235-1264),
  `handleTouchMove` (:1266-1284) and the initial tail pin (:571-584)), no
  programmatic scroll is running (`programmaticScrollRef`) and the trigger is
  armed (`topPaginationArmedRef`, initial `true`, :180). Before firing the hook
  captures the pre-prepend scroll snapshot and disarms.
- Re-arm (:1172-1174): any later scroll event with `scrollTop >= 100` re-arms.
  Both comparisons include the exact `100` px boundary.
- Because the pre-prepend snapshot keeps the visually first message at the same
  screen offset after the page lands, the restored `scrollTop` is by construction
  larger than the distance it was before the load (new rows entered above the
  viewport), so the next scroll event re-arms automatically.
- Newer-page auto-load at the bottom edge (:1199-1208): fires on any scroll
  event while the container is within `SCROLL_AT_BOTTOM_THRESHOLD = 80` px of
  the bottom with newer messages pending and no newer fetch running. Out of
  scope here - see Non-goals.

`packages/web/src/widgets/workspace-message-list/workspace-message-list.ui.tsx`:

- Account-relevant service filtering (:943 `accountServiceMessages`) hides
  service messages the account is not involved with from the rendered list.
- Zero-visible-window auto-advance (:998-1022): when the filtered list renders
  no rows at all while underlying rows exist, the widget loads the next window
  with no scroll input ("an invisible page has no scroll surface from which to
  request the next one"). Direction preference is newer first, then older. The
  effect is deduplicated per fetch by a key of user + conversation + direction +
  first and last underlying row keys, and it stands down while a fetch is in
  flight, the window is positioning, or an anchor handoff is pending.

## Native mapping

Native counterpart today: `packages/native/qml/Main.qml` keeps the manual
`Load older messages` button (bound to `NativeController::canLoadOlder` /
`loadOlder()`, `packages/native/src/native-controller.cpp:8315`) above the
message list, and the viewport capture/restore pair `captureViewport()` /
`restoreViewport()` already preserves the visually first message and its screen
offset across prepends (the contract `#142` re-states; proven by
`messageUpdatesRespectTheVisibleScrollPosition`). This release adds the two
scroll-driven behaviours above and keeps the manual button as the fallback.

1. Trigger geometry in QML. For `messageList` (ListView with `topMargin: 16`)
   the DOM-equivalent scrolled distance is
   `distanceFromTop = contentY - originY + topMargin`. At the very top of the
   loaded window `distanceFromTop == 0`; the same arithmetic already appears in
   the tail formula used by `captureViewport()`. Trigger and re-arm use the
   reference comparisons unchanged (`>= 100` re-arm, `<= 100` trigger) and are
   evaluated in that order on every `contentY` change, mirroring the reference
   handler order (an event at exactly 100 px re-arms and may fire).
2. User-scroll and programmatic mirrors. The reference marks
   `userScrollSeenRef` from the user input handlers themselves - `handleWheel`
   and `handleTouchMove` - plus the scroll-to-bottom-after-send effect
   (:573-584); `contentY` deltas are not a valid proxy, because Qt fires
   `contentYChanged` for initial layout, reflow and clamp passes where the DOM
   only fires scroll events for actual user-visible motion. So the native list
   observes wheel input with a zero-button, non-accepting `MouseArea`
   (`wheel.accepted = false`, `propagateComposedEvents`) that marks the flag
   and lets the wheel reach the Flickable and the delegates - a `WheelHandler`
   inside a `ListView` swallows the wheel instead (verified: the existing wheel
   regression tests freeze with one) - marks the flag from `movementStarted`
   (drag/fling), and marks it from the sent-message tail pin
   (`followSentMessage`), mirroring :584. Viewport restoration (the
   `programmaticScrollRef` analogue) writes `contentY` while `positioning` is
   true, so restore passes can never fire the trigger, and a conversation that
   opens into a short (non-scrollable) window never auto-pages before the user
   interacts. Recorded deviation: dragging the scrollbar marks the flag in the
   reference (trusted scroll event) but cannot be attributed in Qt without the
   contentY proxy; wheel, drag and fling all mark it natively. `hasOlderMessages && !isLoadingOlder` maps to the existing
   `controller.canLoadOlder` property, which also covers snapshot and
   cache-verification guards. No new controller API is needed. The reference
   also suppresses scroll handling entirely while an anchor handoff is pending
   (`anchorHandoffPending`), so the native highlight jump (`anchorToMessage`)
   sets an equivalent one-shot `anchorJumpPending` flag: a programmatic jump
   into the trigger zone neither marks the seen flag nor fires the trigger.
3. Zero-visible-window auto-advance lives in the view, not the controller.
   The reference places the effect in the widget (`workspace-message-list.ui.tsx`)
   because the account-relevance filter lives in the view projection. Native is
   shaped the same way: `accountRelevant()` and `projectServiceRows()` are in
   `Main.qml` (service entries - `key_request`, `key_grant`, `key_reject`,
   `key_announced` - that the viewer is not involved with are removed from the
   projection on encrypted streams). Porting the correlation rule into the C++
   controller would duplicate view logic (the offer flags it reads are already
   computed by the controller projection and consumed by QML only). So after
   `replaceMessages()` publishes a projection, if rows exist in the snapshot
   but the projection is empty, the list advances automatically - newer first
   via `controller.loadNewer()`, otherwise older via `controller.loadOlder()` -
   deduplicated by conversation scope + direction + first/last snapshot row id
   (the reference key), standing down while `positioning` or while the history
   is not ready yet. The dedupe key naturally releases once the projection
   becomes non-empty.
4. No new user-visible strings: the existing `Load older messages` /
   `Load newer messages` buttons and the loading spinner behaviour are
   unchanged, so the translation catalogs stay untouched.

## Deviation note (recorded per skill rule)

The issue's acceptance line suggested the zero-visible case be tested in
`native-native-controller`. As measured above, the behaviour is impossible to
express in the controller without duplicating the QML relevance filter, so it
is implemented in the view. It is therefore tested in `native-native-window`
(one QML-visible scene: an encrypted stream whose first history page contains
only account-irrelevant service rows). The controller group is extended with
the controller-side primitive that this behaviour depends on: a fetched page
whose rows are all account-irrelevant service rows must still commit and move
the older marker while keeping the newer boundary intact.

## Tests

- `native-native-window`: `scrollNearTopAutoLoadsOlderPages` - a paged stream
  fixture (marker via `X-Pagination-Marker`): opening loads exactly one page
  and scrolling never fires a second request before the user scrolls; a real
  `QTest::wheelEvent` up to within 100 px of the top issues the older-page
  request without any button click; the restored viewport sits past 100 px, so
  a second wheel-to-top issues the next page (re-arm proof); while an in-flight
  page keeps the viewport inside the zone no duplicate request is issued.
- `native-native-window`: `zeroVisibleWindowAutoAdvances` - encrypted stream,
  first page only `key_announced` rows (always account-irrelevant), older and
  newer pages available: the window opens with no rendered message rows and
  issues a newer-direction page request on its own (newer-first priority);
  when an ordinary row lands the auto-advance stops and the row renders.
- `native-native-controller`: `allFilteredServicePageStillAdvancesMarkers` - a
  fresh-history page made only of `key_announced` rows commits, advances
  `m_before` (older marker present) and keeps `canLoadOlder` true for the next
  step (the traversal the view-side advance relies on).

## Non-goals

- The reference's bottom-edge newer-page auto-load (:1199-1208) is not shipped
  here: #106 accepts only the older-direction trigger and the zero-visible
  advance, and `loadNewer()` remains manual-button-driven. It will be ported in
  its own release with its own acceptance.
- Mouse-wheel speed/feel tuning and sidebar wheel behaviour belong to #142.
- No geometry or palette changes: the buttons, list metrics and spacing stay
  exactly as shipped.
