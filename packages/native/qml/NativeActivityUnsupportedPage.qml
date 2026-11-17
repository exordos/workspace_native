pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

// Activity reactions collection unsupported state (issue #43).
// The reference has no reactions data source: activity-page.ui.tsx:569-571
// short-circuits the reactions filter to ActivityUnsupportedState
// (activity-page.ui.tsx:74-76, 103-109) - a plain `p-4 text-sm text-text-muted`
// line, i.e. 16 px padding and a 14 px muted line, top-aligned - and issues no
// collection request at all (activity-page.test.tsx:360). The page keeps the
// activity ChatChannelHeader titled activity.reactions
// (activity-page.ui.tsx:563, 648-653; chat-header-channel.ui.tsx:66, 82: text-sm
// font-semibold text-text-primary), which the native activity pages already
// render as the 54 px bg-elevated header with a 14 px DemiBold title and a
// 16 px close button (NativeStarredPage.qml).
// The reference rail hides the reactions entry (sidebar.lib.ts:90-96), so the
// /activity/reactions route is the only way here; there is deliberately no chip.
FocusScope {
    id: page
    objectName: "nativeActivityUnsupportedPage"
    required property var colors
    required property var controller
    onVisibleChanged: { if (visible) forceActiveFocus(); }
    Keys.onEscapePressed: page.controller.closeReactions()

    Rectangle {
        id: header
        objectName: "activityUnsupportedHeader"
        anchors.top: parent.top
        width: parent.width
        height: 54
        radius: 6
        color: page.colors["bg-elevated"]
        Text {
            objectName: "activityUnsupportedTitle"
            anchors.left: parent.left
            anchors.leftMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Reactions")
            textFormat: Text.PlainText
            color: page.colors["text-primary"]
            font.pixelSize: 14
            font.weight: Font.DemiBold
        }
        NativeIconButton {
            id: closeButton
            objectName: "activityUnsupportedCloseButton"
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            colors: page.colors
            asset: "close.svg"
            glyphSize: 16
            help: qsTr("Close reactions")
            onClicked: page.controller.closeReactions()
        }
    }

    // p-4 (16 px) under the header, items-start: one left-aligned, top-anchored,
    // wrapping text-sm (14 px) line in text-text-muted (#989898 light, #707070
    // dark in the default blue-cold palette, themes/blue-cold.ts:79, :16).
    Text {
        id: unsupportedText
        objectName: "activityUnsupportedText"
        anchors.top: header.bottom
        anchors.topMargin: 16
        anchors.left: parent.left
        anchors.leftMargin: 16
        anchors.right: parent.right
        anchors.rightMargin: 16
        text: qsTr("Reactions are not connected to Workspace messaging yet.")
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
        font.pixelSize: 14
        color: page.colors["text-muted"]
    }
}
