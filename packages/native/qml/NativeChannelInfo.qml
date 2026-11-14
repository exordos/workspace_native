pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

// qmllint disable unqualified

// Channel info drawer content (issue #132, docs/NATIVE_DESKTOP_CHANNEL_INFO.md).
// Reference: workspace_ui right-panel-workspace-info.ui.tsx at
// 61dacef78dab2f5d0be76f283bebf49fe29b84af. Mounted as the root screen of the
// chat-info layer of NativeRightDrawer; the shell chrome (close/back) stays in
// the shell. Geometry and colors are measured from the pinned reference.
Item {
    id: page
    required property var colors
    required property var controller
    objectName: "channelInfoPage"
    anchors.fill: parent
    clip: true
    // The shell instantiates this layer at window start but reveals it only
    // when the chat-info root screen is on top; the reference mounts the info
    // store when that screen opens, so the hidden layer must not fetch.
    // Refresh on every reveal (fresh-mount semantics) - a member GET issued
    // while hidden also breaks the window-lifetime stream_bindings
    // discipline the e2ee ui cases pin (#197).
    Component.onCompleted: if (page.visible) controller.refreshChannelInfo()
    onVisibleChanged: if (visible) controller.refreshChannelInfo()
    // Reference store mounts per channel: switching channels re-runs the
    // members load exactly like a fresh mount of the info screen, but only
    // while the screen is actually on top.
    property string watchedStream: ""
    Connections {
        target: page.controller
        function onStateChanged() {
            const info = page.controller.channelInfoStream
            const id = info && info.id ? String(info.id) : ""
            if (id === page.watchedStream)
                return
            page.watchedStream = id
            // A stale selection must not survive a stream change: what was
            // picked for the previous stream may never be submitted on the
            // new one, open dialog or not.
            addMembersDialog.selectedUsers = [];
            if (addMembersDialog.visible)
                addMembersSearch.text = "";
            if (id.length > 0 && page.visible)
                page.controller.refreshChannelInfo()
        }
    }
    property bool addDialogOpen: addMembersDialog.visible
    // The protected chat info drawer (issue #243) routes its member add through
    // Main.qml to this one shared dialog: the reference wires the same
    // channel.addMembers surface into both info presentations.
    function openAddMembers() {
        controller.resetChannelInfoAddState();
        addMembersDialog.open();
    }
    function roleLabel(role) {
        if (role === "owner") return qsTr("Owner");
        if (role === "administrator") return qsTr("Administrator");
        if (role === "moderator") return qsTr("Moderator");
        if (role === "guest") return qsTr("Guest");
        if (role === "member") return qsTr("Member");
        return "";
    }
    function presenceLabel(status) {
        if (status === "active") return qsTr("online");
        if (status === "idle") return qsTr("away");
        if (status === "offline") return qsTr("offline");
        if (status === "do_not_disturb") return qsTr("do not disturb");
        return "";
    }
    function presenceColor(presence) {
        if (presence === "active") return page.colors["indicator-green"];
        if (presence === "idle") return page.colors["indicator-orange"];
        return page.colors["text-muted"];
    }
    // resolveWorkspaceStatusLabel(status) ?? email (reference 131-137).
    function memberSecondary(status, email) {
        const label = page.presenceLabel(status);
        return label.length > 0 ? label : email;
    }
    function memberCountLabel() {
        const count = controller.channelInfoMemberCount;
        return count === 1 ? qsTr("%n member", "", count) : qsTr("%n members", "", count);
    }
    function onlineCountLabel() {
        const count = controller.channelInfoOnlineCount;
        return count === 1 ? qsTr("%n online", "", count) : qsTr("%n online", "", count);
    }
    // Header + blocks follow the current store projection; everything hides
    // itself cleanly before a stream is selected.
    readonly property var streamInfo: controller.channelInfoStream
    readonly property string streamTitle: streamInfo && streamInfo.title ? String(streamInfo.title) : ""
    readonly property string streamColor: streamInfo && streamInfo.color ? String(streamInfo.color) : ""
    readonly property string streamDescription: streamInfo && streamInfo.description ? String(streamInfo.description) : ""
    readonly property string notificationMode: controller.channelInfoNotificationMode
    readonly property bool notificationSupported: notificationMode.length > 0
    readonly property bool notificationBusy: controller.channelInfoNotificationBusy

    // ---- Header: 48px avatar lg, title text-sm font-medium, sub line 11px ---
    // right-panel-workspace-info.ui.tsx:672-685 + avatar.tsx SIZE_CLASS.lg.
    Item {
        id: headerBlock
        objectName: "channelInfoHeader"
        x: 8
        width: parent.width - 16
        y: 0
        height: 65
        Item {
            id: headerRow
            x: 8
            y: 4
            width: parent.width - 16
            height: 48
            Rectangle {
                id: headerAvatar
                objectName: "channelInfoAvatar"
                width: 48; height: 48
                radius: 24
                color: page.streamColor.length > 0 ? page.streamColor : page.colors["bg"]
                border.width: 1
                border.color: page.colors["border-subtle"]
                Text {
                    anchors.centerIn: parent
                    objectName: "channelInfoAvatarInitial"
                    text: page.streamTitle.length > 0 ? page.streamTitle.slice(0, 1) : ""
                    textFormat: Text.PlainText
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                    color: page.colors["text-primary"]
                }
            }
            Item {
                anchors.left: headerAvatar.right
                anchors.leftMargin: 16
                anchors.right: callButton.visible ? callButton.left : parent.right
                anchors.rightMargin: callButton.visible ? 8 : 0
                anchors.verticalCenter: parent.verticalCenter
                height: 36
                Text {
                    id: headerTitle
                    objectName: "channelInfoTitle"
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    text: page.streamTitle
                    textFormat: Text.PlainText
                    font.pixelSize: 14
                    font.weight: Font.Medium
                    color: page.colors["text-primary"]
                    elide: Text.ElideRight
                }
                Text {
                    objectName: "channelInfoSubline"
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: headerTitle.bottom
                    text: page.memberCountLabel() + ", " + page.onlineCountLabel()
                    textFormat: Text.PlainText
                    font.pixelSize: 11
                    color: page.colors["text-secondary"]
                    elide: Text.ElideRight
                }
            }
            // Issue #164 (right-panel-workspace-info.ui.tsx:841-866): the DM
            // call surface, enabled only while the bridge descriptor matches
            // the viewed chat on ownerKey + runtimeGeneration + streamUuid.
            Button {
                id: callButton
                objectName: "channelInfoCallButton"
                Accessible.name: callButtonLabel.text
                visible: page.controller.isDirectConversation
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                padding: 0
                implicitWidth: callButtonRow.implicitWidth + 16
                implicitHeight: 32
                enabled: page.controller.callBridge.callSurfaceEnabled
                contentItem: Row {
                    id: callButtonRow
                    anchors.centerIn: parent
                    spacing: 4
                    Image {
                        source: "qrc:/workspace/icons/phone.svg"
                        sourceSize.width: 18; sourceSize.height: 18
                        anchors.verticalCenter: parent.verticalCenter
                        layer.enabled: true
                        layer.effect: MultiEffect {
                            colorizationColor: callButton.enabled
                                ? page.colors["icon-active"] : page.colors["text-muted"]
                            colorization: 1.0
                        }
                    }
                    Text {
                        id: callButtonLabel
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Call")
                        textFormat: Text.PlainText
                        font.pixelSize: 12
                        color: callButton.enabled
                            ? page.colors["text-secondary"] : page.colors["text-muted"]
                    }
                }
                background: Rectangle {
                    radius: 8
                    color: page.colors["card-bg"]
                    border.width: 1
                    border.color: page.colors["border-subtle"]
                    opacity: callButton.enabled ? 1.0 : 0.5
                }
                onClicked: page.controller.callBridge.invokeCallSurface()
            }
        }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: page.colors["border-subtle"]
        }
    }

    Flickable {
        id: pageScroll
        objectName: "channelInfoScroll"
        anchors.top: headerBlock.bottom
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        clip: true
        contentWidth: width
        contentHeight: flow.height + 16
        boundsBehavior: Flickable.StopAtBounds

        // ScrollArea p-2 with space-y-5 between blocks.
        Column {
            id: flow
            x: 8
            width: pageScroll.width - 16
            y: 8
            spacing: 20

            // ---- Notifications block (reference 688-715) ----
            Column {
                objectName: "channelInfoNotificationsBlock"
                width: flow.width
                spacing: 12
                Row {
                    objectName: "channelInfoNotificationsHeading"
                    spacing: 0
                    Text {
                        text: qsTr("Notifications")
                        textFormat: Text.PlainText
                        font.pixelSize: 14
                        font.weight: Font.Medium
                        color: page.colors["text-primary"]
                    }
                    Text {
                        text: " (" + qsTr("For entire chat") + ")"
                        textFormat: Text.PlainText
                        font.pixelSize: 14
                        font.weight: Font.Medium
                        color: page.colors["text-muted"]
                    }
                }
                // StreamNotificationLevelSwitch size lg: container `flex gap-2
                // rounded-lg bg-bg p-1`, segments `h-9 flex-1 rounded-lg`, icon
                // 28, selected bg-card-bg text-icon-active, idle text-text-muted,
                // disabled opacity-50 (notification-level-switch.lib.ts).
                Rectangle {
                    id: notificationSwitch
                    objectName: "channelInfoNotificationSwitch"
                    visible: page.notificationSupported
                    width: flow.width
                    height: 44
                    radius: 8
                    color: page.colors["bg"]
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 4
                        anchors.rightMargin: 4
                        anchors.topMargin: 4
                        anchors.bottomMargin: 4
                        spacing: 8
                        Repeater {
                            model: [
                                {mode: "mentions_only", objectName: "channelInfoNotifDefault",
                                 asset: "at.svg", label: qsTr("Mentions only")},
                                {mode: "muted", objectName: "channelInfoNotifMuted",
                                 asset: "bell_off.svg", label: qsTr("Muted")},
                                {mode: "all_messages", objectName: "channelInfoNotifSubscribed",
                                 asset: "bell.svg", label: qsTr("All messages")}
                            ]
                            delegate: Button {
                                id: segment
                                required property var modelData
                                objectName: modelData.objectName
                                Layout.fillWidth: true
                                Layout.preferredHeight: 36
                                Layout.minimumWidth: 0
                                enabled: !page.notificationBusy
                                property bool isCurrent: page.notificationMode === modelData.mode
                                display: AbstractButton.IconOnly
                                padding: 0
                                icon.source: "qrc:/workspace/icons/" + modelData.asset
                                icon.width: 28
                                icon.height: 28
                                icon.color: segment.isCurrent ? page.colors["icon-active"] : page.colors["text-muted"]
                                Accessible.name: modelData.label
                                background: Rectangle {
                                    radius: 8
                                    color: segment.isCurrent ? page.colors["card-bg"] : "transparent"
                                    opacity: segment.enabled ? 1.0 : 0.5
                                }
                                onClicked: page.controller.setChannelInfoNotificationMode(segment.modelData.mode)
                            }
                        }
                    }
                }
                Rectangle {
                    objectName: "channelInfoNotificationUnsupported"
                    visible: !page.notificationSupported
                    width: flow.width
                    implicitHeight: unsupportedLabel.height + 16
                    radius: 8
                    color: page.colors["bg-elevated"]
                    Text {
                        id: unsupportedLabel
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        leftPadding: 8
                        rightPadding: 8
                        text: qsTr("This action is not available in this conversation yet.")
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        font.pixelSize: 14
                        color: page.colors["text-muted"]
                    }
                }
                Text {
                    objectName: "channelInfoNotificationError"
                    visible: text.length > 0
                    text: controller.channelInfoNotificationError
                    textFormat: Text.PlainText
                    font.pixelSize: 12
                    color: page.colors["notice-base"]
                    leftPadding: 8
                }
            }

            // ---- Description block (reference 730-739) ----
            Column {
                objectName: "channelInfoDescriptionBlock"
                visible: page.streamDescription.length > 0
                width: flow.width
                spacing: 12
                Text {
                    objectName: "channelInfoDescriptionHeading"
                    text: qsTr("Description")
                    textFormat: Text.PlainText
                    font.pixelSize: 14
                    font.weight: Font.Medium
                    color: page.colors["text-primary"]
                    leftPadding: 8
                }
                Rectangle {
                    objectName: "channelInfoDescriptionValue"
                    width: flow.width
                    implicitHeight: descriptionValue.height + 16
                    radius: 8
                    color: page.colors["bg-elevated"]
                    Text {
                        id: descriptionValue
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        leftPadding: 12
                        rightPadding: 12
                        text: page.streamDescription
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        font.pixelSize: 14
                        color: page.colors["text-primary"]
                    }
                }
            }

            // ---- Members block (reference 748-812) ----
            Column {
                objectName: "channelInfoMembersBlock"
                width: flow.width
                spacing: 12
                Item {
                    objectName: "channelInfoMembersHeadingRow"
                    width: flow.width
                    height: 32
                    Text {
                        objectName: "channelInfoMembersHeading"
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Members")
                        textFormat: Text.PlainText
                        font.pixelSize: 14
                        font.weight: Font.Medium
                        color: page.colors["text-primary"]
                    }
                    NativeIconButton {
                        objectName: "channelInfoAddButton"
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        visible: controller.channelInfoIsOwner
                        colors: page.colors
                        width: 32; height: 32
                        glyphSize: 24
                        asset: "person_add.svg"
                        help: qsTr("Add members")
                        Accessible.name: qsTr("Add members")
                        iconColor: hovered ? page.colors["text-primary"] : page.colors["text-secondary"]
                        background: Rectangle {
                            radius: 6
                            color: parent.hovered ? page.colors["card-bg-active"] : "transparent"
                            border.width: parent.visualFocus ? 1 : 0
                            border.color: page.colors.accent
                        }
                        onClicked: {
                            page.controller.resetChannelInfoAddState();
                            addMembersDialog.open();
                        }
                    }
                }
                Text {
                    objectName: "channelInfoNoMembers"
                    visible: controller.channelInfoMembers.length === 0
                    width: flow.width
                    horizontalAlignment: Text.AlignHCenter
                    topPadding: 12
                    bottomPadding: 12
                    text: qsTr("No members found")
                    textFormat: Text.PlainText
                    font.pixelSize: 14
                    color: page.colors["text-muted"]
                }
                Column {
                    id: memberRows
                    width: flow.width
                    spacing: 4
                    Repeater {
                        model: controller.channelInfoMembers
                        // WorkspaceMemberCard: rounded-lg bg-card-bg px-3 py-2 gap-3,
                        // hover bg-card-bg-active; name text-sm, role text-[10px],
                        // secondary text-[11px]; sm avatar with presence dot; the row
                        // is intentionally not a button. Remove button h-6 w-6,
                        // opacity-0 until row hover, disabled opacity-40 in flight.
                        delegate: Rectangle {
                            id: memberCard
                            required property var modelData
                            objectName: "channelInfoMember-" + modelData.bindingUuid
                            width: memberRows.width
                            height: 48
                            radius: 8
                            color: cardHover.containsMouse ? page.colors["card-bg-active"] : page.colors["card-bg"]
                            property bool removing: controller.channelInfoRemovingMembers.indexOf(modelData.userUuid) >= 0
                            MouseArea {
                                id: cardHover
                                anchors.fill: parent
                                acceptedButtons: Qt.NoButton
                                hoverEnabled: true
                            }
                            Rectangle {
                                id: memberAvatar
                                objectName: "channelInfoMemberAvatar-" + memberCard.modelData.bindingUuid
                                x: 12
                                anchors.verticalCenter: parent.verticalCenter
                                width: 32; height: 32
                                radius: 16
                                color: page.colors["bg-elevated"]
                                border.width: 1
                                border.color: page.colors["border-subtle"]
                                Text {
                                    anchors.centerIn: parent
                                    text: memberCard.modelData.initial
                                    textFormat: Text.PlainText
                                    font.pixelSize: 12
                                    font.weight: Font.DemiBold
                                    color: page.colors["text-primary"]
                                }
                                Rectangle {
                                    objectName: "channelInfoMemberPresence-" + memberCard.modelData.bindingUuid
                                    visible: memberCard.modelData.presence.length > 0
                                    width: 8; height: 8
                                    radius: 4
                                    x: memberAvatar.width - width - 1
                                    y: memberAvatar.height - height - 1
                                    color: page.presenceColor(memberCard.modelData.presence)
                                    border.width: 2
                                    border.color: memberCard.color
                                }
                            }
                            Text {
                                id: memberRole
                                visible: false
                                text: page.roleLabel(memberCard.modelData.role)
                                textFormat: Text.PlainText
                                font.pixelSize: 10
                                color: page.colors["text-secondary"]
                            }
                            Text {
                                id: memberName
                                objectName: "channelInfoMemberName-" + memberCard.modelData.bindingUuid
                                y: 9
                                x: 56
                                width: Math.max(0, memberCard.width - 56 - 24 - 6
                                    - (memberRole.text.length > 0 ? memberRole.width + 6 : 0))
                                text: memberCard.modelData.name
                                textFormat: Text.PlainText
                                font.pixelSize: 14
                                color: page.colors["text-primary"]
                                elide: Text.ElideRight
                            }
                            Text {
                                objectName: "channelInfoMemberRole-" + memberCard.modelData.bindingUuid
                                visible: memberRole.text.length > 0
                                text: memberRole.text
                                x: 56 + memberName.width + 6
                                y: 12
                                textFormat: Text.PlainText
                                font.pixelSize: 10
                                color: page.colors["text-secondary"]
                            }
                            Text {
                                objectName: "channelInfoMemberSecondary-" + memberCard.modelData.bindingUuid
                                y: 26
                                x: 56
                                width: memberCard.width - 56 - 12
                                text: page.memberSecondary(memberCard.modelData.status, memberCard.modelData.email)
                                textFormat: Text.PlainText
                                font.pixelSize: 11
                                color: page.colors["text-secondary"]
                                elide: Text.ElideRight
                            }
                            Button {
                                id: removeButton
                                objectName: "channelInfoRemove-" + memberCard.modelData.bindingUuid
                                visible: memberCard.modelData.canRemove
                                width: 24; height: 24
                                anchors.right: parent.right
                                anchors.rightMargin: 12
                                anchors.verticalCenter: parent.verticalCenter
                                enabled: !memberCard.removing
                                opacity: enabled ? (cardHover.containsMouse ? 1.0 : 0.0) : 0.4
                                Behavior on opacity { NumberAnimation { duration: 120 } }
                                display: AbstractButton.IconOnly
                                padding: 0
                                hoverEnabled: true
                                icon.source: "qrc:/workspace/icons/close.svg"
                                icon.width: 14
                                icon.height: 14
                                icon.color: removeButton.hovered && removeButton.enabled
                                    ? page.colors["notice-base"] : page.colors["text-muted"]
                                Accessible.name: qsTr("Remove from channel: %1").arg(memberCard.modelData.name)
                                background: Rectangle {
                                    radius: 4
                                    color: removeButton.hovered && removeButton.enabled
                                        ? page.colors["bg"] : "transparent"
                                }
                                onClicked: page.controller.removeChannelInfoMember(memberCard.modelData.bindingUuid)
                            }
                        }
                    }
                }
                Text {
                    objectName: "channelInfoMemberError"
                    visible: text.length > 0
                    text: controller.channelInfoMemberError
                    textFormat: Text.PlainText
                    font.pixelSize: 12
                    color: page.colors["notice-base"]
                    leftPadding: 8
                }
            }
        }
    }

    // ---- Add members dialog (reference 99-246) ----
    // max-w-md p-0, header px-4 py-3 border-b, UserPickerList search +
    // multi-select rows, footer Cancel/Add. max-h-[70vh] bounds the height.
    Dialog {
        id: addMembersDialog
        objectName: "addMembersDialog"
        parent: Overlay.overlay
        modal: true
        focus: true
        width: Math.min(448, parent ? parent.width - 48 : 448)
        height: Math.min(560, parent ? parent.height * 0.7 : 560)
        padding: 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        Overlay.modal: Rectangle { color: "#80000000" }
        background: Rectangle {
            objectName: "addMembersSurface"
            radius: 8
            color: page.colors["bg-elevated"]
            border.color: page.colors["border-subtle"]
        }
        onClosed: {
            // The selection and search live on the dialog itself; resetting
            // them keeps a reopen from submitting an earlier request.
            addMembersDialog.selectedUsers = [];
            addMembersSearch.text = "";
        }
        property var selectedUsers: []
        function toggleUser(userUuid) {
            const list = addMembersDialog.selectedUsers.slice();
            const index = list.indexOf(userUuid);
            if (index >= 0)
                list.splice(index, 1);
            else
                list.push(userUuid);
            addMembersDialog.selectedUsers = list;
        }
        // buildWorkspaceUserOptions: verified directory candidates minus the
        // existing members, filtered by name/username/email (userPickerSearchText).
        readonly property var pickerOptions: {
            const existing = {};
            const members = controller.channelInfoMembers || [];
            for (let i = 0; i < members.length; i += 1)
                existing[members[i].userUuid] = true;
            const needle = addMembersSearch.text.trim().toLowerCase();
            const choices = controller.channelMemberChoices || [];
            const out = [];
            for (let i = 0; i < choices.length; i += 1) {
                const choice = choices[i];
                if (existing[choice.id])
                    continue;
                const haystack = (String(choice.displayName) + " " + String(choice.username)
                    + " " + String(choice.email)).toLowerCase();
                if (needle.length > 0 && haystack.indexOf(needle) < 0)
                    continue;
                out.push(choice);
            }
            return out;
        }

        Item {
            anchors.fill: parent
            Text {
                id: addMembersTitle
                objectName: "addMembersTitle"
                x: 16
                y: 12
                width: parent.width - 16 - 40
                text: qsTr("Add members")
                textFormat: Text.PlainText
                font.pixelSize: 14
                font.weight: Font.DemiBold
                color: page.colors["text-primary"]
                elide: Text.ElideRight
            }
            Text {
                objectName: "addMembersSubtitle"
                anchors.top: addMembersTitle.bottom
                anchors.left: addMembersTitle.left
                anchors.right: addMembersTitle.right
                text: page.streamTitle
                textFormat: Text.PlainText
                font.pixelSize: 12
                color: page.colors["text-secondary"]
                elide: Text.ElideRight
            }
            NativeIconButton {
                id: addMembersClose
                objectName: "addMembersClose"
                anchors.top: parent.top
                anchors.topMargin: 12
                anchors.right: parent.right
                anchors.rightMargin: 12
                colors: page.colors
                width: 26; height: 26
                glyphSize: 18
                asset: "close.svg"
                help: qsTr("Close")
                Accessible.name: qsTr("Close")
                iconColor: hovered ? page.colors["text-primary"] : page.colors["text-muted"]
                background: Rectangle {
                    radius: 6
                    color: parent.hovered ? page.colors["card-bg-active"] : "transparent"
                    border.width: parent.visualFocus ? 1 : 0
                    border.color: page.colors.accent
                }
                onClicked: addMembersDialog.close()
            }
            Rectangle {
                anchors.top: addMembersClose.bottom
                anchors.topMargin: 12
                width: parent.width
                height: 1
                color: page.colors["border-subtle"]
            }
            TextField {
                id: addMembersSearch
                objectName: "addMembersSearch"
                anchors.top: parent.top
                anchors.topMargin: 62
                anchors.left: parent.left
                anchors.leftMargin: 16
                anchors.right: parent.right
                anchors.rightMargin: 16
                placeholderText: qsTr("Search users…")
                placeholderTextColor: page.colors["text-muted"]
                color: page.colors["text-primary"]
                font.pixelSize: 14
                leftPadding: 12
                rightPadding: 12
                topPadding: 8
                bottomPadding: 8
                background: Rectangle {
                    radius: 8
                    color: page.colors["bg"]
                    border.width: 1
                    border.color: addMembersSearch.activeFocus ? page.colors.accent : page.colors["border-subtle"]
                }
            }
            Text {
                objectName: "addMembersEmpty"
                visible: addMembersDialog.pickerOptions.length === 0
                anchors.top: addMembersSearch.bottom
                anchors.topMargin: 48
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("No results found")
                textFormat: Text.PlainText
                font.pixelSize: 14
                color: page.colors["text-muted"]
            }
            Rectangle {
                objectName: "addMembersListFrame"
                anchors.top: addMembersSearch.bottom
                anchors.topMargin: 12
                anchors.bottom: addMembersError.top
                anchors.bottomMargin: 8
                anchors.left: parent.left
                anchors.leftMargin: 16
                anchors.right: parent.right
                anchors.rightMargin: 16
                radius: 8
                color: "transparent"
                border.width: 1
                border.color: page.colors["border-subtle"]
                clip: true
                ListView {
                    id: addMembersList
                    objectName: "addMembersList"
                    anchors.fill: parent
                    anchors.margins: 1
                    model: addMembersDialog.pickerOptions
                    boundsBehavior: Flickable.StopAtBounds
                    clip: true
                    delegate: Item {
                        required property var modelData
                        required property int index
                        objectName: "addMembersOption-" + modelData.id
                        width: addMembersList.width
                        height: 44
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 1
                            radius: 6
                            color: optionArea.containsMouse
                                || addMembersDialog.selectedUsers.indexOf(modelData.id) >= 0
                                ? page.colors["card-bg-active"] : "transparent"
                        }
                        MouseArea {
                            id: optionArea
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: addMembersDialog.toggleUser(modelData.id)
                        }
                        Text {
                            objectName: "addMembersOptionName-" + modelData.id
                            x: 12
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.verticalCenterOffset: -9
                            width: parent.width - 12 - 32
                            text: modelData.displayName
                            textFormat: Text.PlainText
                            font.pixelSize: 14
                            font.weight: Font.Medium
                            color: page.colors["text-primary"]
                            elide: Text.ElideRight
                        }
                        Text {
                            objectName: "addMembersOptionEmail-" + modelData.id
                            x: 12
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.verticalCenterOffset: 9
                            width: parent.width - 12 - 32
                            text: modelData.email.length > 0 ? modelData.email : modelData.username
                            textFormat: Text.PlainText
                            font.pixelSize: 11
                            color: page.colors["text-secondary"]
                            elide: Text.ElideRight
                        }
                        ToolButton {
                            objectName: "addMembersCheck-" + modelData.id
                            visible: addMembersDialog.selectedUsers.indexOf(modelData.id) >= 0
                            anchors.right: parent.right
                            anchors.rightMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            focusPolicy: Qt.NoFocus
                            padding: 0
                            display: AbstractButton.IconOnly
                            icon.source: "qrc:/workspace/icons/check.svg"
                            icon.width: 16
                            icon.height: 16
                            icon.color: page.colors["icon-active"]
                            background: Item {}
                        }
                    }
                }
            }
            Text {
                id: addMembersError
                objectName: "addMembersError"
                visible: text.length > 0
                anchors.bottom: addMembersFooter.top
                anchors.bottomMargin: 4
                anchors.left: parent.left
                anchors.leftMargin: 16
                text: controller.channelInfoAddError
                textFormat: Text.PlainText
                font.pixelSize: 12
                color: page.colors["notice-base"]
            }
            Row {
                id: addMembersFooter
                objectName: "addMembersFooter"
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 16
                anchors.right: parent.right
                anchors.rightMargin: 16
                spacing: 8
                Button {
                    objectName: "addMembersCancel"
                    enabled: !controller.channelInfoAddBusy
                    text: qsTr("Cancel")
                    padding: 0
                    implicitWidth: cancelContent.implicitWidth + 24
                    implicitHeight: 30
                    contentItem: Text {
                        id: cancelContent
                        text: parent.text
                        textFormat: Text.PlainText
                        font.pixelSize: 14
                        color: page.colors["text-primary"]
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: 8
                        color: "transparent"
                        border.width: 1
                        border.color: page.colors["border-subtle"]
                        opacity: parent.enabled ? 1.0 : 0.5
                    }
                    onClicked: addMembersDialog.close()
                }
                Button {
                    objectName: "addMembersAdd"
                    enabled: !controller.channelInfoAddBusy && addMembersDialog.selectedUsers.length > 0
                    text: qsTr("Add")
                    padding: 0
                    implicitWidth: addContent.implicitWidth + 24
                    implicitHeight: 30
                    contentItem: Text {
                        id: addContent
                        text: parent.text
                        textFormat: Text.PlainText
                        font.pixelSize: 14
                        color: page.colors["on-accent"]
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: 8
                        color: page.colors.accent
                        opacity: parent.enabled ? 1.0 : 0.5
                    }
                    onClicked: page.controller.addChannelInfoMembers(addMembersDialog.selectedUsers)
                }
            }
        }
        // The add settled either closed after a successful POST or found
        // nothing left to send (duplicate re-filter); clear the selection.
        Connections {
            target: page.controller
            function onChannelInfoAddSettledChanged() {
                addMembersDialog.close();
            }
        }
    }
}
