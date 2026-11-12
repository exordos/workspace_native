import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

pragma ComponentBehavior: Bound

// Top bar workspace session switcher (issue #144).
// Port of workspace_ui packages/web/src/widgets/top-bar/
// top-bar-workspace-session-switcher.ui.tsx at 61dacef78dab2f5d0be76f283bebf49fe29b84af:
// quick 40x40 logo buttons for up to MAX_VISIBLE_SESSIONS=3 remembered
// sessions, a +N overflow chip, and a wide dropdown with per-account rows,
// per-org logout with confirmation and an "Add server" row.
Row {
    id: switcher
    required property var colors
    required property var controller
    spacing: 6
    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Select account")

    // MAX_VISIBLE_SESSIONS (switcher :34).
    readonly property int maxVisibleSessions: 3
    readonly property var rows: controller.sessionSwitcherSessions
    readonly property var visibleRows: rows.slice(0, maxVisibleSessions)
    readonly property int hiddenCount: Math.max(0, rows.length - maxVisibleSessions)

    // The dropdown Badge uses UNREAD_COUNT_DISPLAY_LIMIT = 1000
    // (shared/lib/count-format.lib.ts:1); issue #144 pins the cap label "1000+".
    function badgeLabel(unread) {
        return unread > 1000 ? "1000+" : String(unread);
    }

    function beginLogout(title, accountId) {
        logoutConfirm.pendingTitle = title;
        logoutConfirm.pendingId = accountId;
        sessionMenu.close();
        logoutConfirm.open();
    }

    Repeater {
        model: switcher.visibleRows
        // InstanceQuickButton (switcher :93-153): 40x40 rounded-lg button,
        // current session adds the card-bg-active background and a 2px inset
        // ring-border-subtle on the 36x36 rounded-md logo frame; an
        // unread>0 session paints the 10x10 badge-bg dot top-right.
        delegate: Button {
            id: quickButton
            required property var modelData
            objectName: "sessionQuick-" + quickButton.modelData.id
            width: 40
            height: 40
            padding: 0
            hoverEnabled: true
            background: Rectangle {
                radius: 8
                color: quickButton.modelData.isCurrent || quickButton.hovered
                    ? switcher.colors["card-bg-active"] : "transparent"
            }
            contentItem: Item {
                Rectangle {
                    objectName: "sessionFrame-" + quickButton.modelData.id
                    width: 36
                    height: 36
                    anchors.centerIn: parent
                    radius: 6
                    color: switcher.colors["bg"]
                    clip: true
                    Image {
                        objectName: "sessionLogo-" + quickButton.modelData.id
                        anchors.fill: parent
                        source: "qrc:/workspace/brand/organization-fallback.svg"
                        fillMode: Image.PreserveAspectFit
                        Accessible.ignored: true
                    }
                    Rectangle {
                        objectName: "sessionRing-" + quickButton.modelData.id
                        anchors.fill: parent
                        visible: quickButton.modelData.isCurrent
                        radius: 6
                        color: "transparent"
                        border.width: 2
                        border.color: switcher.colors["border-subtle"]
                    }
                }
                Rectangle {
                    objectName: "sessionDot-" + quickButton.modelData.id
                    visible: quickButton.modelData.unread > 0
                    width: 10
                    height: 10
                    radius: 5
                    color: switcher.colors["badge-bg"]
                    border.width: 1
                    border.color: switcher.colors["bg"]
                    anchors.top: parent.top
                    anchors.right: parent.right
                }
            }
            ToolTip.visible: hovered
            ToolTip.text: quickButton.modelData.displayName
            Accessible.name: quickButton.modelData.isCurrent
                ? qsTr("Current server") + ": " + quickButton.modelData.title
                : quickButton.modelData.title
            onClicked: switcher.controller.selectWorkspaceSession(quickButton.modelData.id)
        }
    }

    Item {
        // Overflow "+N" chip (switcher :332-336): rounded-md bg-bg-elevated,
        // px-1.5 py-0.5, text-xs text-text-muted.
        id: overflowCell
        visible: switcher.hiddenCount > 0
        width: overflowChip.width
        height: 40
        Rectangle {
            id: overflowChip
            objectName: "sessionOverflow"
            anchors.verticalCenter: parent.verticalCenter
            radius: 6
            color: switcher.colors["bg-elevated"]
            implicitWidth: overflowLabel.implicitWidth + 12
            implicitHeight: overflowLabel.implicitHeight + 4
            Text {
                id: overflowLabel
                objectName: "sessionOverflowLabel"
                anchors.centerIn: parent
                text: "+" + switcher.hiddenCount
                textFormat: Text.PlainText
                color: switcher.colors["text-muted"]
                font.pixelSize: 12
            }
        }
    }

    NativeIconButton {
        // Dropdown trigger: 32x32 rounded-lg with chevron-down 14
        // (switcher :344-353).
        objectName: "sessionMenuTrigger"
        colors: switcher.colors
        asset: "chevron-down.svg"
        help: qsTr("Select account")
        width: 32
        height: 32
        glyphSize: 14
        onClicked: sessionMenu.open()
    }

    Popup {
        // DropdownMenu contentVariant="wide" (min-w 220, py-1), sideOffset 6,
        // align start, rounded-lg bg-bg-elevated border-border-subtle.
        id: sessionMenu
        parent: Overlay.overlay
        padding: 4
        implicitWidth: 220
        width: Math.max(220, implicitContentWidth + padding * 2)
        // The menu must never outgrow the window: rows past the bottom
        // scroll inside the viewport instead of leaving the window
        // (review #170). The menu top and the bounded viewport height both
        // derive from the same fixed menuTop so the popup never needs
        // Qt's line-of-sight repositioning (which otherwise pushes an
        // over-tall menu off the top of the window).
        readonly property int menuTop: switcher.mapToItem(null, 0, switcher.height).y + 6
        readonly property int maxViewportHeight: parent
            ? Math.max(80, parent.height - menuTop - 20) : 1000000
        modal: false
        dim: false
        // Grab window focus when open so keyboard activation of the rows
        // (and Escape-to-close) works (review #170).
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        x: switcher.mapToItem(null, 0, 0).x
        y: menuTop
        background: Rectangle {
            color: switcher.colors["bg-elevated"]
            border.color: switcher.colors["border-subtle"]
            radius: 8
        }
        contentItem: Flickable {
            id: sessionMenuScroll
            objectName: "sessionSwitcherScroll"
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            implicitWidth: sessionMenuColumn.implicitWidth
            height: implicitHeight
            implicitHeight: Math.min(sessionMenuColumn.height, sessionMenu.maxViewportHeight)
            contentWidth: width
            contentHeight: sessionMenuColumn.height
            ColumnLayout {
                id: sessionMenuColumn
                objectName: "sessionSwitcherPopup"
                width: sessionMenuScroll.width
                height: implicitHeight
                spacing: 0
                Repeater {
                    model: switcher.rows
                    // One row per remembered session (switcher :203-279): 40x40
                    // logo frame (current: inset ring + card-bg-active row),
                    // unread Badge top-right, login + "host . projectId" column,
                    // 24x24 per-org logout button. Item padding px-3 py-2, gap-2.
                    delegate: Rectangle {
                        id: sessionRow
                        required property var modelData
                        objectName: "sessionRow-" + sessionRow.modelData.id
                        Layout.fillWidth: true
                        implicitHeight: 40 + 16
                        radius: 8
                        color: sessionRow.modelData.isCurrent ? switcher.colors["card-bg-active"]
                            : rowMouse.containsMouse ? switcher.colors["sidebar-item-hover"] : "transparent"
                        // Keyboard access (review #170): each selectable row is a
                        // focusable button-like control activated by Enter/Space.
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Button
                        Accessible.name: sessionRow.modelData.title
                        border.width: sessionRow.activeFocus ? 2 : 0
                        border.color: switcher.colors["border-subtle"]
                        function selectSession() {
                            sessionMenu.close();
                            switcher.controller.selectWorkspaceSession(sessionRow.modelData.id);
                        }
                        // Keyboard focus must never land on a clipped row
                        // (review #170): when the tab chain enters this row,
                        // scroll the viewport so the focus ring is on screen.
                        onActiveFocusChanged: {
                            if (!sessionRow.activeFocus) return
                            const top = sessionRow.mapToItem(sessionMenuScroll.contentItem, 0, 0).y
                            const bottom = top + sessionRow.height
                            if (top < sessionMenuScroll.contentY)
                                sessionMenuScroll.contentY = top
                            else if (bottom > sessionMenuScroll.contentY + sessionMenuScroll.height)
                                sessionMenuScroll.contentY = bottom - sessionMenuScroll.height
                        }
                        Keys.onReturnPressed: (event) => {
                            event.accepted = true;
                            sessionRow.selectSession();
                        }
                        Keys.onSpacePressed: (event) => {
                            event.accepted = true;
                            sessionRow.selectSession();
                        }
                        Accessible.onPressAction: sessionRow.selectSession()
                        MouseArea {
                            // Declared first so the per-row logout button keeps
                            // its own mouse handling; the rest of the row selects.
                            id: rowMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                sessionMenu.close();
                                switcher.controller.selectWorkspaceSession(sessionRow.modelData.id);
                            }
                        }
                        Row {
                            id: rowContent
                            x: 12
                            y: 8
                            spacing: 8
                            Rectangle {
                                id: rowFrame
                                objectName: "sessionRowFrame-" + sessionRow.modelData.id
                                width: 40
                                height: 40
                                radius: 8
                                color: switcher.colors["bg"]
                                Image {
                                    objectName: "sessionRowLogo-" + sessionRow.modelData.id
                                    width: 36
                                    height: 36
                                    anchors.centerIn: parent
                                    source: "qrc:/workspace/brand/organization-fallback.svg"
                                    fillMode: Image.PreserveAspectFit
                                    Accessible.ignored: true
                                }
                                Rectangle {
                                    objectName: "sessionRowRing-" + sessionRow.modelData.id
                                    anchors.fill: parent
                                    visible: sessionRow.modelData.isCurrent
                                    radius: 8
                                    color: "transparent"
                                    border.width: 2
                                    border.color: switcher.colors["border-subtle"]
                                }
                                Rectangle {
                                    // Badge, default size: h-5 min-w-20 px-1.5,
                                    // 11px, rounded-full, bg-sidebar-unread.
                                    objectName: "sessionRowBadge-" + sessionRow.modelData.id
                                    visible: sessionRow.modelData.unread > 0
                                    width: Math.max(20, rowBadgeLabel.implicitWidth + 12)
                                    height: 20
                                    radius: 10
                                    color: switcher.colors["sidebar-unread"]
                                    anchors.top: parent.top
                                    anchors.right: parent.right
                                    Text {
                                        id: rowBadgeLabel
                                        objectName: "sessionRowBadgeLabel-" + sessionRow.modelData.id
                                        anchors.centerIn: parent
                                        text: switcher.badgeLabel(sessionRow.modelData.unread)
                                        textFormat: Text.PlainText
                                        color: switcher.colors["badge-text"]
                                        font.pixelSize: 11
                                        font.weight: Font.Medium
                                    }
                                }
                            }
                            ColumnLayout {
                                width: sessionMenu.width - 40 - 24 - 8 - 8 - 12 - 12 - 8
                                spacing: 2
                                Text {
                                    objectName: "sessionRowPrimary-" + sessionRow.modelData.id
                                    text: sessionRow.modelData.primaryLabel
                                    textFormat: Text.PlainText
                                    color: switcher.colors["text-primary"]
                                    font.pixelSize: 14
                                    font.weight: Font.Medium
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                Text {
                                    objectName: "sessionRowSecondary-" + sessionRow.modelData.id
                                    text: sessionRow.modelData.secondaryLabel
                                    textFormat: Text.PlainText
                                    color: switcher.colors["text-muted"]
                                    font.pixelSize: 12
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                            }
                            ToolButton {
                                // Per-row logout: 24x24 rounded-md, text-notice-base
                                // on notice-base/10 with notice-base/40 border,
                                // logout icon 14.
                                id: rowLogout
                                objectName: "sessionLogout-" + sessionRow.modelData.id
                                width: 24
                                height: 24
                                anchors.verticalCenter: parent.verticalCenter
                                padding: 0
                                hoverEnabled: true
                                background: Rectangle {
                                    radius: 6
                                    color: Qt.alpha(switcher.colors["notice-base"], rowLogout.hovered ? 0.2 : 0.1)
                                    border.width: 1
                                    border.color: Qt.alpha(switcher.colors["notice-base"], 0.4)
                                }
                                icon.source: "qrc:/workspace/icons/logout.svg"
                                icon.color: switcher.colors["notice-base"]
                                icon.width: 14
                                icon.height: 14
                                Accessible.name: qsTr("Logout from organization")
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Logout from organization")
                                onClicked: switcher.beginLogout(sessionRow.modelData.title, sessionRow.modelData.id)
                            }
                        }
                    }
                }
                Rectangle {
                    // Separator: mx-2 my-1 h-px bg-border-subtle.
                    objectName: "sessionMenuSeparator"
                    Layout.leftMargin: 8
                    Layout.rightMargin: 8
                    Layout.topMargin: 4
                    Layout.bottomMargin: 4
                    Layout.fillWidth: true
                    height: 1
                    color: switcher.colors["border-subtle"]
                }
                ToolButton {
                    // "Add server" row: add icon 16 + auth.addServer (switcher
                    // :281-293).
                    objectName: "sessionAddServer"
                    Layout.fillWidth: true
                    implicitHeight: 20 + 16
                    padding: 0
                    hoverEnabled: true
                    display: AbstractButton.TextBesideIcon
                    font.pixelSize: 14
                    text: qsTr("Add server")
                    Accessible.name: qsTr("Add server")
                    spacing: 8
                    leftPadding: 12
                    icon.source: "qrc:/workspace/icons/add.svg"
                    icon.color: switcher.colors["text-muted"]
                    icon.width: 16
                    icon.height: 16
                    background: Rectangle {
                        radius: 8
                        color: parent.hovered ? switcher.colors["sidebar-item-hover"] : "transparent"
                    }
                    onClicked: {
                        sessionMenu.close();
                        switcher.controller.openAddServer();
                    }
                }
            }
        }
    }

    Popup {
        // window.confirm(auth.logoutFromOrgConfirm) equivalent (switcher
        // :256-258); the native client confirms with an owned modal.
        id: logoutConfirm
        property string pendingTitle: ""
        property string pendingId: ""
        parent: Overlay.overlay
        modal: true
        dim: true
        closePolicy: Popup.CloseOnEscape
        width: Math.min(360, parent ? parent.width - 32 : 360)
        x: parent ? (parent.width - width) / 2 : 0
        y: parent ? (parent.height - height) / 2 : 0
        padding: 16
        background: Rectangle {
            color: switcher.colors["bg-elevated"]
            border.color: switcher.colors["border-subtle"]
            radius: 10
        }
        contentItem: ColumnLayout {
            objectName: "sessionLogoutConfirm"
            spacing: 12
            Text {
                objectName: "sessionLogoutConfirmText"
                text: qsTr("Log out from %1?").arg(logoutConfirm.pendingTitle)
                textFormat: Text.PlainText
                color: switcher.colors["text-primary"]
                font.pixelSize: 14
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            RowLayout {
                spacing: 8
                Item { Layout.fillWidth: true }
                NativeButton {
                    objectName: "sessionLogoutCancelButton"
                    colors: switcher.colors
                    text: qsTr("Cancel")
                    onClicked: logoutConfirm.close()
                }
                NativeButton {
                    objectName: "sessionLogoutConfirmButton"
                    colors: switcher.colors
                    primary: true
                    text: qsTr("Log out")
                    onClicked: {
                        const accountId = logoutConfirm.pendingId;
                        logoutConfirm.close();
                        switcher.controller.logoutWorkspaceSession(accountId);
                    }
                }
            }
        }
    }
}
