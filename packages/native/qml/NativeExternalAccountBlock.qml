pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

ColumnLayout {
    id: block
    required property var directory
    required property var colors
    required property bool menuOpen
    property bool expanded: false
    property string formMode: ""
    property var selected: ({})
    readonly property bool formOpen: form.visible || confirmation.visible
    spacing: 5
    function begin(mode, row) {
        directory.cancelForm()
        formMode = mode
        selected = row || ({})
        host.text = selected.server || ""
        email.text = selected.email || ""
        secret.clear()
        if (mode === "delete") confirmation.open()
        else form.open()
    }
    function finish() {
        secret.clear()
        form.close()
        confirmation.close()
        selected = ({})
        formMode = ""
    }
    onExpandedChanged: { if (expanded) directory.open(); else { finish(); directory.close() } }
    onMenuOpenChanged: if (!menuOpen) expanded = false
    Connections {
        target: block.directory
        function onSaved() { block.finish() }
        function onContextLost() { block.expanded = false }
    }
    component MenuGlyph: Image {
        fillMode: Image.PreserveAspectFit
        layer.enabled: true
        layer.effect: MultiEffect { colorization: 1; colorizationColor: block.colors["text-secondary"] }
        Accessible.ignored: true
    }
    Button {
        id: toggle
        objectName: "externalAccountsToggle"; Layout.fillWidth: true; implicitHeight: 40; padding: 8
        contentItem: RowLayout {
            spacing: 12
            MenuGlyph { source: "qrc:/workspace/icons/links_compact.svg"; Layout.preferredWidth: 24; Layout.preferredHeight: 24 }
            Label { Layout.fillWidth: true; text: toggle.text; color: block.colors["text-primary"]; font.pixelSize: 14 }
            MenuGlyph { source: "qrc:/workspace/icons/" + (block.expanded ? "chevron-up" : "chevron-down") + ".svg"; Layout.preferredWidth: 16; Layout.preferredHeight: 16 }
        }
        background: Rectangle { radius: 8; color: toggle.hovered || toggle.visualFocus ? block.colors["card-bg-active"] : "transparent" }
        text: qsTr("External accounts"); Accessible.checkable: true; Accessible.checked: block.expanded
        onClicked: block.expanded = !block.expanded
    }
    Pane {
        visible: block.expanded
        Layout.fillWidth: true
        Layout.rightMargin: 8
        padding: 8
        background: Rectangle { radius: 8; color: "transparent"; border.color: block.colors["border-subtle"] }
        contentItem: ColumnLayout {
            spacing: 20
            Label {
                objectName: "externalAccountsError"; Layout.fillWidth: true
                visible: block.directory.loadFailed; wrapMode: Text.Wrap
                text: qsTr("Could not load external accounts. Check access and try again.")
                color: block.colors["notice-base"]; Accessible.role: Accessible.AlertMessage
            }
            Label {
                visible: block.directory.loading; Layout.fillWidth: true
                text: qsTr("Checking…"); color: block.colors["text-secondary"]
            }
            Label {
                objectName: "externalAccountsEmpty"; Layout.fillWidth: true
                visible: !block.directory.loading && !block.directory.loadFailed && block.directory.rows.length === 0
                text: qsTr("No external accounts connected yet."); wrapMode: Text.Wrap; color: block.colors["text-secondary"]
            }
            Label {
                visible: block.directory.rows.length > 0; text: qsTr("Connected"); color: block.colors["text-primary"]
            }
            Repeater {
                model: block.directory.rows
                ColumnLayout {
                    id: entry; required property var modelData
                    Layout.fillWidth: true
                    RowLayout {
                        Layout.fillWidth: true; Layout.preferredHeight: 40; spacing: 12
                        Image { Layout.preferredWidth: 40; Layout.preferredHeight: 40; source: "qrc:/workspace/icons/zulip-external-account.svg" }
                        ColumnLayout {
                            Layout.fillWidth: true; Layout.minimumWidth: 0; spacing: 1
                            Label {
                                objectName: "externalAccountName-" + entry.modelData.id; Layout.fillWidth: true; Layout.minimumWidth: 0
                                text: entry.modelData.email || entry.modelData.server || "Zulip"; textFormat: Text.PlainText
                                elide: Text.ElideRight; font.pixelSize: 14; color: block.colors["text-primary"]
                            }
                            Label {
                                Layout.fillWidth: true; Layout.minimumWidth: 0
                                objectName: "externalAccountStatus-" + entry.modelData.id
                                text: "Zulip · " + block.statusLabel(entry.modelData); textFormat: Text.PlainText
                                elide: Text.ElideRight; font.pixelSize: 12; color: block.colors["text-secondary"]
                            }
                        }
                        NativeIconButton {
                            objectName: "externalAccountActions-" + entry.modelData.id; asset: "more"; colors: block.colors
                            enabled: !block.directory.busy; Accessible.name: qsTr("External account actions"); implicitWidth: 24; implicitHeight: 24
                            onClicked: menu.popup()
                            Menu {
                                id: menu
                                MenuItem { objectName: "externalAccountEdit"; text: qsTr("Edit"); icon.source: "qrc:/workspace/icons/pen.svg"; onTriggered: block.begin("edit",entry.modelData) }
                                MenuItem {
                                    objectName: "externalAccountReconnect"; text: qsTr("Reconnect"); icon.source: "qrc:/workspace/icons/build.svg"
                                    visible: ["auth_required","degraded"].includes(entry.modelData.status)
                                    height: visible ? implicitHeight : 0
                                    onTriggered: block.begin("reconnect",entry.modelData)
                                }
                                MenuSeparator {}
                                MenuItem { objectName: "externalAccountDelete"; text: qsTr("Delete"); icon.source: "qrc:/workspace/icons/delete.svg"; palette.text: block.colors["notice-base"]; onTriggered: block.begin("delete",entry.modelData) }
                            }
                        }
                    }
                    Label { Layout.fillWidth: true; Layout.leftMargin: 52; visible: text.length > 0; text: entry.modelData.safeError; textFormat: Text.PlainText; wrapMode: Text.Wrap; color: block.colors["notice-base"] }
                }
            }
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("Linked services let you sign in to the same profile from different messengers without setting it up again"); color: block.colors["text-secondary"] }
            Button {
                id: connectButton
                objectName: "externalAccountConnect"; Layout.fillWidth: true; implicitHeight: 36
                text: qsTr("Connect service"); icon.source: "qrc:/workspace/icons/add.svg"
                icon.width: 20; icon.height: 20; icon.color: block.colors.accent
                palette.buttonText: block.colors.accent
                background: Rectangle { radius: 8; color: block.colors["card-bg-active"]; border.width: connectButton.visualFocus ? 2 : 0; border.color: block.colors.accent }
                enabled: !block.directory.busy && !block.directory.loading && !block.directory.loadFailed
                onClicked: block.begin("create", {})
            }
        }
    }
    function statusLabel(row) {
        if (row.live) return qsTr("Connected")
        const labels = {new:qsTr("Checking…"),connecting:qsTr("Checking connection…"),backfill:qsTr("Loading history…"),
            fast_backfill:qsTr("Loading history…"),active:qsTr("Connection is active"),live:qsTr("Connection is active"),
            degraded:qsTr("Connection needs attention"),error:qsTr("Connection needs attention"),
            auth_required:qsTr("Zulip credentials need to be updated"),disconnected:qsTr("Synchronization is stopped"),
            suspended:qsTr("The integration was suspended by an administrator")}
        return labels[row.status] || qsTr("Checking…")
    }
    Dialog {
        id: form; parent: Overlay.overlay; anchors.centerIn: parent
        modal: true; focus: true; width: Math.min(440, parent.width - 32)
        title: block.formMode === "edit" ? qsTr("Edit external account") : block.formMode === "reconnect" ? qsTr("Reconnect") : qsTr("Connect external account")
        readonly property bool duplicate: block.formMode === "create" && block.directory.rows.length > 0
        onClosed: { secret.clear(); block.directory.cancelForm() }
        contentItem: ColumnLayout {
            Label { text: qsTr("Provider"); color: block.colors["text-secondary"] }
            ComboBox { Layout.fillWidth: true; model: ["Zulip"]; Accessible.name: qsTr("Provider") }
            Label { Layout.fillWidth: true; visible: form.duplicate; text: qsTr("Zulip is already connected."); color: block.colors["text-secondary"] }
            NativeField { id: host; objectName: "externalAccountServer"; visible: !form.duplicate; Layout.fillWidth: true; colors: block.colors; placeholderText: qsTr("Server URL"); Accessible.name: placeholderText }
            NativeField { id: email; objectName: "externalAccountEmail"; visible: !form.duplicate; Layout.fillWidth: true; colors: block.colors; placeholderText: qsTr("Email"); Accessible.name: placeholderText }
            NativeField { id: secret; objectName: "externalAccountCredential"; visible: !form.duplicate; Layout.fillWidth: true; colors: block.colors; placeholderText: qsTr("API key"); Accessible.name: placeholderText; echoMode: TextInput.Password }
            Label { visible: block.directory.saveFailed; Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("Could not update the external account. Check the details and try again."); color: block.colors["notice-base"]; Accessible.role: Accessible.AlertMessage }
            RowLayout {
                Layout.fillWidth: true; Item { Layout.fillWidth: true }
                NativeButton { objectName: "externalAccountCancel"; text: qsTr("Cancel"); colors: block.colors; onClicked: form.close() }
                NativeButton {
                    objectName: "externalAccountSubmit"; colors: block.colors; primary: true
                    visible: !form.duplicate; enabled: !block.directory.busy && /^https:\/\/[^\s/]+/.test(host.text.trim()) && email.text.trim().length > 0 && (block.formMode === "edit" || secret.text.trim().length > 0)
                    text: block.formMode === "edit" ? qsTr("Save") : qsTr("Connect")
                    onClicked: { block.directory.save(block.formMode,block.selected.id || "",host.text,email.text,secret.text); secret.clear() }
                }
            }
        }
    }
    Dialog {
        id: confirmation; parent: Overlay.overlay; anchors.centerIn: parent; modal: true; focus: true
        width: Math.min(400,parent.width - 32); title: qsTr("Delete external account?")
        onClosed: block.directory.cancelForm()
        contentItem: ColumnLayout {
            Label { Layout.fillWidth: true; text: block.selected.email || block.selected.server || "Zulip"; textFormat: Text.PlainText; wrapMode: Text.Wrap }
            Label { visible: block.directory.saveFailed; text: qsTr("Could not delete the external account."); Accessible.role: Accessible.AlertMessage }
            RowLayout {
                NativeButton { colors: block.colors; text: qsTr("Cancel"); onClicked: confirmation.close() }
                NativeButton { objectName: "externalAccountDeleteConfirm"; colors: block.colors; text: qsTr("Delete"); enabled: !block.directory.busy; onClicked: block.directory.remove(block.selected.id) }
            }
        }
    }
}
