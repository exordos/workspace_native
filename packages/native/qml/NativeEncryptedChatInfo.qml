pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// qmllint disable unqualified

// Encrypted chat info drawer content (issue #243, docs/NATIVE_DESKTOP_E2EE_CHAT_INFO.md).
// Reference: workspace_ui features/encrypted-chat-info/encrypted-chat-info.ui.tsx at
// 230e02b8b0273dee556a406746d122180a68b3e7 (screen switch :522-632, info screen
// :562-627, settings screen :379-415). The drawer shell keeps all chrome, so this
// view renders only the active screen body and reports navigation through
// `screenChange`, the counterpart of the reference `onScreenChange` callback.
// The `key` and `local-key` screens are empty scaffolds: #244 owns their content.
Item {
    id: page
    required property var colors
    required property var controller
    // "info" | "settings" | "key" | "local-key" (encrypted-chat-info.types.ts:3).
    property string chatScreen: "info"
    signal screenChange(string screen)
    // The member add affordance routes to the shared add-members dialog through
    // Main.qml (the ordinary layer owns the one dialog instance).
    signal addMembersRequested()
    objectName: "encryptedChatInfoLayer"
    anchors.fill: parent
    clip: true
    // Reset the scroll position whenever the active screen changes: the flows
    // have different heights, and a retained offset can leave the shorter
    // screen scrolled out of view (reference remounts per screen).
    onChatScreenChanged: pageScroll.contentY = 0

    // The shell reveals this layer only while it is on top, so a hidden layer
    // never fetches (#197). Reveal re-reads the info projection like the
    // ordinary chat info layer does.
    Component.onCompleted: if (page.visible) page.refresh()
    onVisibleChanged: if (visible) page.refresh()
    // Reference store mounts per channel: a stream switch re-reads the projection.
    property string watchedStream: ""
    function currentStreamId() {
        const info = page.controller.channelInfoStream
        return info && info.id ? String(info.id) : ""
    }
    function refresh() { page.controller.refreshChannelInfo() }
    Connections {
        target: page.controller
        function onStateChanged() {
            const id = page.currentStreamId()
            if (id === page.watchedStream)
                return
            page.watchedStream = id
            if (id.length > 0 && page.visible)
                page.refresh()
        }
    }

    // encrypted-chat-info-connected.ui.tsx:321-329 projection.
    readonly property var streamInfo: controller.channelInfoStream
    readonly property string streamTitle: streamInfo && streamInfo.title ? String(streamInfo.title) : ""
    readonly property string streamColor: streamInfo && streamInfo.color ? String(streamInfo.color) : ""
    readonly property string streamDescription: streamInfo && streamInfo.description
        ? String(streamInfo.description).trim() : ""
    readonly property bool streamPrivate: Boolean(streamInfo && streamInfo["private"])
    readonly property int participantsCount: controller.channelInfoMemberCount
    readonly property int onlineCount: controller.channelInfoOnlineCount
    readonly property bool infoScreen: page.chatScreen === "info"
    readonly property bool settingsScreen: page.chatScreen === "settings"
    readonly property bool keyScreen: page.chatScreen === "key"
    readonly property bool localKeyScreen: page.chatScreen === "local-key"

    // channel.participants / channel.online (i18next plural forms, ru one/few/many).
    function participantsLabel(count) {
        // channel.participants has one/few/many forms; the source key differs for
        // the singular so both entries live in the catalog.
        return count === 1 ? qsTr("%n participant", "", count) : qsTr("%n participants", "", count)
    }
    function onlineLabel(count) {
        return qsTr("%n online", "", count)
    }
    function countsLabel() {
        return participantsLabel(page.participantsCount) + ", " + page.onlineLabel(page.onlineCount);
    }

    // Member card helpers (mirrors the ordinary drawer's WorkspaceMemberCard
    // projection helpers - resolveWorkspaceRoleLabel / resolveWorkspaceStatusLabel).
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
    // resolveWorkspaceStatusLabel(status) ?? email.
    function memberSecondary(status, email) {
        const label = page.presenceLabel(status);
        return label.length > 0 ? label : email;
    }

    Flickable {
        id: pageScroll
        objectName: "protectedChatScroll"
        anchors.fill: parent
        clip: true
        contentWidth: width
        // Size the content to the active screen only: including the hidden
        // screen's flow would let the shorter screen scroll past its end.
        contentHeight: (page.infoScreen
                ? Math.max(infoFlow.y + infoFlow.height, membersSection.y + membersSection.height)
                : Math.max(settingsFlow.y + settingsFlow.height,
                    page.keyScreen || page.localKeyScreen ? 16 : 0)) + 16
        boundsBehavior: Flickable.StopAtBounds

        // ---- Info screen (encrypted-chat-info.ui.tsx:562-627) ----
        // ScrollArea gap-5 py-4 with px-4 blocks inside the shell's px-2 gutter.
        Column {
            id: infoFlow
            objectName: "protectedChatIdentityFlow"
            x: 16
            y: 16
            width: pageScroll.width - 32
            spacing: 20
            visible: page.infoScreen

            // flex items-center gap-4 px-4: the avatar is centred against the
            // three-line column (20 + 8 + 16 + 8 + 16 = 68 px).
            Item {
                id: identityBlock
                objectName: "protectedChatIdentity"
                width: infoFlow.width
                height: Math.max(64, identityColumn.height)

                Rectangle {
                    id: protectedAvatar
                    objectName: "protectedChatAvatar"
                    width: 64; height: 64
                    radius: 32
                    anchors.verticalCenter: parent.verticalCenter
                    // resolveAvatarBackgroundColor fallback is the Avatar bg token.
                    color: page.streamColor.length > 0 ? page.streamColor : page.colors["bg"]
                    border.width: 1
                    border.color: page.colors["border-subtle"]
                    NativeIconButton {
                        anchors.centerIn: parent
                        width: 40; height: 40
                        padding: 0
                        colors: page.colors
                        asset: "chat_lock.svg"
                        glyphSize: 40
                        enabled: false
                        opacity: 1
                        icon.color: page.colors["text-primary"]
                        Accessible.name: qsTr("Protected chat")
                    }
                }

                Column {
                    id: identityColumn
                    x: 80 // 64 px avatar + gap-4
                    width: parent.width - 80
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8
                    Text {
                        objectName: "protectedChatTitle"
                        width: parent.width
                        height: 20
                        text: page.streamTitle
                        textFormat: Text.PlainText
                        font.pixelSize: 20
                        font.weight: Font.Medium
                        lineHeightMode: Text.FixedHeight; lineHeight: 20
                        color: page.colors["text-primary"]
                        elide: Text.ElideRight
                        Accessible.role: Accessible.Heading
                    }
                    Text {
                        objectName: "protectedChatCaption"
                        width: parent.width
                        height: 16
                        text: qsTr("Protected chat")
                        textFormat: Text.PlainText
                        font.pixelSize: 16
                        lineHeightMode: Text.FixedHeight; lineHeight: 16
                        color: page.colors["accent"]
                        elide: Text.ElideRight
                    }
                    Text {
                        objectName: "protectedChatCounts"
                        width: parent.width
                        height: 16
                        text: page.countsLabel()
                        textFormat: Text.PlainText
                        font.pixelSize: 14
                        lineHeightMode: Text.FixedHeight; lineHeight: 16
                        color: page.colors["text-muted"]
                        elide: Text.ElideRight
                    }
                }
            }

            // flex gap-2 px-4 with two h-12 flex-1 tiles (Button rounded-lg).
            Row {
                objectName: "protectedChatActions"
                width: infoFlow.width
                spacing: 8
                height: 48
                Button {
                    id: callTile
                    objectName: "protectedChatCallButton"
                    width: (infoFlow.width - 8) / 2
                    height: 48
                    padding: 0
                    hoverEnabled: true
                    display: AbstractButton.IconOnly
                    // The reference passes the chat call bridge as onCall; the
                    // native counterpart is the header's own call affordance.
                    enabled: page.controller.canStartCall
                    icon.source: "qrc:/workspace/icons/phone.svg"
                    icon.width: 32; icon.height: 32
                    icon.color: page.colors["text-primary"]
                    Accessible.name: qsTr("Call")
                    background: Rectangle {
                        radius: 8
                        color: page.colors["card-bg-active"]
                        opacity: callTile.enabled ? (callTile.down ? 0.8 : 1) : 0.5
                    }
                    onClicked: page.controller.startCall()
                }
                Button {
                    id: settingsTile
                    objectName: "protectedChatSettingsButton"
                    width: (infoFlow.width - 8) / 2
                    height: 48
                    padding: 0
                    hoverEnabled: true
                    display: AbstractButton.IconOnly
                    icon.source: "qrc:/workspace/icons/instant-mix.svg"
                    icon.width: 32; icon.height: 32
                    icon.color: page.colors["text-primary"]
                    Accessible.name: qsTr("Settings")
                    background: Rectangle {
                        radius: 8
                        color: page.colors["accent-soft"]
                        opacity: settingsTile.down ? 0.8 : 1
                    }
                    onClicked: page.screenChange("settings")
                }
            }
        }

        // ---- Member management (reference MemberList :416-480, the last child
        // of the reference info screen). The section spans the scroll viewport
        // (the reference section carries no own padding): border-t pt-5 (20)
        // with a mb-3 h-8 px-4 text-base heading, a space-y-1 px-2 card list
        // and a px-4 text-xs error line. ----
        Item {
            id: membersSection
            objectName: "protectedChatMembersBlock"
            x: 0
            y: infoFlow.y + infoFlow.height + 20
            width: pageScroll.width
            // The section must carry its member flow in the scroll height: the
            // contentHeight binding sizes from this item (reviewer finding
            // 4575277281 - without a height the member rows below the viewport
            // were unreachable). border-t + pt-5 sit above the self-sized flow.
            height: membersFlow.y + membersFlow.height
            visible: page.infoScreen

            Rectangle {
                objectName: "protectedChatMembersDivider"
                width: parent.width
                height: 1
                color: page.colors["border-subtle"]
            }
            Column {
                id: membersFlow
                objectName: "protectedChatMembersFlow"
                y: 21 // border-t (1 px) + pt-5 (20 px)
                width: parent.width
                spacing: 12 // h3 mb-3
                Item {
                    objectName: "protectedChatMembersHeadingRow"
                    width: parent.width
                    height: 32
                    Text {
                        objectName: "protectedChatMembersHeading"
                        anchors.left: parent.left
                        anchors.leftMargin: 16
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Members")
                        textFormat: Text.PlainText
                        font.pixelSize: 16
                        font.weight: Font.Medium
                        color: page.colors["text-primary"]
                        Accessible.role: Accessible.Heading
                    }
                    NativeIconButton {
                        objectName: "protectedChatAddButton"
                        anchors.right: parent.right
                        anchors.rightMargin: 16
                        anchors.verticalCenter: parent.verticalCenter
                        visible: page.controller.channelInfoIsOwner
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
                        onClicked: page.addMembersRequested()
                    }
                }
                Text {
                    objectName: "protectedChatNoMembers"
                    visible: page.controller.channelInfoMembers.length === 0
                    width: parent.width
                    leftPadding: 16
                    text: qsTr("No members found")
                    textFormat: Text.PlainText
                    font.pixelSize: 14
                    color: page.colors["text-muted"]
                }
                Column {
                    id: protectedMemberRows
                    objectName: "protectedChatMemberRows"
                    x: 8
                    width: parent.width - 16
                    spacing: 4
                    Repeater {
                        model: page.controller.channelInfoMembers
                        // Same WorkspaceMemberCard contract the ordinary drawer
                        // measures: rounded-lg bg-card-bg px-3 py-2 gap-3, sm
                        // avatar with presence dot, name 14 / role 10 /
                        // secondary 11, remove button 24 visible on row hover.
                        delegate: Rectangle {
                            id: protectedMemberCard
                            required property var modelData
                            objectName: "protectedChatMember-" + modelData.bindingUuid
                            width: protectedMemberRows.width
                            height: 48
                            radius: 8
                            color: protectedMemberHover.containsMouse
                                ? page.colors["card-bg-active"] : page.colors["card-bg"]
                            property bool removing:
                                page.controller.channelInfoRemovingMembers.indexOf(modelData.userUuid) >= 0
                            MouseArea {
                                id: protectedMemberHover
                                anchors.fill: parent
                                acceptedButtons: Qt.NoButton
                                hoverEnabled: true
                            }
                            Rectangle {
                                objectName: "protectedChatMemberAvatar-" + protectedMemberCard.modelData.bindingUuid
                                x: 12
                                anchors.verticalCenter: parent.verticalCenter
                                width: 32; height: 32
                                radius: 16
                                color: page.colors["bg-elevated"]
                                border.width: 1
                                border.color: page.colors["border-subtle"]
                                Text {
                                    anchors.centerIn: parent
                                    text: protectedMemberCard.modelData.initial
                                    textFormat: Text.PlainText
                                    font.pixelSize: 12
                                    font.weight: Font.DemiBold
                                    color: page.colors["text-primary"]
                                }
                                Rectangle {
                                    objectName: "protectedChatMemberPresence-" + protectedMemberCard.modelData.bindingUuid
                                    visible: protectedMemberCard.modelData.presence.length > 0
                                    width: 8; height: 8
                                    radius: 4
                                    x: 32 - width - 1
                                    y: 32 - height - 1
                                    color: page.presenceColor(protectedMemberCard.modelData.presence)
                                    border.width: 2
                                    border.color: protectedMemberCard.color
                                }
                            }
                            Text {
                                id: protectedMemberRoleMeasure
                                visible: false
                                text: page.roleLabel(protectedMemberCard.modelData.role)
                                textFormat: Text.PlainText
                                font.pixelSize: 10
                            }
                            Text {
                                id: protectedMemberName
                                objectName: "protectedChatMemberName-" + protectedMemberCard.modelData.bindingUuid
                                y: 9
                                x: 56
                                width: Math.max(0, protectedMemberCard.width - 56 - 24 - 6
                                    - (protectedMemberRoleMeasure.text.length > 0
                                        ? protectedMemberRoleMeasure.width + 6 : 0))
                                text: protectedMemberCard.modelData.name
                                textFormat: Text.PlainText
                                font.pixelSize: 14
                                color: page.colors["text-primary"]
                                elide: Text.ElideRight
                            }
                            Text {
                                objectName: "protectedChatMemberRole-" + protectedMemberCard.modelData.bindingUuid
                                visible: protectedMemberRoleMeasure.text.length > 0
                                text: protectedMemberRoleMeasure.text
                                x: 56 + protectedMemberName.width + 6
                                y: 12
                                textFormat: Text.PlainText
                                font.pixelSize: 10
                                color: page.colors["text-secondary"]
                            }
                            Text {
                                objectName: "protectedChatMemberSecondary-" + protectedMemberCard.modelData.bindingUuid
                                y: 26
                                x: 56
                                width: protectedMemberCard.width - 56 - 12
                                text: page.memberSecondary(protectedMemberCard.modelData.status,
                                    protectedMemberCard.modelData.email)
                                textFormat: Text.PlainText
                                font.pixelSize: 11
                                color: page.colors["text-secondary"]
                                elide: Text.ElideRight
                            }
                            Button {
                                id: protectedMemberRemove
                                objectName: "protectedChatRemove-" + protectedMemberCard.modelData.bindingUuid
                                visible: protectedMemberCard.modelData.canRemove
                                width: 24; height: 24
                                anchors.right: parent.right
                                anchors.rightMargin: 12
                                anchors.verticalCenter: parent.verticalCenter
                                enabled: !protectedMemberCard.removing
                                opacity: enabled ? (protectedMemberHover.containsMouse ? 1.0 : 0.0) : 0.4
                                Behavior on opacity { NumberAnimation { duration: 120 } }
                                display: AbstractButton.IconOnly
                                padding: 0
                                hoverEnabled: true
                                icon.source: "qrc:/workspace/icons/close.svg"
                                icon.width: 14
                                icon.height: 14
                                icon.color: protectedMemberRemove.hovered && protectedMemberRemove.enabled
                                    ? page.colors["notice-base"] : page.colors["text-muted"]
                                Accessible.name: qsTr("Remove from channel: %1")
                                    .arg(protectedMemberCard.modelData.name)
                                background: Rectangle {
                                    radius: 4
                                    color: protectedMemberRemove.hovered && protectedMemberRemove.enabled
                                        ? page.colors["bg"] : "transparent"
                                }
                                onClicked: page.controller.removeChannelInfoMember(
                                    protectedMemberCard.modelData.bindingUuid)
                            }
                        }
                    }
                }
                Text {
                    objectName: "protectedChatMemberError"
                    visible: text.length > 0
                    width: parent.width
                    leftPadding: 16
                    text: page.controller.channelInfoMemberError
                    textFormat: Text.PlainText
                    font.pixelSize: 12
                    color: page.colors["notice-base"]
                }
            }
        }

        // ---- Settings screen (ChannelSettings, encrypted-chat-info.ui.tsx:379-415) ----
        Column {
            id: settingsFlow
            objectName: "protectedChatSettingsScreen"
            x: 16
            y: 16
            width: pageScroll.width - 32
            spacing: 20
            visible: page.settingsScreen

            // dl.space-y-3.px-4: label text-sm text-text-muted, value mt-1 text-sm.
            Column {
                width: settingsFlow.width
                spacing: 12
                Column {
                    width: settingsFlow.width
                    spacing: 4
                    Text {
                        objectName: "protectedChatNameLabel"
                        width: parent.width
                        text: qsTr("Channel name")
                        textFormat: Text.PlainText
                        font.pixelSize: 14
                        color: page.colors["text-muted"]
                    }
                    Text {
                        objectName: "protectedChatNameValue"
                        width: parent.width
                        text: page.streamTitle
                        textFormat: Text.PlainText
                        font.pixelSize: 14
                        wrapMode: Text.Wrap
                        color: page.colors["text-primary"]
                    }
                }
                Column {
                    width: settingsFlow.width
                    spacing: 4
                    Text {
                        objectName: "protectedChatDescriptionLabel"
                        width: parent.width
                        text: qsTr("Description (optional)")
                        textFormat: Text.PlainText
                        font.pixelSize: 14
                        color: page.colors["text-muted"]
                    }
                    Text {
                        objectName: "protectedChatDescriptionValue"
                        width: parent.width
                        text: page.streamDescription.length > 0
                            ? page.streamDescription : qsTr("No description")
                        textFormat: Text.PlainText
                        font.pixelSize: 14
                        wrapMode: Text.Wrap
                        color: page.colors["text-primary"]
                    }
                }
            }

            Text {
                objectName: "protectedChatReadOnlyHint"
                width: settingsFlow.width
                text: qsTr("Channel details are currently read-only.")
                textFormat: Text.PlainText
                font.pixelSize: 12
                wrapMode: Text.Wrap
                color: page.colors["text-muted"]
            }

            // PanelMenuRow: py-1.5 around a min-h-8 gap-2 row (44 px), icon in a
            // 32x32 box at 24 px, label text-sm font-medium, trailing chevron 20.
            Button {
                id: keysRow
                objectName: "protectedChatKeysRow"
                width: settingsFlow.width
                height: 44
                padding: 0
                hoverEnabled: true
                Accessible.name: qsTr("Encryption keys")
                contentItem: Item {
                    anchors.fill: parent
                    Row {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        height: 32
                        spacing: 8
                        NativeIconButton {
                            width: 32; height: 32
                            padding: 0
                            colors: page.colors
                            asset: "chat_lock.svg"
                            glyphSize: 24
                            enabled: false
                            opacity: 1
                            icon.color: page.colors["text-secondary"]
                            Accessible.ignored: true
                        }
                        Text {
                            objectName: "protectedChatKeysRowLabel"
                            height: 32
                            text: qsTr("Encryption keys")
                            textFormat: Text.PlainText
                            verticalAlignment: Text.AlignVCenter
                            font.pixelSize: 14
                            font.weight: Font.Medium
                            color: page.colors["text-primary"]
                        }
                    }
                    NativeIconButton {
                        objectName: "protectedChatKeysRowChevron"
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: 20; height: 20
                        padding: 0
                        colors: page.colors
                        asset: "chevron-right.svg"
                        glyphSize: 20
                        enabled: false
                        opacity: 1
                        icon.color: page.colors["text-muted"]
                        Accessible.ignored: true
                    }
                }
                background: Rectangle {
                    radius: 6
                    color: keysRow.hovered ? page.colors["sidebar-item-hover"] : "transparent"
                }
                onClicked: page.screenChange("key")
            }

            // flex items-center justify-between gap-2 px-4 text-sm rows.
            Item {
                objectName: "protectedChatTypeRow"
                width: settingsFlow.width
                height: 20
                Text {
                    id: protectedChatTypeLabel
                    objectName: "protectedChatTypeLabel"
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Channel type")
                    textFormat: Text.PlainText
                    font.pixelSize: 14
                    color: page.colors["text-primary"]
                }
                Text {
                    objectName: "protectedChatTypeValue"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, parent.width - protectedChatTypeLabel.width - 8)
                    text: page.streamPrivate ? qsTr("Closed, protected history") : qsTr("Open")
                    textFormat: Text.PlainText
                    font.pixelSize: 14
                    horizontalAlignment: Text.AlignRight
                    elide: Text.ElideRight
                    color: page.colors["text-muted"]
                }
            }
            Item {
                objectName: "protectedChatMembersRow"
                width: settingsFlow.width
                height: 20
                Text {
                    objectName: "protectedChatMembersLabel"
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Members")
                    textFormat: Text.PlainText
                    font.pixelSize: 14
                    color: page.colors["text-primary"]
                }
                Text {
                    objectName: "protectedChatMembersValue"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: String(page.participantsCount)
                    textFormat: Text.PlainText
                    font.pixelSize: 14
                    color: page.colors["text-muted"]
                }
            }
        }

        // Key screens are scaffolded empty; issue #244 owns their content.
        Item {
            objectName: "encryptedChatKeyScreen"
            x: 16
            y: 16
            width: pageScroll.width - 32
            height: 0
            visible: page.keyScreen
        }
        Item {
            objectName: "encryptedChatLocalKeyScreen"
            x: 16
            y: 16
            width: pageScroll.width - 32
            height: 0
            visible: page.localKeyScreen
        }
    }
}
