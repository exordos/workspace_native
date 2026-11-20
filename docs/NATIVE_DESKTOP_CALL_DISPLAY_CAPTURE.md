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
`QWebEnginePermission` that exposes only `origin`, `permissionType`, `state`
and `isValid`
(`/usr/include/x86_64-linux-gnu/qt6/QtWebEngineCore/qwebenginepermission.h:30-33,70-71`;
`DesktopVideoCapture = 4`). That is the complete trustworthy-signal inventory
this runtime hands the decision point: there is no frame object, no
main-frame flag and no frame-tree query anywhere on the QML/C++ permission
surface, while this view loads the meeting URL as its own main document.
Consequently the reference's positive proof ("not the main frame, and the
requesting origin belongs to a live secure child frame", `main.ts:1003-1016`)
can never be established natively: a request from the meeting main frame
carries exactly the meeting origin, so an origin match cannot separate the
allowed case from the denied case.

Round 2 therefore states the exact trustworthy signal - none exists on this
runtime - and makes the production policy FAIL CLOSED:
`JitsiCallWindowController::isDisplayCaptureRequestTrustworthy(requestingUrl)`
is the decision point `NativeCallWebEngineView.qml` consults for
`DesktopVideoCapture`, and it denies unconditionally until a Qt version
exposes real frame identity. The first round's wiring (hardcoding
`isMainFrame=false` into the modeled child-frame check) fabricated the very
identity the reference requires and granted main-frame requests; that is what
review findings 1 and 2 condemned, and the shipped-source guard now forbids
the view from ever hardcoding a frame flag again.

The modeled gate stays as the unit-tested policy reference:
`isDisplayCaptureRequestAllowed(isMainFrame, requestingUrl)` models the
reference subtree (while a call is open and Ready, the live Jitsi document is
the one live child frame; `close()` removes the entry) and is the function a
future trustworthy frame identity feeds into. Until then no production
request reaches it.

### Share-source picker (`src/call/screen-share-source-picker.{h,cpp}`)

`ScreenShareSourcePicker` selects the share source through the XDG desktop
portal LIVE source-selection contract
`org.freedesktop.portal.ScreenCast` on the session bus (service
`org.freedesktop.portal.Desktop`, object `/org/freedesktop/portal/desktop`),
the platform analogue of the Electron source picker wired at `main.ts:367`.
The picker uses three asynchronous methods. Each returns a Request object
and the picker subscribes to its `Response` before sending the call, using
a distinct `handle_token`.

1. `CreateSession(a{sv})` supplies a unique `session_handle_token`. Its
   successful response returns the session object path in the string-valued
   `session_handle`.
2. `SelectSources(o session, a{sv})` requests one monitor or window with
   `types=3`.
3. `Start(o session, s parent_window, a{sv})` opens the desktop chooser.
   On success, `streams` has D-Bus type `a(ua{sv})`. The picker publishes
   the selected PipeWire `nodeId`, its `properties`, and the `sessionHandle`
   needed to obtain the PipeWire file descriptor with `OpenPipeWireRemote`.
   It keeps the session open until the caller releases the source, replaces
   it with a new selection, or destroys the picker.

Cancelling the chooser reports cancellation and closes the pending session.
An error or timeout also closes any pending request and session.
[ScreenCast portal interface](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.ScreenCast.html)

When no portal is registered on the session bus, the picker falls back
deterministically to Qt's own screen model (`QGuiApplication::screens()`),
publishing one streamable monitor source per screen.

Each exchange is tracked by its own serial (finding 3). The exchange owns
its timeout `QTimer` and the portal Request subscriptions its steps opened,
and `finishCurrentExchange()` stops the timer and disconnects every
subscription before completion is published, so neither a stale timer nor a
late `Response` of a finished exchange can fail or fake a later one;
while an exchange is in flight the slot's only live subscriptions are that
exchange's own steps, which answer strictly in call order.

D-Bus notes honoured by the implementation: a value received inside a
`QVariant` can arrive wrapped in `QDBusVariant` and must be unwrapped before
use, and list members inside a variant arrive as a `QDBusArgument` stream
(`beginArray`/`atEnd`/`extract`), while `QDBusMessage` has no per-message
timeout so the deadline rides the connection call and the per-exchange
timer.

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
- frame trust (round 2, findings 1/2):
  `callWindowDisplayCaptureGateFailsClosedWithoutFrameIdentity` - with the
  embed Ready, the very meeting origin is still denied because the
  requesting frame's identity cannot be verified.
- picker: with no portal the fallback deterministically publishes the Qt
  screen-model sources; the injected portal bridge reports a source or a
  failure;
  `shareSourcePickerDrivesThePortalScreenCastContract` proves the whole
  CreateSession/SelectSources/Start exchange against a session-bus fixture. It
  checks the typed stream tuple, explicit release, and cancellation at Start;
  `shareSourcePickerCancelsTheCompletedExchangeTimerAndSubscription`
  reproduces finding 3 (first exchange completes, second exchange starts,
  the first exchange's timeout deadline elapses inside the second exchange:
  the second is still decided only by its own portal reply);
  `shareSourcePickerRejectsMalformedOrMissingPortalStreams` rejects missing
  results and object-path lists.

`native-call` group: a source-contract guard that the shipped embed view
routes `DesktopVideoCapture` through the controller's frame-trust gate and
never hardcodes a frame flag into the modeled gate (round 2), matching the
existing shipped-source guard pattern.

## Linux desktop verification

A nested Sway desktop with the wlr portal backend, a monitor chooser and
PipeWire exercised the production picker on the session bus. Esc cancelled
the first chooser. Selecting the test monitor in the second returned a
session and a node. Calling `OpenPipeWireRemote` on that session returned
a usable file descriptor.

Selecting a source for the embedded meeting page still requires Qt to
expose a trustworthy requesting frame and a way to bind the chosen stream.
Windows and macOS use other capture APIs and were not exercised here.
