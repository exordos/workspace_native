pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

ScrollView {
    id: panel
    required property var preferences
    required property var colors
    required property bool appearance
    clip: true
    contentWidth: availableWidth
    readonly property var sections: appearance ? [
        {key:"theme", title:qsTr("Theme settings"), choices:[]},
        {key:"sidebarSortMode", title:qsTr("Stream and topic order"), choices:[
            ["last_message",qsTr("Last message")], ["unread_first",qsTr("Unread first")], ["alphabetical",qsTr("By name")]]},
        {key:"folderRailLayout", title:qsTr("Folder layout"), choices:[
            ["vertical",qsTr("Vertical rail")], ["horizontal",qsTr("Horizontal tabs")]]},
        {key:"chatListDensity", title:qsTr("Chat list density"), choices:[
            ["general",qsTr("Standard")], ["compact",qsTr("Compact")]]}
    ] : [
        {key:"chatClickAction",title:qsTr("Chat click"),hint:qsTr("What to open when clicking a chat"),choices:[
            ["stream",qsTr("General chat and topics")],["latestTopic",qsTr("Latest topic"),qsTr("Click the avatar to open the general chat")]]},
        {key:"notificationSound",title:qsTr("Notification sound"),choices:[
            ["default",qsTr("Default")],["subtle",qsTr("Subtle")],["digital",qsTr("Digital")],["glass",qsTr("Glass")],["pulse",qsTr("Pulse")],["none",qsTr("Off")]]},
        {key:"locale",title:qsTr("Language"),choices:[["en","English"],["ru","Русский"]]},
        {key:"authIdleTimeout",title:qsTr("Auto sign-out"),hint:qsTr("After inactivity"),choices:[
            ["6h",qsTr("6 hours")],["12h",qsTr("12 hours")],["24h",qsTr("24 hours")],["3d",qsTr("3 days")],["7d",qsTr("7 days")],["never",qsTr("Never")]]}
    ]
    readonly property var modes: [["light",qsTr("Light")],["dark",qsTr("Dark")],["system",qsTr("System")]]
    readonly property var palettes: [["orange-warm",qsTr("Warm orange")],["blue-cold",qsTr("Cold blue")],
        ["blue-mist",qsTr("Blue mist")],["emerald-chat",qsTr("Emerald chat")]]

    function setChoice(key, value) {
        preferences[key] = value
        if (key === "notificationSound") preferences.previewNotificationSound()
    }
    NativeTheme { id: previewTheme }
    component Option: Button {
        id: option
        required property string key
        required property var choice
        objectName: "accountOption-" + key + "-" + choice[0]
        checked: panel.preferences[key] === choice[0]
        Accessible.checkable: true
        Accessible.checked: checked
        Accessible.name: choice[1]
        onClicked: panel.setChoice(key, choice[0])
        padding: 6
        implicitHeight: choice.length > 2 ? 50 : 36
        contentItem: RowLayout {
            spacing: 4
            Rectangle {
                visible: option.key === "paletteId"
                Layout.preferredWidth: 12; Layout.preferredHeight: 12; radius: 6
                color: visible ? previewTheme.palettes[option.choice[0]][panel.preferences.dark ? "dark" : "light"].accent : "transparent"
            }
            Image {
                visible: option.key === "mode"
                Layout.preferredWidth: 18; Layout.preferredHeight: 18
                source: visible ? "qrc:/workspace/icons/" + ({light:"wb_sunny",dark:"bedtime",system:"desktop_windows"})[option.choice[0]] + ".svg" : ""
                layer.enabled: true
                layer.effect: MultiEffect { colorization: 1; colorizationColor: option.checked ? panel.colors.accent : panel.colors["text-muted"] }
            }
            ColumnLayout {
                Layout.fillWidth: true; spacing: 1
                Label { Layout.fillWidth: true; text: option.choice[1]; font.pixelSize: 13; font.weight: option.checked ? Font.Medium : Font.Normal; elide: Text.ElideRight; color: panel.colors["text-primary"] }
                Label { Layout.fillWidth: true; visible: text.length > 0; text: option.choice[2] || ""; font.pixelSize: 10; wrapMode: Text.Wrap; color: panel.colors["text-secondary"] }
            }
            Label { visible: option.checked; text: "✓"; color: panel.colors.accent; font.pixelSize: 16; Accessible.ignored: true }
        }
        background: Rectangle { radius: 8; color: option.hovered || option.visualFocus ? panel.colors["card-bg-active"] : panel.colors["bg-elevated"]; border.color: option.visualFocus ? panel.colors.accent : panel.colors["border-subtle"] }
    }
    Column {
        width: panel.availableWidth
        Repeater {
            model: panel.sections
            Column {
                id: section
                required property var modelData
                width: panel.availableWidth
                property bool expanded: false
                Button {
                    id: toggle
                    objectName: "accountSetting-" + section.modelData.key
                    width: section.width; height: section.modelData.hint ? 54 : 42
                    padding: 8
                    Accessible.name: section.modelData.title
                    Accessible.checkable: true; Accessible.checked: section.expanded
                    onClicked: section.expanded = !section.expanded
                    contentItem: RowLayout {
                        spacing: 5
                        ColumnLayout {
                            Layout.fillWidth: true; spacing: 1
                            Label { Layout.fillWidth: true; text: section.modelData.title; font.pixelSize: 14; elide: Text.ElideRight; color: panel.colors["text-primary"] }
                            Label { Layout.fillWidth: true; visible: text.length > 0; text: section.modelData.hint || ""; font.pixelSize: 11; wrapMode: Text.Wrap; color: panel.colors["text-muted"] }
                        }
                        Label {
                            objectName: "accountSettingValue-" + section.modelData.key
                            readonly property var selected: section.modelData.choices.find(c => c[0] === panel.preferences[section.modelData.key])
                            text: selected ? selected[1] : ""; font.pixelSize: 12; color: panel.colors["text-secondary"]
                        }
                        Label { text: section.expanded ? "⌃" : "›"; color: panel.colors["text-muted"]; font.pixelSize: 16; Accessible.ignored: true }
                    }
                    background: Rectangle { radius: 8; color: toggle.hovered || toggle.visualFocus ? panel.colors["card-bg-active"] : "transparent" }
                }
                Column {
                    visible: section.expanded && section.modelData.key !== "theme"
                    x: 8; width: parent.width - 16
                    Repeater {
                        model: section.modelData.choices
                        Option { required property var modelData; key: section.modelData.key; choice: modelData; width: parent.width }
                    }
                }
                ColumnLayout {
                    visible: section.expanded && section.modelData.key === "theme"
                    width: parent.width - 16; x: 8
                    GridLayout {
                        columns: 3; Layout.fillWidth: true
                        Repeater { model: panel.modes; Option { required property var modelData; key: "mode"; choice: modelData; Layout.fillWidth: true } }
                    }
                    GridLayout {
                        columns: 2; Layout.fillWidth: true
                        Repeater { model: panel.palettes; Option { required property var modelData; key: "paletteId"; choice: modelData; Layout.fillWidth: true } }
                    }
                }
            }
        }
    }
}
