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

`UpdaterTaskbarProgress::setHint()` is the single platform sink. When a Qt
release ships `QWindow::setProgress`, that call is added there without touching
the state mapping.

## Where the DE ignores the hint (expected environment limitation)

Taskbar rendering is DE-side and optional:

- The current freedesktop EWMH specification (`wm-spec`, `latest-single`,
  fetched 2026-11-16) defines no progress hint at all - grep for `progress`
  returns no matches. There is no standard cross-DE X11 wire hint.
- KDE Plasma and modern GNOME docks honour per-window progress through their
  own private channels; bare window managers (this repo's Xvnc + openbox QA
  stand, and any minimal X session) have no taskbar at all and silently ignore
  the hint.
- Even KDE's own applications implement taskbar progress only for Windows
  (`ITaskbarList3`; e.g. `KDE/elisa` `src/windows/taskbarmanager.{h,cpp}`),
  which confirms there is no X11-side standard to mirror today.

Consequently acceptance asserts the mirror through the window `progress`
property (`QWindow::property("progress")`) in the existing
`native-updater` CTest group against the existing fake update server - never
through pixel screenshots of a dock.

## Acceptance test (native-native-updater group)

`packages/native/tests/native-updater-test.cpp` gains
`downloadProgressMirrorsTheTaskbarHint` (data rows: `downloaded`, `error`,
`cancel`), reusing the existing loopback fake update server:

- during a chunked simulated download the window `progress` property rises
  monotonically from `0` toward `1`, every value inside `[0, 1]`;
- the property is absent while idle/checking/available (no bar before the
  download starts);
- on `downloaded`, on `error`, and on `cancel()` the property becomes `-1`
  (cleared).

The binary needs a `QWindow`, so the test main switches from
`QTEST_GUILESS_MAIN` to a `QGuiApplication` main with the platform forced to
`offscreen`; all pre-existing updater cases are unchanged.
