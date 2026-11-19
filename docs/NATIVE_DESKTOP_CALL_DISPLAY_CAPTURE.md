# Call display-capture gate and share-source picker (issue #87)

Shell-side screen sharing for the embedded Jitsi call: a display-capture
request is granted only to the live secure Jitsi frame, and share-source
picking goes through the native platform picker. This document measures the
pinned Reference contract, defines the native counterpart, and states exactly
what is and is not exercisable on the current Qt runtime.

Reference measurements were taken in the pinned upstream UI checkout at
commit `61dacef78dab2f5d0be76f283bebf49fe29b84af` (verified present;
`packages/native/src/call/jitsi-call.h` already cites the same pin).

Figma token unavailable - mockups not consulted.

## Measured Reference contract (workspace_ui 61dacef78)

### Renderer side: `packages/web/src/features/jitsi-call/jitsi-call-screen-sharing.lib.ts`

- `:8` - `const configuredApis = new WeakSet<object>();`. One hook per Jitsi
  API object: `setupJitsiScreenSharing(api)` at `:10-13` returns early when
  not running under Electron or when the API object was configured before,
  so duplicate `ready` callbacks cannot duplicate native subscriptions.
- `:14-20` - `setupScreenSharingRender(api)` runs inside `try`/`catch`. On a
  throw (the documented case: the SDK preload bridge is missing) the API
  object is removed from the guard set and the failure is reported through
  `reportUnexpectedError("jitsi:screenSharingSetup", error)` at `:19`. A
  missing bridge must not break the active call itself.

### Shell side: `packages/electron/src/main.ts`

- `:951-957` - static `allowedPermissions` set: `media`, `notifications`,
  `fullscreen`, `clipboard-read`, `clipboard-sanitized-write`.
- `:975-984` - `setPermissionRequestHandler`: the `display-capture`
  permission alone is routed to `isAllowedJitsiDisplayCaptureRequest`
  (`:977-980`); every other permission is decided by the static set (`:982`).
- `:986-988` - `setPermissionCheckHandler` answers only from the static set,
  so synchronous checks can never grant display capture.
- `:996-1020` - `isAllowedJitsiDisplayCaptureRequest(webContents, details)`
  semantics, in order:
  1. `:1003-1005` deny unless a `mainWindow` exists, the requesting
     `webContents` is that window's, and `details.isMainFrame` is false -
     the shell's main frame never obtains capture.
  2. `:1007-1009` parse `details.requestingUrl`; deny unless the protocol is
     exactly `https:`.
  3. `:1011-1016` allow only when the frames subtree contains a frame that
     is not the main frame, is not detached, and whose URL origin
     (`getUrlOrigin`, `:1022-1028`, WHATWG origin or null on parse failure)
     equals the requesting URL's origin.
  4. `:1017-1019` an unparseable requesting URL denies.
- `:367` - `setupScreenSharingMain(mainWindow, APP_DISPLAY_NAME, APP_ID)` is
  wired at window creation before the renderer loads (comment `:364-366`:
  the SDK owns the desktop source picker and the `getDisplayMedia`
  response), so Jitsi can use the bridge from its first iframe session.

Negative cases pinned by the reference: main frame, http origin, origin
outside the frame subtree, detached frame.

## Native counterpart

### Pure predicate (`src/call/display-capture-policy.{h,cpp}`)

`workspace::isAllowedDisplayCapture(isMainFrame, requestingUrl, frames)`
mirrors `main.ts:996-1020` one-to-one without any WebEngine dependency:

| Input                                     | Result |
| ----------------------------------------- | ------ |
| `isMainFrame == true`                     | deny   |
| unparseable requesting URL                | deny   |
| scheme not `https`                        | deny   |
| requesting origin matches no live frame   | deny   |
| only matching frame is detached           | deny   |
| https requesting origin == live frame URL | allow  |

`DisplayCaptureFrame { url, detached }` models one child-frame entry of the
reference frames subtree, and `displayCaptureOrigin()` reproduces the WHATWG
origin used by `getUrlOrigin` (lowercase scheme/host, default port dropped,
empty string for unparseable URLs so a null origin can never match).

