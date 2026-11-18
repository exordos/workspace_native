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

## Session log viewer modal (issue #51)

Issue #51 (part of #49). Same reference contract sha
`61dacef78dab2f5d0be76f283bebf49fe29b84af`. Figma token unavailable (401) -
mockups not consulted; the reference code is the pixel oracle. This section
supersedes deviation 4 above: the "View logs" affordances and the logs modal
are this issue's scope.

### Modal shell - `packages/web/src/pages/logs/logs-view-modal.ui.tsx`

- Content (`logs-view-modal.ui.tsx:11` + app-dialog.ui.tsx base): `fixed
  left-1/2 top-1/2 -translate-x-1/2 -translate-y-1/2 w-full max-h-[85vh]
  max-w-4xl rounded-xl border border-border-subtle bg-card-bg p-0 shadow-2xl
  overflow-hidden flex flex-col` (max width 896, max height 85% of viewport,
  radius 12). Overlay `fixed inset-0 bg-black/50` (`:13`,
  `APP_DIALOG_OVERLAY_CLASS`).
- Header (`:47-79`): `flex shrink-0 flex-wrap items-start justify-between
  gap-2 border-b border-border-subtle px-4 py-3` (16px/12px padding). Title
  `text-sm font-semibold text-text-primary` (14px semibold,
  `settings.logs` "Logs" / "Журнал"), subtitle `mt-0.5 text-[11px]
  text-text-muted` (`settings.logsHint` "In-memory session logs (last 500
  entries)."). Action row `flex items-center gap-1` (4px): Refresh, Clear,
  Download buttons `h-8 rounded-md border border-border-subtle bg-card-bg
  px-2.5 text-xs` (32px tall, radius 6, padding 10px, 12px text); Clear and
  Download carry `disabled:opacity-50 disabled:cursor-not-allowed` and are
  disabled while `!hasEntries` (`:70-103`). Close button `rounded p-1
  text-text-muted hover:bg-bg/50` with the `close` icon at 18 px
  (`:93-101`, aria `common.close` "Close"/"Закрыть").
- Body (`:110-125`): `flex min-h-0 flex-1 flex-col overflow-hidden p-3 pt-2`
  (12px padding, 8px top) hosting `LogsPageLogsSection showHeading={false}`.

### Filters + list - `packages/web/src/pages/logs/logs-page-logs.ui.tsx`

- Filter row (`:110-168`): `flex shrink-0 items-center gap-1 overflow-x-auto`
  with: search input `h-8 min-w-context-menu-narrow rounded-md border
  border-border-subtle bg-card-bg px-2 text-xs focus:border-accent`
  (placeholder `settings.logsSearchPlaceholder`); four compact Selects
  `min-w-[110px]` in order source, level, scope, runtime; Reset filters
  button `h-8 whitespace-nowrap px-2 text-xs`.
- Results label (`:170`): `text-[11px] text-text-muted`, key
  `settings.logsResults` "{{visible}} of {{total}} entries".
- Entry rows `LogEntryRow` (`:216-245`): `li rounded-md border
  border-border-subtle p-2`; meta line `flex flex-wrap items-center gap-1
  text-[11px] text-text-muted` with level chip `rounded bg-bg px-1.5 py-0.5
  font-mono uppercase`, runtime chip same shape (not uppercase), scope
  `font-medium text-text-primary`, timestamp `new Date(ts).toLocaleString()`;
  message `mt-1 text-xs leading-snug text-text-primary`; optional data `pre
  mt-1 overflow-x-auto rounded bg-bg p-1.5 text-[11px] leading-snug` with
  `JSON.stringify(entry.data)`. Left accent: warn rows `border-l-2
  border-indicator-yellow` `#ffd633`, error rows `border-l-2
  border-indicator-red` `#f04c4c` (emphasized recent-error rows with no other
  accent get `border-l-2 border-border-subtle`).
