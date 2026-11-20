pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

Column {
    id: block
    required property var controller
    required property var colors
    property bool expanded: true
    readonly property var rows: controller.channelInfoTopics.filter(t => !t.done)
    property var target: null
    property string pendingAction: ""
    property bool failed: false

    function label(name) {
        const text = String(name).trim()
        return ["", "general chat", "общий чат", "(no topic)"].includes(text.toLowerCase()) ? qsTr("General chat") : text
    }
    function modeLabel(mode) {
        if (mode === "mute") return qsTr("Muted")
        if (mode === "follow") return qsTr("Followed")
        if (mode === "unmute") return qsTr("Unmuted")
        return qsTr("Default")
    }
    function context(row, anchor, x, y) {
        if (pendingAction) return
        target = row
        failed = false
        menu.popup(anchor, x, y)
    }
    function act(action, value) {
        if (!target || pendingAction) return
        pendingAction = action
        controller.applyChannelTopicAction(target.id, action, value || "")
    }
    Connections {
        target: block.controller
        function onChannelTopicActionFinished(id, action, ok) {
            if (!block.target || block.target.id !== id || block.pendingAction !== action) return
            block.pendingAction = ""
            block.failed = !ok
            if (ok && rename.visible) rename.close()
        }
        function onStateChanged() {
            if (block.target && !block.rows.some(t => t.id === block.target.id)) {
                menu.close()
                rename.close()
                block.target = null
                block.pendingAction = ""
            }
        }
    }
    Button {
        id: toggle
        objectName: "channelTopicsToggle"
        width: parent.width
        height: 32
        leftPadding: 8; rightPadding: 8
        text: qsTr("Topics")
        Accessible.name: block.expanded ? qsTr("Collapse topics") : qsTr("Expand topics")
        Accessible.checkable: true
        Accessible.checked: block.expanded
        onClicked: block.expanded = !block.expanded
        contentItem: RowLayout {
            Text { Layout.fillWidth: true; text: toggle.text; color: block.colors["text-primary"]; font.pixelSize: 14; font.weight: Font.Medium }
            Image {
                source: block.expanded ? "qrc:/workspace/icons/chevron-up.svg" : "qrc:/workspace/icons/chevron-down.svg"
                sourceSize: Qt.size(16,16)
                layer.enabled: true
                layer.effect: MultiEffect { colorization: 1; colorizationColor: block.colors["text-muted"] }
            }
        }
        background: Rectangle { radius: 6; color: toggle.hovered || toggle.visualFocus ? block.colors["card-bg-active"] : "transparent" }
    }
    Text {
        objectName: "channelTopicsEmpty"
        visible: block.expanded && block.rows.length === 0
        width: parent.width; padding: 8
        text: qsTr("No topics"); color: block.colors["text-muted"]; font.pixelSize: 14
    }
    Repeater {
        model: block.expanded ? block.rows : []
        Button {
            id: row
            required property var modelData
            objectName: "channelTopicRow-" + modelData.id
            width: block.width; height: 36
            leftPadding: 12; rightPadding: 12
            readonly property int count: modelData.activeUnread > 0 ? modelData.activeUnread : modelData.passiveUnread
            readonly property string notification: qsTr("Topic notifications: %1").arg(block.modeLabel(modelData.effectivelyMuted ? "mute" : modelData.notificationMode))
            Accessible.name: block.label(modelData.name)
            Accessible.description: notification
            onClicked: block.controller.selectTopic(modelData.id)
            Keys.onMenuPressed: block.context(modelData, row, 12, row.height)
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: point => block.context(row.modelData, row, point.position.x, point.position.y)
            }
            background: Rectangle { radius: 6; color: row.hovered || row.visualFocus ? block.colors["card-bg-active"] : "transparent" }
            contentItem: RowLayout {
                spacing: 4
                Text {
                    objectName: "channelTopicTitle-" + row.modelData.id
                    Layout.fillWidth: true; Layout.rightMargin: 4
                    text: block.label(row.modelData.name); textFormat: Text.PlainText
                    font.pixelSize: 14; font.italic: String(row.modelData.name).trim().length === 0
                    elide: Text.ElideRight; color: block.colors["text-primary"]
                }
                Text {
                    objectName: "channelTopicMention-" + row.modelData.id
                    visible: row.modelData.hasUnreadPersonalMention
                    text: "@"; font.pixelSize: 12; font.bold: true; color: block.colors.accent
                }
                Rectangle {
                    visible: row.count > 0; implicitWidth: countLabel.implicitWidth + 10; implicitHeight: 18; radius: 9
                    color: row.modelData.activeUnread > 0 ? block.colors.accent : block.colors["card-bg-active"]
                    Text {
                        id: countLabel; objectName: "channelTopicUnread-" + row.modelData.id
                        anchors.centerIn: parent
                        text: row.count > 1000 ? Math.floor(row.count / 1000) + "k+" : String(row.count)
                        font.pixelSize: 11; color: row.modelData.activeUnread > 0 ? "white" : block.colors["text-secondary"]
                    }
                }
                Item {
                    implicitWidth: 20; implicitHeight: 20
                    Accessible.role: Accessible.Graphic; Accessible.name: row.notification
                    Image {
                        objectName: "channelTopicBell-" + row.modelData.id
                        anchors.centerIn: parent; width: 16; height: 16
                        source: row.modelData.effectivelyMuted ? "qrc:/workspace/icons/bell_off.svg" : "qrc:/workspace/icons/bell.svg"
                        layer.enabled: true
                        layer.effect: MultiEffect { colorization: 1; colorizationColor: block.colors["text-muted"] }
                    }
                    HoverHandler { id: hover }
                    ToolTip.visible: hover.hovered; ToolTip.text: row.notification
                }
            }
        }
    }
    Text { visible: block.failed; width: parent.width; wrapMode: Text.Wrap; color: block.colors.danger; text: qsTr("Could not update the topic. Please try again.") }
    Menu {
        id: menu
        enabled: !block.pendingAction
        MenuItem { text: qsTr("Default"); onTriggered: block.act("notifications", "default") }
        MenuItem { objectName: "channelTopicMuteAction"; text: qsTr("Muted"); onTriggered: block.act("notifications", "mute") }
        MenuItem { text: qsTr("Unmuted"); onTriggered: block.act("notifications", "unmute") }
        MenuItem { text: qsTr("Followed"); onTriggered: block.act("notifications", "follow") }
        MenuItem { visible: !!block.target && block.target.unread > 0; height: visible ? implicitHeight : 0; text: qsTr("Mark as read"); onTriggered: block.act("read") }
        MenuItem { text: qsTr("Rename topic"); onTriggered: { nameField.text = block.target.name; rename.open() } }
        MenuItem { text: qsTr("Mark topic as done"); onTriggered: block.act("toggle_done") }
    }
    Dialog {
        id: rename
        parent: Overlay.overlay; anchors.centerIn: parent; modal: true; focus: true
        title: qsTr("Rename topic")
        contentItem: ColumnLayout {
            TextField { id: nameField; Layout.fillWidth: true; enabled: !block.pendingAction }
            Button { text: qsTr("Save"); enabled: !block.pendingAction && nameField.text.trim().length > 0; onClicked: block.act("rename", nameField.text) }
            Button { text: qsTr("Cancel"); enabled: !block.pendingAction; onClicked: rename.close() }
        }
    }
}
