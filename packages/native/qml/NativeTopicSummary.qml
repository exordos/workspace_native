pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Column {
    id: panel
    required property var controller
    required property var colors
    readonly property var topic: controller.channelInfoTopics.find(row => row.id === controller.selectedTopic)
    readonly property string topicId: topic ? topic.id : ""
    readonly property string summary: topic ? String(topic.summary || "") : ""
    readonly property bool enabledForTopic: !topic || topic.summaryEnabled
    readonly property var permissions: controller.channelInfoStream
    signal referenceActivated(string link)
    property bool expanded: false
    property bool saving: false
    property bool failed: false
    property string settingsTopic: ""
    spacing: 12
    visible: Boolean(topic)
    function reset() {
        expanded = summary.length > 0
        if (settings.visible) settings.close()
    }
    onTopicIdChanged: reset()
    Component.onCompleted: reset()
    RowLayout {
        width: panel.width; height: 24; spacing: 4
        Text {
            Layout.maximumWidth: implicitWidth; Layout.fillWidth: true; elide: Text.ElideRight
            text: qsTr("Topic context"); font.pixelSize: 14; font.weight: Font.Medium; color: panel.colors["text-primary"]
        }
        Text { text: "(AI ✨)"; font.pixelSize: 12; color: panel.colors["text-secondary"] }
        Item { Layout.fillWidth: true }
        ToolButton {
            id: gear; objectName: "channelTopicSummarySettings"
            visible: Boolean(panel.permissions.canOpenSummarySettings)
            Layout.preferredWidth: 24; Layout.preferredHeight: 24; padding: 4
            icon.source: "qrc:/workspace/icons/settings.svg"; icon.width: 16; icon.height: 16; icon.color: panel.colors["text-muted"]
            Accessible.name: qsTr("Open AI summary settings")
            onClicked: {
                panel.settingsTopic = panel.topicId; enabledCheck.checked = panel.enabledForTopic
                panel.failed = false; panel.saving = false; settings.open()
            }
        }
        ToolButton {
            objectName: "channelTopicSummaryToggle"
            Layout.preferredWidth: 24; Layout.preferredHeight: 24; padding: 4
            icon.source: panel.expanded ? "qrc:/workspace/icons/chevron-up.svg" : "qrc:/workspace/icons/chevron-down.svg"
            icon.width: 16; icon.height: 16; icon.color: panel.colors["text-muted"]
            Accessible.name: panel.expanded ? qsTr("Collapse topic context") : qsTr("Expand topic context")
            Accessible.checkable: true; Accessible.checked: panel.expanded
            onClicked: panel.expanded = !panel.expanded
        }
    }
    Rectangle {
        visible: panel.expanded
        width: parent.width
        height: Math.min(218, content.implicitHeight + 16)
        radius: 8; color: panel.colors["bg-elevated"]; border.color: panel.colors["border-subtle"]
        Flickable {
            anchors.fill: parent; anchors.margins: 8; clip: true
            contentWidth: width; contentHeight: content.implicitHeight; boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            Column {
                id: content; width: parent.width; spacing: 8
                Text {
                    objectName: "channelTopicSummaryMarkdown"; visible: panel.summary.length > 0
                    width: parent.width; wrapMode: Text.Wrap; textFormat: Text.RichText
                    text: panel.summary ? nativeMarkdown.render(panel.summary, true).html : ""
                    onLinkActivated: link => panel.referenceActivated(link)
                    font.pixelSize: 12; color: panel.colors["text-primary"]; linkColor: panel.colors.accent
                }
                Text {
                    objectName: "channelTopicSummaryEmpty"; visible: !panel.summary.length
                    width: parent.width; wrapMode: Text.Wrap; textFormat: Text.PlainText
                    text: panel.enabledForTopic ? qsTr("The summary will appear after the messages are processed.") : qsTr("There is no summary for this topic.")
                    font.pixelSize: 12; color: panel.colors["text-muted"]
                }
                Text {
                    objectName: "channelTopicSummaryStale"
                    visible: !!panel.summary && !!panel.topic && panel.topic.summaryHasNewMessages
                    width: parent.width; wrapMode: Text.Wrap
                    text: qsTr("There are new messages that are not yet included in the summary.")
                    font.pixelSize: 12; color: panel.colors["text-secondary"]
                }
                Text {
                    objectName: "channelTopicSummaryDisabled"; visible: !panel.enabledForTopic
                    width: parent.width; wrapMode: Text.Wrap
                    text: qsTr("Automatic summary updates are disabled for this topic.")
                    font.pixelSize: 12; color: panel.colors["text-secondary"]
                }
            }
        }
    }
    Dialog {
        id: settings
        parent: Overlay.overlay; anchors.centerIn: parent; modal: true; focus: true
        width: Math.min(460, parent ? parent.width - 32 : 460)
        closePolicy: panel.saving ? Popup.NoAutoClose : Popup.CloseOnEscape | Popup.CloseOnPressOutside
        contentItem: ColumnLayout {
            Text { objectName: "channelTopicSummarySettingsTitle"; text: qsTr("AI summary settings"); color: panel.colors["text-primary"]; font.pixelSize: 18; font.weight: Font.DemiBold }
            CheckBox {
                id: enabledCheck; objectName: "channelTopicSummaryEnabled"
                text: qsTr("Automatic summary updates"); enabled: panel.permissions.canEditTopicSummary && !panel.saving
            }
            Text { visible: panel.failed; text: qsTr("Could not update the topic. Please try again."); color: panel.colors.danger }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                Button { text: qsTr("Close"); enabled: !panel.saving; onClicked: settings.close() }
                Button {
                    objectName: "channelTopicSummarySave"
                    text: qsTr("Save"); visible: panel.permissions.canEditTopicSummary
                    enabled: !panel.saving && enabledCheck.checked !== panel.enabledForTopic
                    onClicked: {
                        panel.saving = true
                        panel.controller.applyChannelTopicAction(panel.settingsTopic, "summary", String(enabledCheck.checked))
                    }
                }
            }
        }
    }
    Connections {
        target: panel.controller
        function onChannelTopicActionFinished(id, action, ok) {
            if (!settings.visible || !panel.saving || id !== panel.settingsTopic || action !== "summary") return
            panel.saving = false; panel.failed = !ok
            if (ok) settings.close()
        }
    }
}
