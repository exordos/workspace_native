# F11 fullscreen toggle matching the reference hidden-menu accelerator

Issue #62 (`Window: F11 fullscreen toggle to match the reference View menu
accelerator`). Design fidelity case: parity-preserving - accelerator parity for
a surface the reference keeps invisible on Linux/Windows. No theming, no new
user-facing strings; Figma mockups do not apply to an OS-level window state.

## Reference contract (workspace_ui 61dacef78dab2f5d0be76f283bebf49fe29b84af, packages/electron/src/main.ts)

Measured at the pinned revision:

- `buildNativeMenu()` (`:853`) registers the application menu; the `View`
  submenu (`:890-904`) ends with the `{ role: "togglefullscreen" }` item
  (`:902`). Electron binds that role to F11 on Windows and Linux.
- `createWindow()` calls `mainWindow.setMenuBarVisibility(false)` on
  non-macOS (`:367-373`). The menu stays registered - that is what binds the
  accelerators - while the visible bar is hidden, so F11 is the only
  fullscreen surface for a Linux/Windows user. There is no menu click, no
  tray item and no renderer control for fullscreen.
- Window-state persistence never stores fullscreen as the normal size. The
  `WindowState` shape (`:234-240`) is `{x?, y?, width, height, isMaximized}` -
  there is no fullscreen member. `saveWindowState()` (`:255-270`) reads
  `isMaximized = win.isMaximized()` and takes
  `bounds = isMaximized ? win.getNormalBounds() : win.getBounds()`
  (the getNormalBounds rule, `:259`): the maximized flag is stored separately
  and the maximized rectangle is never persisted as the normal geometry.
  Saves fire on `close` (both the quit and the tray-hide branch), `resize` and
  `move` (`:399-408`).
- `createWindow()` restores the saved normal bounds and calls
  `maximize()` only when `saved.isMaximized` (`:336-340`, `:387-389`); a
  fullscreen entry is never replayed from disk. States: OS-level window
  state, not themed; locale-independent (the hidden menu's role label is the
  only text and it never renders on Linux/Windows).

So a reference user toggles into fullscreen with F11, and F11 again returns the
window to the state it had before (normal geometry, or maximized), with the
disk state keeping normal bounds + maximized flag the whole time.

## Native gap

The native client persists the same two-part state (`window/x|y|width|height`
plus `window/maximized` in `packages/native/src/native-preferences.cpp`) and
re-applies `Qt::WindowFullScreen` on activation restore
(`NativePreferences::activateWindow`), but no key binding enters or leaves
fullscreen - the restore path is unreachable from the keyboard and a window
that is fullscreen has no way out.

## Native mapping

The seam is `NativePreferences`, the component that already owns the persisted
window state (`m_normalGeometry`, `m_windowMaximized`, the debounced
`saveWindow`) and subscribes to the window's geometry/state signals in
`attachWindow` - the native counterpart of the reference persistence module
above. `attachWindow` additionally installs an event filter on the window:

- `KeyPress` for unmodified `Qt::Key_F11` (auto-repeat ignored) toggles:
  - not fullscreen -> `showFullScreen()`;
  - fullscreen -> `showMaximized()` when `m_windowMaximized`, else
    `showNormal()` - the previous maximized or normal state, mirroring the
    reference restore rule (a maximized user returns maximized; a normal user
    returns at the saved normal geometry).
- Entering fullscreen must not overwrite the persisted normal geometry or the
  maximized flag. That mirrors the getNormalBounds rule and is already
  enforced by the two guards `attachWindow` installs: geometry changes are
  folded into `m_normalGeometry` only while `windowState() == Qt::WindowNoState`,
  and `m_windowMaximized` is only re-derived from `WindowMaximized` /
  `WindowNoState` state changes - neither reacts to `WindowFullScreen`, so the
  debounced save during a fullscreen transition keeps writing the pre-F11
  values.
- Leaving fullscreen through the WM (not just F11) is handled by the same
  guards: the restored normal geometry flows back in only once the state is
  `WindowNoState` again.
- The activation restore at `activateWindow` keeps working unchanged: a
  fullscreen window that was hidden to the tray is re-shown fullscreen, and
  F11 then exits it.

Locale-independent by construction: the binding is a key code, no new `tr()`
strings, nothing themed.

## Tests

CTest group `native-native-window` (`packages/native/tests/native-window-test.cpp`,
gui-labelled: real X display + window manager):

- F11 from a normal window enters `QWindow::FullScreen` and a second F11
  restores the previous normal geometry; the persisted `window/*` values stay
  at the normal bounds across both transitions.
- F11 from a maximized window enters fullscreen and returns maximized; the
  persisted normal size and `window/maximized` mirror the getNormalBounds
  rule (normal size + `true`, never the fullscreen or maximized rectangle).
- A fullscreen window hidden to the tray is re-applied fullscreen by
  `activateWindow()` (the existing restore path) and F11 exits it back to the
  saved normal geometry, without the fullscreen bounds ever reaching the
  settings file.
