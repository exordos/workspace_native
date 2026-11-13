# Native desktop call window: embedded Jitsi session shell (#162)

Status: implemented. Part of #33. Depends on the merged research decision
`docs/NATIVE_DESKTOP_CALL_STACK.md` (#160: QtWebEngine hosting the Jitsi Meet
IFrame External API) and on the merged call-launch contract
`docs/NATIVE_DESKTOP_CALL_LAUNCH.md` (#161: `JitsiCallStore`, meet-url parsing
in `src/call/jitsi-call.h`). This issue adds the call *window shell*: expanded
modal, minimize-to-PiP, header controls, session stability and participant
count sharing.

## Reference contract

All measurements are from `workspace_ui` at
`61dacef78dab2f5d0be76f283bebf49fe29b84af` (read-only clone; sha present).

### Expanded / PiP geometry — `jitsi-call-modal-shell.hook.ts:23-52`, `jitsi-call-pip.lib.ts:8-10`

- Insets: 32 px from every viewport edge when both viewport dimensions are
  `>= 640` (`DESKTOP_BREAKPOINT`), otherwise 16 px. The expanded window is
  `x=inset y=inset size=viewport - 2*inset`, clamped to a minimum of
  280x180 (`MIN_WINDOW_WIDTH`/`MIN_WINDOW_HEIGHT`).
- Golden check: viewport 1440x900 -> bounds `x=32 y=32 1376x836`.
- Expanded is a dialog, not a draggable window: the reference renders an
  `AppDialogShell` with a scrim (`APP_DIALOG_OVERLAY_CLASS`) and an inner `Rnd`
  whose bounds are the inset rect; drag is enabled only in PiP mode.
- PiP default bounds: 320x220, anchored 20 px from the viewport bottom-right
  (`x = max(20, vw-320-20)`, `y = max(20, vh-220-20)`). PiP is draggable and
  resizable with the same 280x180 minimum. User PiP bounds are remembered
  across minimize/expand cycles and reset to the default when a new call
  opens.
- Dismiss semantics: `handleOpenChange(false)` (outside click, Escape) only
  *minimizes* when expanded and does nothing when already minimized; closing
  happens only through the close button or the Jitsi `readyToClose` event
  (`handleReadyToClose` -> `onClose`).
- Minimizing while in browser fullscreen exits fullscreen first
  (`toggleMinimized` in the shell hook); the PiP surface always sits above the
  app content (native: separate frameless tool window).

### Header row — `jitsi-call.ui.tsx:163-219`, `jitsi-call-header.lib.ts`

- Row: `flex justify-between items-center` with a bottom hairline
  `border-border-subtle`, padding 8 px horizontal / 6 px vertical
  (`px-2 py-1.5`), 16 px / 8 px at `sm` (`sm:px-4 sm:py-2`). Control gap
  2 px (`gap-0.5`).
- Title: `text-xs` (12 px) semibold truncated, 14 px at `sm`
  (`sm:text-sm`). Rule: `callName` = trimmed `locationName`, else the parsed
  room name; if non-empty -> `"<call.call> - <callName>"`; else if the
  participant count is known -> `<call.callWithParticipants>`; else plain
  `<call.call>`.
- Subtitle: `<call.participants>` (count) at 11 px muted, truncated; hidden
  entirely when the count is unknown.
- Controls, each `rounded-lg` (8 px radius) with hover `bg-bg/50`
  (half-opacity surface) and `p-1.5` (6 px, 8 px at `sm`): fullscreen icon
  28 px, expanded-only, aria label toggling `call.fullscreenEnter` /
  `call.fullscreenExit`; chevron icon 18 px, `call.expand` when minimized /
  `call.minimize` when expanded with title `call.minimizeToWindow`; close
  icon 18 px, `call.closeCall`.
- Window visuals: `rounded-xl` (12 px), `border-border-subtle`,
  `bg-bg-elevated`, `shadow-xl`.

### Session stability + one-time config — `jitsi-call.ui.tsx:15-31`, `jitsi-call-permissions.lib.ts:4`

