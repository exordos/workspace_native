# Diagnostics page: connection dashboard (status pill, nine cards, server ping)

Issue: #50 (part of #49). Reference contract measured from `workspace_ui` at
sha `61dacef78dab2f5d0be76f283bebf49fe29b84af`. Figma token unavailable -
mockups not consulted; the reference code is the oracle.

## Reference contract (measured)

### Page shell — `packages/web/src/pages/logs/logs-page.ui.tsx`

- Header (`logs-page.ui.tsx:268-307`): `border-b border-border-subtle bg-card-bg
  px-3 py-2.5` (12px/10px padding, bottom border), title
  `text-base font-semibold text-text-primary` (16px semibold, :271-273), hint
  `mt-0.5 text-xs text-text-muted` (2px top gap, 12px, :274).
- Header action buttons (`:277-304`): View logs / Refresh / Copy connection
  report / Copy snapshot, each `h-8 rounded-md border border-border-subtle
  bg-card-bg px-2.5 text-xs` (32px tall, radius 6, padding 10px, 12px text),
  row `gap-1` (4px). No settings navigation button in the header
  (`logs-page.test.tsx:120-124`).
- Content column (`:309-310`): scroll area `p-2` (8px) with an inner column
  `flex flex-col gap-2` (8px gaps): memory section, overview, advanced
  snapshot.
- Collapsible advanced snapshot (`logs-page.ui.tsx:325-334`):
  `details/summary` rounded-lg border card, summary text-xs font-medium, pre
  `mt-2 max-h-52 overflow-auto rounded bg-bg p-1.5 text-[11px] leading-snug`
  (max height 208px, 11px mono-ish text, 6px padding).
- Copy feedback (`:232-236`): the copy state (success/error) resets to idle
  after 2000 ms; the copy label swaps to `settings.diagnosticsCopySuccess` /
  `settings.diagnosticsCopyError` meanwhile (`:244-248`).
- Ping feedback (`:238-242`): a finished ping (success/error) resets to idle
  after 3000 ms; while loading the button is disabled and keeps the idle
  label.
- Copy actions: `handleCopySnapshot` (`:206-210`) writes the full snapshot as
  `JSON.stringify(snapshot, null, 2)`; `handleCopyConnectionReport`
  (`:212-217`) writes the connection-report subset, same pretty-print.
- `handlePingServer` (`:219-230`): sets loading, runs
  `probeApiTransportWithLatency()`, records `latencyMs` + ok/error. Note: the
  reference probe is a post-Zulip-removal stub that always resolves
  `{ ok: false, latencyMs: 0, unsupported: true }`
  (`shared/lib/network-transport-probe.lib.ts:24-31`), so the web build can
  only show "Unreachable". Native implements a real probe (deviation below).

### Overview section — `packages/web/src/pages/logs/logs-page-overview.ui.tsx`

- Status pill (`:41-49`): `inline-flex items-center gap-1.5 rounded-full
  border border-border-subtle bg-bg px-2.5 py-1 text-xs font-medium
  text-text-primary` (radius = full, padding 10px/4px, 6px dot gap) with an
  8x8 round dot (`h-2 w-2 rounded-full`). `STATUS_DOT` map (`:13-17`):
  healthy `bg-indicator-green` `#26c038`, degraded `bg-indicator-yellow`
  `#ffd633`, offline `bg-indicator-red` `#f04c4c` (app.styles.css:336,339,342
  — identical to the shipped `qml/NativeTheme.qml` tokens). Label keys
  `STATUS_LABEL_KEY` (`:19-23`): `settings.diagnosticsStatusHealthy/
  Degraded/Offline`.
- Ping button (`:50-63`): `h-7 rounded-md border border-border-subtle
  bg-card-bg px-2 text-xs`, `disabled:opacity-50` while loading. Label:
  loading → `settings.diagnosticsPingServer`; success with ms →
  `settings.diagnosticsPingSuccess` "Reachable ({{ms}} ms)"; error →
  `settings.diagnosticsPingFailed` "Unreachable"; idle → Ping server.
- Card grid (`:66`): `grid gap-2 sm:grid-cols-2 xl:grid-cols-3` — 1 column
  below 640px, 2 columns from 640px, 3 columns from 1280px, 8px gaps.
- `DiagnosticCard` (`:218-223`): `rounded-lg border border-border-subtle
  bg-card-bg p-3` (radius 8, padding 12), title `mb-1.5 text-xs font-medium`
  (6px gap, 12px medium), body `space-y-0.5 text-[11px] leading-snug`
  (2px line gaps, 11px, line-height 1.375).
- `LogCountBadge` (`:230-233`): `rounded bg-bg px-1.5 py-0.5 font-mono
  uppercase text-text-muted` inside a `flex flex-wrap gap-1.5` row; the Log
  counts card also carries a `View logs` button `h-7 rounded-md bg-bg px-2
  text-[11px]` (`:143-149`) that opens the logs modal
  (`logs-page.test.tsx:182-190`).
