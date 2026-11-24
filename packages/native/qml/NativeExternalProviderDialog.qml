pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

// Issue #633: admin "External integration settings" dialog (design
// docs/NATIVE_DESKTOP_EXTERNAL_PROVIDER_ADMIN.md). All state lives in
// controller.externalProviderAdmin; this file is presentation only.
Dialog {
    id: dialog
    objectName: "externalProviderDialog"
    required property var admin
    required property var colors
    property bool confirming: false

    modal: true
    focus: true
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(768, (parent ? parent.width : 768) - 32)
    padding: 20
    visible: admin.dialogOpen
    onClosed: { dialog.confirming = false; admin.closeDialog() }

    function numberValue(value, fallback) {
        const n = Number(value)
        return Number.isFinite(n) ? n : fallback
    }
    function formatTime(value) {
        if (!value) return ""
        const d = new Date(String(value))
        if (isNaN(d.getTime())) return String(value)
        return Qt.formatDateTime(d, "yyyy-MM-dd HH:mm")
    }
    function healthLabel(kind, key) {
        const maps = {
            account: {
                auth_required: qsTr("Credentials required"), backfill: qsTr("Loading history"),
                connecting: qsTr("Connecting"), degraded: qsTr("Degraded"),
                disconnected: qsTr("Disconnected"), live: qsTr("Operational"),
                suspended: qsTr("Suspended")
            },
            bridge: {
                active: qsTr("Active"), degraded: qsTr("Degraded"), enrolling: qsTr("Enrolling"),
                incompatible: qsTr("Incompatible"), revoked: qsTr("Revoked"), suspended: qsTr("Suspended")
            },
            chat: { available: qsTr("Available to connect"), live: qsTr("Connected") },
            operation: {
                discarded: qsTr("Discarded"), failed: qsTr("Failed"),
                manual_reconciliation_required: qsTr("Manual reconciliation required"),
                queued: qsTr("Queued"), running: qsTr("Running"), succeeded: qsTr("Succeeded")
            },
            metric: {
                queue_depth: qsTr("Queued and running operations"), selected_chats: qsTr("Selected chats"),
                synchronized_messages: qsTr("Synchronized messages"), synchronized_users: qsTr("Synchronized users")
            }
        }
        const table = maps[kind] || ({})
        return table[key] || String(key)
    }
    function countRows(source, kind) {
        const rows = []
        const map = source || ({})
        const keys = Object.keys(map).sort()
        for (const key of keys)
            rows.push({ label: healthLabel(kind, key), count: String(map[key]) })
        return rows
    }

    component Pill: Rectangle {
        id: pillRect
        required property var colors
        required property color tone
        required property string label
        radius: height / 2
        color: Qt.alpha(pillRect.tone, 0.1)
        implicitHeight: 24
        implicitWidth: pillRow.implicitWidth + 16
        RowLayout {
            id: pillRow
            anchors.centerIn: parent
            spacing: 6
            Rectangle { Layout.preferredWidth: 6; Layout.preferredHeight: 6; radius: 3; color: pillRect.tone; Accessible.ignored: true }
            Label { text: pillRect.label; font.pixelSize: 12; color: pillRect.tone }
        }
    }
    component Section: Rectangle {
        id: section
        required property var colors
        required property string heading
        property string description: ""
        property bool dangerStyle: false
        default property alias content: sectionColumn.data
        radius: 12
        border.width: 1
        border.color: section.dangerStyle ? Qt.alpha(section.colors["danger"], 0.3) : section.colors["border-subtle"]
        color: section.dangerStyle ? Qt.alpha(section.colors["danger"], 0.05) : "transparent"
        implicitHeight: sectionColumn.implicitHeight + 32
        ColumnLayout {
            id: sectionColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 16
            spacing: 10
            Label { objectName: "extProviderSection-" + section.heading; Layout.fillWidth: true; text: section.heading; font.pixelSize: 14; font.bold: true; color: section.dangerStyle ? section.colors["danger"] : section.colors["text-primary"] }
            Label { Layout.fillWidth: true; visible: text.length > 0; text: section.description; wrapMode: Text.Wrap; font.pixelSize: 12; color: section.colors["text-muted"] }
        }
    }
    component FieldRow: RowLayout {
        required property var colors
        required property string label
        required property string value
        spacing: 8
        Label { Layout.fillWidth: true; text: label; font.pixelSize: 12; color: colors["text-secondary"] }
        Label { objectName: "extProviderValue-" + label; Layout.rightMargin: 8; text: value; textFormat: Text.PlainText; font.pixelSize: 12; color: colors["text-primary"] }
    }
    component StatBlock: ColumnLayout {
        required property var colors
        required property string heading
        required property var rows
        Layout.minimumWidth: 144
        spacing: 6
        Label { text: heading; font.pixelSize: 12; font.bold: true; color: colors["text-primary"] }
        Repeater {
            model: rows
            delegate: RowLayout {
                required property var modelData
                Layout.fillWidth: true
                spacing: 8
                Label { Layout.fillWidth: true; text: modelData.label; textFormat: Text.PlainText; font.pixelSize: 12; color: colors["text-secondary"] }
                Label { text: modelData.count; textFormat: Text.PlainText; font.pixelSize: 12; color: colors["text-primary"] }
            }
        }
        Label { visible: rows.length === 0; text: qsTr("No data"); font.pixelSize: 12; color: colors["text-muted"] }
    }

    header: RowLayout {
        spacing: 12
        Label {
            Layout.leftMargin: 20
            Layout.topMargin: 16
            Layout.fillWidth: true
            text: qsTr("External integration settings")
            font.pixelSize: 20
            font.bold: true
            color: dialog.colors["text-primary"]
        }
        ToolButton { objectName: "extProviderClose"; Layout.rightMargin: 12; Layout.topMargin: 12; text: "\u00d7"; Accessible.name: qsTr("Close"); onClicked: dialog.close() }
    }

    contentItem: ColumnLayout {
        spacing: 10
        Label {
            objectName: "extProviderPolicyLoading"
            visible: dialog.admin.policyStatus !== "ready" && dialog.admin.policyStatus !== "error"
            Layout.fillWidth: true
            text: qsTr("Loading integration settings\u2026")
            color: dialog.colors["text-muted"]
        }
        Label {
            objectName: "extProviderPolicyError"
            Layout.fillWidth: true
            visible: dialog.admin.policyStatus === "error"
            Accessible.role: Accessible.AlertMessage
            wrapMode: Text.Wrap
            text: qsTr("Could not load the integration settings.")
            color: dialog.colors["danger"]
        }
        NativeButton {
            objectName: "extProviderPolicyRetry"
            visible: dialog.admin.policyStatus === "error"
            colors: dialog.colors
            text: qsTr("Refresh")
            onClicked: dialog.admin.refreshPolicy()
        }
        ScrollView {
            id: extScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: dialog.admin.policyStatus === "ready"
            clip: true
            ColumnLayout {
                width: extScroll.availableWidth
                spacing: 16

                Rectangle {
                    objectName: "extProviderConflictNotice"
                    Layout.fillWidth: true
                    visible: dialog.admin.conflict
                    Accessible.role: Accessible.AlertMessage
                    radius: 8
                    color: Qt.alpha(dialog.colors["danger"], 0.05)
                    border.width: 1
                    border.color: Qt.alpha(dialog.colors["danger"], 0.3)
                    implicitHeight: conflictColumn.implicitHeight + 20
                    ColumnLayout {
                        id: conflictColumn
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 10
                        spacing: 8
                        Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("Another administrator changed these settings. Your draft is preserved in the form."); color: dialog.colors["danger"] }
                        RowLayout {
                            spacing: 8
                            NativeButton { objectName: "extProviderLoadCurrent"; colors: dialog.colors; text: qsTr("Load current settings"); onClicked: dialog.admin.resetDraft() }
                            NativeButton { objectName: "extProviderKeepDraft"; colors: dialog.colors; text: qsTr("Keep my draft"); onClicked: dialog.admin.keepDraft() }
                            Item { Layout.fillWidth: true }
                        }
                    }
                }

                // 1. Overview
                Section {
                    Layout.fillWidth: true
                    colors: dialog.colors
                    heading: qsTr("Overview")
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        FieldRow { colors: dialog.colors; label: qsTr("Provider"); value: "Zulip" }
                        RowLayout {
                            spacing: 8
                            Label { text: qsTr("Technical status"); font.pixelSize: 12; color: dialog.colors["text-secondary"] }
                            Pill {
                                colors: dialog.colors
                                tone: String(dialog.admin.health.status ?? "") === "healthy" ? dialog.colors["call-green"] : dialog.colors["danger"]
                                label: String(dialog.admin.health.status ?? "") === "healthy" ? qsTr("Operational") : qsTr("Unavailable")
                            }
                            Item { Layout.fillWidth: true }
                        }
                        RowLayout {
                            spacing: 8
                            Label { text: qsTr("Availability"); font.pixelSize: 12; color: dialog.colors["text-secondary"] }
                            Pill {
                                colors: dialog.colors
                                tone: dialog.admin.draft.enabled ? dialog.colors["call-green"] : dialog.colors["danger"]
                                label: dialog.admin.draft.enabled ? qsTr("Integration allowed") : qsTr("Integration disabled")
                            }
                            Item { Layout.fillWidth: true }
                        }
                        RowLayout {
                            spacing: 8
                            Label { text: qsTr("Emergency suspension"); font.pixelSize: 12; color: dialog.colors["text-secondary"] }
                            Label {
                                objectName: "extProviderEmergencyRow"
                                text: dialog.admin.policy.emergencySuspended ? qsTr("Integration suspended") : qsTr("Not active")
                                textFormat: Text.PlainText
                                font.pixelSize: 12
                                color: dialog.admin.policy.emergencySuspended ? dialog.colors["danger"] : dialog.colors["text-secondary"]
                            }
                            Item { Layout.fillWidth: true }
                        }
                        RowLayout {
                            spacing: 8
                            Label { objectName: "extProviderPolicyUpdated"; text: qsTr("Updated: %1").arg(dialog.formatTime(dialog.admin.policy.updatedAt)); textFormat: Text.PlainText; font.pixelSize: 12; color: dialog.colors["text-muted"] }
                            Item { Layout.fillWidth: true }
                            NativeButton { objectName: "extProviderPolicyRefresh"; colors: dialog.colors; text: qsTr("Refresh"); onClicked: dialog.admin.refreshPolicy() }
                        }
                    }
                }

                // 2. Availability and limits
                Section {
                    Layout.fillWidth: true
                    colors: dialog.colors
                    heading: qsTr("Availability and limits")
                    description: qsTr("Limits apply to new accounts, selected chats, and transferred files.")
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 14
                        RowLayout {
                            spacing: 10
                            Switch {
                                id: enabledSwitch
                                objectName: "extProviderEnabledSwitch"
                                Accessible.name: qsTr("Allow the Zulip integration")
                                checked: Boolean(dialog.admin.draft.enabled)
                                enabled: !dialog.admin.saveBusy && !dialog.admin.caBlocked
                                onToggled: dialog.admin.setEnabled(checked)
                                indicator: Rectangle {
                                    implicitWidth: 36
                                    implicitHeight: 20
                                    radius: 10
                                    color: enabledSwitch.checked ? dialog.colors.accent : dialog.colors["text-field-bg"]
                                    border.width: 1
                                    border.color: dialog.colors["border-subtle"]
                                    Rectangle {
                                        x: 2 + (enabledSwitch.checked ? 16 : 0)
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 16
                                        height: 16
                                        radius: 8
                                        color: dialog.colors["bg-elevated"]
                                    }
                                }
                                contentItem: Item {}
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1
                                Label { text: qsTr("Allow the Zulip integration"); font.pixelSize: 14; font.bold: true; color: dialog.colors["text-primary"] }
                                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("Allows users in this organization to connect Zulip accounts."); font.pixelSize: 12; color: dialog.colors["text-muted"] }
                            }
                        }
                        Repeater {
                            model: [
                                { key: "max_accounts", label: qsTr("Accounts per user"), maximum: 100000, mib: false },
                                { key: "max_selected_chats_per_account", label: qsTr("Chats per external account"), maximum: 1000000, mib: false },
                                { key: "max_file_bytes", label: qsTr("Maximum file size"), maximum: 5120, mib: true }
                            ]
                            delegate: ColumnLayout {
                                id: limitField
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 4
                                property real draftNumber: dialog.numberValue(dialog.admin.draft[limitField.modelData.key], 0)
                                property bool projectionReady: false
                                property real parsed: {
                                    const raw = limitInput.text.trim()
                                    if (!/^\d+$/.test(raw)) return -1
                                    return Number(raw)
                                }
                                Label { text: limitField.modelData.label; font.pixelSize: 12; font.bold: true; color: dialog.colors["text-secondary"] }
                                RowLayout {
                                    spacing: 8
                                    NativeField {
                                        id: limitInput
                                        objectName: "extProviderLimitsField-" + limitField.modelData.key
                                        Layout.fillWidth: true
                                        Layout.maximumWidth: 220
                                        colors: dialog.colors
                                        text: limitField.modelData.mib ? String(dialog.numberValue(limitField.draftNumber, 0) / 1048576) : String(dialog.numberValue(limitField.draftNumber, 0))
                                        Component.onCompleted: { const field = limitField; Qt.callLater(() => { if (field) field.projectionReady = true }) }
                                        Accessible.name: limitField.modelData.label
                                        enabled: !dialog.admin.saveBusy && !dialog.admin.caBlocked
                                        // Issue #816: the write-back must be driven
                                        // by user editing only. textChanged also
                                        // fires when this very binding re-evaluates
                                        // from draftNumber, so setLimit mutated the
                                        // draft while draftNumber was still being
                                        // evaluated: Qt logged a binding loop on
                                        // draftNumber and froze the field at a
                                        // stale value. textEdited fires only for
                                        // user input, breaking the projection cycle.
                                        onTextEdited: {
                                            if (!limitField.projectionReady) return
                                            if (limitField.parsed < 0) return
                                            const value = limitField.modelData.mib ? limitField.parsed * 1048576 : limitField.parsed
                                            if (value <= limitField.modelData.maximum * (limitField.modelData.mib ? 1048576 : 1))
                                                dialog.admin.setLimit(limitField.modelData.key, value)
                                        }
                                    }
                                    Label { visible: limitField.modelData.mib; text: qsTr("MiB"); font.pixelSize: 12; color: dialog.colors["text-secondary"] }
                                }
                                Label {
                                    objectName: "extProviderRangeError-" + limitField.modelData.key
                                    Layout.fillWidth: true
                                    visible: limitField.parsed < 0 || limitField.parsed > limitField.modelData.maximum
                                    wrapMode: Text.Wrap
                                    text: qsTr("Enter a value from 0 to %1.").arg(limitField.modelData.maximum)
                                    textFormat: Text.PlainText
                                    font.pixelSize: 12
                                    color: dialog.colors["danger"]
                                }
                            }
                        }
                        Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("Lowering these limits does not remove existing accounts or selected chats."); font.pixelSize: 12; color: dialog.colors["text-muted"] }
                        Label {
                            objectName: "extProviderCaNotice"
                            Layout.fillWidth: true
                            visible: dialog.admin.caBlocked
                            Accessible.role: Accessible.AlertMessage
                            wrapMode: Text.Wrap
                            text: qsTr("Availability and limits cannot be changed while a custom certificate bundle is installed. The backend cannot safely preserve it without receiving the certificates again.")
                            font.pixelSize: 12
                            color: dialog.colors["danger"]
                        }
                        Label { objectName: "extProviderSaveError"; Layout.fillWidth: true; visible: dialog.admin.saveFailed; Accessible.role: Accessible.AlertMessage; wrapMode: Text.Wrap; text: qsTr("Could not save the integration settings."); font.pixelSize: 12; color: dialog.colors["danger"] }
                    }
                }

                // 3. Custom certificate authority
                Section {
                    Layout.fillWidth: true
                    colors: dialog.colors
                    heading: qsTr("Custom certificate authority")
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Label {
                            objectName: "extProviderCaEmpty"
                            visible: !dialog.admin.policy.ca?.present
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: qsTr("No custom certificate bundle is installed.")
                            font.pixelSize: 12
                            color: dialog.colors["text-muted"]
                        }
                        ColumnLayout {
                            visible: Boolean(dialog.admin.policy.ca?.present)
                            Layout.fillWidth: true
                            spacing: 6
                            FieldRow { colors: dialog.colors; label: qsTr("Certificates"); value: String(dialog.admin.policy.ca?.certificateCount ?? "") }
                            FieldRow { colors: dialog.colors; label: qsTr("Generation"); value: String(dialog.admin.policy.ca?.generation ?? "") }
                            FieldRow { colors: dialog.colors; label: qsTr("SHA-256 fingerprint"); value: String(dialog.admin.policy.ca?.sha256 ?? "") }
                        }
                    }
                }

                // 4. Technical status
                Section {
                    Layout.fillWidth: true
                    colors: dialog.colors
                    heading: qsTr("Technical status")
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        Label { visible: dialog.admin.healthStatus === "loading"; text: qsTr("Loading technical status\u2026"); font.pixelSize: 12; color: dialog.colors["text-muted"] }
                        RowLayout {
                            visible: dialog.admin.healthStatus === "ready"
                            spacing: 8
                            Pill {
                                colors: dialog.colors
                                tone: String(dialog.admin.health.status ?? "") === "healthy" ? dialog.colors["call-green"] : dialog.colors["danger"]
                                label: String(dialog.admin.health.status ?? "") === "healthy" ? qsTr("Operational") : qsTr("Unavailable")
                            }
                            Label { objectName: "extProviderHealthUpdated"; text: qsTr("Updated: %1").arg(dialog.formatTime(dialog.admin.health.updatedAt)); textFormat: Text.PlainText; font.pixelSize: 12; color: dialog.colors["text-muted"] }
                            Item { Layout.fillWidth: true }
                            NativeButton { objectName: "extProviderHealthRefresh"; colors: dialog.colors; text: qsTr("Refresh"); onClicked: dialog.admin.refreshHealth() }
                        }
                        RowLayout {
                            visible: dialog.admin.healthStatus === "loadError" || dialog.admin.healthStatus === "refreshError"
                            spacing: 8
                            Label {
                                objectName: "extProviderHealthError"
                                Accessible.role: Accessible.AlertMessage
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                text: dialog.admin.healthStatus === "loadError" ? qsTr("Could not load the technical status.") : qsTr("Could not refresh the technical status.")
                                font.pixelSize: 12
                                color: dialog.colors["danger"]
                            }
                            NativeButton { objectName: "extProviderHealthRetry"; colors: dialog.colors; text: qsTr("Refresh"); onClicked: dialog.admin.refreshHealth() }
                        }
                        GridLayout {
                            objectName: "extProviderHealthGrid"
                            visible: dialog.admin.healthStatus === "ready"
                            Layout.fillWidth: true
                            columns: Math.max(1, Math.min(5, Math.floor(width / 160)))
                            columnSpacing: 16
                            rowSpacing: 16
                            StatBlock { colors: dialog.colors; heading: qsTr("Accounts"); rows: dialog.countRows(dialog.admin.health.accountCounts, "account") }
                            StatBlock { colors: dialog.colors; heading: qsTr("Chats"); rows: dialog.countRows(dialog.admin.health.chatCounts, "chat") }
                            StatBlock { colors: dialog.colors; heading: qsTr("Bridge instances"); rows: dialog.countRows(dialog.admin.health.bridgeCounts, "bridge") }
                            StatBlock { colors: dialog.colors; heading: qsTr("Operations"); rows: dialog.countRows(dialog.admin.health.operationCounts, "operation") }
                            StatBlock { colors: dialog.colors; heading: qsTr("Metrics"); rows: dialog.countRows(dialog.admin.health.metrics, "metric") }
                        }
                    }
                }

                // 5. Emergency controls
                Section {
                    Layout.fillWidth: true
                    colors: dialog.colors
                    dangerStyle: true
                    heading: qsTr("Emergency controls")
                    description: qsTr("These actions affect the Zulip integration for every user in the organization.")
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        Label { objectName: "extProviderActionError"; Layout.fillWidth: true; visible: dialog.admin.actionFailed; Accessible.role: Accessible.AlertMessage; wrapMode: Text.Wrap; text: qsTr("Could not complete the action. The form settings were not changed."); font.pixelSize: 12; color: dialog.colors["danger"] }
                        NativeButton {
                            objectName: "extProviderSuspend"
                            visible: !dialog.confirming && !dialog.admin.policy.emergencySuspended
                            colors: dialog.colors
                            enabled: !dialog.admin.saveBusy && dialog.admin.actionStatus === "none"
                            text: dialog.admin.actionStatus === "suspending" ? qsTr("Suspending\u2026") : qsTr("Suspend integration")
                            onClicked: dialog.confirming = true
                        }
                        NativeButton {
                            objectName: "extProviderResume"
                            visible: !dialog.confirming && Boolean(dialog.admin.policy.emergencySuspended)
                            colors: dialog.colors
                            enabled: !dialog.admin.saveBusy && dialog.admin.actionStatus === "none"
                            text: dialog.admin.actionStatus === "resuming" ? qsTr("Resuming\u2026") : qsTr("Resume integration")
                            onClicked: dialog.admin.resume()
                        }
                        ColumnLayout {
                            objectName: "extProviderConfirmPanel"
                            visible: dialog.confirming
                            Layout.fillWidth: true
                            spacing: 8
                            Accessible.role: Accessible.Dialog
                            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("Suspend the Zulip integration?"); font.pixelSize: 14; font.bold: true; color: dialog.colors["text-primary"] }
                            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("Zulip connections will be suspended for every user in the organization."); font.pixelSize: 12; color: dialog.colors["text-muted"] }
                            RowLayout {
                                spacing: 8
                                NativeButton { objectName: "extProviderSuspendConfirm"; colors: dialog.colors; primary: true; enabled: dialog.admin.actionStatus === "none"; text: qsTr("Suspend for everyone"); onClicked: { dialog.confirming = false; dialog.admin.suspend() } }
                                NativeButton { objectName: "extProviderSuspendCancel"; colors: dialog.colors; text: qsTr("Cancel"); onClicked: dialog.confirming = false }
                                Item { Layout.fillWidth: true }
                            }
                        }
                    }
                }
            }
        }
    }

    footer: RowLayout {
        visible: dialog.admin.policyStatus === "ready"
        Item { Layout.fillWidth: true }
        NativeButton {
            objectName: "extProviderSave"
            colors: dialog.colors
            primary: true
            enabled: dialog.admin.canSave
            text: dialog.admin.saveBusy ? qsTr("Saving\u2026") : qsTr("Save")
            onClicked: dialog.admin.save()
        }
    }
}