- "Recent errors" block (`:96-108`): `details[open] rounded-lg border
  border-border-subtle bg-card-bg p-2`, summary `text-xs font-medium`
  (`settings.diagnosticsRecentErrors`), inner `ul mt-2 max-h-40 space-y-1
  overflow-auto` (max height 160). Only rendered when recent errors exist
  (errors in the last 15 min, newest first, capped at 8 -
  diagnostics-collect.lib.ts:153-154).
- Empty state (`:172-176`): centered column `rounded-lg border
  border-border-subtle p-4` with `grid` icon size 32 opacity-50 and
  `settings.logsEmpty` "No logs yet" text-xs.
- Filter semantics `filterLogEntries` (`:248-268` and the in-component
  useMemo): level, source, scope and runtime filters AND together
  ("all"/empty = pass); search = `query.trim().toLowerCase()` substring
  (case-insensitive) of `` `${level} ${scope} ${runtime} ${message}
  ${JSON.stringify(data)}` `` (missing data serializes to empty). Source
  buckets (`logs-source-filter.lib.ts:3-32`): `api` = scope `api`; `console`
  = scope `console`; `trace` = scope prefix `trace:`; `actions` = scope in
  {`action`, `auth`, `realtime`} or prefix `action:`; `app` = every other
  scope.
- Export (`logs-export.lib.ts:7-23`): filename `workspace-logs-YYYYMMDD-
  HHMMSS.json` from UTC parts (zero-padded); body = `JSON.stringify(entries,
  null, 2)` (2-space pretty) with each entry's `data` re-run through
  `logger.redact()`; export/download disabled on empty history.

### History source - `shared/lib/logger.ts`

In-memory ring buffer `MAX_BUFFER_SIZE = 500`, push order oldest first with
oldest evicted; `getLogHistory` newest-first view for consumers,
`subscribeLogHistory` notifies on every append and clear, `clearLogHistory`
empties. Entry shape `{level, scope, message, timestamp(ISO-8601), runtime,
data?}` with `redact()` applied on write: sensitive keys (case-insensitive,
`-`/`_` stripped: apikey, api_key, apikey, password, passwd, secret, token,
authorization, auth, cookie, session, sessionid, csrf, csrftoken,
creditcard, ssn, private_key, privatekey) become `"[REDACTED]"`; string
values matching `^[A-Za-z0-9+/=]{20,}$`, `^Basic\s+` or `^Bearer\s+` become
`"[REDACTED]"`; objects recurse to depth 8 then `"[max depth]"`; arrays map
element-wise.

### Native design

- New C++ surface in `src/native-diagnostics.{h,cpp}` (`workspace::diagnostics`):
  `SessionLogEntry` (level/scope/message/timestamp/runtime/data), `redactLogValue`
  (exact reference port), `logSourceBucket`/`matchesLogSourceFilter`,
  `logEntryMatchesFilters` (reference AND-substring semantics),
  `buildLogsExportFilename(nowMs)` (UTC `workspace-logs-YYYYMMDD-HHMMSS.json`),
  `serializeLogsForExport` (entries -> 2-space pretty JSON, `data` redacted),
  and `SessionLogBuffer` (QObject ring buffer, newest-first `entries()`,
  `append`, `clear`, `entriesChanged()` signal, cap 500 like the reference).
- Controller (`src/native-controller.{h,cpp}`): owns one `SessionLogBuffer`;
  `useLogging` also labels the buffer with the platform runtime. QML surface:
  `diagnosticsSessionLogEntries()`, `diagnosticsSessionLogsFiltered(level,
  source, scope, runtime, search)`, `appendSessionLog(level, scope, message,
  data)`, `clearSessionLog()`, `diagnosticsLogsExportFilename()`,
  `diagnosticsLogsExportJson()`, `exportSessionLog(path)`, signal
  `sessionLogChanged()`. Live producers (real state only, no synthetic feed):
  realtime `connectionChanged` transitions log scope `connection-health`
  (live = info, retrying = warn); realtime apply failures log scope
  `realtime` level error; answered HTTP requests with status >= 400 and
  transport failures log scope `api` (4xx warn, 5xx/transport error) with
  method/path/status data; transport offline/online flips log scope
  `connection-health`.
