import QtQuick
import QtQuick.Controls

// Call window shell (issue #162): the visual counterpart of
// workspace::JitsiCallWindowController, a pixel-close port of the reference
// jitsi-call.ui.tsx window surface (workspace_ui 61dacef7): rounded
// bg-elevated surface with a border-subtle hairline, header row with
// truncated title/subtitle and fullscreen/minimize/close controls, and the
// loading / load-failed / invalid-link embed states. All geometry, mode and
// state decisions come from the controller (docs/NATIVE_DESKTOP_CALL_WINDOW.md);
// this file only renders and routes clicks.
Item {
    id: shell
    required property var colors
    required property var controller
    // Expanded renders inside the app scrim; PiP renders inside a frameless
    // tool window that the wrapper moves and resizes.
    property bool pipMode: false
    // Set by the PiP wrapper so the header drag can move that window.
    property var dragWindow: null

    // Reference constants (jitsi-call.ui.tsx).
    readonly property int headerRadius: 12
    readonly property bool atSmall: shell.width >= 640
    readonly property int headerPaddingX: atSmall ? 16 : 8
    readonly property int headerPaddingY: atSmall ? 8 : 6
    readonly property int buttonPadding: atSmall ? 8 : 6

    // The controller is a C++ context object; its localized texts already
    // follow the app locale through QTranslator.
    readonly property string titleText: controller.titleText
    readonly property string subtitleText: controller.subtitleText
    readonly property string stateText: controller.stateText
    // #162 finding 2: exactly ONE embed surface exists per call session;
    // Main.qml owns it and re-parents it between the expanded shell and the
    // PiP window. Each shell only offers this slot, so a minimize/expand
    // toggle moves the presentation and never creates or destroys a second
    // WebEngine session for the same meeting.
    property Item embedHost: embedAreaItem

    Rectangle {
        id: surface
        objectName: "callWindowSurface"
        anchors.fill: parent
        radius: shell.headerRadius
        color: shell.colors["bg-elevated"]
        border.width: 1
        border.color: shell.colors["border-subtle"]
        clip: true
    }

    Rectangle {
        // Header bottom hairline (border-b border-border-subtle).
        objectName: "callWindowHeaderBorder"
        anchors.left: parent.left
        anchors.right: parent.right
        y: headerItem.y + headerItem.height
        height: 1
        color: shell.colors["border-subtle"]
    }

    Item {
        id: headerItem
        objectName: "callWindowHeader"
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: Math.max(titleColumn.height, controlsRow.height) + shell.headerPaddingY * 2

        Column {
            id: titleColumn
            objectName: "callWindowTitleColumn"
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            leftPadding: shell.headerPaddingX
            rightPadding: shell.headerPaddingX
            width: Math.max(0, Math.min(
                shell.width - controlsRow.implicitWidth - shell.headerPaddingX * 3,
                titleText.implicitWidth))
            spacing: 0

            Text {
                id: titleText
                objectName: "callWindowTitleText"
                width: parent.availableWidth
                text: shell.titleText
                elide: Text.ElideRight
                font.pixelSize: shell.atSmall ? 14 : 12
                font.weight: Font.DemiBold
                color: shell.colors["text-primary"]
            }
            Text {
                // Subtitle is hidden while the participant count is unknown.
                objectName: "callWindowSubtitleText"
                visible: shell.subtitleText.length > 0
                width: parent.availableWidth
                text: shell.subtitleText
                elide: Text.ElideRight
                font.pixelSize: 11
                color: shell.colors["text-muted"]
            }
        }

        Row {
            id: controlsRow
            objectName: "callWindowControls"
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            rightPadding: shell.headerPaddingX - shell.buttonPadding
            spacing: 2 // gap-0.5

            ShellIconButton {
                id: fullscreenButton
                objectName: "callWindowFullscreenButton"
                visible: !shell.pipMode // fullscreen control is expanded-only
                glyph: shell.controller.fullscreen ? "fullscreen-exit" : "fullscreen"
                glyphSize: 28
                label: shell.controller.fullscreenLabel
                tooltip: shell.controller.fullscreenLabel
                colors: shell.colors
                onClicked: shell.controller.toggleFullscreen()
            }
            ShellIconButton {
                id: minimizeButton
                objectName: "callWindowMinimizeButton"
                glyph: shell.pipMode ? "chevron-up" : "chevron-down"
                glyphSize: 18
                label: shell.controller.minimizeLabel
                tooltip: shell.controller.minimizeTooltip
                colors: shell.colors
                onClicked: shell.controller.toggleMinimize()
            }
            ShellIconButton {
                id: closeButton
                objectName: "callWindowCloseButton"
                glyph: "close"
                glyphSize: 18
                label: shell.controller.closeLabel
                tooltip: shell.controller.closeLabel
                colors: shell.colors
                onClicked: shell.controller.close()
            }
        }

        // Dragging the header moves the PiP surface only; the expanded
        // window is a dialog and never drags (reference enables user controls
        // only in PiP mode).
        MouseArea {
            id: rootHeaderMouse
            anchors.fill: parent
            enabled: shell.pipMode && shell.dragWindow !== null
            z: -1
            property real grabX: 0
            property real grabY: 0
            onPressed: (mouse) => {
                grabX = mouse.x
                grabY = mouse.y
            }
            onPositionChanged: (mouse) => {
                if (!pressed || shell.dragWindow === null)
                    return
                shell.dragWindow.x += shell.x + mouse.x - grabX
                shell.dragWindow.y += shell.y + mouse.y - grabY
            }
            onReleased: {
                const target = shell.dragWindow
                if (target !== null)
                    shell.controller.setPipBounds(Qt.rect(target.x, target.y,
                        target.width, target.height))
            }
        }
    }

    Item {
        id: embedAreaItem
        objectName: "callWindowEmbedArea"
        anchors.top: headerItem.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true

        Column {
            objectName: "callWindowLoadingState"
            anchors.centerIn: parent
            spacing: 12
            visible: shell.controller.embedState === 1 // Loading
            BusyIndicator {
                objectName: "callWindowBusy"
                anchors.horizontalCenter: parent.horizontalCenter
                running: visible
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: shell.stateText
                font.pixelSize: 14
                color: shell.colors["text-muted"]
            }
        }

        Column {
            objectName: "callWindowErrorState"
            anchors.centerIn: parent
            spacing: 12
            visible: shell.controller.embedState === 3 // LoadFailed
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: shell.stateText
                font.pixelSize: 14
                color: shell.colors["notice-base"]
            }
            Rectangle {
                objectName: "callWindowRetryButton"
                anchors.horizontalCenter: parent.horizontalCenter
                width: retryLabel.implicitWidth + 32
                height: retryLabel.implicitHeight + 16
                radius: 8
                color: shell.colors.accent
                Text {
                    id: retryLabel
                    anchors.centerIn: parent
                    text: shell.controller.retryText
                    font.pixelSize: 14
                    font.weight: Font.Medium
                    color: "black"
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: shell.controller.retry()
                }
            }
        }

        Text {
            objectName: "callWindowInvalidState"
            anchors.centerIn: parent
            visible: shell.controller.embedState === 4 // InvalidUrl
            text: shell.stateText
            font.pixelSize: 14
            color: shell.colors["notice-base"]
        }

        // The embed surface itself is owned by Main.qml (single instance per
        // call session, finding 2); this slot only hosts it while visible.
    }
}
