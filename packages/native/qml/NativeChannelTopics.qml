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

    // Issue #636 (docs/NATIVE_DESKTOP_TOPIC_CONTEXT_ACTIONS.md): the row menu
    // lives in the shared NativeTopicContextMenu surface hosted by the drawer
    // page; this block only reports which row asked for it. The reference
    // trigger is right-click capture + keyboard Menu
    // (workspace-topic-context-menu.ui.tsx).
    signal requestTopicMenu(var topic, var anchor, real x, real y)

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
            Keys.onMenuPressed: block.requestTopicMenu(row.modelData, row, 12, row.height)
            // Issue #647: the tap's row-local `position` is already in the
            // row's coordinates; Button does not expose mapFromScene to QML.
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: point => block.requestTopicMenu(row.modelData, row,
                    point.position.x, point.position.y)
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
}