- QML (`qml/NativeDiagnosticsPage.qml`): header gains the `View logs`
  `HeaderButton` (first action, `diagnosticsViewLogsButton`) and the Log
  counts card gains its `View logs` button (`h-7 rounded-md bg-bg px-2
  text-[11px]`, reference logs-page-overview.ui.tsx:143-149); both open the
  modal (`logsModal`, hidden Popup until opened). Modal: 896x(85% of page)
  centered card `rounded-xl border-border-subtle bg-card-bg` over a 50%-black
  overlay (Popup `modal: true` + `Overlay`), header px-4 py-3 with title +
  hint + Refresh/Clear/Export h-8 buttons (Clear/Export `enabled: false;
  opacity: 0.5` while the buffer is empty) and the 18 px close icon
  (`close.svg` shipped in the icon bundle). Filter row: search Field h-8
  text-xs + source/level/scope/runtime selects (h-8 instances of NativeSelect
  min width 110) + Reset filters button; results label "%1 of %2 entries"
  11px; recent-errors block (from the ring buffer, same window/cap math as
  #50's collector) max-h 160 open by default when present; entry list rows
  with the measured chips/timestamp/message/data-pre and warn/error left
  borders; empty state = 32px `grid.svg` at opacity 0.5 + "No logs yet".
  Export goes through `NativeDialogs.FileDialog` (`SaveFile`,
  `diagnosticsLogsSaveDialog`, suggested name from the controller) writing to
  the picked path via `exportSessionLog` - the repo's existing save flow
  (same pattern as `attachmentSaveDialog`); no Electron-style blob download.
  Live refresh: the page re-reads the filtered rows on
  `controller.sessionLogChanged()` while the modal is visible (reference
  `subscribeLogHistory`).

### Declared deviations

1. Runtime value set: the reference runtime filter enumerates the web values
   (`getRuntime()` -> `electron` | `pwa` | `browser`); the native data set
   records the native client platform label. The fixed native value set is
   `linux` | `macos` | `windows` (`Q_OS_*` mapped), stamped by the logger on
   every entry; the scope/runtime Selects stay reference-driven lists built
   from present entries plus the "All …" option.
2. Scope options list from present entries (reference does the same) - with
   the native scope taxonomy actually produced (`api`, `realtime`,
   `connection-health`, `app` today).
3. Clear wipes only the in-memory ring (reference `clearLogHistory`); the
   on-disk rotating log of #77 is untouched - the reference has no on-disk
   surface in the window either.
4. The web "sr-only" filter labels map to `Accessible.name` on the native
   controls (no separate hidden text nodes in QML).
5. Data pre block serializes with Qt's JSON (single-line compact) - the
   reference `JSON.stringify(data)` is likewise single-line.

### i18n

All visible strings are `qsTr` with the reference en values (placeholders
`{{visible}}`/`{{total}}` become `%1`/%2) and the ru values from
`ru.json` (`settings.logs*`, `common.close`, `settings.diagnosticsRecentErrors`,
`settings.logsViewModal`); new strings land in
`packages/native/translations/workspace_ru.ts` so the native-native-locale
catalog guards pass.

### Tests

- `native-native-controller`: ring buffer append/subscribe/clear + 500-cap
  eviction + newest-first; source bucket mapping per the rules above; filter
  semantics (level/source/scope/runtime ANDs, trimmed case-insensitive
  substring incl. JSON-serialized data); `redactLogValue` key/pattern/depth
  matrix; export filename shape `workspace-logs-[0-9]{8}-[0-9]{6}[.]json`
  (UTC parts), export JSON round-trips to the same entries with data
  redacted and 2-space pretty-print; controller surface: append ->
  `sessionLogChanged`, filtered invokables, `clearSessionLog`, and
  `exportSessionLog` writing the file under a QStandardPaths temp dir.
- `native-native-window` (inside `NativeWindowTest`): modal hidden until
  opened from the header View logs button and from the Log counts card
  button; empty state with Clear/Export disabled; appended lines appear live
  while open (incl. recent-errors block for a fresh error row); level and
  source selects narrow the list live with the "x of y entries" label;
  search narrows further; Reset filters restores; Clear switches to the
  empty state and disables itself; Export opens the save dialog.

## Memory panel (issue #52)

Issue: #52 (part of #49). Same reference sha as the page section above. Figma
token unavailable - mockups not consulted; the reference code is the oracle.

### Reference contract (measured) - `packages/web/src/pages/logs/logs-page-memory.ui.tsx`

- Polling (`logs-page-memory.ui.tsx:11`): `MEMORY_POLL_INTERVAL_MS = 3000`; the
  section refreshes on mount and on every interval tick while mounted
  (`:60-65`), plus on `refreshToken` bumps (`:67-70`).
- Section shell (`:71-83`): `rounded-lg border border-border-subtle bg-card-bg
  p-3` (radius 8, 12px padding); heading row `mb-2 flex items-center gap-2`
  with the `settings.diagnosticsMemory` title `text-xs font-medium
  text-text-primary` (12px medium) and a `text-[11px] text-text-muted` "..."
  loading marker while a fetch is in flight.
- Unavailable state (`:86-90`): when no metric exists and nothing is loading, a
  `text-[11px] text-text-muted` paragraph renders
  `settings.diagnosticsMemoryUnavailable` and the section header stays.
- Metric cards (`:96-124`): grid `gap-2 sm:grid-cols-2 lg:grid-cols-3`
  (8px gaps; 2 columns from 640px, 3 from 1024px). Each card renders only
  when its metric exists (capability flags `capabilities.jsHeapAvailable`,
  `deviceMemoryAvailable`, `processMetricsAvailable`). `MemoryCard`
  (`:218-224`): `rounded-md border border-border-subtle bg-bg px-2 py-1.5`
  (radius 6, 8px/6px padding), label `text-[11px] text-text-muted`, value
  `text-sm font-medium text-text-primary` (14px medium), optional detail
  `text-[11px] text-text-muted`. Cards in the Electron dataset: JS heap
  (value + "Heap limit" detail), Total app memory (`totalWorkingSetKb`),
  System free memory (freeKb + "<total> total" detail), Device RAM.
- Progress bar (`:126-146`): label row `text-[11px] text-text-muted` with the
  metric name left and the integer percent right (`justify-between`); track
  `h-1.5 overflow-hidden rounded-full bg-bg` (6px tall) with
  `role=progressbar`, `aria-valuemin=0`, `aria-valuemax=100`,
  `aria-valuenow=<percent>`; fill `h-full rounded-full bg-accent
  transition-[width]` with `width: <percent>%`. Percent math
  (`shared/lib/diagnostics-memory.lib.ts:84-91`): `used/limit*100`, rounded,
  clamped to 0..100; null when the limit is missing.
- Process table (`:160-197`): header label `text-[11px] font-medium
  text-text-primary`; scroller `overflow-x-auto`; table `w-full min-w-[420px]
  border-collapse text-[11px] text-text-muted`; header row `border-b
  border-border-subtle text-left` with columns Type / PID / Working set /
  Peak / CPU (CPU is the `settings.diagnosticsMemoryCpu` key, the other four
  are literal strings in the reference); every cell `font-mono px-1 py-1`
  (4px/4px padding); rows separated by `border-b border-border-subtle/60`.
  CPU renders `cpuPercent.toFixed(1) + "%"` and an em dash when null
  (`:210-214`).
- Byte formatting: `formatBytes`/`formatKilobytes`
  (`shared/lib/format-bytes.lib.ts:16-31`): units B/KB/MB/GB, 1024 base,
  precision 0 for bytes, 1 decimal when the value is >= 10, else 2 decimals
  ("8.00 MB", "512.0 MB", "8.00 GB total").
- Dataset source: Electron IPC `diagnostics:getMemorySnapshot`
  (`packages/electron/src/main.ts`): per-process `workingSetSize`,
  `peakWorkingSetSize`, `percentCPUUsage`, system total/free KB and
  `totalWorkingSetKb`; collected into the `DiagnosticsMemorySnapshot` shape in
  `shared/lib/diagnostics-memory.lib.ts:15-40` (collectedAt, runtime, jsHeap,
  deviceMemoryGb, electron{totalWorkingSetKb, system{totalKb, freeKb},
  processes[], renderer, main}, capabilities).
- i18n: `settings.diagnosticsMemory*` keys exist in both locale files
  (`en.json:1216-1226`, `ru.json:1228-1238`): Memory / JS heap (this tab) /
  Heap limit / Device RAM (estimate) / Total app memory / System free memory
  / Processes / CPU / unavailable text.

### Native design

New small provider `NativeMemoryDiagnostics` in
`packages/native/src/native-diagnostics.h/.cpp` (namespace
`workspace::diagnostics`), procfs reads gated on `Q_OS_LINUX` exactly like
`NativeLogging::memorySnapshot()`:

- `/proc/self/status`: `VmRSS` -> working set (kB), `VmHWM` -> peak working
  set (kB, the kernel-monotonic peak; the provider additionally clamps the
  reported peak to be monotonic across polls so a shrinking fixture can never
  lower it).
- `/proc/meminfo`: `MemTotal`, `MemFree`, `MemAvailable` (kB).
- `/proc/self/stat` (`utime`/`stime`, `starttime`) plus `/proc/uptime`:
  cumulative CPU percent since process start - the same semantics as
  Chromium `PercentCPUUsage`, which is what the Electron IPC reports. Null
  when either source is missing or malformed.
- `setProcRoot(path)` injects the fixture root (tests drive exact values;
  mirrors the existing `NativeLogging::setProcRoot` seam). The controller
  exposes `diagnosticsMemorySnapshot()` (Q_INVOKABLE, used by the QML poll)
  and a `setDiagnosticsMemoryProcRoot` seam.

Snapshot map (what the `memory` key of `diagnosticsSnapshot()` carries and
what `diagnosticsMemorySnapshot()` returns):

```
{
  "collectedAt": ISO-8601 UTC,
  "runtime": "native",
  "system": { "totalKb": n, "freeKb": n, "availableKb": n },
  "app": { "workingSetKb": n, "peakWorkingSetKb": n, "totalWorkingSetKb": n },
  "processes": [ { "type": "Main", "pid": n, "workingSetKb": n,
                   "peakWorkingSetKb": n, "cpuPercent": double|null } ],
  "capabilities": { "processMetricsAvailable": bool,
                    "systemMemoryAvailable": bool }
}
```

Malformed or missing input: every value falls back to 0, `processes` is empty,
both capability flags are false, and the panel renders the unavailable state
(`MemTotal > MemAvailable > 0` is the availability rule for system memory).

`NativeController::diagnosticsSnapshot()` no longer gates the `memory` key on
`m_logging`; it always delegates to this provider (the panel needs metrics
even with file logging off). `NativeLogging::memorySnapshot()` keeps its
accepted shape for its own consumers. The clock-driven `memory.collectedAt`
and the process-state values are excluded from the snapshot-export equality
case, which now pins the memory provider to a fixed fixture root.

### Panel layout mapping (compact native shape)

- Section: existing `diagnosticsMemoryCard` block upgraded from the two-line
  summary to the full reference section; `radius: 8`, `border-subtle`,
  `card-bg`, p-3 (12px). Heading `diagnosticsMemoryTitle` (12px medium) plus
  `diagnosticsMemoryLoadingMarker` ("<U+2026>") while a poll is in flight
  (logs-page-memory.ui.tsx:74-83).
- Cards: `diagnosticsMemoryMetricGrid` Grid, spacing 8 (gap-2), columns
  1 / 2 (page >= 640) / 3 (page >= 1024) (logs-page-memory.ui.tsx:96,
  Tailwind sm/lg). Each `diagnosticsMemoryMetric-<key>`: radius 6,
  border-subtle on `bg`, px-2 py-1.5; label 11px muted, value 14px medium
  primary, optional detail 11px muted (logs-page-memory.ui.tsx:218-224).
  Rendered cards: "Total app memory" (`totalWorkingSetKb`, gated on
  `processMetricsAvailable`) and "System free memory" (`freeKb`, detail
  "<total> total", gated on `systemMemoryAvailable`).
- Progress bar: track `diagnosticsMemoryBarTrack` 6px tall, radius 3, on `bg`
  (h-1.5 rounded-full bg-bg); fill `diagnosticsMemoryBarFill` anchored left,
  `accent`, width = track width * percent / 100; label row "Memory usage" +
  integer percent, 11px muted (logs-page-memory.ui.tsx:126-146). The
  Accessible role is ProgressBar with value 0..100.
- Process table: label "Processes" 11px medium, then header row and one row
  per process, minimum width 420 (min-w-[420px]), cells 11px monospace with
  4px horizontal / 4px vertical padding, 1px bottom border border-subtle
  (logs-page-memory.ui.tsx:160-197). CPU cell uses one decimal + percent, em
  dash when null (logs-page-memory.ui.tsx:210-214).
- Unavailable: `diagnosticsMemoryUnavailableText` renders "Memory metrics are
  not available in this build." (the native reading of
  `settings.diagnosticsMemoryUnavailable`; the reference string names the
  browser, which this client is not) whenever neither capability flag is set
  and no poll is in flight.

