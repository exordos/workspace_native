pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Item {
    id: capsule
    objectName: "quickReactionCapsule-" + messageId
    required property var colors
    required property var choices
    required property var reactions
    required property string messageId
    property bool allowed: false
    property string emojiFamily: ""
    property int emojiPixelSize: 24
    property real availableHeight: 176
    property bool expanded: false
    property bool suppressFocusOpen: false
    readonly property bool hasFocus: buttons.activeFocus
    signal aboutToOpen
    signal chosen(string emojiName)
    width: 0
    height: 0
    z: 3

    function open() {
        aboutToOpen();
        leaveTimer.stop();
        if (allowed && choices.length > 1)
            expanded = true;
    }
    function close(restoreFocus) {
        enterTimer.stop();
        leaveTimer.stop();
        suppressFocusOpen = restoreFocus;
        expanded = false;
        buttons.currentIndex = 0;
        if (restoreFocus)
            buttons.forceActiveFocus(Qt.OtherFocusReason);
    }
    function pick(name, restoreFocus) {
        close(restoreFocus);
        chosen(name);
    }
    function moveChoice(key) {
        const last = choices.length - 1;
        if (!expanded) {
            open();
            buttons.currentIndex = key === Qt.Key_Up || key === Qt.Key_End ? last : 0;
        } else if (key === Qt.Key_Home)
            buttons.currentIndex = 0;
        else if (key === Qt.Key_End)
            buttons.currentIndex = last;
        else
            buttons.currentIndex = (buttons.currentIndex + (key === Qt.Key_Up ? -1 : 1) + choices.length) % choices.length;
        const rowY = (choices.length - 1 - buttons.currentIndex) * 36;
        if (rowY < buttons.contentY)
            buttons.contentY = rowY;
        else if (rowY + 36 > buttons.contentY + buttons.height)
            buttons.contentY = rowY + 36 - buttons.height;
        buttons.forceActiveFocus(Qt.TabFocusReason);
    }
    function owned(name) {
        for (let i = 0; i < reactions.length; ++i)
            if (reactions[i].emojiName === name)
                return Boolean(reactions[i].ownKnown && reactions[i].own);
        return false;
    }
    onAllowedChanged: if (!allowed)
        close(false)
    onHasFocusChanged: if (!hasFocus) {
        suppressFocusOpen = false;
        close(false);
    }
    Timer {
        id: enterTimer
        interval: 150
        onTriggered: capsule.open()
    }
    Timer {
        id: leaveTimer
        interval: 200
        onTriggered: capsule.close(false)
    }
    Rectangle {
        id: frame
        width: 40
        height: capsule.expanded ? Math.min(176, Math.max(40, capsule.availableHeight), capsule.choices.length * 36 + 4) : 40
        x: -20
        y: -height
        radius: 20
        color: capsule.colors["card-bg"]
        border.color: capsule.colors["border-subtle"]
        scale: capsule.expanded ? 1 : 0.6
        transformOrigin: Item.Bottom
        clip: true
        HoverHandler {
            id: frameHover
            onHoveredChanged: {
                if (hovered) {
                    leaveTimer.stop();
                    if (!capsule.expanded)
                        enterTimer.restart();
                } else {
                    enterTimer.stop();
                    leaveTimer.restart();
                }
            }
        }
        Flickable {
            id: buttons
            objectName: "quick-reactions-" + capsule.messageId
            x: 2
            y: 2
            width: 36
            height: parent.height - 4
            property int currentIndex: 0
            contentWidth: 36
            contentHeight: quickColumn.height
            onContentHeightChanged: contentY = Math.max(0, contentHeight - height)
            onHeightChanged: contentY = Math.max(0, contentHeight - height)
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            activeFocusOnTab: true
            onActiveFocusChanged: if (activeFocus && !capsule.suppressFocusOpen)
                capsule.open()
            Keys.onPressed: event => {
                if ([Qt.Key_Up, Qt.Key_Down, Qt.Key_Home, Qt.Key_End].indexOf(event.key) >= 0) {
                    capsule.moveChoice(event.key);
                    event.accepted = true;
                } else if (event.key === Qt.Key_Escape && capsule.expanded) {
                    capsule.close(true);
                    event.accepted = true;
                } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Space) {
                    const entry = capsule.choices[currentIndex];
                    if (capsule.allowed && entry) {
                        capsule.close(true);
                        capsule.chosen(entry.emojiName);
                    }
                    event.accepted = true;
                }
            }
            Column {
                id: quickColumn
                width: 36
                Repeater {
                    model: capsule.expanded ? capsule.choices.slice().reverse() : capsule.choices.slice(0, 1)
                    delegate: ToolButton {
                        id: quickChoice
                        required property var modelData
                        required property int index
                        readonly property int logicalIndex: capsule.expanded ? capsule.choices.length - 1 - index : index
                        property bool touchPreview: false
                        readonly property bool owned: capsule.owned(modelData.emojiName)
                        objectName: logicalIndex === 0 ? "add-reaction-" + capsule.messageId : "quick-reaction-" + capsule.messageId + "-" + modelData.emojiName
                        width: 36
                        height: 36
                        padding: 0
                        hoverEnabled: true
                        enabled: capsule.allowed
                        activeFocusOnTab: false
                        focusPolicy: Qt.NoFocus
                        Accessible.name: owned ? qsTr("Remove your reaction: %1").arg(modelData.emojiName) : qsTr("Add reaction: %1").arg(modelData.emojiName)
                        Accessible.checkable: true
                        Accessible.checked: owned
                        ToolTip.visible: hovered && !capsule.expanded
                        ToolTip.text: Accessible.name
                        background: Rectangle {
                            radius: 18
                            color: quickChoice.owned ? capsule.colors["accent-soft"] : quickChoice.hovered || (buttons.activeFocus && buttons.currentIndex === quickChoice.logicalIndex) ? capsule.colors["sidebar-item-hover"] : "transparent"
                            border.width: buttons.activeFocus && buttons.currentIndex === quickChoice.logicalIndex ? 1 : 0
                            border.color: capsule.colors.accent
                        }
                        contentItem: Text {
                            text: quickChoice.modelData.pickerUnicode || quickChoice.modelData.unicode
                            textFormat: Text.PlainText
                            font.family: capsule.emojiFamily
                            font.pixelSize: capsule.emojiPixelSize
                            color: capsule.colors["text-primary"]
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        onPressed: {
                            touchPreview = false;
                            if (!capsule.expanded)
                                enterTimer.stop();
                        }
                        TapHandler {
                            acceptedDevices: PointerDevice.TouchScreen
                            onPressedChanged: if (pressed && !capsule.expanded) {
                                quickChoice.touchPreview = true;
                                capsule.open();
                            }
                        }
                        onClicked: {
                            if (touchPreview) {
                                touchPreview = false;
                                return;
                            }
                            capsule.pick(modelData.emojiName, false);
                        }
                    }
                }
            }
        }
    }
}
