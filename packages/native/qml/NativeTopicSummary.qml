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
                panel.settingsTopic = panel.topicId
                panel.failed = false; panel.saving = false
                if (settings.tabs.length === 0 && settings.allowedTabs().length === 0) return
                settings.open()
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
    NativeTopicSummarySettingsDialog {
        id: settings
        parent: Overlay.overlay
        controller: panel.controller
        colors: panel.colors
        topicId: panel.settingsTopic
        topicName: panel.topic && panel.topic.name ? String(panel.topic.name) : ""
        streamName: panel.permissions.name ? String(panel.permissions.name) : ""
        onTopicConfigRequested: (topic, enabled, prompt, promptIsDefault, effort) => {
            panel.saving = true
            // Controller seam for prompt/reasoning edits (this round's controller extension).
            if (typeof panel.controller.saveTopicSummaryTopic === "function")
                panel.controller.saveTopicSummaryTopic(topic, enabled, promptIsDefault ? "" : prompt, effort)
            else
                panel.controller.applyChannelTopicAction(topic, "summary", String(enabled))
        }
    }
    Connections {
        target: panel.controller
        function onChannelTopicActionFinished(id, action, ok) {
            if (!settings.visible || !panel.saving || id !== panel.settingsTopic || action !== "summary") return
            panel.saving = false
        }
    }
}