### Call-surface seam (`JitsiCallWindowController`)

Qt 6.10.2 delivers capture gates as `permissionRequested` with a
`QWebEnginePermission` that exposes only `origin` and `permissionType`
(`/usr/include/x86_64-linux-gnu/qt6/QtWebEngineCore/qwebenginepermission.h:30-31,47-59`;
`DesktopVideoCapture = 4`). Unlike Electron it carries no frame identity, so
the native shell models the frame subtree itself: while a call is open and
its embed is Ready, the live Jitsi meeting document is the one live child
frame; every other requesting origin is modeled as a foreign (main-frame)
request. The controller exposes
`isDisplayCaptureRequestAllowed(isMainFrame, requestingUrl)` as the tested
decision point, and `NativeCallWebEngineView.qml` routes
`DesktopVideoCapture` through it (an origin match is itself the proof that
the request did not come from the embedding document). A request flagged as
a main-frame request is denied even when its origin matches.

The meeting origin itself follows the existing single-session grant model:
`close()` returns the controller to `Closed`, which removes the live frame
entry, so a finished or never-loaded session cannot hold capture either.

### Share-source picker (`src/call/screen-share-source-picker.{h,cpp}`)

`ScreenShareSourcePicker` selects the share source through the XDG desktop
portal `org.freedesktop.portal.Screenshot.PictureUri` on the session bus
(service `org.freedesktop.portal.Desktop`, object
`/org/freedesktop/portal/desktop`), the platform analogue of the Electron
source picker wired at `main.ts:367`. When no portal is registered on the
session bus, the picker falls back deterministically to Qt's own screen
model (`QGuiApplication::screens()`), publishing one source per screen.

D-Bus notes honoured by the implementation: a value received inside a
`QVariant` can arrive wrapped in `QDBusVariant` and must be unwrapped before
use, and list members inside a variant arrive as a `QDBusArgument` stream
(`beginArray`/`atEnd`/`extract`), while `QDBusMessage` has no per-message
timeout so the blocking call uses `QDBus::Block` with milliseconds.

The portal transport and the portal-availability probe are injectable seams
(the established `std::function` seam pattern of the call surface), so the
portal success/failure paths and the deterministic fallback are all
exercised in QtTest without a portal daemon on the test display.

### Honest scope limit

Qt 6.10.2 gives the application the grant decision
(`QWebEnginePermission::grant()`/`deny()`) but no public API to choose which
screen or window the granted `getDisplayMedia` session captures, so the
portal/picker result cannot yet be fed into the WebEngine capture pipeline
end to end. This issue therefore delivers the security gate and the tested
picker contract plus the narrow controller seam a future capture pipeline
will call; no placeholder UI is invented. When Qt exposes source selection,
the picker result is the value that gets passed through.

## Tests

`native-native-controller` group (packages/native/tests/native-controller-test.cpp):

- pure predicate: main frame denies; http denies; unknown origin denies;
  detached frame denies; live https child-frame origin match allows;
  origin normalization (default port, host case).
- integration: a live call window (open -> embed Ready) grants the live
  meeting frame origin, and a mock main frame carrying the very same meeting
  origin cannot obtain capture; an http meeting URL and an unknown origin
  are denied; after close the live frame entry is gone.
- picker: with no portal the fallback deterministically publishes the Qt
  screen-model sources; the injected portal bridge reports a source or a
  failure; the portal `Result` parsing unwraps `QDBusVariant`.

`native-call` group: a source-contract guard that the shipped embed view
routes `DesktopVideoCapture` through the controller gate instead of the
previous blanket deny, matching the existing shipped-source guard pattern.

## Not established

Live portal dialog acceptance on a real desktop host (the portal paths here
are proven over injected transports), actual `getDisplayMedia` source
selection inside the meeting page (blocked by the Qt API gap above), and
Wayland/other-platform picker behavior.
