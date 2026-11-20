# Native call awareness: inhibit display sleep, show the active call in the tray tooltip

Issue #86 parity work (part of #33). The reference contract is `workspace_ui`
`61dacef78dab2f5d0be76f283bebf49fe29b84af`.

Figma token unavailable - mockups not consulted (the token endpoint returns
401 "Token has expired" since 2026-11-16; this is a shell-only surface with no
new UI, so no mockups are required by scope).

## Reference contract (measured at the pinned sha)

`packages/electron/src/main.ts:1268-1322` owns three IPC handlers; React's
`packages/web/src/shared/lib/call-state.ts` (`callState.start` /
`updateParticipants` / `end`) is the single trigger on the web side. The media
session, the web wake lock and the pulsing document title belong to the calls
feature slices, not to this issue.

- `call:start` (`main.ts:1269-1287`): the room name is sanitized -
  `data.room.replace(/[\x00-\x1f\x7f]/g, "").slice(0, 128) || "Call"` - a
  non-string payload also yields `"Call"`. Control characters are stripped
  first, then the string is capped at 128 chars; the empty result falls back
  to `"Call"` (whitespace-only rooms stay as-is - the reference does not trim).
  The display-sleep blocker (`powerSaveBlocker.start("prevent-display-sleep")`)
  is started exactly once: `if (callPowerSaveBlockerId == null)`. The tray
  tooltip becomes `"{appName} - In call: {room}"`. The issue contract pins the
  ASCII `" - "` separator even though the reference source uses an em dash.
  The `participants` field of `call:start` is not rendered at start; the count
  only ever arrives through `call:update`. The macOS dock badge lines are
  out of scope on Linux.
- `call:end` (`main.ts:1289-1306`): clears the active room, stops the blocker
  if one was started (and forgets the id), and restores the bare app-name
  tooltip.
- `call:update` (`main.ts:1308-1313`): only while a call is active
  (`if (activeCallRoom && tray)`) the tooltip is rewritten to
  `"{appName} - In call: {room} ({n} participants)"` with
  `participants ?? 0`. While idle it does nothing.

States: idle vs active call. Tooltip text is rendered in the active app locale
(the room name is data, never localized).

## Native design

`packages/native/src/native-call-awareness.{h,cpp}` adds
`workspace::CallAwareness` to `workspace-native-core`, modelled on the
`PowerWatcher` seam (issue #78, docs/NATIVE_DESKTOP_POWER_STATE.md):

- `start(const QDBusConnection &bus = defaultBus())` where the default is the
  **session** bus (ScreenSaver and PowerManagement.Inhibit are session
  services); the injectable parameter keeps the private fixture bus usable in
  tests, and a connection that is not connected degrades to "inhibit is a
  no-op, the rest of the behaviour still holds".
- `Q_INVOKABLE` methods for the shell call state:
  `callStarted(QString room, int participants)`,
  `callParticipantsUpdated(int count)`, `callEnded()`. These mirror the
  `call:start` / `call:update` / `call:end` IPC events one-to-one, including
  "update ignored while idle" and "inhibit acquired exactly once per call".
- Room sanitization is a pure static `CallAwareness::sanitizeRoomName`, the
  direct port of the reference rule (strip `[\x00-\x1f\x7f]`, cap at 128,
  empty -> `"Call"`).
- Display-sleep inhibition walks the freedesktop chain on the session bus:
  `org.freedesktop.ScreenSaver` `/ScreenSaver` `Inhibit(appId, reason) ->
  cookie` first; if that service is missing or errors, fall back to
  `org.freedesktop.PowerManagement.Inhibit`
  `/org/freedesktop/PowerManagement/Inhibit`. The returned cookie is held for
  the call lifetime and released with `UnInhibit` / `Uninhibit` on `callEnded`
  and in the destructor. Failing both services leaves the tooltip behaviour
  untouched - the same degrade posture as the reference power paths.
- The D-Bus surface is deliberately thin: two blocking method calls with a
  short timeout, driven through the injected `QDBusConnection`, so tests can
  export stub services on a private bus (the `PowerBusFixture` pattern).

## Tray tooltip

The tooltip text itself is composed in `NativeDesktop` (it owns the app name
and already rebuilds the tooltip in `refreshLabels()` on unread/locale
changes, so the call suffix must survive those refreshes). `CallAwareness`
emits `callStateChanged(room, participants)` with the sanitized room and
`participants < 0` meaning "no count yet" (start parity: the count appears
only after `callParticipantsUpdated`). `NativeDesktop::setCallState(room,
participants)` stores the state and calls `refreshLabels()`, which now paints:

- idle: `Workspace` (the bare app-name tooltip, unchanged),
- active: `tr("%1 - In call: %2")` -> `Workspace - In call: standup`,
- with a count: `... + tr(" (%1 participants)")` -> `Workspace - In call: standup (5 participants)`.

Both wrappers are `tr()` strings and were added to
`packages/native/translations/workspace_ru.ts` so the native-native-locale
catalog guard stays green; the room name stays data.

`main.cpp` starts awareness on the session bus and subscribes it to the same
call window that follows the controller's active-call store. A successful embed
load acquires the inhibitor and publishes the parsed room. Until the embed is
ready, including after a failed load, the tray stays idle. Live participant
counts from the meeting bridge update the tooltip. Switching between expanded
and PiP preserves both the room and the inhibitor. When the shell closes, the
store clears, or the observed window is destroyed, the service releases the
inhibitor and restores the idle tooltip.

## Tests (mock D-Bus, per the #78 precedent)

`joinedCallUpdatesTrayAndKeepsOneInhibitorThroughPip` follows a synthetic
meeting from the controller's Join action through embed failure, retry, live
participant updates, PiP and two independent close paths. It asserts the
private D-Bus service's acquire/release calls and the actual tray tooltip.

`native-controller-test` (the group the issue names) gains a
`ScreenSaverStub` / `PowerManagementInhibitStub` pair exported on the private
session bus fixture, and cases for: cookie held + tooltip
`Workspace - In call: standup` after `callStarted("standup", 3)`;
`callParticipantsUpdated(5)` -> `(5 participants)`; `callEnded()` releases the
cookie and restores the bare tooltip; sanitization (control chars, 200-char
cap at 128, empty -> `Call`); exactly-once inhibit across repeated starts;
update-while-idle ignored; PowerManagement fallback when ScreenSaver is
absent; full degradation on a bus with no inhibit services. The live product
surface (a running call) does not exist yet in the native client - mock-proven
behaviour where the live surface does not exist follows the
docs/NATIVE_DESKTOP_POWER_STATE.md / protected-screens precedent (#222/#243).
