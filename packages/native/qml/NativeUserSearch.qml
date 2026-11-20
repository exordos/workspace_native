pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: search
    required property var controller
    required property var colors
    objectName: "globalUserSearch"
    parent: Overlay.overlay
    x: Math.round((parent.width - width) / 2)
    y: Math.round(parent.height * 0.16)
    width: Math.min(672, parent.width - 32)
    height: Math.min(parent.height * 0.68, 88 + (heading.visible ? 28 : 0)
        + (empty.visible ? 48 : 0) + (errorLabel.visible ? errorLabel.implicitHeight + 12 : 0)
        + (retry.visible ? retry.implicitHeight : 0) + rows.length * 42)
    padding: 0
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    property var rows: []
    property string owner: ""
    property string pendingPeer: ""

    function refresh() {
        if (!visible) return
        const next = controller.searchWorkspaceUsers(input.text)
        if (JSON.stringify(next) !== JSON.stringify(rows)) rows = next
    }
    function finishExisting() {
        if (pendingPeer && controller.directPeer.id === pendingPeer && controller.selectedTopic) {
            pendingPeer = ""
            controller.showSection("chat")
            close()
        }
    }
    function select(peer) {
        if (controller.directChatBusy) return
        if (controller.directPeer.id === peer) {
            controller.showSection("chat")
            close()
            return
        }
        pendingPeer = peer
        controller.openDirectChatFromProfile(peer)
        finishExisting()
    }
    onOpened: {
        owner = controller.ownStatusContextKey
        refresh()
        input.forceActiveFocus()
    }
    onClosed: {
        pendingPeer = ""
        owner = ""
        input.clear()
        rows = []
    }
    Connections {
        target: search.controller
        function onStateChanged() {
            if (!search.visible) return
            if (search.owner && search.owner !== search.controller.ownStatusContextKey) {
                search.close()
                return
            }
            search.finishExisting()
            search.refresh()
        }
        function onDirectChatOpened(stream, peer, topic) {
            if (!search.visible || search.pendingPeer !== peer || search.controller.selectedStream !== stream) return
            search.controller.selectTopic(topic)
        }
    }
    background: Rectangle { radius: 12; color: search.colors["card-bg"]; border.color: search.colors["border-subtle"] }
    contentItem: ColumnLayout {
        spacing: 0
        NativeField {
            id: input
            objectName: "globalUserSearchInput"
            Layout.fillWidth: true
            Layout.leftMargin: 20; Layout.rightMargin: 20; Layout.topMargin: 16; Layout.bottomMargin: 16
            Layout.preferredHeight: 40
            colors: search.colors
            placeholderText: qsTr("Search")
            Accessible.name: placeholderText
            onTextChanged: search.refresh()
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: search.colors["border-subtle"] }
        Text {
            id: heading
            visible: search.rows.length > 0
            Layout.fillWidth: true; Layout.leftMargin: 24; topPadding: 8; bottomPadding: 4
            text: qsTr("Start chat"); font.capitalization: Font.AllUppercase
            font.letterSpacing: 0.4; font.pixelSize: 11; color: search.colors["text-muted"]
        }
        Text {
            id: empty; objectName: "globalUserSearchEmpty"
            visible: input.text.trim().length > 0 && search.rows.length === 0
            Layout.fillWidth: true; Layout.preferredHeight: 48
            horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
            text: qsTr("No results found"); color: search.colors["text-muted"]; font.pixelSize: 14
        }
        Text {
            id: errorLabel; objectName: "globalUserSearchError"
            visible: !!search.pendingPeer && search.controller.directChatForm.peerUuid === search.pendingPeer && text.length > 0
            text: search.controller.directChatError; textFormat: Text.PlainText
            Layout.fillWidth: true; Layout.margins: 6; wrapMode: Text.Wrap
            color: search.colors.danger
        }
        NativeButton {
            id: retry
            visible: errorLabel.visible && search.controller.canRetryDirectChat
            Layout.leftMargin: 12
            colors: search.colors
            text: qsTr("Retry"); onClicked: search.controller.retryDirectChat()
        }
        ListView {
            id: list
            objectName: "globalUserSearchResults"
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.leftMargin: 12; Layout.rightMargin: 12; Layout.bottomMargin: 8
            model: search.rows; clip: true; spacing: 2
            ScrollBar.vertical: ScrollBar {}
            delegate: Button {
                id: row
                required property var modelData
                objectName: "globalUserSearchRow-" + modelData.id
                width: list.width; height: 40; leftPadding: 12; rightPadding: 12
                enabled: !search.controller.directChatBusy
                Accessible.name: modelData.displayName + " (" + (modelData.email || modelData.id) + ")"
                onClicked: search.select(modelData.id)
                background: Rectangle { radius: 6; color: row.hovered || row.visualFocus ? search.colors["card-bg-active"] : "transparent" }
                contentItem: RowLayout {
                    spacing: 8
                    Rectangle {
                        Layout.preferredWidth: 8; Layout.preferredHeight: 8; radius: 4
                        visible: !!row.modelData.presence
                        color: row.modelData.presence === "active" ? search.colors["indicator-green"]
                            : row.modelData.presence === "idle" ? search.colors["indicator-orange"] : search.colors["text-muted"]
                    }
                    ColumnLayout {
                        Layout.fillWidth: true; Layout.minimumWidth: 0; spacing: 0
                        Text {
                            objectName: "globalUserSearchName-" + row.modelData.id
                            Layout.fillWidth: true; Layout.minimumWidth: 0
                            text: row.modelData.displayName; textFormat: Text.PlainText; elide: Text.ElideRight
                            font.pixelSize: 14; font.weight: Font.Medium; color: search.colors["text-primary"]
                        }
                        Text {
                            objectName: "globalUserSearchSecondary-" + row.modelData.id
                            Layout.fillWidth: true; Layout.minimumWidth: 0
                            text: row.modelData.statusLabel || row.modelData.email; visible: text.length > 0
                            textFormat: Text.PlainText; elide: Text.ElideRight; font.pixelSize: 11
                            color: search.colors["text-secondary"]
                        }
                    }
                }
            }
        }
    }
}
