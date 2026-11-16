# Updater taskbar progress mirror (issue #76)

Reference measurement sha: `61dacef78dab2f5d0be76f283bebf49fe29b84af`
(reference app `workspace_ui`, measured read-only with
`git show 61dacef7:packages/electron/src/main.ts`).

## Reference contract (measured)

The Electron shell mirrors the autoUpdater download percent into the OS taskbar
entry of the main window and clears it when the package is ready or the
download errored. Measured locations in `packages/electron/src/main.ts` at the
pinned sha:

- `main.ts:712-719` - the sink every progress write passes through:

  ```ts
  function setProgressBar(progress: number): void {
    if (!mainWindow) return;
    if (progress < 0) {
      mainWindow.setProgressBar(-1);
    } else {
      mainWindow.setProgressBar(Math.min(1, Math.max(0, progress)));
    }
  }
  ```

  A negative value clears the bar; any other value is clamped into `[0, 1]`.

- `main.ts:1208-1211` - the IPC entry `os:setProgressBar` maps a non-finite
  number to `-1` (clear) before the clamp above:

  ```ts
  ipcMain.on("os:setProgressBar", (_event, progress: number) => {
    const p = Number(progress);
    setProgressBar(Number.isFinite(p) ? p : -1);
  });
  ```

- `main.ts:1363-1371` - the `download-progress` event pushes
  `setProgressBar(progress.percent / 100)`. `progress.percent` is a percent in
  `0..100`, so the taskbar receives a fraction of the download. Because the
  event only fires while bytes arrive and `transferred` never decreases inside
  one download, the pushed fraction rises monotonically from `0` toward `1`.
  The accompanying `updater:status` payload carries `Math.round(percent)` for
  the in-app state machine only; the taskbar hint itself keeps full precision.

- `main.ts:1373-1376` - `update-downloaded` clears the bar: `setProgressBar(-1)`.
- `main.ts:1379-1381` - `error` clears the bar: `setProgressBar(-1)`.
- `packages/web/src/shared/lib/updater.ts` holds the React `UpdateStatus` state
  machine (checking/available/downloading percent/ready/up-to-date/error) that
  the `updater:status` sends feed. The taskbar mirror is independent of it.

There is no in-app visual change: the bar renders in the DE taskbar/dock, not
inside the window, and both locales are unaffected.

## Native counterpart and the QWindow progress hint

The native updater (`packages/native/src/native-updater.cpp`) already owns the
equivalent state machine and a `progress` property. `m_progress` is
`received / expectedSize` (`consume()`), i.e. it already lives in the same
domain as the reference's `percent / 100`, rises monotonically within one
download (`m_received` only grows, `m_expectedSize` is fixed, and every
`readyRead` chunk emits `changed()`), and resets to `0` when a new `download()`
starts. The states `downloaded` and `error` are the native equivalents of the
reference's `update-downloaded` and `error` events.

The task asks for `QWindow::setProgress`. Measured on the verified kit
(Qt 6.10.2, `qmake6 -query QT_VERSION`): `QWindow` in
`/usr/include/x86_64-linux-gnu/qt6/QtGui/qwindow.h` exposes **no**
`setProgress`/`progress` API (grep for `progress` in that header: no matches).
So the native client cannot call that Qt API today. The hint therefore goes
through the repository's own mirror class at a single integration point that
stores the value as the window's `progress` property:

- `workspace::UpdaterTaskbarProgress` (new, `src/native-updater-taskbar.{h,cpp}`)
  attaches a `QWindow*` to a `NativeUpdater*` in C++ only (never QML), observes
  `changed()`, and writes the window `progress` property:
  - `downloading`: `hintFor(progress)` - the reference IPC mapping reproduced
    1:1: non-finite or negative -> `-1` (clear), otherwise clamped to `[0, 1]`
    (`main.ts:712-719`, `main.ts:1208-1211`).
  - `downloaded`, `error`: `-1` (clear) - `main.ts:1373-1376`, `main.ts:1379-1381`.
  - `idle` (the native `cancel()`, which has no Electron counterpart): `-1`, so
    a cancelled download cannot leave a stuck bar.
  - `checking`, `available`, `current`, `installing`, `ready`: untouched, like
    the reference which never pushes progress on those events.
