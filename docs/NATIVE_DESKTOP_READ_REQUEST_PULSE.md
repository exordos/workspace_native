# Read-request pulse: accent overlay on peer bubbles awaiting a read receipt

Issue #113 (part of #105). Design fidelity: parity-preserving
(docs/NATIVE_DESKTOP_PARITY.md).
Reference: workspace_ui `61dacef78dab2f5d0be76f283bebf49fe29b84af`.
Figma token unavailable - mockups not consulted.

## Reference contract

- Overlay (`packages/web/src/app/app.styles.css:128-156`): a
  `workspace-message-read-request-pulse::before` pseudo-element painted on the
  bubble shell - `position: absolute; inset: 0; z-index: -1; pointer-events:
  none; background-color: var(--color-accent); border-radius: inherit;`
  animating `opacity` through `0 -> var(--workspace-read-request-highlight-opacity
  = 0.2) -> 0` over `1.6 s ease-in-out infinite` (800 ms up, 800 ms down).
  The comment at :139 pins the intent: only the background overlay animates so
  the message content stays fully opaque; `z-index: -1` keeps the wash between
  the bubble background and its content.
- Applied per bubble (`workspace-message-bubble-presentation.ui.tsx:171-172,
  :531-532, :856-871`): the class is present exactly when
  `effectiveReadRequestPending = !isPreview && serverMessage &&
  !serverMessage.read` plus the list-level read-request boundary coverage
  (`workspace-message-list.ui.tsx:661`). Outbound bubbles never pulse.
- Window-activity gate (`workspace-visible-message-read.hook.ts`, `isWindowActive`
  from `shared/lib/visibility.ts`; `app.styles.css:228-267`): a background
  window stops requesting reads, and the global activity ruleset pauses CSS
  animations for background windows. Net effect to reproduce: the pulse runs
  while the window is active and is not running while it is inactive.
- Reduced motion (`app.styles.css:269-286`): under
  `prefers-reduced-motion: reduce` the animation is `none` and the overlay
  renders as a static accent wash at the full 0.2 opacity.
- States: light + dark use the palette `accent` token; ru + en carry no visible
  text (the overlay is purely visual), so no locale strings change.
- Electron (`packages/electron/src/main.ts`): window focus/visibility drives
  the reference activity gate; the native stand-in is the QML `Window.active`
  state of the shell window.

## Native counterpart today

`packages/native/qml/Main.qml` marks visible inbound messages read through the
`visibleReadTimer -> messageList.markVisibleRead()` pipeline
(Main.qml ~:6770-7077) and uses `readBoundary` only for scroll anchoring
(~:3644-3655 area of the reference note; in-tree around :6627 and :6967+).
`message.read` flips once the controller confirms the `read_up_to` mutation
(`native-controller.cpp:markVisibleMessagesRead -> sendReadRequest`), which is
exactly the window a read is "requested but not yet receipted" - but no visual
pulse existed.

## Native design

One overlay + one animation inside the message bubble in `Main.qml`:

- The controller exposes the active and queued `read_up_to` boundary UUIDs.
  The list walks its canonical history in reverse. Each boundary covers its
  row and earlier rows in the same topic, preserving microsecond and UUID
  order. Inbound unread bubbles pulse only while covered. Newer rows and
  other topics stay idle until a request covers them.
- Inside `id: bubble`, a `Rectangle` (`objectName: message-read-pulse-<uuid>`)
  `anchors.fill: parent`, `z: -1` (Qt Quick keeps negative-z children above
  the parent's own fill but below sibling content - the same stacking as
  `z-index: -1` with `isolation: isolate`), `color: root.colors.accent`,
  corner radii mirrored from the bubble (the CSS uses
  `border-radius: inherit`), `visible: readRequestPending`, and
  `opacity: root.prefersReducedMotion ? 0.2 : 0`.
- A `SequentialAnimation` (`objectName: message-read-pulse-anim-<uuid>`)
  drives that opacity: two `NumberAnimation`s (0 -> 0.2, 0.2 -> 0, 800 ms
  each, `Easing.InOutQuad` per leg - CSS applies the timing function per
  keyframe segment, and InOutQuad is the project's established stand-in for
  `ease-in-out`). `loops: Animation.Infinite`, and
  `running: readRequestPending && root.active && !root.prefersReducedMotion`.
- Reduced motion reuses the existing `root.prefersReducedMotion` binding
  (`NativePreferences` XDG portal + override, issue #107 precedent): the
  animation never runs and the overlay shows the static 0.2 wash, matching
  the CSS fallback.
- When the read marking confirms, `modelData.read` flips, `readRequestPending`
  goes false and the overlay hides/stops - "gone after activation".

### Deliberate deviations

- The CSS pauses the keyframe mid-flight for background windows (the overlay
  freezes at its current opacity). The native port stops the animation and
  lets the opacity binding settle to 0: a half-tinted bubble frozen at an
  arbitrary phase is neither pixel-meaningful nor deterministic to assert,
  and the acceptance criterion is "the active-state change stops the
  animation". Re-activation restarts the pulse from 0.
- No gate was added to `visibleReadTimer`/read marking itself; only the visual
  pulse is gated on `root.active` (scope of this issue).
- No new visible text, so no new translation entries (`workspace_ru.ts`
  untouched).
- `readRequestBoundariesChanged` updates the list when a boundary is
  submitted, extended, confirmed, rejected or cancelled. Window activity
  and reduced motion use their own existing property notifications.

## Acceptance (native-window-test)

`readRequestPulseStates` (light + dark, motion + reduced motion):

1. A covered inbound unread bubble shows the overlay and its
   `message-read-pulse-anim-<id>` animation is `running` while the window is
   active; already-read inbound bubbles show nothing; the overlay colour is
   the palette `accent` token in both modes.
2. Activating a decoy window (the WM takes focus away, `Window.active` goes
   false) stops the animation while the overlay stays pending.
3. Re-activating resumes it; under reduced motion the animation never runs
   and the overlay renders statically at opacity 0.2.
4. Releasing the held `read_up_to` response confirms the read; the overlay
   hides and the animation stops.

Animation state is asserted through the `running` property by `objectName`
(deterministic), never by sampling opacity over wall-clock time. The mock
server's `Response.defer` gate holds the `read_up_to` delivery so the unread
window is deterministic.

The partial aggregate request case holds an actual read response, checks a
newer unread row and an older row from another topic, then extends the
same-topic boundary and confirms both requests.
