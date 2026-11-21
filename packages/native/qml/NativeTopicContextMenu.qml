pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

// Issue #636 (docs/NATIVE_DESKTOP_TOPIC_CONTEXT_ACTIONS.md): the topic context
// menu (reference workspace-topic-context-menu.ui.tsx at workspace_ui
// 75bd21f). One instance per surface (sidebar rows in Main.qml, channel
// information drawer rows in NativeChannelInfo.qml); the menu is a sibling of
// the row lists - never built inside a row delegate - matching the accepted
// streamMenu precedent so no items materialize in a menu at open time.
Item {
    id: block
    required property var controller
    required property var colors
    // Which surface hosts this instance (sidebar / channelInfo); the menu is
    // parented to the window overlay, so test hooks tag the popup itself.
    property string surface: "sidebar"
    Accessible.ignored: true

    property string targetStream: ""
    property string targetTopic: ""
    property string targetOwner: ""
    // Pulse bumped on every catalog/guard transition; the row and guard
    // bindings below depend on it and re-resolve live while the menu is open.
    property int stateTick: 0
    // Review finding 4652383415: the read/done/notification rows close the
    // menu before dispatch, so their failures need this surface's notice (the
    // rename dialog keeps its inline error). The tag names which action the
    // last dispatch from THIS instance was waiting for, attributing the
    // finished signal to the surface that actually sent the write.
    property string dispatchedNotice: ""
    readonly property var topicRow: stateTick >= 0 ? resolveRow() : null
    readonly property var actionState: stateTick >= 0 && targetTopic.length
        ? controller.topicActionState(targetStream, targetTopic) : ({})
    readonly property var streamRow: stateTick >= 0 ? resolveStream() : null

    // Reference menu geometry (issue #636: streamMenu contract width 180 with
    // 5/1 padding, reference narrow variant + sideOffset 4 align start).
    // Segment switch follows notification-level-switch.lib.ts:44-46 (sm):
    // icon 22, container p-0.5 gap-0.5 rounded-md bg-bg; segments h-7 min-w-7
    // (28 px); tones from :70-71 (selected card-bg + icon-active).
    function resolveRow() {
        if (!targetTopic.length) return null
        if (controller.selectedStream === targetStream) {
            for (const row of controller.topics)
                if (String(row.id) === targetTopic) return row
        }
        const state = controller.sidebarTopicState(targetStream, true)
        const rows = state && state.rows ? state.rows : []
        for (const row of rows)
            if (String(row.id) === targetTopic) return row
        return null
    }
    function resolveStream() {
        for (const stream of controller.streams)
            if (String(stream.id) === targetStream) return stream
        return null
    }
    function rowUnread() {
        const row = block.topicRow
        if (!row) return 0
        return Number(row.unread || 0) + Number(row.passiveUnread || 0)
    }
    // messenger-notification-mode.lib.ts:92-130 level <-> mode maps.
    function levelForMode(mode) {
        if (block.streamRow && block.streamRow.isDirect && mode === "unmute") return "followed"
        if (mode === "unmute") return "unmuted"
        if (mode === "follow") return "followed"
        if (mode === "mute") return "muted"
        return "inherit"
    }
    function modeForLevel(level) {
        if (level === "unmuted") return "unmute"
        if (level === "followed") return "follow"
        if (level === "muted") return "mute"
        return "default"
    }
    // notification-level-switch.lib.ts:132-136 option order and gating.
    readonly property var levelList: {
        if (!(stateTick >= 0)) return []
        const row = block.topicRow
        if (!row) return []
        const direct = block.streamRow && block.streamRow.isDirect
        const levels = ["muted", "inherit"]
        if (!direct && (block.streamRow && block.streamRow.notificationMode === "muted"
            || String(row.notificationMode || "") === "unmute")) levels.push("unmuted")
        levels.push("followed")
        return levels
    }
    function levelAsset(level) {
        if (level === "muted") return "bell_off.svg"
        if (level === "inherit") return "bell.svg"
        if (level === "unmuted") return "at.svg"
        return "topic_follow.svg"
    }
    // channel.topicVisibility{Muted,Default,Unmuted,Followed} (ru.json:909-912)
    function levelLabel(level) {
        if (level === "muted") return qsTr("Mute")
        if (level === "inherit") return qsTr("Default")
        if (level === "unmuted") return qsTr("Unmute")
        return qsTr("Follow")
    }
    function currentLevel() {
        const row = block.topicRow
        return row ? levelForMode(String(row.notificationMode || "")) : "inherit"
    }

    function open(rowStream, topic, anchor, point) {
        // authenticated() alone gates the session: ownerKey stays empty for
        // synthetic accounts (isUuid-failed user), so a length precondition
        // would wedge the menu; owner drift while open is fenced by the
        // targetOwner comparison in the state-changed handler below.
        if (!controller.authenticated) return
        targetStream = rowStream
        targetTopic = topic
        targetOwner = controller.ownerKey
        block.dispatchedNotice = ""
        // The menu parent is the window overlay; position it at the requested
        // anchor point (reference context anchor: sideOffset 4, align start -
        // the caller maps its own press/row point).
        const local = anchor.mapToItem(topicMenu.parent, point)
        topicMenu.x = local.x
        topicMenu.y = local.y
        topicMenu.popup()
    }

    // Stale target / session guards mirror the streamMenu bookmark pattern.
    Connections {
        target: block.controller
        function onTopicActionsChanged() { block.stateTick++ }
        function onTopicsChanged() { block.stateTick++ }
        function onSidebarTopicsChanged() { block.stateTick++ }
        function onSidebarExpansionChanged() { block.stateTick++ }
        function onStreamsChanged() { block.stateTick++ }
        function onStateChanged() {
            block.stateTick++
            if (!block.controller.authenticated || block.targetOwner !== block.controller.ownerKey) {
                topicMenu.close()
                topicRenameDialog.close()
            }
        }
        function onTopicActionFinished(stream, topic, action, ok) {
            if (action === "rename" && topicRenameDialog.opened
                && stream === topicRenameDialog.renameStream && topic === topicRenameDialog.renameTopic) {
                topicRenameDialog.submitted = true
                if (ok) topicRenameDialog.close()
            }
            // Review finding 4652383415: a failed read/notification/done
            // write closes the menu before the answer lands, so surface the
            // settled error as this surface's notice. Rename failures stay
            // inside the dialog (pinned by the renewal case).
            if (action === block.dispatchedNotice && stream === block.targetStream
                && topic === block.targetTopic) {
                block.dispatchedNotice = ""
                if (ok) return
                const noticeState = block.controller.topicActionState(stream, topic)
                const message = String(noticeState[action + "Error"] || "")
                if (!message.length) return
                topicActionErrorText.text = message
                topicActionErrorToast.open()
            }
        }
    }
    onTopicRowChanged: if (topicMenu.opened && !block.topicRow) topicMenu.close()

    Menu {
        id: topicMenu
        objectName: "topicContextMenu"
        property string menuSurface: block.surface
        parent: Overlay.overlay
        width: 180
        topPadding: 5
        bottomPadding: 5
        leftPadding: 1
        rightPadding: 1
        modal: false
        dim: false
        focus: true
        background: Rectangle {
            radius: 6
            color: block.colors["bg-elevated"]
            border.color: block.colors["border-subtle"]
        }
        // The notifications block is a plain Item (not a MenuItem) exactly like
        // the streamMenu switch (Main.qml streamNotificationItem): it stays out
        // of keyboard highlight navigation while its segment buttons stay live.
        Item {
            objectName: "topicNotificationItem"
            implicitWidth: 178
            implicitHeight: notificationColumn.implicitHeight + 8
            Accessible.ignored: true
            MouseArea {
                anchors.fill: parent
                onClicked: topicMenu.close()
            }
            Column {
                id: notificationColumn
                x: 8
                y: 4
                width: parent.width - 16
                spacing: 4
                Text {
                    objectName: "topicNotificationsHeading"
                    text: qsTr("Topic notifications")
                    textFormat: Text.PlainText
                    font.pixelSize: 10
                    font.weight: Font.Medium
                    color: block.colors["text-muted"]
                }
                Rectangle {
                    objectName: "topicNotificationSwitch"
                    width: parent.width
                    height: 32
                    radius: 6
                    color: block.colors["bg"]
                    Accessible.role: Accessible.Grouping
                    Accessible.name: qsTr("Topic notifications")
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 2
                        spacing: 2
                        Repeater {
                            model: block.levelList
                            delegate: Button {
                                id: levelSegment
                                required property var modelData
                                objectName: modelData === "muted"
                                    ? "channelTopicMuteAction" : "topicNotifSegment-" + modelData
                                // While a write for this topic is in flight the
                                // whole switch goes inactive (reference
                                // disabled={notificationPending}, review
                                // cid 4629685053 precedent in streamMenu).
                                enabled: !block.actionState.notificationsPending
                                Layout.fillWidth: true
                                Layout.preferredHeight: 28
                                padding: 0
                                display: AbstractButton.IconOnly
                                readonly property bool isCurrent: block.currentLevel() === modelData
                                Accessible.role: Accessible.RadioButton
                                Accessible.checkable: true
                                Accessible.checked: isCurrent
                                Accessible.name: block.levelLabel(modelData)
                                icon.source: "qrc:/workspace/icons/" + block.levelAsset(modelData)
                                icon.width: 22
                                icon.height: 22
                                icon.color: isCurrent ? block.colors["icon-active"] : block.colors["text-muted"]
                                background: Rectangle {
                                    radius: 6
                                    color: levelSegment.isCurrent ? block.colors["card-bg"] : "transparent"
                                    opacity: levelSegment.enabled ? 1.0 : 0.5
                                }
                                onClicked: {
                                    const selected = levelSegment.isCurrent
                                    topicMenu.close()
                                    if (!selected) {
                                        block.dispatchedNotice = "notifications"
                                        block.controller.applyTopicAction(block.targetStream, block.targetTopic,
                                            "notifications", block.modeForLevel(String(modelData)))
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        // Item list and order follow workspace-topic-context-menu.ui.tsx:
        // notifications block, Mark as read (only with unread, :242), rename,
        // done toggle (:265). px-2 py-2 text-sm text-text-primary
        // hover:bg-sidebar-hover (item class :24).
        TopicMenuAction {
            objectName: "topicMarkReadAction"
            visible: block.rowUnread() > 0
            height: visible ? implicitHeight : 0
            text: qsTr("Mark as read")
            asset: "check.svg"
            enabled: visible && !block.actionState.readPending
            onTriggered: {
                topicMenu.close()
                block.dispatchedNotice = "read"
                block.controller.applyTopicAction(block.targetStream, block.targetTopic, "read", "")
            }
        }
        TopicMenuAction {
            objectName: "topicRenameAction"
            text: qsTr("Rename topic")
            asset: "pen.svg"
            enabled: !block.actionState.renamePending && !block.actionState.toggle_donePending
            onTriggered: {
                const row = block.topicRow
                topicMenu.close()
                if (!row) return
                topicRenameDialog.renameStream = block.targetStream
                topicRenameDialog.renameTopic = block.targetTopic
                topicRenameDialog.originalName = String(row.name || "")
                topicRenameDialog.streamTitleText = block.streamRow
                    ? String(block.streamRow.displayName || block.streamRow.name || "") : ""
                topicRenameDialog.open()
            }
        }
        TopicMenuAction {
            objectName: "topicDoneAction"
            text: block.topicRow && block.topicRow.done
                ? qsTr("Mark topic as not done") : qsTr("Mark topic as done")
            asset: "check.svg"
            enabled: !block.actionState.toggle_donePending
            onTriggered: {
                topicMenu.close()
                block.dispatchedNotice = "toggle_done"
                block.controller.applyTopicAction(block.targetStream, block.targetTopic, "toggle_done", "")
            }
        }
    }

    component TopicMenuAction: MenuItem {
        id: action
        property string asset: ""
        implicitWidth: 178
        implicitHeight: 36
        height: visible ? implicitHeight : 0
        // px-2 py-2 text-sm gap-2 (reference WORKSPACE_TOPIC_MENU_ITEM_CLASS).
        leftPadding: 8
        rightPadding: 8
        topPadding: 8
        bottomPadding: 8
        spacing: 8
        font.pixelSize: 14
        font.weight: Font.Normal
        icon.source: asset.length ? "qrc:/workspace/icons/" + asset : ""
        icon.width: 20
        icon.height: 20
        icon.color: block.colors["icon-base"]
        palette.windowText: block.colors["text-primary"]
        opacity: enabled ? 1 : 0.5
        background: Rectangle {
            color: action.highlighted || action.hovered ? block.colors["sidebar-item-hover"] : "transparent"
        }
    }

    // RenameWorkspaceTopicDialog (reference :38-103): title "Rename topic",
    // description = stream title, "Topic name" input prefilled, Save disabled
    // while empty or pending, Enter submits, an unchanged name closes without
    // a request (:175).
    Popup {
        id: topicRenameDialog
        objectName: "topicRenameDialog"
        // Same surface-tag contract as topicMenu: the test hooks identify the
        // hosting surface on the popup itself, not on the object path.
        property string dialogSurface: block.surface
        parent: Overlay.overlay
        x: Math.round((Overlay.overlay.width - width) / 2)
        y: Math.round((Overlay.overlay.height - height) / 2)
        width: Math.min(Overlay.overlay.width - 48, 448)
        padding: 24
        modal: true
        dim: true
        focus: true
        property string renameStream: ""
        property string renameTopic: ""
        property string originalName: ""
        property string streamTitleText: ""
        property bool submitted: false
        readonly property var renameState: block.stateTick >= 0
            && renameStream.length ? block.controller.topicActionState(renameStream, renameTopic) : ({})
        readonly property bool renamePending: renameState.renamePending === true
        closePolicy: topicRenameDialog.renamePending
            ? Popup.NoAutoClose : Popup.CloseOnEscape | Popup.CloseOnPressOutside
        Overlay.modal: Rectangle { color: "#80000000" }
        background: Rectangle {
            radius: 10
            color: block.colors["bg-elevated"]
            border.color: block.colors["border-subtle"]
        }
        function submitRename() {
            const name = renameInput.text.trim()
            if (!name.length || topicRenameDialog.renamePending) return
            if (name === topicRenameDialog.originalName) { topicRenameDialog.close(); return }
            topicRenameDialog.submitted = true
            block.controller.applyTopicAction(topicRenameDialog.renameStream,
                topicRenameDialog.renameTopic, "rename", name)
        }
        onOpened: {
            renameInput.text = topicRenameDialog.originalName
            renameInput.forceActiveFocus(Qt.TabFocusReason)
            renameInput.selectAll()
        }
        onClosed: topicRenameDialog.submitted = false
        contentItem: Column {
            spacing: 16
            Text {
                objectName: "topicRenameTitle"
                width: parent.width
                text: qsTr("Rename topic")
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                font.pixelSize: 18
                font.weight: Font.DemiBold
                color: block.colors["text-primary"]
            }
            Text {
                objectName: "topicRenameDescription"
                width: parent.width
                visible: text.length > 0
                text: topicRenameDialog.streamTitleText
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                font.pixelSize: 14
                color: block.colors["text-muted"]
            }
            Column {
                width: parent.width
                spacing: 6
                Text {
                    text: qsTr("Topic name")
                    font.pixelSize: 14
                    color: block.colors["text-muted"]
                }
                TextField {
                    id: renameInput
                    objectName: "topicRenameInput"
                    width: parent.width
                    enabled: !topicRenameDialog.renamePending
                    font.pixelSize: 14
                    Accessible.name: qsTr("Topic name")
                    onAccepted: topicRenameDialog.submitRename()
                    background: Rectangle {
                        radius: 8
                        color: block.colors["bg"]
                        border.color: block.colors["border-subtle"]
                    }
                }
            }
            Text {
                objectName: "topicRenameError"
                width: parent.width
                visible: topicRenameDialog.submitted && String(topicRenameDialog.renameState.renameError || "").length > 0
                text: visible ? String(topicRenameDialog.renameState.renameError) : ""
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                font.pixelSize: 14
                color: block.colors.danger
                Accessible.role: Accessible.AlertMessage
            }
            Row {
                anchors.right: parent.right
                spacing: 8
                Button {
                    objectName: "topicRenameCancel"
                    text: qsTr("Cancel")
                    enabled: !topicRenameDialog.renamePending
                    padding: 8
                    onClicked: topicRenameDialog.close()
                    background: Rectangle {
                        radius: 8
                        color: parent.hovered ? block.colors["sidebar-item-hover"] : "transparent"
                        border.color: block.colors["border-subtle"]
                    }
                }
                Button {
                    objectName: "topicRenameSave"
                    text: qsTr("Save")
                    enabled: !topicRenameDialog.renamePending && renameInput.text.trim().length > 0
                    padding: 8
                    onClicked: topicRenameDialog.submitRename()
                    background: Rectangle {
                        radius: 8
                        color: parent.hovered ? block.colors["sidebar-item-hover"] : block.colors["card-bg"]
                        opacity: enabled ? 1.0 : 0.5
                    }
                }
            }
        }
    }

    // Action-failure notice (review finding 4652383415). Contract mirrors the
    // accepted ownStatusErrorToast (Main.qml): top-right, auto-dismiss after
    // 5 s, AlertMessage for screen readers. One notice per menu surface, so
    // the sidebar and the channel drawer never duplicate or steal each other's
    // feedback (dialogSurface routes the test hook, as for the rename popup).
    Popup {
        id: topicActionErrorToast
        objectName: "topicActionErrorToast"
        property string dialogSurface: block.surface
        readonly property string noticeText: topicActionErrorText.text
        parent: Overlay.overlay
        x: Math.max(16, Overlay.overlay.width - width - 16)
        y: 16
        z: 10000
        width: Math.min(384, Overlay.overlay.width - 32)
        leftPadding: 16; rightPadding: 16; topPadding: 12; bottomPadding: 12
        closePolicy: Popup.NoAutoClose
        modal: false
        focus: false
        background: Rectangle {
            radius: 8
            color: Qt.rgba(block.colors.danger.r, block.colors.danger.g, block.colors.danger.b, 0.1)
            border.color: Qt.rgba(block.colors.danger.r, block.colors.danger.g, block.colors.danger.b, 0.4)
        }
        contentItem: Text {
            id: topicActionErrorText
            objectName: "topicActionErrorText"
            text: ""
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: block.colors.danger
            font.pixelSize: 14
            Accessible.role: Accessible.AlertMessage
            Accessible.name: text
        }
        Timer { interval: 5000; running: topicActionErrorToast.visible; onTriggered: topicActionErrorToast.close() }
    }
}
