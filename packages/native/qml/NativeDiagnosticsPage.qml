pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

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
    }

    Timer {
        id: liveRefresh
        interval: 120
        onTriggered: if (page.visible) page.refresh()
    }

    onVisibleChanged: if (visible) page.refresh()
    Component.onCompleted: if (visible) page.refresh()

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
        Text {
            id: headerButtonLabel
            anchors.centerIn: parent
            text: headerButton.buttonLabel
            font.pixelSize: 12 // text-xs
            color: page.colors["text-primary"]
        }
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
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

            // Memory block from the accepted #77 provider (reference
            // logs-page-memory.ui.tsx section in the compact native shape:
            // total app memory + system free from the /proc snapshot).
            Rectangle {
                objectName: "diagnosticsMemoryCard"
                width: column.width
                height: memoryColumn.height + 24 // p-3
                radius: 8
                color: page.colors["card-bg"]
                border.color: page.colors["border-subtle"]
                border.width: 1
                Column {
                    id: memoryColumn
                    x: 12
                    y: 12
                    width: parent.width - 24
                    spacing: 2
                    Text {
                        objectName: "diagnosticsMemoryTitle"
                        text: qsTr("Memory")
                        font.pixelSize: 12
                        font.weight: Font.Medium
                        color: page.colors["text-primary"]
                    }
                    Repeater {
                        id: memoryRepeater
                        model: {
                            const memory = (page.snapshot && page.snapshot.memory) || null;
                            const rows = [];
                            if (memory) {
                                const main = memory.main || {};
                                const system = memory.system || {};
                                if (main.rss !== undefined && Number(main.rss) > 0)
                                    rows.push(qsTr("Total app memory: %1")
                                        .arg(page.formatMegabytes(Number(main.rss), false)));
                                if (system.free !== undefined && Number(system.free) > 0)
                                    rows.push(qsTr("System free memory: %1")
                                        .arg(page.formatMegabytes(Number(system.free), true)));
                            }
                            if (rows.length === 0)
                                rows.push(qsTr("Memory metrics are not available in this build."));
                            return rows;
                        }
                        delegate: Text {
                            required property var modelData
                            required property int index
                            objectName: "diagnosticsMemoryLine-" + index
                            text: modelData
                            font.pixelSize: 11
                            lineHeight: 15
                            lineHeightMode: Text.FixedHeight
                            color: page.colors["text-muted"]
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
}