### Polling contract

`memoryPollIntervalMs` defaults to 3000 (MEMORY_POLL_INTERVAL_MS,
logs-page-memory.ui.tsx:11) and is injectable so window tests drive ticks
without waiting on wall clock. `diagnosticsMemoryPollTimer` (repeat) starts
when the diagnostics section becomes visible and stops when it leaves, so the
poll never runs in another section; the first snapshot is pulled immediately
on becoming visible (reference refreshes on mount). Each tick bumps
`memoryPollTicks` (test seam) and calls `controller.diagnosticsMemorySnapshot()`.

### Declared deviations (dataset only; layout parity)

- Single-process table row: the reference lists Electron child processes
  (Browser/GPU/Utility/Tab); this client is one process, so the table renders
  exactly one `Main` row with the same five columns. No fake rows.
- No JS heap and no Device RAM cards: Qt exposes no Chromium heap and no
  `navigator.deviceMemory`; capability gating hides both
  (logs-page-memory.ui.tsx:96-124 renders cards only when the metric exists).
- The progress bar tracks system memory utilization
  (`(MemTotal - MemAvailable) / MemTotal`) because the JS heap metric it
  mirrors in the reference has no native equivalent. Same percent math and
  visual contract as `getJsHeapUtilizationPercent`
  (diagnostics-memory.lib.ts:84-91).
- CPU percent semantics: cumulative CPU since process start (Chromium
  PercentCPUUsage), computed from `/proc/self/stat` + `/proc/uptime`.

### i18n (new `NativeDiagnosticsPage` catalog entries)

New English source strings (context `NativeDiagnosticsPage`): "Total app
memory", "System free memory", "%1 total", "Memory usage", "Processes",
"Type", "PID", "Working set", "Peak", "CPU". The Russian translations follow
the reference locale wording (settings.diagnosticsMemory* entries in the
reference ru.json: card labels, "Processes", "CPU"); the remaining column
headers get short Russian readings. The actual strings live in
`packages/native/translations/workspace_ru.ts`; `native-native-locale`
guards that every extracted source string has one reviewed translation that
preserves its arguments, and vice versa.