- The nine cards (`:67-207`) in order: Connection (phase / failure reason /
  reconnect attempts), Realtime (events received / online·tab-visible line /
  last event age), Session (login or `diagnosticsNoSession` / origin /
  workspace session count / session remaining minutes), Runtime (detected
  runtime / app version / brand name), Notifications (permission /
  permission-caption), Log counts (error/warn/info/debug badges + View logs),
  Performance (Web Vitals list or `diagnosticsPerformanceEmpty`, plus the API
  latency line when samples exist), Local cache (unavailable text, or cached
  messages / owner meta / folders), Stores (streams / conversations / users).
- Behaviour contract cases (`logs-page.test.tsx`): no settings button in the
  header (`:120-124`), overview cards render (`:197-202`), View logs opens
  the modal (`:182-190`).

### Status math — `packages/web/src/pages/logs/diagnostics-collect.lib.ts`

- `RECENT_ERROR_LIMIT = 8`, `RECENT_ERROR_WINDOW_MS = 900000` (`:153-154`).
- `resolveDiagnosticsOverallStatus(connection, online, recentErrorCount)`
  (`:157-175`): `offline` if `connection.phase === "offline"` or the browser
  is offline; `degraded` if the phase is `degraded`/`blocked`/`rate_limited`/
  `connecting` or any error entry exists in the last 15 minutes; `healthy`
  otherwise. Recent errors are `level === "error"` entries with a timestamp
  inside the window, newest first, capped at 8 (`:177-182`).
- `buildConnectionReportSnapshot` (`:328-345`): copy-report subset =
  `collectedAt`, `overallStatus`, `connection`, `rateLimitBlockedUntil`,
  `realtime`, `workspaceSession`, `recentErrors` (timestamp/scope/message/
  data).
- API latency summary (`shared/lib/diagnostics-api-latency.lib.ts:20-24,
  46-63`): sorted samples; `percentile(sorted, p) = sorted[min(n-1,
  floor(n*p))]`; `medianMs = percentile(0.5)`, `p95Ms = percentile(0.95)`,
  `sampleCount`, top-3 `slowest`.

### Entry point — `packages/web/src/widgets/right-panel/right-panel-user-menu.ui.tsx`

- `openDiagnostics` (`:182-184`) navigates to `/settings/logs`; the
  `PanelMenuRow` (`:372-379`, label `settings.connectionDiagnostics`, icon
  `lab_profile`, chevron) sits directly below the "App version" (About) row
  (`:361-371`).

### i18n — `packages/web/src/i18n/locales/en.json` / `ru.json`

Keys used by this screen (en/ru values quoted in the translation table
below): `settings.connectionDiagnostics`, `settings.diagnosticsHint`,
`settings.diagnosticsOverview`, `settings.diagnosticsStatusHealthy/
Degraded/Offline`, `settings.diagnosticsPingServer/PingSuccess/PingFailed`,
the nine card titles `settings.diagnosticsConnection/Realtime/Session/
Runtime/Notifications/LogCounts/Performance/Cache/Stores`,
`settings.diagnosticsNoSession`, `settings.diagnosticsReconnectAttempt`,
`settings.diagnosticsEventsReceived`, `settings.diagnosticsOffline`,
`settings.diagnosticsTabVisible/TabHidden`, `settings.diagnosticsLastEvent`,
`settings.diagnosticsWorkspaceSessionsCount`, `settings.diagnosticsSessionRemaining`,
`settings.diagnosticsNotificationPermission`, `settings.diagnosticsPerformanceEmpty`,
`settings.diagnosticsApiLatency`, `settings.diagnosticsCacheUnavailable`,
`settings.diagnosticsCacheChatListYes/No`, `settings.diagnosticsCacheFolders`,
`settings.diagnosticsStoresStreams/Dms/Users`,
`settings.diagnosticsAdvancedSnapshot`, `settings.diagnosticsCopySnapshot/
CopySuccess/CopyError/CopyConnectionReport`, `settings.logsRefresh`,
`settings.logsViewModal`, `presence.online`.

### Electron shell

No dedicated shell behaviour; only the deeplink allowlist admits logs routes
(`packages/electron/src/deeplink-route.lib.ts:51-57` — the single-segment
`/settings` and `/logs` entries).

## Native design

### New files

- `packages/native/qml/NativeDiagnosticsPage.qml` — the page shell + overview
  (mounted in Main.qml beside the other section pages). `FocusScope` root,
  `objectName: "nativeDiagnosticsPage"`, `pragma ComponentBehavior: Bound`,
  Repeater delegates with `required property var modelData`, layout-only
  Rows (no left/right anchors inside them), tinted glyphs only where the
  reference has them (none on this screen).
