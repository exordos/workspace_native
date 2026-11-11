# Top overlay banners: connection status and bootstrap error (#146)

Design fidelity case: parity-preserving. The reference layer is reproduced, no
restyle. Reference: workspace_ui 61dacef78dab2f5d0be76f283bebf49fe29b84af,
`layout.ui.tsx:303-326` (prioritized top banner via `resolveLayoutTopBannerKind`),
`layout-connection-banner.lib.ts:8-40` (message priority and severity),
`layout-bootstrap-error-banner.ui.tsx:17-41` (bootstrap banner with Retry).

## Reference contract

- One prioritized connection banner: `app.offline` > `app.rateLimitResume{{s}}`
  > `app.retryIn{{s}}` > `app.reconnecting` > `app.connectionDegraded`;
  severity critical for offline/blocked, warning otherwise.
- A separate bootstrap error banner (`app.loadFailed` + primary Retry) shown
  while the chat shell is rendered after a failed messenger bootstrap; Retry
  re-runs bootstrap (`bootstrapRetryNonce`).
- Tokens: `notice-base` accent (`#3d5eff` dark / `#7087ff` light in the cold
  palette), bottom 2px accent border, accent at 20 percent background,
  `text-primary` label. `role=alert`, assertive live region.

## Native surface (existing state preferred)

- `RealtimeClient::connectionChanged` already feeds `NativeController::syncState`
  (`useRealtime`). Values: `live`, `connecting`, `catching_up`, `retrying`,
  `invalid_data`, `snapshot_required`, `authentication_required`,
  `waiting_for_session`, `stopped`.
- Bootstrap = `reloadSnapshot()` (also wired to `RealtimeClient::snapshotRequired`).
  A snapshot that cannot complete calls `report()`, which cancels the snapshot and
  emits `snapshotFailed`; completion emits `snapshotReloaded`.
- Transport health: `ApiClient::finish` distinguishes a transport-level failure
  (no HTTP response: refused/reset/closed, or the request timed out without
  completing) from an HTTP status. `ApiResult` now carries `transportFailed`
  and `retryAfterSeconds` (parsed `Retry-After`).

## New read-only state on NativeController

- `connectionPhase` (`online` | `reconnecting` | `degraded` | `rate_limited` |
  `offline`), NOTIFY `stateChanged`. Precedence reproduces the reference:
  1. `offline` - the last JSON request failed at the transport level
     (`transportFailed`); any HTTP response (status > 0) proves reachability
     again and clears it. This is the navigator online/offline analogue the
     reference uses; the optional `QNetworkInformation` source was left out
     because its Linux backend is not guaranteed on the supported targets.
  2. `rate_limited` - a JSON request answered HTTP 429; `Retry-After` seconds
     (default 30 when absent) start a wall-clock deadline. The countdown expires
     by itself (a fresh 429 extends it), like the reference retryAfterMs timer;
     unrelated successful responses do not clear it early.
  3. `reconnecting` - `syncState` in `connecting`/`catching_up`/`retrying`/
     `waiting_for_session` (websocket drop with reconnect pending).
  4. `degraded` - `syncState` in `invalid_data`/`snapshot_required`.
  5. `online` otherwise (`live`, poll mode with no realtime client).
- `connectionSeverity`: `critical` for `offline`, `warning` otherwise
  (reference: critical for offline/blocked else warning).
- `connectionRetryInSeconds`: remaining countdown seconds for `rate_limited`
  (0 otherwise); a 1 s timer re-emits `stateChanged` so the label counts down.
- `bootstrapError`: set when a bootstrap fails (`snapshotFailed`) while the
  session is authenticated, cleared by the next successful snapshot
  (`snapshotReloaded`) or by `reset()` (sign-out/logout transitions).
- `retryBootstrap()` (Q_INVOKABLE): re-runs the existing bootstrap path
  (`reloadSnapshot()`), the counterpart of the reference Retry nonce.

## QML (Main.qml)

A `topBannerOverlay` Column is added as the last normal root child, anchored
just below the header (`topBar.bottom`) so it renders above the sidebar/chat
split and never covers the header (asserted by the window test). It contains
the bootstrap error banner first (`bootstrapErrorBanner`, text is the reported
error with a fallback caption and a `bootstrapRetryButton` primary Retry wired
to `controller.retryBootstrap()`), then the connection banner
(`connectionBanner`). Both reproduce the reference strip: accent at alpha 0.2
background, 2px bottom accent border at alpha 0.6, `text-primary` label,
`Accessible.role: AlertMessage` (Qt Quick exposes alert semantics through
`Accessible.role`; there is no separate aria-live property - recorded
deviation). Warning uses the `notice-base` token, critical uses `danger`
(recorded mapping decision: the reference critical severity color resolves to
the critical token of the active palette, native `danger`). Banner text keys
mirror `app.offline`, `app.rateLimitResume`, `app.reconnecting`,
`app.connectionDegraded`, `app.loadFailed`, `app.retry` and are added to the
Russian catalog together with every new user-visible string.

## Tests

- `native-native-controller`:
  - transport drop flips `connectionPhase` to `offline` (banner text source)
    and a successful response clears it;
  - realtime websocket drop flips the phase to `reconnecting` and auto-reconnect
    clears it (existing `RealtimeGateway` harness);
  - a 429 with `Retry-After` shows `rate_limited` with a positive countdown that
    clears when the countdown expires;
  - a malformed bootstrap catalog sets `bootstrapError`; `retryBootstrap()`
    against a healthy server clears the error and the flag.
- `native-native-window`: the overlay renders above the split view below the
  header with the critical (`danger`) and warning (`notice-base`) tint tokens,
  the header stays clickable while a banner is up, and the bootstrap banner's
  Retry click recovers the shell.
