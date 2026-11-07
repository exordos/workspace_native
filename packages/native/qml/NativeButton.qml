import QtQuick
import QtQuick.Controls

Button {
    id: control
    required property var colors
    property bool primary: false
    implicitHeight: 40
    implicitWidth: Math.max(80, contentItem.implicitWidth + 28)
    horizontalPadding: 14
    verticalPadding: 8
    hoverEnabled: true
    contentItem: Text {
        text: control.text
        textFormat: Text.PlainText
        color: control.primary ? control.colors["on-accent"] : control.colors["text-primary"]
        font: control.font
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        opacity: control.enabled ? 1 : 0.5
    }
    background: Rectangle {
        radius: 10
        color: control.primary ? control.colors.accent : control.hovered ? control.colors["card-bg-active"] : control.colors["card-bg"]
        opacity: control.enabled ? (control.down ? 0.8 : 1) : 0.5
        border.width: control.visualFocus ? 2 : 1
        border.color: control.visualFocus ? control.colors.accent : control.colors["border-subtle"]
    }
}
