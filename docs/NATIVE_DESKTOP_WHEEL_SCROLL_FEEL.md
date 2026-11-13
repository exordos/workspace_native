# Native Desktop: mouse-wheel scroll feel (issue #142)

Status: implemented in this PR (wheel half of #142; the continuous-history half
shipped in docs/NATIVE_DESKTOP_HISTORY_PREFETCH.md with #156).

Issue: #142 "Improve mouse-wheel scrolling in messages and the contact list".
The issue carries **no Reference contract block**, so this document records the
measurements taken from the reference React/Electron client (`workspace_ui`) and
from Qt 6.10.2 on this host, and derives the native contract from them.

## 1. Reference measurements (workspace_ui @ 75bd21f)

All paths relative to `packages/web/src`.

1. The message-list scroller is a plain overflow scroller: the container is
   `scrollContainerRef` with `onWheel={handleWheel}`
   (`widgets/workspace-message-list/workspace-message-list.ui.tsx:1346-1349`
   and `:1369-1372`; the vertical scroller class comes from
   `shared/ui/scroll-area/scroll-area.tsx` `AXIS_CLASSES.y = "overflow-y-auto"`).
2. `handleWheel`
   (`widgets/workspace-message-list/workspace-message-list-scroll.hook.ts:1235-1281`)
   is an **observer only** for the vertical axis: it records user-scroll intent
   (`userScrolledAwayFromBottomRef`, last scrollTop, render anchor), marks the
   at-bottom state, and never calls `preventDefault()` and never writes
   `scrollTop`. The vertical wheel distance is therefore **100% browser
   default**; the reference app defines no custom px-per-notch mapping for the
   vertical axis.
3. The only explicit delta→distance mapping in the reference is the horizontal
   wheel translation in `shared/ui/scroll-area/scroll-area.tsx:24-52`
   (x-axis only): `DOM_DELTA_LINE → lineHeight || 16 px` per line,
   `DOM_DELTA_PAGE → clientWidth` per page, pixel deltas pass through 1:1, and
   the non-passive listener `preventDefault()`s only when it actually moved the
   scroller — at an edge the wheel hands off to the outer container.
4. The reference test suite never overrides the notch distance: its canonical
   wheel gesture is `fireEvent.wheel(root, { deltaY: -100 })`
   (`workspace-message-list-scroll.hook.test.tsx:572`, also `:203`, `:384`,
   `:403`, `:427` use ±1 as an intent-only signal). One "notch" in the
   reference contract is therefore **±100 px in `deltaMode = DOM_DELTA_PIXEL`**
   — Chromium's normalized mouse-wheel notch (`kWheelDelta = 100` in
   `ui/events/event_constants.h`; external constant, could not be re-fetched
   from this host at design time — flagged as such).
5. No `scroll-behavior` CSS on the scroller (grep over
   `widgets/workspace-message-list/` and `shared/ui/scroll-area/` finds none),
   i.e. final scroll position after a notch equals the delta (Chromium may
   animate the approach; the endpoint is delta-exact).

**Reference contract derived:** one mouse-wheel notch scrolls ≈100 px
deterministically (cadence-independent, no acceleration), touchpad/high-res
input moves 1:1 with the reported pixel delta, direction changes retarget from
the current position, and scrolling stops at content edges with the event
handing off to the outer scroller.

## 2. Native measurements before the change (Qt 6.10.2, this host)

`qtdeclarative v6.10.2 src/quick/items/qquickflickable.cpp` (fetched from the
upstream tag) — `QQuickFlickable::wheelEvent`, lines 1655-1877:

1. `messageList`/`streamList` are plain `ListView`s, so wheel scrolling ran
   through the default Flickable handler.
2. Default `wheelDeceleration = 15000` (line 240) is below
   `_q_MaximumWheelDeceleration`, so the **acceleration branch** (lines
   1797-1838) is active: an idle single notch (angleDelta ±120) flicks
   `initialWheelFlickDistance = QStyleHints::wheelScrollLines() * 24`
   (line 243; 3 lines × 24 = **72 px** at the host default) with easing, and
   repeated fast wheeling accumulates velocity samples so the per-notch
   distance grows with input cadence. A single-event wheel test measured this
   as ≈72 px/notch — ~28 % below the reference and cadence-dependent.
3. Events arriving with a non-increasing timestamp are dropped
   ("insufficient elapsed time", lines 1701-1705).
4. The `pixelDelta` branch (lines 1844-1866) drags the Flickable only after an
   accumulated-direction heuristic, so light touchpad input can be swallowed
   until the accumulation ratio passes.
5. `contentYChanged` is **not** a user-scroll proxy (fires on layout, reflow,
   clamp), which is why #156/#253 attribute user input through the wheel
   observer `userScrollSeen`/`userScrollGeneration`
   (`docs/NATIVE_DESKTOP_HISTORY_PREFETCH.md`,
   `docs/NATIVE_DESKTOP_WINDOW_RESTORE_TAIL.md`). This change keeps that
   observer and its counters as the single intent source.
