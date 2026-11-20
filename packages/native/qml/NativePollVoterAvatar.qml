pragma ComponentBehavior: Bound
import QtQuick

Rectangle {
    id: avatar
    required property string userUuid
    required property var colors
    required property var controller
    readonly property var user: {
        controller.ownerKey
        return controller.profileSnapshot(userUuid)
    }
    readonly property string displayName: user.title && user.title !== userUuid
        ? String(user.title) : "#" + userUuid.slice(0, 8)
    radius: width / 2
    color: colors["bg-elevated"]
    border.color: colors["border-subtle"]
    Text {
        anchors.centerIn: parent
        text: avatar.displayName.substring(0, 1)
        textFormat: Text.PlainText
        color: avatar.colors["text-primary"]
        font.pixelSize: 12
    }
    NativeAvatarImage {
        anchors.fill: parent
        reference: String(avatar.user.avatar || "")
    }
}
