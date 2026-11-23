# Call WebEngine runtime and call-shell failure paths (issue #728)

Status: draft for review. Reference: production call view
`qml/NativeCallWebEngineView.qml` (imports `QtWebEngine` + `QtWebChannel`),
call shell `qml/Main.qml` (`callWindowEmbedLoader`), state machine
`src/call/native-call-window.{h,cpp}` (`EmbedState`).

## Problem

1. The Linux release does not ship the QtWebEngine/QtWebChannel runtime. The
   call view is instantiated lazily through a `Loader`, so
   `qt_generate_deploy_qml_app_script` cannot see the imports; the published
   package has no QtWebEngine QML module, no `libQt6WebEngine*`/`libQt6WebChannel`
   libraries, no `QtWebEngineProcess` helper and no WebEngine `.pak` resources.
   Clicking an existing call card fails at QML import and the shell stays on
   "Loading call client..." forever.
2. The failure is silent by design today: a `Loader` whose source fails to
   instantiate never calls `onLoaded`, and nothing forwards `Loader.Error` to
   `JitsiCallWindowController`. The controller also has no deadline for a load
   that never reports success or failure (issue cites
   `native-call-window.cpp:209-232`).

## Design

### 1. Ship the runtime (Linux packaging)

`CMakeLists.txt` gains a dedicated install step (Linux only, alongside the
existing deploy script) that copies into the package:

- QML modules `QtWebEngine` (QtWebEngineQuick plugin incl. the `quick`
  submodule and its dependent `QtWebEngine/*` type dirs) and `QtWebChannel`
  from `${Qt6_QML_IMPORT_PATH}`.
- Shared libraries they link: `libQt6WebEngineCore`, `libQt6WebEngineQuick`,
  `libQt6WebEngineQuickDelegatesQml`, `libQt6WebChannel`, plus transitive Qt
  libs the deploy tool does not already place (`libQt6Positioning`,
  `libQt6QuickWidgets` - include-if-missing copies only).
- `QtWebEngineProcess` from `${_qt6_WebEngineProcess_executable location}`
  into `${CMAKE_INSTALL_LIBEXECDIR}` and the resource trees
  (`qtwebengine_resources*.pak`, `locales/`) into
  `${CMAKE_INSTALL_DATADIR}/qt-webengine/{resources,locales}`.

`FinalizeLinuxRuntime.cmake` already sets `$ORIGIN` rpaths for every
`libQt6*.so*`, so bundled WebEngine libraries are covered without change;
the process helper and `.pak` locations are exported through the launcher
environment: `main()` sets `QTWEBENGINEPROCESS_PATH`,
`QTWEBENGINE_RESOURCES_PATH` and `QTWEBENGINE_LOCALES_PATH` to the bundled
locations when a bundle marker is present (same rule for the installed runtime
probe and the window tests), so dev trees keep using system paths.

The step is a required configure input: when `Qt6::WebEngineQuick` is not
found the configure fails with `FATAL_ERROR` (same rule the libheif plugins
adopted), so CI catches a missing runtime instead of shipping a call-less
package.

### 2. The shell must leave Loading (QML + C++)

- `callWindowEmbedLoader` forwards instantiation failure:
  `onStatusChanged: if (status === Loader.Error) controller.callWindow.apiLoadFailed()`.
  The existing `LoadFailed` overlay (retry affordance from #263) then renders.
- `JitsiCallWindowController` gets a finite embed deadline: entering
  `Loading` arms a single-shot timer (`m_embedDeadlineTimer`, production
  30 s, test seam `setEmbedDeadlineMsForTest`); expiry calls
  `apiLoadFailed()`. `apiLoadSucceeded`/`apiLoadFailed`/`endCall` disarm it.
  Retry restarts the deadline because it re-enters `Loading`.

### 3. Verification

- `tests/verify-installed-runtime.py`: `QtWebEngine` and `QtWebChannel` join
  the `MODULES` inventory, and a new Linux-only offline probe loads
  `NativeCallWebEngineView.qml` in a `QQmlEngine` restricted to the bundled
  import tree and asserts `Ready` (issue evidence recipe), plus presence
  checks for `QtWebEngineProcess` and the resources tree.
- Window-layer regression: a call shell whose embed source cannot instantiate
  (test seam forcing the loader source to a missing component) leaves
  `Loading` and shows the retry affordance; a stalled load (deferred mock)
  settles to `LoadFailed` when the compressed deadline fires.

## Acceptance mapping (issue #728)

| Criterion | Where |
| --- | --- |
| Runtime shipped + real card opens a meeting | packaging step + verify-installed-runtime probe + live smoke |
| Error + recovery, finite deadline | `Loader.onStatusChanged` wiring + embed deadline timer + window cases |
| Installed-package validation extended | verify-installed-runtime.py module + probe checks |
| Regression case for failed instantiation | native-window-test case (call shell group) |

Deviations: none from the reference design (Electron ships its own Chromium;
the native port maps the same "embed must fail loudly" contract onto Qt
runtime shipping and the existing `EmbedState` machine).
