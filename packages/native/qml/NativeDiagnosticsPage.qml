pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs as NativeDialogs
import QtQuick.Effects
import QtCore

// Diagnostics page: connection dashboard (issue #50,
// docs/NATIVE_DESKTOP_DIAGNOSTICS_PAGE.md). Pixel-faithful port of the
// reference logs-page measured at workspace_ui
// 61dacef78dab2f5d0be76f283bebf49fe29b84af: header border-b border-border-subtle
// bg-card-bg px-3 py-2.5 with title text-base font-semibold and hint text-xs
// text-muted (logs-page.ui.tsx:268-275); header buttons h-8 rounded-md border
// px-2.5 text-xs (logs-page.ui.tsx:277-304 — no View logs affordance until the
// native logs modal issue); content column p-2 gap-2; status pill rounded-full
// border px-2.5 py-1 text-xs with the 8px STATUS_DOT
// (logs-page-overview.ui.tsx:13-17,41-49: healthy indicator-green, degraded
// indicator-yellow, offline indicator-red); Ping server button h-7 rounded-md
// px-2 text-xs with the 3 s reset and disabled opacity-50
// (logs-page-overview.ui.tsx:50-63, logs-page.ui.tsx:238-242); the nine-card
// grid gap-2 sm:grid-cols-2 xl:grid-cols-3 with DiagnosticCard rounded-lg
// border p-3, title mb-1.5 text-xs font-medium, body space-y-0.5 text-[11px]
// leading-snug (logs-page-overview.ui.tsx:66,218-223); copy feedback resets
// after 2000 ms (logs-page.ui.tsx:232-236); collapsible Advanced snapshot with
// a 208px-tall 11px JSON pre block (logs-page.ui.tsx:325-334). Every value
// comes from NativeController::diagnosticsSnapshot(): no fake numbers,
// unavailable sources render the reference empty/unavailable text.
FocusScope {
    id: page
    objectName: "nativeDiagnosticsPage"
    required property var controller
    required property var colors

    // Reference timings (logs-page.ui.tsx:232-242).
    readonly property int copyFeedbackMs: 2000
    readonly property int pingFeedbackMs: 3000

    // Dataset state: collected at open, on Refresh and on live connection
    // changes while visible; the copy action and the advanced block share the
    // same pretty-printed text (logs-page.ui.tsx:201-204).
    property var snapshot: ({})
    property string snapshotJson: ""
    property var cards: []
    property string copyState: "idle"   // idle | success | error
    property string pingState: "idle"   // idle | loading | success | error
    property int pingLatencyMs: -1
    property bool advancedExpanded: false

    // ---- Issue #51: session log viewer modal state ------------------------
    // (docs/NATIVE_DESKTOP_DIAGNOSTICS_PAGE.md, modal section). The rows come
    // from the controller ring buffer (reference logger.ts getLogHistory,
    // newest first); the filter values mirror the reference Select state of
    // LogsPageLogsSection ("all" = pass, search trimmed case-insensitive).
    property var logEntries: []
    property var logsRows: []
    property var logsScopeOptions: []
    property var logsRuntimeOptions: []
    property var logsRecentErrors: []
    property string logsLevelFilter: "all"
    property string logsSourceFilter: "all"
    property string logsScopeFilter: "all"
    property string logsRuntimeFilter: "all"
    property string logsSearchQuery: ""
    // Export feedback state (review 4616699201 on #411): idle | error, the
    // same inline-state pattern the ping/copy feedback uses on this page.
    property string logsExportState: "idle"

    function refreshLogs() {
        page.logEntries = page.controller.diagnosticsSessionLogEntries();
        page.logsRows = page.controller.diagnosticsSessionLogsFiltered(
            page.logsLevelFilter, page.logsSourceFilter, page.logsScopeFilter,
            page.logsRuntimeFilter, page.logsSearchQuery);
        // Reference derives the scope/runtime Select options from the
        // present entries (logs-page-logs.ui.tsx:33-45).
        const scopes = new Set();
        const runtimes = new Set();
        for (const entry of page.logEntries) {
            scopes.add(String(entry.scope));
            runtimes.add(String(entry.runtime));
        }
        page.logsScopeOptions = Array.from(scopes).sort();
        page.logsRuntimeOptions = Array.from(runtimes).sort();
        // "Recent errors": error entries inside the reference 15-minute
        // window, newest first, capped at 8 (diagnostics-collect.lib.ts:153).
        const cutoff = Date.now() - 900000;
        const recent = [];
        for (const entry of page.logEntries) {
            if (recent.length >= 8)
                break;
            if (String(entry.level) === "error" && Date.parse(String(entry.timestamp)) >= cutoff)
                recent.push(entry);
        }
        page.logsRecentErrors = recent;
    }

    function openLogsModal() {
        page.resetLogFilters();
        logsModal.open();
    }

    function resetLogFilters() {
        page.logsLevelFilter = "all";
        page.logsSourceFilter = "all";
        page.logsScopeFilter = "all";
        page.logsRuntimeFilter = "all";
        page.logsSearchQuery = "";
        logsSearchField.text = "";
        logsSourceFilterSelect.currentIndex = 0;
        logsLevelFilterSelect.currentIndex = 0;
        logsScopeFilterSelect.currentIndex = 0;
        logsRuntimeFilterSelect.currentIndex = 0;
        page.refreshLogs();
    }

    function logTimestamp(stamp) {
        const when = new Date(String(stamp));
        return Number.isNaN(when.getTime()) ? String(stamp)
            : Qt.formatDateTime(when, Qt.DefaultLocaleLongDate);
    }

    function refresh() {
        page.snapshot = page.controller.diagnosticsSnapshot();
        page.snapshotJson = page.controller.diagnosticsSnapshotJson();
        page.cards = page.buildCards();
    }

    function statusColor(status) {
        if (status === "offline")
            return page.colors["indicator-red"];
        if (status === "degraded")
            return page.colors["indicator-yellow"];
        return page.colors["indicator-green"];
    }

    function statusLabel(status) {
        if (status === "offline")
            return qsTr("Offline");
        if (status === "degraded")
            return qsTr("Degraded");
        return qsTr("Healthy");
    }

    function formatMegabytes(bytes, fromKilobytes) {
        const value = fromKilobytes ? bytes * 1024 : bytes;
        if (!Number.isFinite(value) || value < 0)
            return "\u2014";
        if (value >= 1024 * 1024 * 1024)
            return (value / (1024 * 1024 * 1024)).toFixed(1) + " GiB";
        if (value >= 1024 * 1024)
            return Math.round(value / (1024 * 1024)) + " MiB";
        return Math.round(value / 1024) + " KiB";
    }

    // ---- Memory panel state (issue #52, logs-page-memory.ui.tsx) ---------
    // Reference poll constant MEMORY_POLL_INTERVAL_MS = 3000
    // (logs-page-memory.ui.tsx:11); injectable so window tests drive ticks
    // without waiting on wall clock.
    property int memoryPollIntervalMs: 3000
    property var memory: null
    property bool memoryLoading: true
    property int memoryPollTicks: 0 // test seam for the poll timer

    readonly property bool memoryHasAnyMetric: {
        if (page.memory === null)
            return false;
        const capabilities = page.memory.capabilities || {};
        return capabilities.processMetricsAvailable === true
            || capabilities.systemMemoryAvailable === true;
    }

    // Card grid breakpoints: gap-2 sm:grid-cols-2 lg:grid-cols-3
    // (logs-page-memory.ui.tsx:96; Tailwind sm=640 lg=1024).
    readonly property int memoryGridColumns:
        page.width >= 1024 ? 3 : (page.width >= 640 ? 2 : 1)

    // Same percent math as getJsHeapUtilizationPercent
    // (diagnostics-memory.lib.ts:84-91): round, clamp 0..100; the native
    // dataset uses system memory (used = MemTotal - MemAvailable) because
    // Qt exposes no Chromium heap (declared deviation).
    readonly property int memoryUsagePercent: {
        if (page.memory === null)
            return -1;
        const capabilities = page.memory.capabilities || {};
        if (capabilities.systemMemoryAvailable !== true)
            return -1;
        const system = page.memory.system || {};
        const total = Number(system.totalKb || 0);
        const available = Number(system.availableKb || 0);
        if (total <= 0)
            return -1;
        const percent = Math.round((total - available) / total * 100);
        return Math.min(100, Math.max(0, percent));
    }

    function memoryCards() {
        const memory = page.memory;
        const cards = [];
        if (!memory)
            return cards;
        const capabilities = memory.capabilities || {};
        const app = memory.app || {};
        const system = memory.system || {};
        // Cards render only when their metric exists (capability gating,
        // logs-page-memory.ui.tsx:96-124); the JS heap and device RAM cards
        // have no Qt runtime source and stay hidden (design doc deviations).
        if (capabilities.processMetricsAvailable === true)
            cards.push({ key: "app", label: qsTr("Total app memory"),
                value: page.formatKilobytes(Number(app.totalWorkingSetKb || 0)),
                detail: "" });
        if (capabilities.systemMemoryAvailable === true)
            cards.push({ key: "systemFree", label: qsTr("System free memory"),
                value: page.formatKilobytes(Number(system.freeKb || 0)),
                detail: qsTr("%1 total")
                    .arg(page.formatKilobytes(Number(system.totalKb || 0))) });
        return cards;
    }

    function memoryProcesses() {
        const source = page.memory ? page.memory.processes : null;
        const processes = [];
        // A QVariantList arriving through a var property is not guaranteed to
        // be a native JS array, so copy element-by-element into one.
        if (source) {
            const size = Number(source.length || 0);
            for (let index = 0; index < size; ++index)
                processes.push(source[index]);
        }
        return processes;
    }

    function refreshMemory() {
        // One poll tick (logs-page-memory.ui.tsx:31-45): the provider mirrors
        // the reference collector; a failed read returns capability flags
        // false, which the panel renders as the unavailable state.
        page.memoryLoading = true;
        page.memory = page.controller.diagnosticsMemorySnapshot();
        page.memoryLoading = false;
    }

    // Port of formatBytes/formatKilobytes (format-bytes.lib.ts:21-31):
    // B/KB/MB/GB base 1024, 0 decimals for bytes, 1 at value >= 10, else 2.
    function formatBytes(bytes) {
        let value = Number(bytes);
        if (!Number.isFinite(value) || value <= 0)
            return "0 B";
        const units = ["B", "KB", "MB", "GB"];
        let unit = 0;
        while (value >= 1024 && unit < units.length - 1) {
            value /= 1024;
            ++unit;
        }
        const precision = unit === 0 ? 0 : (value >= 10 ? 1 : 2);
        return value.toFixed(precision) + " " + units[unit];
    }

    function formatKilobytes(kilobytes) {
        return page.formatBytes(Number(kilobytes) * 1024);
    }

    // Reference formatCpuPercent (logs-page-memory.ui.tsx:210-214): one
    // decimal plus percent sign, em dash when the source is null.
    function formatMemoryCpuPercent(cpuPercent) {
        if (cpuPercent === null || cpuPercent === undefined
                || !Number.isFinite(Number(cpuPercent)))
            return "\u2014";
        return Number(cpuPercent).toFixed(1) + "%";
    }

    Timer {
        id: memoryPoll
        objectName: "diagnosticsMemoryPollTimer"
        interval: page.memoryPollIntervalMs
        repeat: true
        onTriggered: {
            page.memoryPollTicks += 1;
            page.refreshMemory();
        }
    }

    function cardTitle(key) {
        switch (key) {
        case "connection": return qsTr("Connection");
        case "realtime": return qsTr("Realtime");
        case "session": return qsTr("Session");
        case "runtime": return qsTr("Runtime");
        case "notifications": return qsTr("Notifications");
        case "logcounts": return qsTr("Log counts");
        case "performance": return qsTr("Performance");
        case "cache": return qsTr("Local cache");
        default: return qsTr("Stores");
        }
    }

    // Card list for the grid Repeater (the nine reference cards). Every line
    // is a string; the first renders font-medium text-primary like the
    // reference card bodies, the rest text-muted. `kind: "logcounts"` renders
    // the level badges instead of lines.
    function buildCards() {
        const snapshot = page.snapshot || {};
        const connection = snapshot.connection || {};
        const realtime = snapshot.realtime || {};
        const realtimeStats = realtime.stats || {};
        const session = snapshot.session || {};
        const workspaceSession = snapshot.workspaceSession || {};
        const runtime = snapshot.runtime || {};
        const environment = snapshot.environment || {};
        const notifications = snapshot.notifications || {};
        const performance = snapshot.performance || {};
        const apiLatency = performance.apiLatency || {};
        const logs = snapshot.logs || {};
        const byLevel = logs.byLevel || {};
        const stores = snapshot.stores || {};
        const cache = snapshot.cache;
        const cards = [];
        let lines = [];

        lines = [String(connection.phase || "")];
        if (connection.failureReason)
            lines.push(String(connection.failureReason));
        lines.push(qsTr("Reconnect attempts: %1").arg(Number(connection.reconnectAttempt || 0)));
        cards.push({ key: "connection", kind: "lines", lines: lines, badges: [] });

        const lastEventAt = realtimeStats.lastEventAt;
        const collectedMs = Date.parse(String(snapshot.collectedAt || ""));
        lines = [qsTr("Events received: %1").arg(Number(realtimeStats.eventsReceivedCount || 0))];
        lines.push((realtime.online ? qsTr("online") : qsTr("Offline")) + " \u00b7 "
            + (realtime.windowVisible ? qsTr("Window visible") : qsTr("Window hidden")));
        if (lastEventAt !== null && lastEventAt !== undefined && Number.isFinite(collectedMs))
            lines.push(qsTr("Last %1 event %2s ago")
                .arg(String(realtimeStats.lastEventType || "?"))
                .arg(Math.round((collectedMs - Number(lastEventAt)) / 1000)));
        cards.push({ key: "realtime", kind: "lines", lines: lines, badges: [] });

        lines = [String(workspaceSession.login || qsTr("Not signed in")),
            String(workspaceSession.organizationOrigin || "\u2014"),
            qsTr("%1 Workspace sessions").arg(Number(workspaceSession.count || 0))];
        if (session.sessionRemainingMs !== null && session.sessionRemainingMs !== undefined)
            lines.push(qsTr("Session expires in ~%1 min")
                .arg(String(Math.ceil(Number(session.sessionRemainingMs) / 60000))));
        cards.push({ key: "session", kind: "lines", lines: lines, badges: [] });

        lines = [String(runtime.detectedRuntime || "\u2014"),
            "v" + String(environment.appVersion || "dev"),
            String(environment.brandAppName || "Exordos Workspace")];
        cards.push({ key: "runtime", kind: "lines", lines: lines, badges: [] });

        cards.push({ key: "notifications", kind: "lines", badges: [],
            lines: [String(notifications.permission || "unsupported"),
                qsTr("OS/browser permission")] });

        cards.push({ key: "logcounts", kind: "logcounts", lines: [],
            badges: [{ level: "error", count: Number(byLevel.error || 0) },
                { level: "warn", count: Number(byLevel.warn || 0) },
                { level: "info", count: Number(byLevel.info || 0) },
                { level: "debug", count: Number(byLevel.debug || 0) }] });

        const vitals = performance.vitals || [];
        lines = [];
        if (vitals.length === 0)
            lines.push(qsTr("No performance metrics recorded yet"));
        for (const vital of vitals)
            lines.push(String(vital.name) + ": " + String(vital.value));
        if (Number(apiLatency.sampleCount || 0) > 0)
            lines.push(qsTr("API median %1 ms \u00b7 p95 %2 ms (%3 calls)")
                .arg(apiLatency.medianMs === null || apiLatency.medianMs === undefined
                     ? 0 : apiLatency.medianMs)
                .arg(apiLatency.p95Ms === null || apiLatency.p95Ms === undefined
                     ? 0 : apiLatency.p95Ms)
                .arg(apiLatency.sampleCount));
        cards.push({ key: "performance", kind: "lines", lines: lines, badges: [] });

        if (cache === null || cache === undefined)
            lines = [qsTr("Local cache unavailable")];
        else
            lines = [qsTr("%1 cached responses").arg(Number(cache.entries || 0)),
                cache.hasOwnerMeta ? qsTr("Workspace cache owner present")
                                   : qsTr("No Workspace cache owner")];
        cards.push({ key: "cache", kind: "lines", lines: lines, badges: [] });

        cards.push({ key: "stores", kind: "lines", badges: [],
            lines: [qsTr("%1 streams").arg(Number(stores.streamsCount || 0)),
                qsTr("%1 conversations").arg(Number(stores.conversationsCount || 0)),
                qsTr("%1 users").arg(Number(stores.usersCount || 0))] });
        return cards;
    }

    // Card grid breakpoints: gap-2 sm:grid-cols-2 xl:grid-cols-3
    // (logs-page-overview.ui.tsx:66; Tailwind sm=640 xl=1280).
    readonly property int gridColumns: page.width >= 1280 ? 3 : (page.width >= 640 ? 2 : 1)
    readonly property real gridSpacing: 8
    readonly property real cellWidth: (column.width - gridSpacing * (gridColumns - 1)) / gridColumns

    // Ping button label per the reference state machine
    // (logs-page-overview.ui.tsx:56-62).
    readonly property string pingLabel: {
        if (page.pingState === "success" && page.pingLatencyMs >= 0)
            return qsTr("Reachable (%1 ms)").arg(page.pingLatencyMs);
        if (page.pingState === "error")
            return qsTr("Unreachable");
        return qsTr("Ping server");
    }
    readonly property string copySnapshotLabel: {
        if (page.copyState === "success")
            return qsTr("Snapshot copied");
        if (page.copyState === "error")
            return qsTr("Copy failed");
        return qsTr("Copy snapshot");
    }

    Timer {
        id: copyReset
        interval: page.copyFeedbackMs
        onTriggered: page.copyState = "idle"
    }
    Timer {
        id: pingReset
        interval: page.pingFeedbackMs
        onTriggered: {
            page.pingState = "idle";
            page.pingLatencyMs = -1;
        }
    }
    Connections {
        target: page.controller
        function onStateChanged() {
            // The pill and the cards follow live connection changes while the
            // page is open; debounce so a burst of stateChanged emits collects
            // into one snapshot.
            if (page.visible)
                liveRefresh.restart();
        }
        function onPingFinished(ok, latencyMs) {
            page.pingLatencyMs = ok ? latencyMs : -1;
            page.pingState = ok ? "success" : "error";
            pingReset.restart();
        }
        function onSessionLogChanged() {
            // Reference subscribeLogHistory: the modal re-reads rows live
            // (logs-page.test.tsx "entries append while the modal is open").
            if (logsModal.visible)
                page.refreshLogs();
        }
    }

    Timer {
        id: liveRefresh
        interval: 120
        onTriggered: if (page.visible) page.refresh()
    }

    onVisibleChanged: {
        if (visible) {
            page.refresh();
            page.refreshMemory();
            memoryPoll.start();
        } else {
            memoryPoll.stop(); // poll only while the section is visible
        }
    }
    Component.onCompleted: {
        if (visible) {
            page.refresh();
            page.refreshMemory();
            memoryPoll.start();
        }
    }
    onMemoryPollIntervalMsChanged:
        if (memoryPoll.running) { memoryPoll.stop(); memoryPoll.start(); }

    component HeaderButton: Rectangle {
        id: headerButton
        property string buttonLabel: ""
        signal actioned()
        property bool hovered: false
        implicitWidth: headerButtonLabel.implicitWidth + 20 // px-2.5
        implicitHeight: 32 // h-8
        radius: 6 // rounded-md
        color: hovered && enabled ? page.colors["bg-elevated"] : page.colors["card-bg"]
        border.color: page.colors["border-subtle"]
        border.width: 1
        opacity: enabled ? 1.0 : 0.5 // disabled:opacity-50
        Text {
            id: headerButtonLabel
            anchors.centerIn: parent
            text: headerButton.buttonLabel
            font.pixelSize: 12 // text-xs
            color: page.colors["text-primary"]
        }
        MouseArea {
            anchors.fill: parent
            enabled: headerButton.enabled
            hoverEnabled: true
            cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            onEntered: headerButton.hovered = true
            onExited: headerButton.hovered = false
            onClicked: headerButton.actioned()
        }
    }

    // ---- Header (logs-page.ui.tsx:268-307) --------------------------------
    Rectangle {
        id: header
        objectName: "diagnosticsHeader"
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: headerRow.implicitHeight + 20 // py-2.5
        color: page.colors["card-bg"]
        Rectangle { // border-b border-border-subtle
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: page.colors["border-subtle"]
        }
        Row {
            id: headerRow
            objectName: "diagnosticsHeaderRow"
            x: 12 // px-3
            y: 10
            width: parent.width - 24
            spacing: 4
            Column {
                width: Math.max(120, headerRow.width - headerActions.implicitWidth - 8)
                spacing: 2 // mt-0.5
                Text {
                    objectName: "diagnosticsTitle"
                    text: qsTr("Diagnostics")
                    font.pixelSize: 16 // text-base
                    font.weight: Font.DemiBold // font-semibold
                    color: page.colors["text-primary"]
                    elide: Text.ElideRight
                    width: parent.width
                }
                Text {
                    objectName: "diagnosticsHint"
                    text: qsTr("Full runtime diagnostics with searchable session logs.")
                    font.pixelSize: 12 // text-xs
                    color: page.colors["text-muted"]
                    elide: Text.ElideRight
                    width: parent.width
                }
            }
            Row {
                id: headerActions
                objectName: "diagnosticsHeaderActions"
                spacing: 4 // gap-1
                HeaderButton {
                    objectName: "diagnosticsViewLogsButton"
                    buttonLabel: qsTr("View logs")
                    onActioned: page.openLogsModal()
                }
                HeaderButton {
                    objectName: "diagnosticsRefreshButton"
                    buttonLabel: qsTr("Refresh")
                    onActioned: page.refresh()
                }
                HeaderButton {
                    objectName: "diagnosticsCopyReportButton"
                    buttonLabel: qsTr("Copy connection report")
                    onActioned: {
                        page.controller.copyToClipboard(page.controller.diagnosticsConnectionReportJson());
                        page.copyState = "success";
                        copyReset.restart();
                    }
                }
                HeaderButton {
                    objectName: "diagnosticsCopySnapshotButton"
                    buttonLabel: page.copySnapshotLabel
                    onActioned: {
                        page.controller.copyToClipboard(page.snapshotJson);
                        page.copyState = "success";
                        copyReset.restart();
                    }
                }
            }
        }
    }

    // ---- Scrollable content column (p-2, inner column gap-2) ---------------
    Flickable {
        id: content
        objectName: "diagnosticsContent"
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true
        contentWidth: width
        contentHeight: column.height + 16
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        Column {
            id: column
            x: 8
            y: 8
            width: content.width - 16
            spacing: 8

            // Memory panel (issue #52, reference logs-page-memory.ui.tsx;
            // docs/NATIVE_DESKTOP_DIAGNOSTICS_PAGE.md, "Memory panel").
            // Section shell: rounded-lg border border-border-subtle bg-card-bg p-3
            // (logs-page-memory.ui.tsx:71).
            Rectangle {
                objectName: "diagnosticsMemoryCard"
                width: column.width
                height: memorySectionColumn.implicitHeight + 24 // p-3
                radius: 8 // rounded-lg (logs-page-memory.ui.tsx:71)
                color: page.colors["card-bg"]
                border.color: page.colors["border-subtle"]
                border.width: 1
                Column {
                    id: memorySectionColumn
                    x: 12 // p-3
                    y: 12
                    width: parent.width - 24
                    spacing: 8 // space-y-2 (logs-page-memory.ui.tsx:85)

                    // Heading + inline loading marker (logs-page-memory.ui.tsx:74-83).
                    Row {
                        spacing: 8 // flex items-center gap-2
                        Text {
                            objectName: "diagnosticsMemoryTitle"
                            text: qsTr("Memory")
                            font.pixelSize: 12 // text-xs
                            font.weight: Font.Medium // font-medium
                            color: page.colors["text-primary"]
                        }
                        Text {
                            objectName: "diagnosticsMemoryLoadingMarker"
                            visible: page.memoryLoading
                            text: "\u2026"
                            font.pixelSize: 11 // text-[11px]
                            color: page.colors["text-muted"]
                        }
                    }

                    // Unavailable state (logs-page-memory.ui.tsx:86-90): the
                    // text replaces the metrics and the header stays.
                    Text {
                        objectName: "diagnosticsMemoryUnavailableText"
                        visible: page.memory !== null && !page.memoryLoading
                                 && !page.memoryHasAnyMetric
                        text: qsTr("Memory metrics are not available in this build.")
                        width: memorySectionColumn.width
                        wrapMode: Text.WordWrap
                        font.pixelSize: 11
                        lineHeight: 15
                        lineHeightMode: Text.FixedHeight
                        color: page.colors["text-muted"]
                    }

                    Column {
                        objectName: "diagnosticsMemoryMetricsBlock"
                        visible: page.memoryHasAnyMetric
                        width: memorySectionColumn.width
                        spacing: 8

                        // Metric cards: grid gap-2 sm:grid-cols-2 lg:grid-cols-3
                        // (logs-page-memory.ui.tsx:96; Tailwind sm=640 lg=1024).
                        Grid {
                            objectName: "diagnosticsMemoryMetricGrid"
                            width: memorySectionColumn.width
                            columns: page.memoryGridColumns
                            spacing: 8 // gap-2
                            Repeater {
                                model: page.memoryCards()
                                delegate: Rectangle {
                                    required property var modelData
                                    objectName: "diagnosticsMemoryMetric-" + modelData.key
                                    width: (memorySectionColumn.width
                                            - 8 * (page.memoryGridColumns - 1))
                                           / page.memoryGridColumns
                                    height: metricColumn.implicitHeight + 12 // py-1.5
                                    radius: 6 // rounded-md (logs-page-memory.ui.tsx:218)
                                    color: page.colors["bg"]
                                    border.color: page.colors["border-subtle"]
                                    border.width: 1
                                    Column {
                                        id: metricColumn
                                        x: 8 // px-2
                                        y: 6 // py-1.5
                                        width: parent.width - 16
                                        Text {
                                            objectName: "diagnosticsMemoryMetric-"
                                                + modelData.key + "-label"
                                            text: modelData.label
                                            font.pixelSize: 11 // text-[11px]
                                            color: page.colors["text-muted"]
                                        }
                                        Text {
                                            objectName: "diagnosticsMemoryMetric-"
                                                + modelData.key + "-value"
                                            text: modelData.value
                                            font.pixelSize: 14 // text-sm
                                            font.weight: Font.Medium // font-medium
                                            color: page.colors["text-primary"]
                                        }
                                        Text {
                                            objectName: "diagnosticsMemoryMetric-"
                                                + modelData.key + "-detail"
                                            text: modelData.detail
                                            visible: modelData.detail !== ""
                                            font.pixelSize: 11 // text-[11px]
                                            color: page.colors["text-muted"]
                                        }
                                    }
                                }
                            }
                        }

                        // Progress bar (logs-page-memory.ui.tsx:126-146): label
                        // row text-[11px], track h-1.5 rounded-full bg-bg, fill
                        // h-full rounded-full bg-accent width = percent.
                        Column {
                            objectName: "diagnosticsMemoryBarBlock"
                            visible: page.memoryUsagePercent >= 0
                            width: memorySectionColumn.width
                            spacing: 4 // mb-1
                            Item {
                                width: memorySectionColumn.width
                                height: 15
                                Text {
                                    anchors.left: parent.left
                                    text: qsTr("Memory usage")
                                    font.pixelSize: 11
                                    color: page.colors["text-muted"]
                                }
                                Text {
                                    objectName: "diagnosticsMemoryBarPercent"
                                    anchors.right: parent.right
                                    text: page.memoryUsagePercent >= 0
                                          ? page.memoryUsagePercent + "%" : ""
                                    font.pixelSize: 11
                                    color: page.colors["text-muted"]
                                }
                            }
                            Rectangle {
                                id: barTrack
                                objectName: "diagnosticsMemoryBarTrack"
                                width: memorySectionColumn.width
                                height: 6 // h-1.5
                                radius: 3 // rounded-full
                                color: page.colors["bg"]
                                // role=progressbar aria-valuemin 0 aria-valuemax 100
                                // (logs-page-memory.ui.tsx:131-135); the integer
                                // percent is exposed by the label row and the
                                // fill ratio (this Qt build has no
                                // Accessible.value attached property).
                                Accessible.role: Accessible.ProgressBar
                                Accessible.name: qsTr("Memory usage")
                                Rectangle {
                                    objectName: "diagnosticsMemoryBarFill"
                                    width: barTrack.width * page.memoryUsagePercent / 100
                                    height: parent.height
                                    radius: 3 // rounded-full
                                    color: page.colors["accent"]
                                }
                            }
                        }

                        // Process table (logs-page-memory.ui.tsx:160-197): the
                        // declared deviation renders the single Main row with
                        // the reference five columns, cells font-mono px-1 py-1.
                        Column {
                            objectName: "diagnosticsMemoryProcessesBlock"
                            visible: page.memoryProcesses().length > 0
                            width: memorySectionColumn.width
                            spacing: 0
                            Text {
                                objectName: "diagnosticsMemoryProcessesLabel"
                                text: qsTr("Processes")
                                font.pixelSize: 11 // text-[11px] font-medium
                                font.weight: Font.Medium
                                color: page.colors["text-primary"]
                                bottomPadding: 4 // mb-1
                            }
                            Row {
                                objectName: "diagnosticsMemoryTableHeader"
                                width: memorySectionColumn.width
                                Text { objectName: "diagnosticsMemoryTableHeader-type";
                                    text: qsTr("Type"); width: 96
                                    leftPadding: 4; rightPadding: 4; topPadding: 4; bottomPadding: 4
                                    font.pixelSize: 11; font.weight: Font.Medium
                                    color: page.colors["text-primary"] }
                                Text { objectName: "diagnosticsMemoryTableHeader-pid";
                                    text: qsTr("PID"); width: 64
                                    leftPadding: 4; rightPadding: 4; topPadding: 4; bottomPadding: 4
                                    font.pixelSize: 11; font.weight: Font.Medium
                                    color: page.colors["text-primary"] }
                                Text { objectName: "diagnosticsMemoryTableHeader-workingSet";
                                    text: qsTr("Working set"); width: 96
                                    leftPadding: 4; rightPadding: 4; topPadding: 4; bottomPadding: 4
                                    font.pixelSize: 11; font.weight: Font.Medium
                                    color: page.colors["text-primary"] }
                                Text { objectName: "diagnosticsMemoryTableHeader-peak";
                                    text: qsTr("Peak"); width: 96
                                    leftPadding: 4; rightPadding: 4; topPadding: 4; bottomPadding: 4
                                    font.pixelSize: 11; font.weight: Font.Medium
                                    color: page.colors["text-primary"] }
                                Text { objectName: "diagnosticsMemoryTableHeader-cpu";
                                    text: qsTr("CPU")
                                    width: Math.max(56, memorySectionColumn.width - 256)
                                    leftPadding: 4; rightPadding: 4; topPadding: 4; bottomPadding: 4
                                    font.pixelSize: 11; font.weight: Font.Medium
                                    color: page.colors["text-primary"] }
                            }
                            Rectangle { // border-b border-border-subtle header rule
                                width: memorySectionColumn.width
                                height: 1
                                color: page.colors["border-subtle"]
                            }
                            Repeater {
                                model: page.memoryProcesses()
                                delegate: Column {
                                    required property var modelData
                                    required property int index
                                    objectName: "diagnosticsMemoryProcessRow-" + index
                                    width: memorySectionColumn.width
                                    spacing: 0
                                    Row {
                                        width: memorySectionColumn.width
                                        Text {
                                            objectName: "diagnosticsMemoryProcessCell" + index + "-type"
                                            text: String(modelData.type)
                                            width: 96
                                            leftPadding: 4; rightPadding: 4; topPadding: 4; bottomPadding: 4
                                            font.pixelSize: 11; font.family: "monospace"
                                            color: page.colors["text-primary"]
                                        }
                                        Text {
                                            objectName: "diagnosticsMemoryProcessCell" + index + "-pid"
                                            text: String(modelData.pid)
                                            width: 64
                                            leftPadding: 4; rightPadding: 4; topPadding: 4; bottomPadding: 4
                                            font.pixelSize: 11; font.family: "monospace"
                                            color: page.colors["text-muted"]
                                        }
                                        Text {
                                            objectName: "diagnosticsMemoryProcessCell" + index + "-workingSet"
                                            text: page.formatKilobytes(Number(modelData.workingSetKb))
                                            width: 96
                                            leftPadding: 4; rightPadding: 4; topPadding: 4; bottomPadding: 4
                                            font.pixelSize: 11; font.family: "monospace"
                                            color: page.colors["text-muted"]
                                        }
                                        Text {
                                            objectName: "diagnosticsMemoryProcessCell" + index + "-peak"
                                            text: page.formatKilobytes(Number(modelData.peakWorkingSetKb))
                                            width: 96
                                            leftPadding: 4; rightPadding: 4; topPadding: 4; bottomPadding: 4
                                            font.pixelSize: 11; font.family: "monospace"
                                            color: page.colors["text-muted"]
                                        }
                                        Text {
                                            objectName: "diagnosticsMemoryProcessCell" + index + "-cpu"
                                            text: page.formatMemoryCpuPercent(modelData.cpuPercent)
                                            width: Math.max(56, memorySectionColumn.width - 256)
                                            leftPadding: 4; rightPadding: 4; topPadding: 4; bottomPadding: 4
                                            font.pixelSize: 11; font.family: "monospace"
                                            color: page.colors["text-muted"]
                                        }
                                    }
                                    Rectangle { // border-border-subtle/60 row rule
                                        width: memorySectionColumn.width
                                        height: 1
                                        color: page.colors["border-subtle"]
                                        opacity: 0.6
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Status pill + Ping row (logs-page-overview.ui.tsx:40-64).
            Row {
                objectName: "diagnosticsOverviewRow"
                spacing: 8
                Rectangle {
                    id: statusPill
                    objectName: "diagnosticsStatusPill"
                    width: pillRow.implicitWidth + 20 // px-2.5
                    height: pillRow.implicitHeight + 8 // py-1
                    radius: height / 2 // rounded-full
                    color: page.colors["bg"]
                    border.color: page.colors["border-subtle"]
                    border.width: 1
                    Row {
                        id: pillRow
                        objectName: "diagnosticsStatusPillRow"
                        anchors.centerIn: parent
                        spacing: 6 // gap-1.5
                        Rectangle {
                            id: statusDot
                            objectName: "diagnosticsStatusDot"
                            anchors.verticalCenter: parent.verticalCenter
                            width: 8 // h-2 w-2
                            height: 8
                            radius: 4
                            color: page.statusColor(String((page.snapshot || {}).overallStatus || "healthy"))
                        }
                        Text {
                            id: statusText
                            objectName: "diagnosticsStatusText"
                            anchors.verticalCenter: parent.verticalCenter
                            text: page.statusLabel(String((page.snapshot || {}).overallStatus || "healthy"))
                            font.pixelSize: 12
                            font.weight: Font.Medium
                            color: page.colors["text-primary"]
                        }
                    }
                }
                Rectangle {
                    id: pingButton
                    objectName: "diagnosticsPingButton"
                    width: pingButtonLabel.implicitWidth + 16 // px-2
                    height: 28 // h-7
                    radius: 6
                    color: page.colors["card-bg"]
                    border.color: page.colors["border-subtle"]
                    border.width: 1
                    opacity: page.pingState === "loading" ? 0.5 : 1.0 // disabled:opacity-50
                    Text {
                        id: pingButtonLabel
                        objectName: "diagnosticsPingLabel"
                        anchors.centerIn: parent
                        text: page.pingLabel
                        font.pixelSize: 12
                        color: page.colors["text-primary"]
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: page.pingState !== "loading"
                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: {
                            // Issue #50 review 4611502346: stop the previous
                            // ping's feedback reset first, otherwise the stale
                            // timer clears this ping's loading state mid-flight.
                            pingReset.stop();
                            page.pingState = "loading";
                            page.pingLatencyMs = -1;
                            page.controller.pingServer();
                        }
                    }
                }
            }

            // Nine-card grid (logs-page-overview.ui.tsx:66-208).
            Grid {
                objectName: "diagnosticsCardGrid"
                width: column.width
                columns: page.gridColumns
                spacing: page.gridSpacing
                Repeater {
                    model: page.cards
                    delegate: Rectangle {
                        id: card
                        required property var modelData
                        readonly property string cardKey: String(modelData.key)
                        objectName: "diagnosticsCard-" + cardKey
                        width: page.cellWidth
                        height: cardColumn.height + 24 // p-3
                        radius: 8 // rounded-lg
                        color: page.colors["card-bg"]
                        border.color: page.colors["border-subtle"]
                        border.width: 1
                        Column {
                            id: cardColumn
                            x: 12
                            y: 12
                            width: parent.width - 24
                            spacing: 2 // space-y-0.5
                            Text {
                                objectName: "diagnosticsCardTitle-" + card.cardKey
                                text: page.cardTitle(card.cardKey)
                                font.pixelSize: 12 // text-xs
                                font.weight: Font.Medium
                                color: page.colors["text-primary"]
                                bottomPadding: 4 // mb-1.5 (6px total incl. body spacing)
                            }
                            Repeater {
                                model: card.modelData.kind === "logcounts" ? [] : card.modelData.lines
                                delegate: Text {
                                    required property var modelData
                                    required property int index
                                    objectName: "diagnosticsCardLine-" + card.cardKey + "-" + index
                                    text: String(modelData)
                                    font.pixelSize: 11 // text-[11px]
                                    lineHeight: 15 // leading-snug
                                    lineHeightMode: Text.FixedHeight
                                    font.weight: index === 0 ? Font.Medium : Font.Normal
                                    color: index === 0 ? page.colors["text-primary"]
                                                       : page.colors["text-muted"]
                                    wrapMode: Text.Wrap
                                    width: parent ? parent.width : 0
                                }
                            }
                            Row {
                                visible: card.modelData.kind === "logcounts"
                                spacing: 6 // gap-1.5
                                Repeater {
                                    model: card.modelData.kind === "logcounts"
                                        ? card.modelData.badges : []
                                    delegate: Rectangle {
                                        required property var modelData
                                        objectName: "diagnosticsLogBadge-" + String(modelData.level)
                                        width: badgeLabel.implicitWidth + 12 // px-1.5
                                        height: badgeLabel.implicitHeight + 4 // py-0.5
                                        radius: 4
                                        color: page.colors["bg"]
                                        Text {
                                            id: badgeLabel
                                            anchors.centerIn: parent
                                            text: String(modelData.level).toUpperCase() + ": " + modelData.count
                                            font.pixelSize: 11
                                            font.family: "monospace"
                                            color: page.colors["text-muted"]
                                        }
                                    }
                                }
                                // Log counts card "View logs" button
                                // (logs-page-overview.ui.tsx:143-149): h-7 rounded-md
                                // bg-bg px-2 text-[11px] opening the logs modal.
                                Rectangle {
                                    objectName: "diagnosticsCardLogCountsViewButton"
                                    visible: card.modelData.kind === "logcounts"
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: cardViewLogsLabel.implicitWidth + 16 // px-2
                                    height: 28 // h-7
                                    radius: 6 // rounded-md
                                    color: page.colors["bg"]
                                    Text {
                                        id: cardViewLogsLabel
                                        anchors.centerIn: parent
                                        text: qsTr("View logs")
                                        font.pixelSize: 11 // text-[11px]
                                        color: page.colors["text-primary"]
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: page.openLogsModal()
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Collapsible advanced snapshot (logs-page.ui.tsx:325-334).
            Rectangle {
                objectName: "diagnosticsAdvancedCard"
                width: column.width
                height: advancedColumn.height + 16 // p-2
                radius: 8
                color: page.colors["card-bg"]
                border.color: page.colors["border-subtle"]
                border.width: 1
                Column {
                    id: advancedColumn
                    x: 8
                    y: 8
                    width: parent ? parent.width - 16 : 0
                    spacing: 8 // mt-2
                    Item {
                        width: parent.width
                        height: advancedToggle.implicitHeight
                        Text {
                            id: advancedToggle
                            objectName: "diagnosticsAdvancedToggle"
                            text: (page.advancedExpanded ? "\u25be " : "\u25b8 ") + qsTr("Advanced snapshot")
                            font.pixelSize: 12
                            font.weight: Font.Medium
                            color: page.colors["text-primary"]
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: page.advancedExpanded = !page.advancedExpanded
                            }
                        }
                    }
                    Flickable {
                        objectName: "diagnosticsSnapshotPre"
                        visible: page.advancedExpanded
                        width: parent ? parent.width : 0
                        height: Math.min(208, snapshotPreText.height + 12) // max-h-52
                        clip: true
                        contentWidth: snapshotPreText.width + 12
                        contentHeight: snapshotPreText.height + 12
                        ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        Rectangle {
                            anchors.fill: parent
                            radius: 4 // rounded
                            color: page.colors["bg"]
                        }
                        Text {
                            id: snapshotPreText
                            x: 6 // p-1.5
                            y: 6
                            text: page.snapshotJson
                            font.pixelSize: 11
                            lineHeight: 15
                            lineHeightMode: Text.FixedHeight
                            color: page.colors["text-muted"]
                        }
                    }
                }
            }
        }
    }

    // ---- Issue #51: session log viewer modal ------------------------------
    // logs-view-modal.ui.tsx at 61dacef78: dialog content max-h-[85vh]
    // max-w-4xl rounded-xl border-border-subtle bg-card-bg p-0 over a
    // bg-black/50 overlay; header px-4 py-3 (title text-sm font-semibold +
    // subtitle text-[11px]) with Refresh / Clear / Download h-8 buttons
    // (Clear and Download disabled while there are no entries,
    // logs-view-modal.ui.tsx:70-103) and the 18 px close icon. Body p-3 pt-2
    // hosts the LogsPageLogsSection (filter row h-8, results label,
    // recent-errors block and the entry rows measured in the docs).
    Popup {
        id: logsModal
        objectName: "logsModal"
        parent: Overlay.overlay
        anchors.centerIn: Overlay.overlay
        modal: true
        dim: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        visible: false
        padding: 0
        width: Math.min(896, (parent ? parent.width : 896) - 32) // max-w-4xl
        height: Math.min((parent ? parent.height : 800) * 0.85, 600) // max-h-[85vh]

        background: Rectangle {
            radius: 12 // rounded-xl
            color: page.colors["card-bg"]
            border.color: page.colors["border-subtle"]
            border.width: 1
        }

        onOpened: {
            // A fresh open starts from a clean slate, including a stale export
            // error from the previous session of the modal.
            page.logsExportState = "idle"
            page.refreshLogs()
        }

        // Popup children live inside the popup's internal content item;
        // the reference layout is reproduced inside an explicit contentItem so
        // children can anchor to their parent like every other popup in the
        // client (NativeMediaViewer).
        contentItem: Item {
            id: logsModalContent

        // Header (logs-view-modal.ui.tsx:47-104).
        Rectangle {
            id: logsModalHeader
            objectName: "logsModalHeader"
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: logsModalHeaderRow.implicitHeight + 24 // py-3
            color: "transparent"
            Rectangle { // border-b border-border-subtle
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: page.colors["border-subtle"]
            }
            Row {
                id: logsModalHeaderRow
                x: 16 // px-4
                y: 12
                width: parent.width - 32
                spacing: 8 // gap-2
                Column {
                    width: Math.max(120, logsModalHeaderRow.width - logsModalActions.implicitWidth - 8)
                    spacing: 2 // mt-0.5
                    Text {
                        objectName: "logsModalTitle"
                        text: qsTr("Logs")
                        font.pixelSize: 14 // text-sm
                        font.weight: Font.DemiBold // font-semibold
                        color: page.colors["text-primary"]
                        elide: Text.ElideRight
                        width: parent.width
                    }
                    Text {
                        objectName: "logsModalSubtitle"
                        text: qsTr("In-memory session logs (last 500 entries).")
                        font.pixelSize: 11 // text-[11px]
                        color: page.colors["text-muted"]
                        elide: Text.ElideRight
                        width: parent.width
                    }
                }
                Row {
                    id: logsModalActions
                    spacing: 4 // gap-1
                    HeaderButton {
                        objectName: "logsRefreshButton"
                        buttonLabel: qsTr("Refresh")
                        onActioned: page.refreshLogs()
                    }
                    HeaderButton {
                        objectName: "logsClearButton"
                        buttonLabel: qsTr("Clear logs")
                        enabled: page.logEntries.length > 0
                        onActioned: page.controller.clearSessionLog()
                    }
                    HeaderButton {
                        objectName: "logsExportButton"
                        buttonLabel: qsTr("Export logs")
                        enabled: page.logEntries.length > 0
                        onActioned: {
                            // StandardPaths.writableLocation already yields
                            // a file:// url, so no manual scheme prefixing.
                            const base = StandardPaths.writableLocation(StandardPaths.DownloadLocation)
                                || StandardPaths.writableLocation(StandardPaths.HomeLocation);
                            const name = page.controller.diagnosticsLogsExportFilename();
                            logsSaveDialog.currentFolder = base
                            logsSaveDialog.currentFile = base + "/" + name
                            logsSaveDialog.open()
                        }
                    }
                    // Close icon button (Icon name="close" size 18, aria-label
                    // common.close).
                    Rectangle {
                        objectName: "logsCloseButton"
                        width: 26 // p-1 around the 18 px glyph
                        height: 26
                        radius: 4
                        color: logsCloseMouseArea.containsMouse
                            ? page.colors["bg"] : "transparent"
                        Image {
                            anchors.centerIn: parent
                            source: "qrc:/workspace/icons/close.svg"
                            sourceSize.width: 18 // size={18}
                            sourceSize.height: 18
                            asynchronous: false
                            layer.enabled: true
                            layer.effect: MultiEffect {
                                colorizationColor: page.colors["text-muted"]
                                colorization: 1.0
                            }
                        }
                        MouseArea {
                            id: logsCloseMouseArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: logsModal.close()
                        }
                        Accessible.name: qsTr("Close")
                    }
                }
            }
        }

        // Filter row (logs-page-logs.ui.tsx:110-168): h-8 controls, search +
        // source/level/scope/runtime compact selects (min-w-[110px]) + Reset.
        // The reference row is overflow-x-auto (review 4616699176 on #411):
        // when the popup is narrower than the row the controls stay reachable
        // through horizontal scrolling instead of clipping past the popup edge.
        Flickable {
            id: logsFilterScroll
            objectName: "logsFilterScroll"
            anchors.top: logsModalHeader.bottom
            anchors.topMargin: 8 // pt-2
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 12 // p-3
            anchors.rightMargin: 12
            height: logsFilterRow.implicitHeight
            contentWidth: logsFilterRow.implicitWidth
            flickableDirection: Flickable.HorizontalFlick
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
            Row {
                id: logsFilterRow
                objectName: "logsFilterRow"
                spacing: 4 // gap-1
                Accessible.name: qsTr("Search logs")
                TextField {
                id: logsSearchField
                objectName: "logsSearchField"
                width: 220 // min-w-context-menu-narrow
                height: 32 // h-8
                placeholderText: qsTr("Search message, scope, runtime, or payload…")
                font.pixelSize: 12 // text-xs
                color: page.colors["text-primary"]
                leftPadding: 8 // px-2
                rightPadding: 8
                verticalAlignment: Text.AlignVCenter
                Accessible.name: qsTr("Search logs")
                onTextChanged: {
                    page.logsSearchQuery = text
                    page.refreshLogs()
                }
                background: Rectangle {
                    radius: 6 // rounded-md
                    color: page.colors["bg"]
                    border.color: logsSearchField.activeFocus
                        ? page.colors["accent"] : page.colors["border-subtle"] // focus:border-accent
                    border.width: 1
                }
            }
            NativeSelect {
                id: logsSourceFilterSelect
                objectName: "logsSourceFilterSelect"
                colors: page.colors
                implicitHeight: 32 // h-8 compact select
                width: Math.max(110, implicitContentWidth + 44) // min-w-[110px]
                leftPadding: 8
                rightPadding: 28
                placeholderText: qsTr("All sources")
                Accessible.name: qsTr("Log source")
                model: [
                    { id: "all", displayLabel: qsTr("All sources") },
                    { id: "api", displayLabel: qsTr("API") },
                    { id: "actions", displayLabel: qsTr("Actions") },
                    { id: "console", displayLabel: qsTr("Console") },
                    { id: "trace", displayLabel: qsTr("Pipeline traces") },
                    { id: "app", displayLabel: qsTr("App") }
                ]
                onCurrentValueChanged: {
                    page.logsSourceFilter = currentValue === undefined || currentText.length === 0
                        ? "all" : String(currentValue)
                    page.refreshLogs()
                }
            }
            NativeSelect {
                id: logsLevelFilterSelect
                objectName: "logsLevelFilterSelect"
                colors: page.colors
                implicitHeight: 32
                width: Math.max(110, implicitContentWidth + 44)
                leftPadding: 8
                rightPadding: 28
                placeholderText: qsTr("All levels")
                Accessible.name: qsTr("Log level")
                model: [
                    { id: "all", displayLabel: qsTr("All levels") },
                    { id: "debug", displayLabel: qsTr("Debug") },
                    { id: "info", displayLabel: qsTr("Info") },
                    { id: "warn", displayLabel: qsTr("Warn") },
                    { id: "error", displayLabel: qsTr("Error") }
                ]
                onCurrentValueChanged: {
                    page.logsLevelFilter = currentValue === undefined || currentText.length === 0
                        ? "all" : String(currentValue)
                    page.refreshLogs()
                }
            }
            NativeSelect {
                id: logsScopeFilterSelect
                objectName: "logsScopeFilterSelect"
                colors: page.colors
                implicitHeight: 32
                width: Math.max(110, implicitContentWidth + 44)
                leftPadding: 8
                rightPadding: 28
                placeholderText: qsTr("All scopes")
                Accessible.name: qsTr("Log scope")
                model: [{ id: "all", displayLabel: qsTr("All scopes") }].concat(
                    page.logsScopeOptions.map(function (scope) {
                        return { id: scope, displayLabel: scope }
                    }))
                onCurrentValueChanged: {
                    page.logsScopeFilter = currentValue === undefined || currentText.length === 0
                        ? "all" : String(currentValue)
                    page.refreshLogs()
                }
            }
            NativeSelect {
                id: logsRuntimeFilterSelect
                objectName: "logsRuntimeFilterSelect"
                colors: page.colors
                implicitHeight: 32
                width: Math.max(110, implicitContentWidth + 44)
                leftPadding: 8
                rightPadding: 28
                placeholderText: qsTr("All runtimes")
                Accessible.name: qsTr("Runtime")
                model: [{ id: "all", displayLabel: qsTr("All runtimes") }].concat(
                    page.logsRuntimeOptions.map(function (runtime) {
                        return { id: runtime, displayLabel: runtime }
                    }))
                onCurrentValueChanged: {
                    page.logsRuntimeFilter = currentValue === undefined || currentText.length === 0
                        ? "all" : String(currentValue)
                    page.refreshLogs()
                }
            }
            HeaderButton {
                objectName: "logsResetFiltersButton"
                buttonLabel: qsTr("Reset filters")
                onActioned: page.resetLogFilters()
            }
            }
        }

        // Results label "visible of total entries" (text-[11px]).
        Text {
            id: logsResultsLabel
            objectName: "logsResultsLabel"
            anchors.top: logsFilterScroll.bottom
            anchors.topMargin: 6 // gap-1.5
            anchors.left: parent.left
            anchors.leftMargin: 12
            text: qsTr("%1 of %2 entries").arg(page.logsRows.length).arg(page.logEntries.length)
            font.pixelSize: 11
            color: page.colors["text-muted"]
        }

        // Export failure feedback (review 4616699201 on #411): the save
        // handler used to discard exportSessionLog's result, so an unwritable
        // destination left the user without a file or an error. The inline
        // red line follows the page's existing inline-state pattern (the ping
        // / copy feedback states) and clears on the next attempt or reopen.
        //
        // Review 4617359914 on #411: the line was anchored to the right of the
        // results label with no right bound and no wrapping, so its recovery
        // instructions ran past the popup edge at narrow widths. It now flows
        // on its own row below the results label, bounded by the popup content
        // width with the same 12 px side margins the neighbouring blocks use,
        // and wraps instead of growing sideways.
        Text {
            id: logsExportErrorText
            objectName: "logsExportError"
            visible: page.logsExportState === "error"
            anchors.top: logsResultsLabel.bottom
            anchors.topMargin: visible ? 4 : 0
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            text: qsTr("The logs could not be exported to the selected location. Pick another folder and try again.")
            font.pixelSize: 11 // text-[11px]
            color: page.colors["indicator-red"]
            wrapMode: Text.Wrap // wraps within the popup, so the row grows in height
        }

        // "Recent errors" block (open by default, only when present):
        // rounded-lg border card with a max-h-40 inner list.
        Rectangle {
            id: logsRecentErrorsBlock
            objectName: "logsRecentErrorsBlock"
            visible: page.logsRecentErrors.length > 0
            // Flows below the export-error row when it is showing, so the
            // wrapped error line (review 4617359914) gets its own height.
            anchors.top: logsExportErrorText.visible ? logsExportErrorText.bottom : logsResultsLabel.bottom
            anchors.topMargin: visible ? 6 : 0
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            radius: 8 // rounded-lg
            color: page.colors["card-bg"]
            border.color: page.colors["border-subtle"]
            border.width: 1
            height: visible ? logsRecentErrorsColumn.height + 16 : 0 // p-2
            Column {
                id: logsRecentErrorsColumn
                x: 8
                y: 8
                width: logsRecentErrorsBlock.width - 16
                spacing: 8 // mt-2
                Text {
                    objectName: "logsRecentErrorsTitle"
                    text: qsTr("Recent errors")
                    font.pixelSize: 12 // text-xs font-medium
                    font.weight: Font.Medium
                    color: page.colors["text-primary"]
                }
                Flickable {
                    width: parent.width
                    height: Math.min(160, logsRecentErrorsList.height) // max-h-40
                    clip: true
                    contentHeight: logsRecentErrorsList.height
                    Column {
                        id: logsRecentErrorsList
                        width: parent ? parent.width : 0
                        spacing: 4 // space-y-1
                        Repeater {
                            model: page.logsRecentErrors
                            delegate: LogEntryRow {
                                required property var modelData
                                required property int index
                                objectName: "logsRecentErrorRow-" + index
                                entry: modelData
                                rowColors: page.colors
                                emphasize: true
                            }
                        }
                    }
                }
            }
        }

        // Entry list / empty state (both fill the remaining body, p-3 bottom).
        Flickable {
            id: logsEntryListArea
            objectName: "logsEntryList"
            visible: page.logsRows.length > 0
            anchors.top: logsRecentErrorsBlock.bottom
            anchors.topMargin: 6
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            anchors.bottomMargin: 12
            clip: true
            contentHeight: logsEntryColumn.height
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            Accessible.name: qsTr("Session log entries")
            Column {
                id: logsEntryColumn
                width: parent ? parent.width : 0 // the Flickable body
                spacing: 4 // gap-1
                Repeater {
                    model: page.logsRows
                    delegate: LogEntryRow {
                        required property var modelData
                        required property int index
                        entry: modelData
                        rowColors: page.colors
                        rowIndex: index
                    }
                }
            }
        }
        Rectangle {
            id: logsEmptyState
            objectName: "logsEmptyState"
            visible: page.logsRows.length === 0
            anchors.top: logsRecentErrorsBlock.bottom
            anchors.topMargin: 6
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            anchors.bottomMargin: 12
            radius: 8 // rounded-lg
            color: "transparent"
            border.color: page.colors["border-subtle"]
            border.width: 1
            Column {
                anchors.centerIn: parent
                spacing: 8 // gap-2
                Image {
                    anchors.horizontalCenter: parent.horizontalCenter
                    source: "qrc:/workspace/icons/grid.svg" // Icon name="grid" size 32
                    sourceSize.width: 32
                    sourceSize.height: 32
                    asynchronous: false
                    opacity: 0.5
                    layer.enabled: true
                    layer.effect: MultiEffect {
                        colorizationColor: page.colors["text-muted"]
                        colorization: 1.0
                    }
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("No logs yet")
                    font.pixelSize: 12
                    color: page.colors["text-muted"]
                }
            }
        }
        }
    }

    // Save-as dialog for the JSON export (the native save flow: reference
    // downloadLogsAsFile is replaced by the client's FileDialog + file write).
    NativeDialogs.FileDialog {
        id: logsSaveDialog
        objectName: "diagnosticsLogsSaveDialog"
        title: qsTr("Export logs")
        fileMode: NativeDialogs.FileDialog.SaveFile
        defaultSuffix: "json"
        nameFilters: ["JSON (*.json)"]
        onAccepted: {
            // Review 4616699201 on #411: the export result must not be
            // discarded - a failed write shows the inline error line next to
            // the results label; success clears it again.
            page.logsExportState = page.controller.exportSessionLog(logsSaveDialog.selectedFile)
                ? "idle" : "error"
        }
    }

    // One log row (logs-page-logs.ui.tsx:216-245): rounded-md border p-2 with
    // the meta line (level + runtime chips, scope, locale timestamp), the
    // message and the optional data pre. warn/error rows get the 2 px left
    // accent (indicator-yellow / indicator-red).
    component LogEntryRow: Rectangle {
        id: logRow
        required property var entry
        required property var rowColors
        property bool emphasize: false
        property int rowIndex: 0
        objectName: "logsEntryRow-" + rowIndex
        width: parent ? parent.width : 0
        height: logRowColumn.height + 16 // p-2
        radius: 6 // rounded-md
        color: "transparent"
        border.color: rowColors["border-subtle"]
        border.width: 1
        readonly property string entryLevel: String(entry.level)
        Rectangle { // border-l-2 accent
            visible: logRow.entryLevel === "error" || logRow.entryLevel === "warn"
                || logRow.emphasize
            x: 0
            y: 0
            width: 2
            height: parent.height
            color: logRow.entryLevel === "error" ? rowColors["indicator-red"]
                : logRow.entryLevel === "warn" ? rowColors["indicator-yellow"]
                    : rowColors["border-subtle"]
        }
        Column {
            id: logRowColumn
            x: 8 // p-2
            y: 8
            width: logRow.width - 16
            spacing: 4 // mt-1 lines
            Row {
                spacing: 4 // gap-1
                Rectangle { // level chip rounded bg-bg px-1.5 py-0.5 font-mono uppercase
                    anchors.verticalCenter: parent.verticalCenter
                    width: logRowLevelLabel.implicitWidth + 12
                    height: logRowLevelLabel.implicitHeight + 4
                    radius: 4
                    color: rowColors["bg"]
                    Text {
                        id: logRowLevelLabel
                        anchors.centerIn: parent
                        text: logRow.entryLevel.toUpperCase()
                        font.pixelSize: 11
                        font.family: "monospace"
                        color: rowColors["text-muted"]
                    }
                }
                Rectangle { // runtime chip (same shape, not upper-cased)
                    anchors.verticalCenter: parent.verticalCenter
                    width: logRowRuntimeLabel.implicitWidth + 12
                    height: logRowRuntimeLabel.implicitHeight + 4
                    radius: 4
                    color: rowColors["bg"]
                    Text {
                        id: logRowRuntimeLabel
                        anchors.centerIn: parent
                        text: String(logRow.entry.runtime)
                        font.pixelSize: 11
                        font.family: "monospace"
                        color: rowColors["text-muted"]
                    }
                }
                Text { // scope font-medium text-text-primary
                    anchors.verticalCenter: parent.verticalCenter
                    text: String(logRow.entry.scope)
                    font.pixelSize: 11
                    font.weight: Font.Medium
                    color: rowColors["text-primary"]
                }
                Text { // locale timestamp (toLocaleString)
                    anchors.verticalCenter: parent.verticalCenter
                    text: page.logTimestamp(logRow.entry.timestamp)
                    font.pixelSize: 11
                    color: rowColors["text-muted"]
                }
            }
            Text { // message text-xs leading-snug
                width: logRowColumn.width
                text: String(logRow.entry.message)
                font.pixelSize: 12
                lineHeight: 17 // leading-snug
                lineHeightMode: Text.FixedHeight
                wrapMode: Text.Wrap
                color: rowColors["text-primary"]
            }
            // Optional data pre (JSON.stringify(data)): overflow-x-auto like
            // the reference pre (review 4616699185 on #411) - the full payload
            // stays laid out, horizontally scrollable and selectable instead
            // of eliding at the row width.
            Rectangle {
                objectName: "logsEntryDataPre-" + logRow.rowIndex
                visible: logRow.entry.data !== undefined
                width: logRowColumn.width
                height: visible ? 24 : 0
                radius: 4 // rounded
                color: rowColors["bg"]
                clip: true
                Flickable {
                    id: logsEntryDataScroll
                    objectName: "logsEntryDataScroll-" + logRow.rowIndex
                    anchors.fill: parent
                    anchors.leftMargin: 6 // p-1.5
                    anchors.rightMargin: 6
                    contentWidth: Math.max(logsEntryDataText.implicitWidth, width)
                    flickableDirection: Flickable.HorizontalFlick
                    boundsBehavior: Flickable.StopAtBounds
                    clip: true
                    ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
                    Text {
                        id: logsEntryDataText
                        objectName: "logsEntryDataText-" + logRow.rowIndex
                        width: implicitWidth
                        height: logsEntryDataScroll.height
                        verticalAlignment: Text.AlignVCenter
                        text: logRow.entry.data === undefined
                            ? "" : JSON.stringify(logRow.entry.data)
                        font.pixelSize: 11
                        color: rowColors["text-muted"]
                    }
                }
            }
        }
    }
}
