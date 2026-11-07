import QtQuick
import QtQuick.Controls

TextField {
    id: control
    required property var colors
    implicitHeight: 44
    padding: 12
    font.pixelSize: 14
    color: colors["text-primary"]
    placeholderTextColor: colors["text-muted"]
    selectionColor: colors.accent
    selectedTextColor: colors["on-accent"]
    selectByMouse: true
    background: Rectangle {
        radius: 10
        color: control.colors["text-field-bg"]
        border.width: 1
        border.color: control.activeFocus ? control.colors.accent : control.colors["border-subtle"]
    }
}
