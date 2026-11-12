# Native desktop call stack: Jitsi embed decision (#160)

Status: decided. Part of #33. Prerequisite for the call-window, incoming-call and
launch/join issues, and the hooks tracked as #86 (power) and #87 (display-capture
source picker).

Decision: **(a) QtWebEngine hosting the Jitsi Meet IFrame External API.**
Option (b), a native libwebrtc room client, is rejected and recorded below.

## Reference contract

All reference line numbers are from `workspace_ui` at
`61dacef78dab2f5d0be76f283bebf49fe29b84af` (read-only reference clone).

The reference embed is a browser engine hosting Jitsi's own IFrame API:

- Vendored `public/vendor/jitsi-external_api.js` loaded on demand by
  `packages/web/src/shared/lib/jitsi-external-api.loader.ts:9-27`. The loader
  injects a `<script>` whose `src` is the app's own base URL
  (`${BASE_URL}vendor/jitsi-external_api.js`) exactly once and resolves when
  `window.JitsiMeetExternalAPI` exists, so `@jitsi/react-sdk`'s
  `fetchExternalApi` never injects a remote script that strict CSP would block.
- `@jitsi/react-sdk` 1.4.4 and `@jitsi/electron-sdk` 10.0.4
  (`packages/web/package.json:34-35`) render and bridge the session.
- Embed config (`packages/web/src/features/jitsi-call/jitsi-call.ui.tsx:18-21`):
  `startWithAudioMuted: true`, `prejoinConfig: { enabled: false }`, plus a
  per-session `startWithVideoMuted`. The shell re-renders must never remount the
  session (`JitsiCallEmbed` is `React.memo`-isolated).
- Required capabilities
  (`packages/web/src/features/jitsi-call/jitsi-call-permissions.lib.ts:4`):
  iframe `allow` policy `camera; microphone; fullscreen; display-capture`.
- Electron main process: `setupScreenSharingMain(mainWindow, APP_DISPLAY_NAME,
  APP_ID)` at `packages/electron/src/main.ts:364-367` — the Jitsi Electron SDK
  owns the desktop source picker and the `getDisplayMedia` response.
  `packages/electron/src/main.ts:978` installs `setPermissionRequestHandler`:
  `display-capture` is answered only by `isAllowedJitsiDisplayCaptureRequest`
  (`:996-1020`, requests must come from a live, non-main, `https:` frame whose
  origin matches an in-subtree frame), everything else against a fixed set
  (`media`, `notifications`, `fullscreen`, `clipboard-read`,
  `clipboard-sanitized-write`, `:951-957`).
- Renderer IPC bridge `packages/electron/src/preload.ts:160-164`
  (`call:start/end/update`) for tray/notification call state.
- Power events: `packages/electron/src/main.ts:810-817` forwards
  `powerMonitor` `suspend`/`resume`/`on-battery`/`on-ac` to the renderer;
  `:1103` exposes `power:getState` battery state.
- States light + dark, ru + en are provided by Jitsi's own chrome under
  option (a); the native client only hosts the view.

## Decision and rationale

**(a) QtWebEngine `WebEngineView` hosting the same Jitsi IFrame External API.**

- Parity: `docs/NATIVE_DESKTOP_PARITY.md` requires a parity-preserving port of
  the reference UI. The reference in-call UI *is* Jitsi's chrome (mute, camera,
  screen share, participants, chat, layouts, device menus, and its own
  light/dark and ru/en translations). A WebEngine view reproduces all of it for
  free and keeps the vendored-script + config-overwrite contract intact; the
  native side only supplies embedding, permission gating and window chrome.
- Direct mapping of every reference contract point: the Electron
  `setPermissionRequestHandler` becomes `QWebEnginePage::featurePermissionRequested`
  in a page subclass; the vendored CSP-safe loader becomes a script served from
  the application's own resource scheme (qrc/custom scheme handler) through
  `QWebEngineUrlSchemeHandler`; `startWithAudioMuted`/`prejoinConfig` overwrites
  pass through unchanged via `JitsiMeetExternalAPI` options injected into the
  hosted page.