- `packages/native/src/native-diagnostics.{h,cpp}` — the collector's pure
  core in `namespace workspace::diagnostics`: `recentErrorLimit()=8`,
  `recentErrorWindowMs()=900000`, `resolveOverallStatus(phase, online,
  recentErrorCount)` (same three-branch math), `summarizeLatency(samples)`
  (same percentile math), and `parseSessionLog(text, nowMs)` which reads the
  `NativeLogging` line format `<ISO-8601> <level>: <message>` (levels
  `debug|info|warning|critical|fatal`, docs/NATIVE_DESKTOP_DIAGNOSTICS.md)
  into by-level counts (native `warning` → web `warn`, `critical`/`fatal` →
  `error`) plus recent-error entries in the 15-minute window capped at 8.

### Controller surface (`src/native-controller.cpp`)

- `showSection("diagnostics")`: new `ContentPage::Diagnostics` branch, same
  shape as the `licenses` branch (close other content pages, set
  `m_section = "diagnostics"`, emit `stateChanged`). Route id is
  `diagnostics`.
- Deep links: the whitelist already admits `/logs` and `/settings`
  (`src/native-deeplink.cpp:69-73`); the two-segment logs variants are
  extension-guarded into the same patterns and the route dispatcher gains
  `logsRoutePattern` (`^(?:/org/([^/]+))?/(?:logs|settings/logs)/?(?:[?#].*)?$`)
  with the same origin guard as `/calls` and `/licenses`, calling
  `showSection("diagnostics")`. This makes `ew://open/logs` and
  `ew://open/org/<org>/settings/logs` resolve to the page (acceptance).
- Entry wiring in `qml/Main.qml`: a "Diagnostics" `NativeButton`
  (`objectName: "diagnosticsAction"`) directly after the About
  (`aboutAction`) button in the account profile popup — mirroring the
  reference user menu order (Diagnostics below App version,
  right-panel-user-menu.ui.tsx:361-379) — that closes the popup and calls
  `controller.showSection("diagnostics")`.
- New counters feeding the dataset (all live controller state, no fakes):
  - `m_realtimeEventsReceived`, `m_realtimeLastEventType`,
    `m_realtimeLastEventAtMs` — incremented in the `useRealtime`
    `eventReceived` handler (payload `kind` is the event type), mirroring
    `diagnostics-realtime.lib.ts:16-20`.
  - `m_realtimeReconnectAttempts` — incremented on every realtime
    `connectionChanged("retrying")` transition (the reconnect backoff retry
    is one attempt), mirroring `connection.reconnectAttempt`.
  - API latency samples — the controller `request()` wrapper times each
    dispatch-to-callback round on a monotonic clock and keeps the last 200
    completed HTTP samples (status > 0) in a bounded queue. This is the
    "API latency from HTTP response timing" source.
  - `Session::accessRemainingMs()` — new const getter exposing
    `m_accessExpiresAt - m_clock.elapsed()` (already computed internally at
    `src/session.cpp:413-414`), feeding `sessionRemainingMs`.
  - `useLogging(NativeLogging *)` — the page reads the rotating log file
    installed by issue #77 (`installNativeLogHandler`, `src/main.cpp`).
- Dataset QML surface: `Q_INVOKABLE QVariantMap diagnosticsSnapshot()` (the
  dataset object), `Q_INVOKABLE QString diagnosticsSnapshotJson()` (the same
  object pretty-printed with 2-space indent like `JSON.stringify(x, null, 2)`),
  `Q_INVOKABLE QString diagnosticsConnectionReportJson()` (the
  `buildConnectionReportSnapshot` subset), and `Q_INVOKABLE QString
  diagnosticsOverallStatus()` = `resolveOverallStatus(connectionPhase(),
  !m_transportOffline, recentErrorCount)`.
- Ping: `Q_INVOKABLE void pingServer()` + `signal pingFinished(bool ok, int
  latencyMs)`. A click runs one authenticated `GET messenger + "/streams/"`
  (page_limit 1) through the normal `request()` path and reports elapsed ms
  and `status > 0` (an HTTP response proves reachability; a transport error
  is "Unreachable"). Concurrent pings collapse to the in-flight one.

### Card-to-native-source map (parity-preserving design)

