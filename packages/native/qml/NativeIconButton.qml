import QtQuick
import QtQuick.Controls

ToolButton {
    id: control
    required property var colors
    property string asset: ""
    property string help: ""
    property color iconColor: colors["icon-base"]
    property int glyphSize: 24
    property int glyphWidth: glyphSize
    property int glyphHeight: glyphSize
    property int cornerRadius: 6
    property bool selected: false
    implicitWidth: 36
    implicitHeight: 36
    padding: 0
    hoverEnabled: true
    display: AbstractButton.IconOnly
    icon.source: asset.length ? "qrc:/workspace/icons/" + asset : ""
    icon.color: enabled ? iconColor : colors["icon-disable"]
    icon.width: glyphWidth
    icon.height: glyphHeight
    Accessible.name: help
    ToolTip.visible: hovered && help.length > 0
    ToolTip.text: help
    background: Rectangle {
        radius: control.cornerRadius
        color: control.selected ? control.colors["card-bg-active"] : control.down || control.hovered ? control.colors["sidebar-item-hover"] : "transparent"
        border.width: control.visualFocus ? 1 : 0
        border.color: control.colors.accent
    }
}