- WebRTC correctness: Jitsi's libwebrtc build, SFU (Jicofo/JVB) negotiation,
  stats, quality adaptation and simulcast stay upstream-maintained.

**(b) Native libwebrtc reimplementation — rejected.** A native room client means
re-implementing the Jitsi in-call UI (every control, overlay and translation the
parity matrix grades against), XMPP/signaling, RTP/RTCP handling against JVB,
E2EE and device management. That is outside parity scope and would diverge from
the reference on every Jitsi release. Reconsider only if QtWebEngine is rejected
on packaging grounds; the size data below shows that is costly but bounded.

## Binary-size impact (measured on this host)

Host: Ubuntu 26.04 (resolute), Qt 6.10.2 (`qmake6` reports
`6.10.2+dfsg-7`). Exact apt simulation output for the command named in the
issue (`apt-get -s install qml6-module-qtwebengine libqt6webengine6-data
libqt6webenginecore6* qt6-webengine-dev`):

- `0 upgraded, 27 newly installed, 0 to remove and 7 not upgraded.`
- This apt build does not print an `After this operation, ... disk space` line
  in `-s` mode (recorded honestly); instead the sum of the 27 packages'
  `Installed-Size` metadata is **288,921 KiB ≈ 282 MB** of extra installed disk
  footprint for that full development set (runtime libraries + QML module +
  Chromium binaries + bundled ICU/translations + the `-dev`, `pdf`,
  `positioning` and `webchannel` packages the dev meta-package pulls in).
- The install of the runtime+dev set (`qml6-module-qtwebengine
  libqt6webengine6-data libqt6webenginecore6 libqt6webenginecore6-bin
  qt6-webengine-dev`, `--no-install-recommends`) completed with exit code 0 on
  this host; shipping would exclude the `-dev` packages and reuse
  QtWebEngine's own runtime libraries, so the *runtime* delta is the
  `libqt6webenginecore6` (bundled Chromium + ICU), `libqt6webenginecore6-bin`
  (`QtWebEngineProcess`), `libqt6webenginequick6`, `libqt6webengine6-data` and
  `qml6-module-qtwebengine` subset of the 282 MB above — i.e. the large
  majority of the delta is Chromium itself and cannot shrink for option (a).
  Consequence for packaging/deployment work: `FinalizeLinuxRuntime.cmake` and
  `verify-installed-runtime.py` must grow QtWebEngine copies (including
  `QtWebEngineProcess` and `--webEngineResources` data), and installers need a
  size/dependency plan. This is the known cost of option (a) noted in the issue.

Runtime also creates a per-profile Chromium cache next to the app cache (disk
footprint grows at runtime, separate from the 10 GB Workspace cache setting).

## Per-OS permission matrix

Legend: **proven here** = exercised on this host during the spike (see
"Spike evidence" for what actually passed); **reference** = documented from the
Electron reference or upstream docs, not executed on this host. Linux is the
native acceptance OS; the Windows/macOS columns describe the Electron
reference behavior and Qt's general behavior, and are labeled reference-only.