- `JITSI_CONFIG_OVERWRITE_BASE = { startWithAudioMuted: true, prejoinConfig: {
  enabled: false } }` merged with a per-session `startWithVideoMuted`
  (default `true`, from the call payload).
- The embed is isolated in a `React.memo` component so shell re-renders
  (minimize/expand/resize/fullscreen) never remount it; `jitsi-call.ui.test.tsx`
  asserts the mount count. The native counterpart is a **recreation counter**
  on the controller: the embed is created exactly once per open call and is
  destroyed only when the call closes; a test asserts the counter stays 1
  across every shell toggle.
- The iframe `allow` policy `camera; microphone; fullscreen; display-capture`
  is applied once when the frame is created (`configureJitsiIframe`) so
  minimize never looks like a new session. Native: the same four capabilities
  are granted once per session via `WebEnginePage::featurePermissionRequested`
  (QML `featurePermissionRequested`); shell toggles never revoke a grant.
- Display name: trimmed call display name, else translated `call.participant`
  ("Participant"/"Участник"). In the native payload the store carries
  `locationName` as the display source (see #161); an empty value falls back.

### Participant count — `jitsi-participant-count.hook.ts`, shell hook `handleApiReady`, `shared/config/constants.ts:37,39`

- On API ready: read `getNumberOfParticipants()`, subscribe
  `participantJoined`/`participantLeft` (count), and read
  `getParticipantsInfo()` into the shared store, then start a reconcile poll
  `JITSI_PARTICIPANTS_POLL_MS = 5000` while focused,
  `JITSI_PARTICIPANTS_POLL_UNFOCUSED_MS = 30000` while not.
- Closing resets the count to unknown and clears the shared roster for the
  meeting URL.
- The native client reuses the `JitsiCallStore` payload contract from #161 and
  the shared participant roster exposed to the message card.
- Native mapping (review finding 5, round 3): `workspace::JitsiEmbedBridge`
  (`src/call/native-call-window.{h,cpp}`) is the production live source. The
  embed page's external_api reads arrive through `reportParticipantCount()` /
  `reportRoster()` (the native transport of `getNumberOfParticipants()` /
  `getParticipantsInfo()` until the #160 external_api.js handshake delivers
  them) and the two listeners through `handleParticipantsJoined/Left()`.
  `NativeController` attaches the bridge once at construction, making the
  bridge the shell controller's count provider; the focus-aware timer then
  only reconciles against the bridge, and closing the call resets the live
  count and roster exactly like the reference hook.

### States — `jitsi-call.ui.tsx:221-254`

- `loading`: centered spinner (lg) + `call.apiLoading`.
- `error`: `call.apiLoadFailed` (notice color) + accent button `app.retry`
  that re-runs the API loader.
- Invalid URL (`parseJitsiUrl` and the loose fallback both fail): the shell
  never embeds; native renders the `call.invalidLink` state in the same slot.

### i18n — locales `ru.json`/`en.json` at the reference sha

Keys `call.call`, `call.callWithParticipants`, `call.participants`,
`call.participant`, `call.invalidLink`, `call.apiLoading`,
`call.apiLoadFailed`, `call.fullscreenEnter`, `call.fullscreenExit`,
`call.expand`, `call.minimize`, `call.minimizeToWindow`, `call.closeCall`,
`app.retry`. Every new native `tr()` string gets its Russian text in
`packages/native/translations/workspace_ru.ts` copied from the reference ru
locale (with `{{count}}` mapped to the Qt numerus form `%n`).

## Native design

- `packages/native/src/call/native-call-window.{h,cpp}` —
  `workspace::JitsiCallWindowController`, a QObject state machine with no GUI
  dependency. All geometry, state transitions, permission one-time policy,
  poll cadence and title rules live here so the offscreen `native-native-call`
  QtTest suite proves them:
  - `expandedBounds(viewport)` / `defaultPipBounds(viewport)` pure statics
    replicating the insets/breakpoint/min-size/PiP constants above.
  - Modes `Closed/Expanded/Minimized`; `requestDismiss()` minimizes while
    expanded and is ignored while minimized; `close()`/`readyToClose()` are the
    only ways to end; `toggleMinimize()` exits fullscreen.
  - Embed lifecycle `Loading -> Ready | LoadFailed | InvalidUrl` with an
    injectable embed factory (seam for the WebEngine view) and an
    `embedCreations` counter — the native mapping of the reference mount-count
    contract. Shell toggles never increment it.
  - Participant count: injectable count/info providers; `participantJoined/`
    participantLeft events and a focus-aware `QTimer` reconcile poll owned by
    the controller — started when the embed reaches Ready, cadence 5000 ms
    focused / 30000 ms unfocused, restarted on focus changes and stopped on
    close or load failure.
  - One-time config accessor (`startWithAudioMuted: true`,
    `prejoinConfig.enabled: false`, `startWithVideoMuted` per payload),
    allow-policy string, granted-flags set once at embed creation.
  - Localized text via `tr()` (context `JitsiCallWindowController`) using the
    exact reference en strings; ru catalog entries copied from the reference
    ru locale, guarded by `native-native-locale`.
- `packages/native/qml/NativeCallWindow.qml` — the visual shell: expanded mode
  renders as a scrim overlay inside `Main.qml` (outside click/Escape route to
  `requestDismiss()`), minimized mode renders inside a frameless `Window`
  (tool surface above app content) bound to the controller bounds, with the
  header row and the three states per the geometry above. The embed surface
  is a single `Loader` owned by `Main.qml`, activated as soon as the URL is
  valid (while `Loading` — the WebEngine view's own `loadingChanged` events
  drive `apiLoadSucceeded`/`apiLoadFailed`, so creation is never gated on
  them) and re-parented between the expanded shell and the PiP window; one
  WebEngine session per call survives every toggle (finding 2). Capture and
  fullscreen permissions are granted only to the exact meeting origin via
  `JitsiCallWindowController::isMeetingOrigin` (deny-by-default, finding 3).
  The fullscreen control drives the app window's `FullScreen` visibility and
  the window's external exits sync back through `syncFullscreen` (finding 4).
  Default PiP bounds are mapped through the app window's screen position
  (`defaultPipBounds(viewport, screenOrigin)`); user-placed bounds stay
  screen-absolute (finding 6).
- Wiring: `NativeController` exposes the window controller next to `calls`
  (`JitsiCallStore`); opening a call (card join or call start, #161) opens the
  shell with the active call payload; closing the shell calls
  `calls.closeCall()`.

## Tests

- `native-native-call` (offscreen): expanded bounds at 1440x900 =
  `32,32 1376x836` and the 16 px/mobile + min-size clamps; PiP default
  `1100,660 320x220` at 1440x900; PiP bounds preserved across two minimize
  cycles; dismiss semantics (expanded -> minimize, minimized -> ignored);
  embed recreation count == 1 across toggles; one-time config and
  permissions-not-revoked; participant event + poll cadence 5000/30000;
  title/subtitle rules including the unknown-count hidden subtitle; the three
  embed states with the reference en strings and ru catalog equality.
- `native-native-window` (Xvnc): expanded shell overlay geometry at the real
  1280x800 viewport, PiP window default bounds `940,560 320x220`, and the
  state strings rendered with the ru locale.

## Deviations recorded

- PiP is a native frameless tool window instead of an in-viewport `Rnd` div —
  the Qt mapping of "floating above app content" with a real z-order.
- `{{count}}` interpolation becomes Qt numerus `%n` forms (ru one/few/many,
  en one/other); texts are otherwise byte-identical to the reference locale.
- Screen-share source picking stays in #87 (per #160 the X11 path is not
  supported by QtWebEngine on this host); the allow policy still grants
  `display-capture` once as the reference does.
- The live embed page (external_api.js handshake) is the #160 embed stack; the
  shell proves the mount-count contract with the injectable recreation
  counter instead of a network test. `JitsiEmbedBridge` is the named sink the
  handshake feeds: the bridge holds the shared roster (`liveRoster`), but the
  reference's message-card roster surface has no native counterpart yet (the
  #161 card contract carries no roster), so the roster source ships without
  a consumer; the count source is fully wired into the shell today.