6. A `WheelHandler` child of a `ListView` swallows wheel events even when it
   sets `accepted = false` (observed on this repo's wheel tests), so the wheel
   is observed with
   `MouseArea { acceptedButtons: Qt.NoButton; propagateComposedEvents: true }`
   (the shipped messageList observer pattern).

## 3. Native approach

Keep the shipped wheel observer nodes and turn them into the scroller (they
already sit on `messageList`; an identical node is added to `streamList`, which
also covers expanded topic rows because those rows are delegate children and
the wheel reaches the list's observer through the sibling chain):

* Intent bookkeeping unchanged: every wheel marks `userScrollSeen` and bumps
  `userScrollGeneration` **before** the movement, so the #156 reserve trigger
  and the #253 resize tail-pin hand-off keep working untouched.
* Delta mapping:
  * angle path (`angleDelta.y != 0`, no synthesized pixel phase attached):
    `distance = angleDelta.y / 120 * wheelPixelsPerNotch` with
    `wheelPixelsPerNotch = max(1, Qt.styleHints.wheelScrollLines) * 100 / 3`
    — 100 px/notch at the default 3 scroll lines (the measured reference
    notch), and the system's scroll-lines preference scales it (the issue's
    "respect the user's system scrolling preferences" clause; Chromium ignores
    that setting, Telegram Desktop honors it — deviation recorded below).
  * pixel path (synthesized source with non-zero `pixelDelta.y` and a real
    scroll phase): left to Qt's built-in Flickable path on purpose — measured
    on this host, the window delivers synthesized pixel wheels straight to the
    Flickable under the pointer (touchpad grab semantics), bypassing sibling
    observers, and that path drags 1:1 with the reported pixels after DPR
    normalization: the precise reference touchpad behavior, preserved
    unchanged ("Improving a conventional mouse wheel must preserve precise
    touchpad scrolling").
* Movement is written directly to `contentY` (same mechanism as the shipped
  viewport-restore code) instead of flicking: cadence-independent, exactly the
  reference endpoint contract, and stable on direction change/stop because
  there is no lingering velocity. An in-flight drag/fling is stopped first
  (`flick(0, 0)`) the way a wheel interrupts a smooth scroll in the browser.
* Out-of-range targets settle via `Flickable.returnToBounds()` rather than a
  hand-rolled clamp: programmatic `contentY` writes do not refresh
  `atYBeginning`/`atYEnd` on their own (Qt only recomputes them in
  `viewportMoved`/fixup), and the shipped matrix tests read those flags after
  big wheels; the fixup keeps the flags, the drag extents and the edge
  semantics exactly where Qt put them.
* The pane under the pointer scrolls and nothing else: each list handles only
  its own wheel, matching "Scroll the pane under the pointer without moving
  the other list".
* `onContentYChanged` keeps driving `noteScrollReserve`, `tailFollow` and the
  read/inspection passes, since those are coordinate-driven and agnostic to how
  the coordinate moved.

## 4. Deviations from the reference (and why)

1. **Instant movement instead of Chromium's smooth-scroll animation.** The
   reference animates ~300 ms per notch inside the compositor; the *contract*
   visible to logic is the delta-exact endpoint. Telegram Desktop — the other
   comparison client named in the issue — also steps instantly per wheel event.
   Instant writes keep the #156 reserve, the #253 hand-off and the read-marker
   passes deterministic and animation-free (Qt's own no-acceleration wheel mode
   likewise uses a fixed eased step rather than velocity accumulation).
2. **System scroll-lines scaling.** Chromium ignores `wheelScrollLines`; we
   scale 100 px/notch by `wheelScrollLines / 3`, which equals the reference at
   the default and honors user preference otherwise.
3. **Reference contract block gap (process note).** #142 shipped without a
   Reference contract block; the measurements above are the lane's substitute
   and are recorded on the issue as required by the docs-first rule.

## 5. Test contract (native-native-window)

New QtTest cases (`wheelNotchScrollsMatchTheReferenceContract`,
`wheelDirectionReversalAndTouchpadStayPredictable`, data-driven over both
lists) drive deterministic `QTest::wheelEvent` angle data (±120/notch) and
synthesized `pixelDelta` events. They assert per-notch movement ≥ the measured
reference threshold (99 px = the 100 px reference notch − 1 %) with a comfort
ceiling, cadence-independent repeats, the list under the pointer being the
only list that moves, a clean return-to-origin on direction reversal, and
sub-notch touchpad steps that are identical for identical input, reverse
proportionally, and drift nothing after stopping. The existing prefetch reserve
cases and the `messageUpdatesRespectTheVisibleScrollPosition` matrix
(including the #253 resize tail-pin hand-off) must stay green.
