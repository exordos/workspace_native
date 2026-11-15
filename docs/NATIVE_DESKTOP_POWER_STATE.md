# Native power state: reconnect on resume, battery-aware presence cadence

Issue #78 parity work. The reference contract is `workspace_ui`
`61dacef78dab2f5d0be76f283bebf49fe29b84af`.

## Reference contract (measured)

React `packages/web/src/shared/lib/power.ts` forwards OS power events from the
Electron shell:

- `powerMonitor` `suspend` / `resume` / `on-battery` / `on-ac` arrive as one
  `power:change` channel (Electron `packages/electron/src/main.ts:811-817`);
  the initial battery read is `power:getState` -> `{ onBattery:
  powerMonitor.isOnBatteryPower() }` (`main.ts:1103`).
- `onPowerResume` is consumed by
  `packages/web/src/widgets/layout/layout-workspace-realtime.hook.ts:377-386`:
  every realtime manager runs `manager.reconnectAll("power_resume")` with no
  delay on resume.
- `isOnBattery()` is consumed by
  `packages/web/src/entities/user/user-workspace-presence-reporter.lib.ts:107`:
  the presence heartbeat cadence is `workspacePresenceIntervalMs(state,
  reportIntervalMs, isOnBattery())`.

Measured cadence constants (`user-workspace-presence-reporter.lib.ts:20-25`):

| Constant | Value (ms) |
| --- | --- |
| `WORKSPACE_PRESENCE_REPORT_INTERVAL_MS` (active) | 30 000 |
| `WORKSPACE_PRESENCE_UNFOCUSED_INTERVAL_MS` (visible) | 120 000 |
| `WORKSPACE_PRESENCE_UNFOCUSED_BATTERY_INTERVAL_MS` (visible on battery) | 160 000 |
| `WORKSPACE_PRESENCE_HIDDEN_INTERVAL_MS` (hidden) | 300 000 |
| `WORKSPACE_PRESENCE_BATTERY_MULTIPLIER` | 2 |

`workspacePresenceIntervalMs` semantics: `active` returns
`baseIntervalMs * batteryFactor`; `visible` returns the fixed unfocused
constant (120 000 mains, 160 000 battery — the only tier that is capped
against a larger configured base); `hidden` returns
`max(baseIntervalMs, 300 000) * batteryFactor`. The heartbeat is a radio
wake-up, so the whole ladder doubles on battery except the visible tier, which
has its own measured constant.

## Native design

New `workspace::PowerWatcher` (`packages/native/src/power-watcher.{h,cpp}`),
a session-bus D-Bus observer:

- `org.freedesktop.login1`, object `/org/freedesktop/login1`, interface
  `org.freedesktop.login1.Manager`: `PrepareForSleep` signal (`true` = about
  to sleep, `false` = just resumed). Emits `sleepAboutToStart()` /
  `resumed()`.
- `org.freedesktop.UPower`, object `/org/freedesktop/UPower`, interface
  `org.freedesktop.UPower`, property `OnBattery`: one async
  `org.freedesktop.DBus.Properties.Get` (reply unwrapped from `QDBusVariant`)
  plus a `PropertiesChanged` subscription. Emits `batteryChanged(bool)`.
- Graceful no-daemon path: with no session bus the watcher subscribes to
  nothing; with a bus but no `logind`/`UPower`, the signal rules simply never
  match and the `Get` error is ignored, so the client keeps its mains-power
  behaviour (`isOnBattery()` stays `false`). This mirrors the reference
  browser build where no bridge exists and `isOnBattery()` stays false.
  `start()` takes an injectable `QDBusConnection` (default: session bus) so
  tests drive it from a private bus fixture.

`NativeController` wiring:

- `setPowerWatcher(PowerWatcher *)` connects `resumed` to the realtime
  client's immediate reconnect and `batteryChanged` to the presence cadence.
- Resume: `RealtimeClient::reconnectNow(reason)` drops the backoff state
  (suspend + resume with cursors kept) and re-establishes the realtime link
  and epoch catch-up immediately — the counterpart of the reference
  `reconnectAll("power_resume")` with no delay.
- Battery: `setOnBattery(bool)` re-times the presence heartbeat. The
  heartbeat reproduces the reference ladder for the two window states the
  native shell distinguishes today: focused `30 000 ms * batteryFactor`,
  unfocused `120 000 ms` mains / `160 000 ms` battery. The hidden tier
  (`30 000 ms * batteryFactor`, battery `600 000 ms`) needs a window-hidden
  signal the native shell does not project yet and stays reference-only.
  The heartbeat POSTs `/api/workspace/v1/users/{uuid}/actions/presence/invoke`
  with `{"status": "active"}` (the same endpoint the manual status editor
  uses), never while the account is manually away/DND — a heartbeat must not
  overwrite a manual status, matching the reference rule that what a beat is
  allowed to claim is decided separately from when it fires.
- `PrepareForSleep(true)` is observed but needs no native action: the OS
  freezes the process anyway; the reference only timestamps the suspension.

## Tests

Extended inside the existing `native-native-controller` CTest group (no new
groups, the gate group count is fixed). A test-local `dbus-daemon --session`
provides the fixture bus; the watcher is started on it with a second named
connection registered as `org.freedesktop.login1` / `org.freedesktop.UPower`:

- `PrepareForSleep(false)` emitted on the fixture bus makes the realtime test
  double (the shared `RealtimeGateway` websocket) record a second socket
  within the test tick; no healthy-socket path reconnects otherwise, so the
  extra socket can only come from the resume event.
- A `UPower` stub exported with `OnBattery=true` before the watcher starts
  widens the presence interval to `60 000 ms` through the initial `Get`;
  a `PropertiesChanged` with `OnBattery=false` toggles it back to
  `30 000 ms` and back again.
- Bare bus: a watcher on a bus without the services starts, keeps the mains
  cadence, never reconnects a healthy realtime socket and never crashes.

## Live verification note

Resume events cannot be forced on the QA host (no sleep control), so the
resume-triggered reconnect is proven by the D-Bus fixture cases above,
following the client-side precedent of issue #77. The live pass covers the
normal build/login/health/message-send path to show no regression from the
watcher or the heartbeat.
