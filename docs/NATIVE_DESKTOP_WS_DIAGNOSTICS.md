# WebSocket activity summary in Server Diagnostics (issue #668)

## Goal

Server Diagnostics gains a `websocket` dataset that lets a user diagnose
connection and event-delivery problems, including after reconnects. The
summary is transport-level only: event metadata and connection state, never
message bodies, tokens or credentials.

## Source of truth

`RealtimeClient` (`src/realtime-client.cpp`) owns the WebSocket lifecycle:
`setState()` funnels every connection-state transition (and emits
`connectionChanged`), the socket `disconnected` handler sees close codes
(4401 auth, 4410 cursor-expired, others = transport drop), `errorOccurred`
sees handshake/socket errors, and `dispatch()` delivers every accepted event
envelope. All accounting is fed from these existing seams - no new timers,
no polling.

The transport is receive-only: acknowledgements and catch-up go over HTTP
(covered by the existing `performance.apiLatency` dataset). The dataset
therefore carries `receiveOnly: true` and no sent-traffic counters; the
reference app has no WebSocket diagnostics surface to clone
(`diagnostics-realtime.lib.ts` only keeps `eventsReceivedCount/lastEventAt/
lastEventType`, which native already mirrors in `realtime.stats`), so this
issue is native-new functionality and the issue body is the contract.

## Dataset shape (`diagnosticsSnapshot()["websocket"]`)

```
{
  "state": "<last state string>",          // mirrored from setState()
  "endpoint": "wss://host/api/workspace/v1/events/ws",  // no query, no token
  "receiveOnly": true,
  "connectedSinceMs": <epoch-ms|null>,     // entered "live" and still live
  "lastConnectedAtMs": <epoch-ms|null>,    // last transition INTO live
  "lastDisconnectedAtMs": <epoch-ms|null>, // last transition OUT of live
  "eventsReceivedCount": <int>,
  "lastEventAt": <epoch-ms|null>,
  "lastEventType": <string|null>,          // type || object_type[.action]
  "receivedByType": { "<type>": <count>, ... },
  "recentEventsPerMinute": <double>,       // accepted events in last 60 s
  "recentFailures": [                      // newest first, cap 20
    { "atMs": <epoch-ms>, "reason": "socket_closed|socket_error",
      "closeCode": <int|0> }, ...],
  "connectionHistory": [                   // newest first, cap 50
    { "atMs": <epoch-ms>, "state": "<state>" }, ...]
}
```

When the controller runs without a `RealtimeClient` the dataset is
`{"state": "disabled", "receiveOnly": true}` so the page always has a shape
to bind.

## Semantics

- **"live" is the connected sentinel.** `connectedSinceMs` is stamped on the
  transition into `live` and cleared on any transition out of it. The
  `lastDisconnectedAtMs` stamp is written only by a failure record (real
  socket close/error) while a connection was live: the normal
  `live <-> catching_up` oscillation during catch-up rounds must not be
  reported as a disconnect.
- **Failure records** are written in the socket `disconnected` handler
  (`reason "socket_closed"`, real close code) and in `errorOccurred`
  (`reason "socket_error"`, `closeCode` carries the
  `QAbstractSocket::SocketError` enum value; the free-form `errorString()`
  is deliberately not recorded because it can echo the request URL).
- **Event type** is the envelope `type` when present (error frames such as
  `EventsCursorExpiredError`), else `object_type` plus `.`-joined `action`.
- **Privacy**: nothing recorded contains payload bodies. The endpoint is
  printed without query parameters; the bearer token travels in the
  WebSocket subprotocol (`bearer.<token>`, `api-client.cpp:322`) and is
  therefore never part of any recorded field.

## Ownership and bounds

`workspace::diagnostics::RealtimeActivityLedger` (native-diagnostics.{h,cpp})
is a value class owned by `RealtimeClient`. Ring caps: 50 state entries,
20 failures, 300 recent event timestamps (all bounds enforced on insert,
oldest dropped). The ledger is not thread-safe; the realtime client is
single-threaded, same as its callers.

`NativeController::diagnosticsSnapshot()` inserts the dataset under
`"websocket"`; `NativeDiagnosticsPage.qml` appends it as extra rows to the
Realtime card's line block - `state · endpoint`, the rounded per-minute event
rate when non-zero, and the most recent failure as `reason (closeCode)` -
suppressed entirely while the state is `disabled` (no dedicated card).

## Tests

- `tests/realtime-client-test.cpp`
  - `websocketSummaryTracksLifecycle`: fixture gateway to `live`, push one
    accepted event, close the socket with 4410 - asserts state mirror,
    `connectedSinceMs`, `lastConnectedAtMs`, by-type count, per-minute rate,
    failure close code, history ordering (newest first).
  - `websocketSummaryEndpointOmitsCredentials`: endpoint equals
    `ws://127.0.0.1:<port>/api/workspace/v1/events/ws` and contains no
    `bearer`/token text.
- `tests/native-controller-diagnostics-test.cpp`
  - `websocketDatasetIsSummaryOnly`: dataset present with the documented key
    set and no payload-bearing keys; disabled shape without realtime.
- `tests/native-window-test.cpp`
  - `diagnosticsRealtimeCardAppendsWebsocketRowsAndSuppressesDisabled`: an
    injected snapshot with a `websocket` dataset renders the state/endpoint,
    rounded per-minute and `reason (closeCode)` rows on the Realtime card;
    the rows vanish when the dataset is absent or the state is `disabled`.