| Card | Reference source | Native source (real only, never fake) |
| --- | --- | --- |
| Connection | connection-health phase/failureReason/reconnectAttempt | `connectionPhase()` (`offline`/`rate_limited`/`reconnecting`/`degraded`/`online`), `connectionRetryInSeconds()` as failure sub-line when rate-limited, `m_realtimeReconnectAttempts` |
| Realtime | diagnostics-realtime stats + online + tab visible | event counters above, `!m_transportOffline`, `m_windowVisible` (closest native concept to tab visibility, per issue), windowFocused presence |
| Session | workspace auth sessions + session remaining | `Session::loginName()`, API server origin, `accessRemainingMs()` (sub-line omitted when <=0/unknown) |
| Runtime | detectedRuntime/appVersion/brand | `"Qt " + QT_VERSION_STR` desktop runtime, `QCoreApplication::applicationVersion()`, `QCoreApplication::organizationName() + " Workspace"` brand line |
| Notifications | Notification.permission | native has no OS permission query today → the reference `unsupported` value (a legal reference state), caption line retained |
| Log counts | in-memory logger history byLevel | `NativeLogging` rotating file parsed by `parseSessionLog` (the native session log; no file → all-zero real counts) |
| Performance | Web Vitals + api latency logs | no Vitals equivalent → reference empty text (native wording); API latency from the new response-timing samples via `summarizeLatency` |
| Local cache | IndexedDB owner footprint | `NativeCacheService::available()`/`stats()` (entries, bytes, owner present); unavailable → native unavailable text |
| Stores | messenger store counts | `streams().size()`, direct-chat conversations, user-directory entries, folders |
| Memory block | diagnostics:getMemorySnapshot | `NativeLogging::memorySnapshot()` (already accepted, docs/NATIVE_DESKTOP_DIAGNOSTICS.md) |

Web-only fields declared out of dataset: service worker controller, browser
viewport/screen/userAgent/languages, navigator locales (native equivalents
live in preferences and are reported as locale/theme fields), tab
visibility (mapped to window visibility), Web Vitals, IndexedDB partition
keys, Zulip rate-limit gate (`rateLimitBlockedUntil` mirrors the native
`m_rateLimitedUntilMs` instead).

### Deviations from the reference (recorded)

1. Native ping is a real reachability probe (HTTP answer → latency); the
   reference probe is a dead stub that always reports "Unreachable". The
   issue acceptance explicitly requires ms feedback, so the native probe
   implements the reference's *intended* behaviour.
2. The reference whitelist admits only single-segment `/settings` and
   `/logs`; the issue acceptance requires `ew://open/org/acme/settings/logs`
   (the actual web route) to resolve, so the two-segment variants are added
   to the native whitelist.
3. Log counts read the on-disk rotating log (max 1 MiB + rotated), not an
   in-memory ring: Qt has no logger ring buffer, and issue #77 established
   the file as the native log surface. Scopes/runtimes lists stay empty
   (native log lines carry no scope).
4. The "View logs" affordance (header button + Log-counts card button that
   open the logs modal) is out of this issue's scope: the native logs modal
   is a separate page of #49. The Log counts card renders the real badges
   without the button; the header keeps Refresh/Copy buttons only.
5. Copy feedback and ping feedback reproduce the reference timings exactly
   (2000 ms copy reset, 3000 ms ping reset, disabled ping while loading).
6. `diagnosticsCacheUnavailable` ("IndexedDB unavailable") would be a lie in
   Qt; the native string reuses the key with honest wording ("Local cache
   unavailable"). Same for `diagnosticsPerformanceEmpty` (no "Web Vitals" in
   Qt → "No performance metrics recorded yet").

### States and i18n

Light + dark, ru + en (four-row window case). Every visible string uses
`qsTr` with the reference English source values where they exist and the
Russian values from `ru.json` (`{{ms}}`-style placeholders become `%1`);
new native-only strings are added to
`packages/native/translations/workspace_ru.ts` so the native-native-locale
catalog guard passes.

### Tests

- `native-native-controller` (pure math + dataset): `resolveOverallStatus`
  truth table (offline phase / offline network / degraded / blocked /
  rate_limited / connecting / reconnecting / recent errors / healthy),
  `parseSessionLog` (by-level mapping, 15-min cutoff, cap 8, newest-first),
  `summarizeLatency` percentile reproduction, `diagnosticsSnapshot()` key
  set + status, `diagnosticsSnapshotJson()` pretty-print, connection report
  subset, `showSection("diagnostics")` exclusivity + auth guard, `ew://open/
  logs` and `ew://open/org/<org>/settings/logs` route resolution (and a
  foreign-org rejection), ping success/transport-failure signals against the
  fixture server, realtime counters.
- `native-native-window` (`NativeWindowTest`, inside the class before its
  closing brace): open via `showSection("logs"-mapped route)/profile-menu
  action → pill + exactly 9 cards with objectNames; seeded states flip the
  pill Healthy (online) → Degraded (429 rate-limit countdown) → Offline
  (aborted transport) with dot colors `#26c038/#ffd633/#f04c4c`; Ping shows
  "Reachable (N ms)" then resets to "Ping server" after 3 s; Copy snapshot
  puts 2-space-indented JSON on the clipboard and swaps the label for 2 s;
  the advanced snapshot collapse reveals the JSON block (max height 208).