- Dedup: a value identical to the last pushed hint is not re-written, matching
  `QWindow::setProgress`'s documented dedupe.

`UpdaterTaskbarProgress::setHint()` is the single platform sink: it writes the
window property and publishes every new hint on the real desktop channel
described in the next section. When a Qt release ships `QWindow::setProgress`,
that call is added at the same sink without touching the state mapping.

## Platform channel: com.canonical.Unity.LauncherEntry (fix for finding 4581288189)

Reviewer finding: a dynamic `QWindow` property is consumed by nothing on any
platform, so the property-only sink was vacuous ("the tests pass without
displaying a bar"). The fix forwards progress and clear events through
`com.canonical.Unity.LauncherEntry.Update`, the de-facto standard session-bus
taskbar protocol on Linux, used the same way Chrome, Firefox and libunity-based
clients use it.

Protocol (measured against the cited pages 2026-11-16):

- Signal `com.canonical.Unity.LauncherEntry.Update(s app-uri, a{sv} properties)`
  on the session bus. The low-level D-Bus API section of the Unity Launcher API
  wiki (`wiki.ubuntu.com/Unity/LauncherAPI`, quoted by the community worked
  example at askubuntu.com/questions/65054 "Unity Launcher API for C++")
  defines exactly this Qt pattern: `QDBusMessage::createSignal(path,
  "com.canonical.Unity.LauncherEntry", "Update")`, `signal << "application://firefox.desktop"`,
  a `QVariantMap` with `progress-visible` (b) and `progress` (d, "float between
  0 and 1"), sent via `QDBusConnection::sessionBus().send(signal)`.
- `app-uri` is `application://` + the basename of the installed desktop file.
  Valve's producer-side note (github.com/ValveSoftware/steam-for-linux#4777)
  confirms the identical `sa{sv}` frame:
  `"application://steam.desktop"` + `{ "progress-visible": true, "progress": 0.5 }`.
- The object path carries no meaning: "The exact object path is not important,
  and only being used to avoid internal collisions" (forum.snapcraft.io
  topic 22004 on libunity internals, which broadcasts from
  `/com/canonical/unity/launcherentry/$hash`). We use
  `/com/canonical/Unity/LauncherEntry`.
- Launchers subscribe with sender-agnostic signal match rules, e.g. a GNOME
  shell consumer subscribes `Gio.DBus.session.signal_subscribe(null,
  'com.canonical.Unity.LauncherEntry', 'Update', null, null, ...)` and reads
  `props['progress-visible']` / `props['progress']` (basskibo/linux_dynamic_island
  `sources/progress.js`). Consumers consume the app-uri minus its prefix:
  `appUri.replace('application://', '')` then `Shell.AppSystem.lookup_app`
  (same file). Dash-to-Dock has honoured these launcher progress/badge
  properties since v62 ("support for Ubuntu launchers notification badges
  and progress", micheleg.github.io/dash-to-dock release notes v62).
- A property change may carry only the changed keys ("you don't always have to
  specify all parameters", stackoverflow.com/questions/43875343), so the clear
  frame sends just `progress-visible=false`.

Implementation (`src/native-updater-taskbar.cpp`, `UpdaterTaskbarProgress::publish`):

- `hint >= 0`: `{ "progress-visible": true, "progress": <hint as double> }` with
  `hint` already clamped to `[0, 1]` by `hintFor()` (reference clamp kept
  1:1). `hint < 0` (clear): `{ "progress-visible": false }` and no stale
  `progress` key.
- Broadcast (no destination), matching the wiki pattern: a broadcast reaches
  every match-rule listener as well as any launcher that owns a well-known
  name, while a name-addressed signal would only reach a name owner that real
  consumers (GNOME extensions) do not take.
- Fire-and-forget: with no launcher listening (bare WMs, headless X) the daemon
  simply has no receiver, so the documented Xvnc limitation below stays honest
  while real desktops render the bar. There is no delivery state worth
  tracking. Late-listener delivery - the libunity "re-publish when the
  launcher re-appears" extra in the shape real consumers need - is handled by
  the two republish mechanisms documented in the next section.
- app-uri: `application://com.exordos.workspace.native.preview.desktop` - the
  installed file name (`packages/native/CMakeLists.txt`,
  `install(FILES resources/workspace-native.desktop ... RENAME
  com.exordos.workspace.native.preview.desktop)`).

KDE Plasma additionally respects the X11 `_KDE_NET_WM_PROGRESSIVE_ACTIVITY`
client message (KDE window-manager-protocol extensions). It was intentionally
not added: it needs raw xcb hand-rolled events on top, Plasma also honours the
LauncherEntry signal, and LauncherEntry alone already matches "supported
platform taskbar APIs".

## Where the DE ignores the channel (expected environment limitation)

Taskbar rendering is DE-side and optional:

- The current freedesktop EWMH specification (`wm-spec`, `latest-single`,
  fetched 2026-11-16) defines no progress hint at all - grep for `progress`
  returns no matches. There is no standard cross-DE X11 wire hint.
- Taskbar progress rendering is done by launchers that implement the
  LauncherEntry protocol above (Unity7, Dash-to-Dock, Plank, Latte); bare
  window managers (this repo's Xvnc + openbox QA stand, and any minimal X
  session) run no such launcher, so nobody subscribes to the Update signal
  there and the broadcast is dropped unchanged - exactly the documented,
  honest limitation of the QA stand, not of the channel.
- Even KDE's own applications implement taskbar progress only for Windows
  (`ITaskbarList3`; e.g. `KDE/elisa` `src/windows/taskbarmanager.{h,cpp}`),
  which confirms there is no X11-side standard to mirror today.

## Late consumers: republish mechanisms (findings 4581477136, rounds 3 and 4)

Signals are not replayed, so a launcher that starts - or restarts - while a
download has stalled missed every earlier frame and would otherwise show
nothing until new bytes move. Two complementary mechanisms close that gap.

Round 3 - `NameOwnerChanged` republish. The mirror subscribes to
`org.freedesktop.DBus` `NameOwnerChanged` on the session bus and, when the
well-known `com.canonical.Unity.LauncherEntry` name gains a non-empty owner,
republishes the current hint state (progress-visible + progress, or the
clear) straight through `publish()`, bypassing the dedupe guard, so the late
name-owning launcher receives the current state without waiting for fresh
progress. While no hint has been pushed yet nothing is republished - the
silent pre-download contract is unchanged.

Round 4 - throttled periodic republish. The round-3 trigger only fires for
consumers that request the well-known name. The protocol notes above describe
the more common real consumer differently: a GNOME shell extension subscribes
`signal_subscribe(null, 'com.canonical.Unity.LauncherEntry', 'Update', null,
null)` (basskibo/linux_dynamic_island `sources/progress.js`) and never owns
any name, so no `NameOwnerChanged` ever involves it. Such a consumer starting
during a stalled download (no new `changed()` ticks, hence no new broadcast)
would still miss the current progress. Therefore, while a progress hint is
active, `UpdaterTaskbarProgress` re-broadcasts the current hint on a repeating
`QTimer` (`startRepublishing()`/`stopRepublishing()` in
`src/native-updater-taskbar.cpp`):

- Interval: `kRepublishIntervalMs = 2000` ms. The interval is the worst-case
  invisibility window for a consumer that appears mid-stall: at 2 s a late
  GNOME extension shows the bar within 2 s of subscribing, which is already
  perceived as immediate, while a shorter interval buys nothing visible (the
  bar cannot move faster than progress ticks anyway) and multiplies idle
  traffic; a longer one (5 s+) leaves a stalled late consumer visibly
  uninformed for several seconds.
- Bounded and quiet, not a broadcast storm: at most one small frame (fixed
  app-uri plus two properties, a few dozen bytes) per 2 s - worst case 0.5
  signals/s - and only while a visible hint is active. `downloaded`, `error`,
  `cancel()`/idle clear the hint and stop the timer; the clear frame itself
  is broadcast exactly once, because a consumer that subscribes after the
  clear already renders the correct default of no bar. Before the first
  hint and on headless sessions without a bus nothing is ever published, as
  before.
- The heartbeat calls `publish()` directly, bypassing the `setHint()` dedupe:
  the wire value is unchanged, but the just-subscribed consumer has not
  received it. This mirrors the same reasoning as the round-3 trigger.

Consequently acceptance is asserted at the protocol layer in the existing
`native-updater` CTest group against the existing fake update server - the
window `progress` property plus the Update signals observed by a fake
launcher on a private bus - never through pixel screenshots of a dock.

## Acceptance tests (native-native-updater group)

`packages/native/tests/native-updater-test.cpp` gains
`downloadProgressMirrorsTheTaskbarHint` (data rows: `downloaded`, `error`,
`cancel`), reusing the existing loopback fake update server:

- during a chunked simulated download the window `progress` property rises
  monotonically from `0` toward `1`, every value inside `[0, 1]`;
- the property is absent while idle/checking/available (no bar before the
  download starts);
- on `downloaded`, on `error`, and on `cancel()` the property becomes `-1`
  (cleared).

`downloadProgressReachesTheLauncherEntryChannel` (data rows `downloaded`,
`error`, `cancel`, plus `late-launcher` and `late-listener`) is the
protocol-level check for finding 4581288189. The case re-executes itself with
`dbus-run-session` (the `native-controller-test` downloadedFileActions
pattern) so the fake never shadows a live desktop bus, registers a fake
launcher (well-known name `com.canonical.Unity.LauncherEntry`, object
`/com/canonical/Unity/LauncherEntry`, plus a sender-agnostic match rule, on a
second bus connection because a daemon never echoes a connection's own
signals back), and then drives the same chunked fake download. It asserts
the wire frames themselves:

- nothing is published while checking/available;
- at least one `progress-visible=true` frame with `progress > 0` (bytes
  actually moved) crosses the bus before the terminal state;
- the published `progress` values stay in `[0, 1]`, never decrease, and
  `downloaded` proves `1.0` reached the launcher;
- every frame's app-uri is
  `application://com.exordos.workspace.native.preview.desktop`;
- the last frame on `downloaded`, `error` and `cancel()` is the clear:
  `progress-visible=false` with no stale `progress` key;
- `late-launcher` (finding 4581477136) stalls the download at `progress > 0`
  with no launcher on the bus - every frame so far dropped - then registers
  the fake launcher; without any new bytes it must record a republished frame
  with `progress-visible=true` and `progress` equal to the mirror's current
  hint. On the pre-fix code the late launcher records nothing and the row
  fails RED.
- `late-listener` (finding 4581477136 round 4) is the nameless variant: the
  fake consumer installs only a sender-agnostic, path-agnostic match rule for
  `Update` (the `signal_subscribe(null, ...)` shape of the GNOME extension
  cited above) and never requests the well-known name, so no
  `NameOwnerChanged` involves it. After the same stall it subscribes and must
  record the current hint with no new bytes (QTRY-bounded at 5 s, proving the
  heartbeat interval is at most that); over a fixed 6 s window it may record
  at most 4 further frames (proving a throttled republish, roughly 1.5 s or
  slower on average, not a broadcast storm); after `cancel()` it records the
  clear frame and then nothing for 4 s (proving the heartbeat stops on a
  terminal state). On the pre-fix code the late listener records nothing and
  the row fails RED.

The bus case also gets a focused ctest entry,
`native-updater-launcher-entry`, so `ctest -R '^native-updater'` gates it on
its own; the full CI `^native-updater$` binary run keeps executing every row
inside the main entry, so CI coverage is unchanged.

With the property-only sink this case fails on every row (the fake records
zero frames); it went RED before the publish implementation went GREEN.

The binary needs a `QWindow`, so the test main switches from
`QTEST_GUILESS_MAIN` to a `QGuiApplication` main with the platform forced to
`offscreen`; all pre-existing updater cases are unchanged.