| Capability | Linux (native target) | Windows | macOS |
| --- | --- | --- | --- |
| Microphone | App gate: `featurePermissionRequested`(`AudioCapture`) in the page subclass (native counterpart of Electron's handler). Device access goes through PulseAudio API served by PipeWire/pipewire-pulse (confirmed running on this host, PipeWire 1.6.2). No mature microphone portal exists, so no OS prompt is expected outside the app gate. Sandbox/first-run: none on bare X; Flatpak packaging later would need portal device rules. Reference for Electron side: `media` in `allowedPermissions`. | Chromium opens WASAPI capture; OS-level control is Settings → Privacy → Microphone (per-app toggle, reference-only). Sole in-app gate is the page's `featurePermissionRequested` equivalent (Electron: handler). No OS prompt bubble in Electron path. | TCC prompt (once per app, `NSMicrophoneUsageDescription` required in the bundle Info.plist, reference-only). Electron gate = same handler as Linux column. |
| Camera | App gate: `VideoCapture` on `featurePermissionRequested`. Device path depends on Chromium's Linux camera stack: with PipeWire the camera portal (`org.freedesktop.portal.Camera.AccessCamera` → `OpenPipeWireRemote`) fronts V4L2 (upstream Chromium behavior, cited reference; whether Ubuntu's QtWebEngine 6.10 takes this path on X11 was **not** proven — this host has no camera device, see Spike evidence). Portal permission store can permanently deny after a dismissed dialog — the native app must surface reset guidance (`flatpak permissions`/`dconf`). | Chromium opens Media Foundation capture; OS control is Settings → Privacy → Camera (reference-only). In-app gate as above. | TCC prompt (once, `NSCameraUsageDescription`, reference-only). Gate as above. |
| Display capture (`getDisplayMedia`) | **Proven on this host (X11): unsupported** — the promise rejected `AbortError: Invalid state` and no permission request reached the Qt 6.10 `permissionRequested` gate (no source chooser). Desktop capture in QtWebEngine as packaged on Ubuntu 26.04 depends on the Wayland + xdg-desktop-portal ScreenCast → PipeWire path (reference: upstream Chromium/portal split; not exercised here). So #87 must be planned for Wayland (portal `PickScreen`/`CreateSession`) and treat X11 as no-screen-share until an upstream path exists. Electron reference: Jitsi electron-sdk owns the picker (`setupScreenSharingMain`, main.ts:364-367) and the gate only admits non-main `https` frames of an active call (main.ts:978, 996-1020) — the native gate must mirror that origin/frame rule. | Electron reference: no OS prompt; `desktopCapturer` enumerates windows, Jitsi's in-app picker (electron-sdk) selects the source. Windows privacy has no display-capture toggle. | Electron reference: Jitsi's in-app picker; macOS Screen Recording privacy (System Settings → Privacy & Security → Screen Recording) must be granted once for capture to return real content (reference-only; TCC applies to the app bundle). |
| Fullscreen | `featurePermissionRequested`(`Fullscreen`) grant for the call view. Reference allow-policy includes `fullscreen`. | Same as Linux column (Chromium-internal, no OS prompt). | Same. |
| Notifications (call invites) | Already handled natively (org.freedesktop.Notifications, see notifications module); calls reuse it. | Windows toast (reference). | macOS notification center (reference). |

The iframe `allow` policy string
(`camera; microphone; fullscreen; display-capture`) only governs the embedded
frame inside the hosted page; under QtWebEngine the authoritative gate is the
permission signal. Qt 6.10 uses `WebEngineView::permissionRequested`
(`QWebEnginePermission` with `permissionType`, `origin`, `grant()`/`deny()` —
the legacy `featurePermissionRequested` is notification-only); the spike used
the 6.10 API. It exposes the requesting origin and frame context — enough to
implement the reference's "Jitsi child frame only" rule. The legacy signal
name remains in the earlier prose as the family term for the gate.

## Audio device routing

- The page (Jitsi chrome) enumerates devices via `mediaDevices.enumerateDevices()`
  and routes by device id; with no explicit choice Chromium follows the OS
  default. On Linux that is PipeWire's current default sink/source managed by
  wireplumber — measured on this host: `pactl info` reports server
  `PulseAudio (on PipeWire 1.6.2)`, default
  `alsa_output.pci-0000_00_04.0.analog-stereo` /
  `alsa_input.pci-0000_00_04.0.analog-stereo`.
