pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// Call card for meeting-URL messages (docs/NATIVE_DESKTOP_CALL_LAUNCH.md).
// Port of workspace_ui 61dacef7
// workspace-message-bubble-jitsi-card.ui.tsx: 'Call' label in call-green
// #26c038 at 15px, room display name, accent divider + location,
// participant-initial avatars, click/Enter/Space join with the accessible
// name 'Join call'. Participants: no native participant polling exists yet,
// so the card renders the reference fallback path (message author only).
Item {
    id: card
    required property var colors
    required property bool own
    required property string callUrl
    required property string roomName
    required property string locationName
    required property string authorName
    required property string timeText
    signal joinRequested(string meetingUrl)

    implicitHeight: 70
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: qsTr("Join call")
    Accessible.onPressAction: card.joinRequested(card.callUrl)
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
            event.accepted = true
            card.joinRequested(card.callUrl)
        }
    }
    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: card.joinRequested(card.callUrl)
    }

    readonly property string callGreen: "#26c038"
    readonly property string displayName: card.roomName.length > 0 ? card.roomName : qsTr("Call name")
    readonly property string initials: {
        const parts = card.authorName.trim().split(/\s+/).filter(part => part.length > 0)
        const letters = ((parts.length > 0 ? parts[0][0] : "") + (parts.length > 1 ? parts[1][0] : "")).toUpperCase()
        return letters.length > 0 ? letters : "?"
    }
    readonly property string borderColor: card.own ? colors["msg-own-bg"] : colors["bg-elevated"]

    // Row 1: Call label, room display name, accent divider + location, phone.
    Item {
        id: titleRow
        width: parent.width
        height: 20
        Text {
            id: callLabel
            objectName: "callCardLabel"
            text: qsTr("Call")
            textFormat: Text.PlainText
            color: card.callGreen
            font.pixelSize: 15
            font.weight: Font.DemiBold
            lineHeightMode: Text.FixedHeight
            lineHeight: 20
        }
        Text {
            id: roomLabel
            objectName: "callCardName"
            x: callLabel.width + 8
            width: Math.max(0, Math.min(implicitWidth,
                titleRow.width - callLabel.width - 8 - phoneIcon.width - 8 - locationRow.width - (locationRow.visible ? 8 : 0)))
            text: card.displayName
            textFormat: Text.PlainText
            color: card.colors["text-primary"]
            font.pixelSize: 15
            font.weight: Font.Medium
            lineHeightMode: Text.FixedHeight
            lineHeight: 20
            elide: Text.ElideRight
        }
        Row {
            id: locationRow
            visible: card.locationName.length > 0
            x: roomLabel.x + roomLabel.width + 8
            height: 20
            spacing: 8
            Rectangle {
                width: 4; height: 16; radius: 2
                anchors.verticalCenter: parent.verticalCenter
                color: card.colors.accent
                Accessible.ignored: true
            }
            Text {
                width: Math.max(0, Math.min(implicitWidth,
                    titleRow.width - locationRow.x - phoneIcon.width - 8))
                text: card.locationName
                textFormat: Text.PlainText
                color: card.colors["text-muted"]
                font.pixelSize: 15
                lineHeightMode: Text.FixedHeight
                lineHeight: 20
                elide: Text.ElideRight
            }
        }
        NativeIconButton {
            id: phoneIcon
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            colors: card.colors
            asset: "phone.svg"
            glyphSize: 18
            width: 22; height: 22; padding: 0
            enabled: false
            opacity: 1
            Accessible.ignored: true
            icon.color: card.callGreen
        }
    }

    // Row 2: chevron, participant-initial avatars, message time.
    Item {
        id: metaRow
        y: 38
        width: parent.width
        height: 32
        NativeIconButton {
            id: chevron
            objectName: "callCardChevron"
            anchors.verticalCenter: parent.verticalCenter
            colors: card.colors
            asset: "chevron-right.svg"
            glyphSize: 12
            width: 14; height: 14; padding: 1
            enabled: false
            opacity: 1
            Accessible.ignored: true
            icon.color: card.colors["text-muted"]
            rotation: 45
        }
        Rectangle {
            id: authorAvatar
            objectName: "callCardAvatar"
            x: chevron.width + 2
            anchors.verticalCenter: parent.verticalCenter
            width: 32; height: 32; radius: 16
            color: card.colors["bg"]
            border.width: 2
            border.color: card.borderColor
            Text {
                anchors.centerIn: parent
                text: card.initials
                textFormat: Text.PlainText
                color: card.colors["text-primary"]
                font.pixelSize: 10
            }
        }
        Text {
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            text: card.timeText
            textFormat: Text.PlainText
            color: card.colors["text-muted"]
            font.pixelSize: 12
            Accessible.ignored: true
        }
    }
}