- Consequence: the native client must not force devices; it may only expose
  OS-level routing (wireplumber/HW controls). Jitsi's own device menu remains
  the user-facing router (parity). Hot-plug changes are observed by Chromium
  via device-change events; the call window must not restart the session on
  device changes, matching the reference "shell re-render must not remount".
- macOS/Windows (reference-only): CoreAudio/WASAPI device changes follow the
  same page-side model inside Chromium; nothing native to add beyond hosting.
- PipeWire null/silent sources: usable for headless capture tests
  (`pactl load-module module-null-sink` or PipeWire `v4l2loopback`); see spike
  evidence for what this host actually provided.

## Hooks to #86 (power) and #87 (display-capture picker)

- **#86 power.** Reference behavior: Electron forwards suspend/resume/battery
  to the renderer so reconnect happens without waiting for the idle watchdog
  (main.ts:810-817). Native plan: while a call session is open, hold a
  `org.freedesktop.login1` `Inhibit("handle-user-inhibit", ...)` inhibitor and
  watch `PrepareForSleep`; on resume, signal the WebEngine call page to force
  a reconnect (Jitsi's own reconnect path). Battery state goes through the
  existing D-Bus infrastructure. Not implemented in the spike — design hook only.
- **#87 display-capture picker.** Reference: electron-sdk `setupScreenSharingMain`
  shows Jitsi's picker and answers `getDisplayMedia` itself; the permission
  handler additionally restricts capture to an active call's secure child
  frame. Native plan: `featurePermissionRequested(DisplayCapture)` → deny unless
  (1) a call session is open, (2) the requesting frame is non-main-frame,
  `https:` and origin-matches the configured Jitsi domain (same rule as
  reference main.ts:996-1020) → then route to a source picker: QtWebEngine's
  built-in chooser under X11 (seen during the spike, see evidence) or portal
  ScreenCast PickScreen under Wayland (reference), and grant.

## Spike: what was and was NOT empirically proven on this host

Spike module: `packages/native/spike/call`, built only with
`-DWORKSPACE_NATIVE_CALL_SPIKE=ON` (default `OFF`; the default build and all
CTest groups are untouched, and nothing from the spike is registered in CTest).
Spike binary `native-call-spike` ran on this host under `Xvnc :34` + openbox
(1280x800, depth 24, X11), no camera hardware, PipeWire 1.6.2 with an emulated
ALSA stereo input (`pactl` default source
`alsa_input.pci-0000_00_04.0.analog-stereo`), internet reachable
(`https://meet.jit.si/` HTTP 200 from the same host). QtWebEngine ran with
`--no-sandbox --disable-gpu --disable-dev-shm-usage` (headless VM; recorded).

**Proven on this host** (stdout logs + `import -window root` screenshots):

- QtWebEngine 6.10.2 initializes and renders inside a QML `Window`
  (`WebEngineView`) on this host's X display; local page load succeeded
  (`LOAD_OK app://spike/probe.html`) and screenshots show live-rendered DOM.
- Audio capture end to end through the app gate: the page's
  `getUserMedia({audio:true})` raised Qt 6.10's
  `WebEngineView::permissionRequested` (`QWebEnginePermission`,
  `type=1` = MediaAudioCapture) from origin `app://spike/`; the spike granted
  it (native counterpart of Electron's `setPermissionRequestHandler`,
  reference main.ts:978) and the returned track was `state=live`,
  `sampleRate=48000`, default PipeWire device — a live capture track existed
  (content is the host's emulated silent ALSA source; see non-claims).
- Device enumeration: `enumerateDevices()` reported 1 audioinput + 1
  audiooutput; inside the real meet.jit.si page Jitsi's `RTCUtils` logged the
  same 2-device list and ran `configureInitialDevices`.
- Media rendering inside the QML-hosted view: a `canvas.captureStream()`
  `MediaStream` played in `<video>` — `videoWidth=320 videoHeight=180
  readyState=4`, animated frames advancing, pixel spot-check matched. This is
  a **local loopback MediaStream, not a WebRTC remote track**.
- Reference vendored-loader contract on the app's own base url: the spike
  vendored `external_api.js` (see `spike/call/vendor/README.md`) and
  `ensureJitsiExternalApiLoaded()` resolved from
  `app://spike/vendor/jitsi-external_api.js` (`LOADER_OK`), then
  `new JitsiMeetExternalAPI('meet.jit.si', {configOverwrite: {
  startWithAudioMuted, startWithVideoMuted, prejoinConfig off}})` created the
  iframe at the full view size (`IFRAME src=https://meet.jit.si/WSNativeSpike160
  #jitsi_meet_external_api w=1000 h=700`).
- Jitsi's own chrome renders in the QML window when the hosted document is
  `https://meet.jit.si/<room>` (same reference config overwrites passed via
  URL): screenshot shows the full Jitsi UI — toolbar with mic-off and
  camera-off states (matching the config), participant placeholder tile,
  room title — on a fresh room the logs show real signaling progress: XMPP
  moderator flow obtained `focus@auth.meet.jit.si` and JVB codec preference
  init ran inside QtWebEngine.
- Camera absence path: `getUserMedia({video:true})` →
  `NotFoundError: Requested device not found` (host has no camera), recorded.

**Findings that change the matrix (also proven here):**

- `getDisplayMedia` under **X11 with QtWebEngine 6.10**: the call rejected
  with `AbortError: Invalid state` and **no permission request ever reached
  the `permissionRequested` gate** — this Qt build offers no display capture
  on X11 (no source chooser appeared). The #87 hook therefore has to be
  planned against Wayland + xdg-desktop-portal ScreenCast (reference), or an
  upstream/patched capture path; treat X11 screen share as unsupported by
  QtWebEngine as packaged on Ubuntu 26.04.
- A custom **local** scheme page (`app://` registered with
  `LocalScheme | SecureScheme | CorsEnabled | LocalAccessAllowed`, default
  port 443) can host a *vendored* script (loader proof above) but a *remote*
  `<script>` load fails and the `JitsiMeetExternalAPI` iframe does not
  render network content inside that local page (screenshot: broken-frame
  glyph though the iframe element existed at full size). Production embed
  should therefore host the Jitsi document directly (top-level or iframe from
  an https app page), as the reference does from its own https origin. The
  exact Blink scheme-flag combination that would relax this was not chased —
  out of spike scope.

**NOT proven on this host (explicit non-claims):**

- Real microphone/camera quality with physical hardware (host has none; the
  ALSA input is an emulated profile; camera `NotFoundError` was the camera
  result).
- An actual two-party session rendering **remote WebRTC media pixels** in the
  QML window: no second authenticated peer with real media was run against
  the spike, and the joined sessions were audio+video muted with no camera.
  Remote-media rendering is proven only via the local `MediaStream` loopback;
  the Jitsi join evidence covers signaling up to focus/JVB negotiation, not
  decoded remote frames.
- The camera **portal** path (`AccessCamera`), portal ScreenCast display
  capture, and any GNOME portal dialog visuals: the spike ran under
  Xorg/Xvnc; those matrix rows remain reference-only.
- Windows and macOS columns in full (reference-only by the issue's allowance).
- Packaging/installer behavior with QtWebEngine (staging checks, deploy
  script, `QtWebEngineProcess` shipping) — flagged above for the
  implementation issues, not exercised here.
- The exact meet.jit.si lobby/moderator policy per room (first room hit
  "Asking to join meeting…" lobby UI — itself rendered proof — second random
  room got focus immediately); production uses the Workspace-configured Jitsi
  domain, so this is incidental.

Spike is throwaway: no CTest registration, no default-build impact, and the
call-window implementation issues (#native-call series) replace it against
`docs/NATIVE_DESKTOP_CALL_STACK.md`'s decision.
