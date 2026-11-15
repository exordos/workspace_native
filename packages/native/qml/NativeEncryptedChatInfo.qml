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
    // Locale name for the joined date labels (Main.qml binds preferences.locale).
    property string localeName: "en"
    // Opened saved-key uuid on the local-key screen (drawer currentChatKeyUuid).
    property string keyUuid: ""
    // The reference collapsible starts collapsed (LocalKeys useState(false)).
    property bool localKeysExpanded: false
    signal screenChange(string screen)
    signal localKeyOpen(string keyUuid)
    // The member add affordance routes to the shared add-members dialog through
    // Main.qml (the ordinary layer owns the one dialog instance).
    signal addMembersRequested()
    objectName: "encryptedChatInfoLayer"
    anchors.fill: parent
    clip: true
    // Reset the scroll position whenever the active screen changes: the flows
    // have different heights, and a retained offset can leave the shorter
    // screen scrolled out of view (reference remounts per screen).
    onChatScreenChanged: {
        pageScroll.contentY = 0
        // Reveal-driven key enumeration (#197 convention: never fetch hidden);
        // the durable vault read is synchronous (docs/NATIVE_DESKTOP_E2EE_KEY_MANAGEMENT.md).
        // The handler runs before the derived readonly bindings below re-evaluate,
        // so it must read chatScreen directly rather than page.keyScreen.
        if (page.chatScreen === "key" || page.chatScreen === "local-key")
            page.controller.refreshChatLocalKeys()
    }

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
    function refresh() {
        page.controller.refreshChannelInfo()
        if (page.keyScreen || page.localKeyScreen)
            page.controller.refreshChatLocalKeys()
    }
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

    // ---- Key management projections (issue #244) ----
    readonly property var chatEncryption: controller.encryption
    readonly property var keyInventory: controller.chatKeyInventory
    readonly property string inventoryState: keyInventory && keyInventory.state ? String(keyInventory.state) : "loading"
    readonly property string inventoryError: keyInventory && keyInventory.error ? String(keyInventory.error) : ""
    readonly property var inventoryKeys: keyInventory && keyInventory.keys ? keyInventory.keys : []
    readonly property string currentKeyUuid: chatEncryption && chatEncryption.currentKeyUuid
        ? String(chatEncryption.currentKeyUuid) : ""
    readonly property bool hasCurrentKey: currentKeyUuid.length > 0
    // Reference currentKeyMissing (connected ui:344-349): key known, inventory
    // loaded and no local row carries it.
    readonly property bool currentKeyMissing: inventoryState === "ready" && hasCurrentKey
        && !page.hasLocalRowFor(currentKeyUuid)
    readonly property string requestedKeyUuid: chatEncryption && String(chatEncryption.state || "") === "requested"
        && chatEncryption.keyUuid ? String(chatEncryption.keyUuid) : ""
    // The row opened on the local-key screen (null while loading/unknown/deleted).
    readonly property var localKeyRow: {
        if (page.keyUuid.length === 0)
            return null
        const keys = page.inventoryKeys
        for (let index = 0; index < keys.length; ++index)
            if (String(keys[index].keyUuid) === page.keyUuid)
                return keys[index]
        return null
    }
    function hasLocalRowFor(keyUuid) {
        const keys = page.inventoryKeys
        for (let index = 0; index < keys.length; ++index)
            if (String(keys[index].keyUuid) === keyUuid)
                return true
        return false
    }
    // joined-style labels (formatDateJoined / localKeyDateUnknown fallbacks).
    function keyDateLabel(isoDate, unknownText) {
        const value = String(isoDate || "")
        return value.length > 0 ? page.controller.formatChatKeyDate(value, page.localeName) : unknownText
    }
    function shortKeyUuid(keyUuid) {
        const value = String(keyUuid)
        return value.slice(0, 8) + "\u2026" + value.slice(-4)
    }

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
                : page.keyScreen
                    ? Math.max(keySummaryFlow.y + keySummaryFlow.height,
                        keyLocalSection.y + keyLocalSection.height)
                    : page.localKeyScreen
                        ? localKeyFlow.y + localKeyFlow.height
                        : settingsFlow.y + settingsFlow.height) + 16
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

        // ---- Key screen: KeySummary (encrypted-chat-info.ui.tsx:87-129) ----
        // section.space-y-3.px-4 inside the ScrollArea gap-5 py-4 flow.
        Column {
            id: keySummaryFlow
            objectName: "encryptedChatKeyScreen"
            x: 16
            y: 16
            width: pageScroll.width - 32
            spacing: 12
            visible: page.keyScreen

            // Same re-layout cure as localKeysFlow: the present-key block
            // and the created line appear when the verified projection and
            // the key_announced scan land asynchronously.
            readonly property var layoutWatch: ({hasKey: page.hasCurrentKey ? 1 : 0,
                announced: page.chatEncryption && page.chatEncryption.keyAnnouncedAt
                    ? page.chatEncryption.keyAnnouncedAt : "",
                missing: page.currentKeyMissing ? 1 : 0})
            onLayoutWatchChanged: Qt.callLater(function () { keySummaryFlow.forceLayout() })
            Accessible.name: qsTr("Encryption keys")

            TextEdit {
                id: keyClipboard
                objectName: "encryptedChatKeyClipboard"
                width: 0; height: 0
                opacity: 0
                text: ""
            }

            // Present-key block (space-y-2).
            Column {
                objectName: "encryptedChatKeyPresentBlock"
                width: keySummaryFlow.width
                spacing: 8
                visible: page.hasCurrentKey
                // flex items-center justify-between gap-2 text-sm text-text-muted
                Item {
                    width: parent.width
                    height: 20
                    Text {
                        objectName: "encryptedChatKeyUuidLabel"
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Current key ID")
                        textFormat: Text.PlainText
                        font.pixelSize: 14
                        color: page.colors["text-muted"]
                    }
                    Button {
                        id: keyCopyButton
                        objectName: "encryptedChatKeyCopyButton"
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: 24; height: 24
                        padding: 0
                        hoverEnabled: true
                        display: AbstractButton.IconOnly
                        Accessible.name: qsTr("Copy key ID")
                        icon.source: "qrc:/workspace/icons/copy.svg"
                        icon.width: 24; icon.height: 24
                        icon.color: page.colors["text-muted"]
                        background: Rectangle {
                            radius: 4
                            color: keyCopyButton.hovered ? page.colors["sidebar-item-hover"] : "transparent"
                        }
                        // Reference onCopyKey writes the UUID through the
                        // clipboard helper guarded by the runtime scope; the
                        // native drawer is already scoped to the viewed stream.
                        onClicked: {
                            keyClipboard.text = page.currentKeyUuid
                            keyClipboard.selectAll()
                            keyClipboard.copy()
                        }
                    }
                }
                // rounded-lg bg-card-bg px-2 py-1.5 text-sm leading-5 truncate
                Rectangle {
                    objectName: "encryptedChatKeyChip"
                    width: parent.width
                    height: 32
                    radius: 8
                    color: page.colors["card-bg"]
                    Text {
                        objectName: "encryptedChatKeyUuid"
                        x: 8
                        width: parent.width - 16
                        anchors.verticalCenter: parent.verticalCenter
                        height: 20
                        text: page.currentKeyUuid
                        textFormat: Text.PlainText
                        font.pixelSize: 14
                        lineHeightMode: Text.FixedHeight; lineHeight: 20
                        elide: Text.ElideRight
                        color: page.colors["text-primary"]
                    }
                }
                // createdLabel: e2ee.keyCreatedAt over the earliest announcement.
                Text {
                    objectName: "encryptedChatKeyCreated"
                    width: parent.width
                    height: visible ? 16 : 0
                    visible: page.chatEncryption && String(page.chatEncryption.keyAnnouncedAt || "").length > 0
                    text: visible
                        ? qsTr("Created: %1").arg(page.controller.formatChatKeyDate(
                            String(page.chatEncryption.keyAnnouncedAt), page.localeName))
                        : ""
                    textFormat: Text.PlainText
                    font.pixelSize: 14
                    lineHeightMode: Text.FixedHeight; lineHeight: 16
                    elide: Text.ElideRight
                    color: page.colors["text-muted"]
                }
            }

            Text {
                objectName: "encryptedChatKeyUnavailable"
                visible: !page.hasCurrentKey
                width: keySummaryFlow.width
                text: qsTr("Key unavailable")
                textFormat: Text.PlainText
                font.pixelSize: 14
                color: page.colors["text-muted"]
            }

            // fullWidth rotate button (Button md: h-9, rounded-lg, leading 20 px
            // cached.svg) - the reference canRotateKey affordance.
            Button {
                id: reissueButton
                objectName: "encryptedChatReissueButton"
                visible: page.chatEncryption && page.chatEncryption.canRotate === true
                width: keySummaryFlow.width
                height: 36
                padding: 0
                hoverEnabled: true
                enabled: page.chatEncryption && !page.chatEncryption.busy
                    && page.chatEncryption.reissueBusy !== true
                Accessible.name: qsTr("Reissue key")
                contentItem: Item {
                    Row {
                        anchors.centerIn: parent
                        spacing: 6
                        Image {
                            source: "qrc:/workspace/icons/cached.svg"
                            sourceSize.width: 20; sourceSize.height: 20
                            width: 20; height: 20
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Reissue key")
                            textFormat: Text.PlainText
                            font.pixelSize: 14
                            font.weight: Font.Medium
                            color: page.colors["on-accent"]
                        }
                    }
                }
                background: Rectangle {
                    radius: 8
                    color: page.colors["accent"]
                    opacity: reissueButton.enabled ? (reissueButton.down ? 0.8 : 1) : 0.5
                }
                onClicked: page.controller.reissueChatKey()
            }

            // Action failures surface inline (native has no drawer toast layer).
            Text {
                objectName: "encryptedChatKeyActionError"
                visible: page.chatEncryption && String(page.chatEncryption.actionError || "").length > 0
                width: keySummaryFlow.width
                text: page.chatEncryption ? String(page.chatEncryption.actionError) : ""
                textFormat: Text.PlainText
                font.pixelSize: 12
                wrapMode: Text.Wrap
                color: page.colors["notice-base"]
                Accessible.role: Accessible.AlertMessage
            }
        }

        // ---- Key screen: LocalKeys section (encrypted-chat-info.ui.tsx:130-230) ----
        // section.space-y-2.border-t.border-border-subtle.pt-3 spans the viewport
        // width; its blocks carry the 16 px inset internally.
        Item {
            id: keyLocalSection
            objectName: "encryptedChatLocalKeysSection"
            x: 0
            y: keySummaryFlow.y + keySummaryFlow.height + 20
            width: pageScroll.width
            height: localKeysFlow.y + localKeysFlow.height
            visible: page.keyScreen

            Rectangle {
                objectName: "encryptedChatLocalKeysDivider"
                width: parent.width
                height: 1
                color: page.colors["border-subtle"]
            }
            Column {
                id: localKeysFlow
                objectName: "encryptedChatLocalKeysFlow"
                y: 13 // pt-3
                width: parent.width
                spacing: 8

                // Qt keeps the current Column layout when a child flips to
                // visible through an async binding (the expand click lands
                // before the inventory read settles): the flow would stay at
                // the collapsed height, the rows would paint past the section
                // bounds and the hit test would skip them entirely (#244
                // window case evidence). Re-force the layout whenever one of
                // the states behind the children's visible bindings changes.
                readonly property var layoutWatch: ({expanded: page.localKeysExpanded,
                    state: page.inventoryState, count: page.inventoryKeys.length,
                    missing: page.currentKeyMissing, requested: page.requestedKeyUuid,
                    current: page.currentKeyUuid})
                onLayoutWatchChanged: Qt.callLater(function () { localKeysFlow.forceLayout() })

                // currentKeyMissing block: hint + request-current-key recovery.
                Column {
                    objectName: "encryptedChatCurrentKeyMissing"
                    visible: page.currentKeyMissing
                    width: parent.width
                    spacing: 8
                    leftPadding: 16
                    rightPadding: 16
                    bottomPadding: 8
                    Text {
                        objectName: "encryptedChatCurrentKeyMissingHint"
                        width: parent.width
                        text: qsTr("The current key is missing from this device.")
                        textFormat: Text.PlainText
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                        color: page.colors["text-muted"]
                    }
                    Button {
                        id: requestCurrentKeyButton
                        objectName: "encryptedChatRequestCurrentKeyButton"
                        width: parent.width - parent.leftPadding - parent.rightPadding
                        height: 36
                        padding: 0
                        hoverEnabled: true
                        enabled: page.chatEncryption && page.chatEncryption.canRequest === true
                        Accessible.name: qsTr("Request current key")
                        contentItem: Text {
                            anchors.centerIn: parent
                            text: qsTr("Request current key")
                            textFormat: Text.PlainText
                            font.pixelSize: 14
                            font.weight: Font.Medium
                            color: page.colors["accent"]
                            opacity: requestCurrentKeyButton.enabled ? 1 : 0.5
                        }
                        background: Rectangle {
                            radius: 8
                            color: "transparent"
                            border.width: 1
                            border.color: page.colors["accent"]
                            opacity: requestCurrentKeyButton.enabled ? 1 : 0.5
                        }
                        onClicked: page.controller.requestChatKey("")
                    }
                    Text {
                        objectName: "encryptedChatCurrentKeyRequested"
                        visible: page.requestedKeyUuid.length > 0
                            && page.requestedKeyUuid === page.currentKeyUuid
                        width: parent.width
                        text: qsTr("Request sent. Waiting for the key.")
                        textFormat: Text.PlainText
                        font.pixelSize: 12
                        color: page.colors["text-muted"]
                    }
                }

                // Collapsible PanelMenuRow (44 px), chevron 20.
                Button {
                    id: localKeysToggle
                    objectName: "encryptedChatLocalKeysToggle"
                    width: parent.width
                    height: 44
                    padding: 0
                    hoverEnabled: true
                    Accessible.name: qsTr("Keys on this device (%1)").arg(page.inventoryKeys.length)
                    contentItem: Item {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: 16
                        anchors.rightMargin: 16
                        anchors.verticalCenter: parent.verticalCenter
                        height: 20
                        Text {
                            objectName: "encryptedChatLocalKeysToggleLabel"
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Keys on this device (%1)").arg(page.inventoryKeys.length)
                            textFormat: Text.PlainText
                            font.pixelSize: 14
                            font.weight: Font.Medium
                            color: page.colors["text-primary"]
                        }
                        NativeIconButton {
                            objectName: "encryptedChatLocalKeysChevron"
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            width: 20; height: 20
                            padding: 0
                            colors: page.colors
                            asset: "chevron-down.svg"
                            glyphSize: 20
                            rotation: page.localKeysExpanded ? 180 : 0
                            enabled: false
                            opacity: 1
                            icon.color: page.colors["text-muted"]
                            Accessible.ignored: true
                        }
                    }
                    background: Rectangle {
                        radius: 6
                        color: localKeysToggle.hovered ? page.colors["sidebar-item-hover"] : "transparent"
                    }
                    onClicked: page.localKeysExpanded = !page.localKeysExpanded
                }

                // Loading / error / empty / list, gated by the expanded state.
                Text {
                    objectName: "encryptedChatLocalKeysLoading"
                    visible: page.localKeysExpanded && page.inventoryState === "loading"
                    width: parent.width
                    leftPadding: 16
                    text: qsTr("Loading…")
                    textFormat: Text.PlainText
                    font.pixelSize: 12
                    color: page.colors["text-muted"]
                }
                Text {
                    objectName: "encryptedChatLocalKeysError"
                    visible: page.localKeysExpanded && page.inventoryState === "error"
                    width: parent.width
                    leftPadding: 16
                    text: page.inventoryError
                    textFormat: Text.PlainText
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                    color: page.colors["notice-base"]
                    Accessible.role: Accessible.AlertMessage
                }
                Button {
                    id: localKeysRetry
                    objectName: "encryptedChatLocalKeysRetry"
                    visible: page.localKeysExpanded && page.inventoryState === "error"
                    width: implicitContentWidth + 24
                    height: 32
                    leftPadding: 12; rightPadding: 12
                    topPadding: 0; bottomPadding: 0
                    Accessible.name: qsTr("Retry")
                    text: qsTr("Retry")
                    font.pixelSize: 12
                    palette.buttonText: page.colors["text-primary"]
                    background: Rectangle {
                        radius: 8
                        color: page.colors["card-bg-active"]
                    }
                    onClicked: page.controller.refreshChatLocalKeys()
                }
                Text {
                    objectName: "encryptedChatLocalKeysEmpty"
                    visible: page.localKeysExpanded && page.inventoryState === "ready"
                        && page.inventoryKeys.length === 0
                    width: parent.width
                    leftPadding: 16
                    text: qsTr("No saved keys for this chat on this device.")
                    textFormat: Text.PlainText
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                    color: page.colors["text-muted"]
                }
                Column {
                    objectName: "encryptedChatLocalKeysList"
                    visible: page.localKeysExpanded && page.inventoryState === "ready"
                        && page.inventoryKeys.length > 0
                    width: parent.width
                    Repeater {
                        model: page.inventoryKeys
                        delegate: Button {
                            id: localKeyRowButton
                            required property var modelData
                            objectName: "encryptedChatLocalKeyRow-" + String(modelData.keyUuid)
                            width: parent.width
                            height: 50
                            padding: 0
                            hoverEnabled: true
                            Accessible.name: qsTr("Saved key") + ": " + String(modelData.keyUuid)
                            contentItem: Item {
                                anchors.fill: parent
                                Column {
                                    id: localKeyRowColumn
                                    x: 16
                                    width: parent.width - 16 - 16 - 8 - 16
                                    y: 8
                                    spacing: 2
                                    Item {
                                        width: parent.width
                                        height: 16
                                        Text {
                                            id: localKeyRowBadge
                                            objectName: "encryptedChatLocalKeyRowBadge-" + String(localKeyRowButton.modelData.keyUuid)
                                            anchors.right: parent.right
                                            anchors.verticalCenter: parent.verticalCenter
                                            text: localKeyRowButton.modelData.isCurrent
                                                ? qsTr("Current") : qsTr("Historical")
                                            textFormat: Text.PlainText
                                            font.pixelSize: 12
                                            color: localKeyRowButton.modelData.isCurrent
                                                ? page.colors["accent"] : page.colors["text-muted"]
                                        }
                                        Text {
                                            objectName: "encryptedChatLocalKeyRowUuid-" + String(localKeyRowButton.modelData.keyUuid)
                                            anchors.left: parent.left
                                            anchors.verticalCenter: parent.verticalCenter
                                            width: Math.max(0, parent.width - localKeyRowBadge.implicitWidth - 8)
                                            text: page.shortKeyUuid(localKeyRowButton.modelData.keyUuid)
                                            textFormat: Text.PlainText
                                            font.pixelSize: 12
                                            font.family: "monospace"
                                            elide: Text.ElideRight
                                            color: page.colors["text-primary"]
                                        }
                                    }
                                    Text {
                                        objectName: "encryptedChatLocalKeyRowAdded-" + String(localKeyRowButton.modelData.keyUuid)
                                        width: parent.width
                                        height: 16
                                        text: qsTr("Added: %1").arg(page.keyDateLabel(
                                            localKeyRowButton.modelData.addedAt, qsTr("Unknown")))
                                        textFormat: Text.PlainText
                                        font.pixelSize: 12
                                        lineHeightMode: Text.FixedHeight; lineHeight: 16
                                        elide: Text.ElideRight
                                        color: page.colors["text-muted"]
                                    }
                                }
                                NativeIconButton {
                                    anchors.right: parent.right
                                    anchors.rightMargin: 16
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 16; height: 16
                                    padding: 0
                                    colors: page.colors
                                    asset: "chevron-right.svg"
                                    glyphSize: 16
                                    enabled: false
                                    opacity: 1
                                    icon.color: page.colors["text-muted"]
                                    Accessible.ignored: true
                                }
                            }
                            background: Rectangle {
                                color: localKeyRowButton.hovered
                                    ? page.colors["sidebar-item-hover"] : "transparent"
                            }
                            onClicked: page.localKeyOpen(String(localKeyRowButton.modelData.keyUuid))
                        }
                    }
                }
            }
        }

        // ---- Local-key screen: LocalKeyDetails (encrypted-chat-info.ui.tsx:231-301) ----
        Column {
            id: localKeyFlow
            objectName: "encryptedChatLocalKeyScreen"
            x: 16
            y: 16
            width: pageScroll.width - 32
            spacing: 20
            visible: page.localKeyScreen

            // Same re-layout cure as localKeysFlow: the details blocks swap
            // on the async inventory read and on the vault deletion, and Qt
            // keeps the stale layout otherwise (rows land outside the hit
            // test bounds; see the localKeysFlow note).
            readonly property var layoutWatch: ({state: page.inventoryState,
                hasRow: page.localKeyRow ? 1 : 0, requested: page.requestedKeyUuid})
            onLayoutWatchChanged: Qt.callLater(function () { localKeyFlow.forceLayout() })
            Accessible.name: qsTr("Saved key")

            Text {
                objectName: "encryptedChatLocalKeyLoading"
                visible: page.inventoryState === "loading"
                width: parent.width
                text: qsTr("Loading…")
                textFormat: Text.PlainText
                font.pixelSize: 14
                color: page.colors["text-muted"]
            }
            Column {
                objectName: "encryptedChatLocalKeyErrorBlock"
                visible: page.inventoryState === "error"
                width: parent.width
                spacing: 12
                Text {
                    objectName: "encryptedChatLocalKeyError"
                    width: parent.width
                    text: page.inventoryError
                    textFormat: Text.PlainText
                    font.pixelSize: 14
                    wrapMode: Text.Wrap
                    color: page.colors["notice-base"]
                    Accessible.role: Accessible.AlertMessage
                }
                Button {
                    objectName: "encryptedChatLocalKeyErrorRetry"
                    width: implicitContentWidth + 24
                    height: 32
                    leftPadding: 12; rightPadding: 12
                    topPadding: 0; bottomPadding: 0
                    text: qsTr("Retry")
                    font.pixelSize: 12
                    Accessible.name: qsTr("Retry")
                    palette.buttonText: page.colors["text-primary"]
                    background: Rectangle { radius: 8; color: page.colors["card-bg-active"] }
                    onClicked: page.controller.refreshChatLocalKeys()
                }
            }
            Text {
                objectName: "encryptedChatLocalKeyUnavailable"
                visible: page.inventoryState === "ready" && page.localKeyRow === null
                width: parent.width
                text: qsTr("This key is no longer saved on this device.")
                textFormat: Text.PlainText
                font.pixelSize: 14
                wrapMode: Text.Wrap
                color: page.colors["text-muted"]
            }

            Column {
                objectName: "encryptedChatLocalKeyDetails"
                visible: page.localKeyRow !== null
                width: parent.width
                spacing: 20

                Column {
                    width: parent.width
                    spacing: 8
                    Text {
                        objectName: "encryptedChatLocalKeyUuidLabel"
                        width: parent.width
                        text: qsTr("Key ID")
                        textFormat: Text.PlainText
                        font.pixelSize: 14
                        color: page.colors["text-muted"]
                    }
                    // select-text break-all rounded-lg bg-card-bg px-2 py-2 font-mono text-sm
                    Rectangle {
                        width: parent.width
                        radius: 8
                        color: page.colors["card-bg"]
                        height: localKeyUuidValue.height + 16
                        Text {
                            id: localKeyUuidValue
                            objectName: "encryptedChatLocalKeyUuid"
                            x: 8
                            width: parent.width - 16
                            y: 8
                            text: page.localKeyRow ? String(page.localKeyRow.keyUuid) : ""
                            textFormat: Text.PlainText
                            font.pixelSize: 14
                            font.family: "monospace"
                            wrapMode: Text.WrapAnywhere
                            color: page.colors["text-primary"]
                        }
                    }
                    Text {
                        objectName: "encryptedChatLocalKeyState"
                        width: parent.width
                        text: page.localKeyRow && page.localKeyRow.isCurrent
                            ? qsTr("Current") : qsTr("Historical")
                        textFormat: Text.PlainText
                        font.pixelSize: 12
                        color: page.localKeyRow && page.localKeyRow.isCurrent
                            ? page.colors["accent"] : page.colors["text-muted"]
                    }
                }

                // dl.space-y-3: dt text-text-muted / dd mt-1.
                Column {
                    width: parent.width
                    spacing: 12
                    Column {
                        width: parent.width
                        spacing: 4
                        Text {
                            objectName: "encryptedChatLocalKeyAnnouncedLabel"
                            width: parent.width
                            text: qsTr("Announced in chat")
                            textFormat: Text.PlainText
                            font.pixelSize: 14
                            color: page.colors["text-muted"]
                        }
                        Text {
                            objectName: "encryptedChatLocalKeyAnnouncedValue"
                            width: parent.width
                            text: page.localKeyRow
                                ? page.keyDateLabel(page.localKeyRow.announcedAt,
                                    qsTr("Not found in loaded history"))
                                : ""
                            textFormat: Text.PlainText
                            font.pixelSize: 14
                            wrapMode: Text.Wrap
                            color: page.colors["text-primary"]
                        }
                    }
                    Column {
                        width: parent.width
                        spacing: 4
                        Text {
                            objectName: "encryptedChatLocalKeyAddedLabel"
                            width: parent.width
                            text: qsTr("Added to this device")
                            textFormat: Text.PlainText
                            font.pixelSize: 14
                            color: page.colors["text-muted"]
                        }
                        Text {
                            objectName: "encryptedChatLocalKeyAddedValue"
                            width: parent.width
                            text: page.localKeyRow
                                ? page.keyDateLabel(page.localKeyRow.addedAt, qsTr("Unknown"))
                                : ""
                            textFormat: Text.PlainText
                            font.pixelSize: 14
                            wrapMode: Text.Wrap
                            color: page.colors["text-primary"]
                        }
                    }
                }

                Button {
                    id: requestLocalKeyButton
                    objectName: "encryptedChatRequestLocalKeyButton"
                    width: parent.width
                    height: 36
                    padding: 0
                    hoverEnabled: true
                    enabled: page.chatEncryption && page.chatEncryption.canRequest === true
                    Accessible.name: qsTr("Request this key again")
                    contentItem: Text {
                        anchors.centerIn: parent
                        text: qsTr("Request this key again")
                        textFormat: Text.PlainText
                        font.pixelSize: 14
                        font.weight: Font.Medium
                        color: page.colors["accent"]
                        opacity: requestLocalKeyButton.enabled ? 1 : 0.5
                    }
                    background: Rectangle {
                        radius: 8
                        color: "transparent"
                        border.width: 1
                        border.color: page.colors["accent"]
                        opacity: requestLocalKeyButton.enabled ? 1 : 0.5
                    }
                    onClicked: page.controller.requestChatKey(page.keyUuid)
                }
                Text {
                    objectName: "encryptedChatLocalKeyRequested"
                    visible: page.requestedKeyUuid.length > 0 && page.requestedKeyUuid === page.keyUuid
                    width: parent.width
                    text: qsTr("Request sent. The saved key remains available.")
                    textFormat: Text.PlainText
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                    color: page.colors["text-muted"]
                }
                Button {
                    id: deleteLocalKeyButton
                    objectName: "encryptedChatDeleteLocalKeyButton"
                    width: parent.width
                    height: 36
                    padding: 0
                    hoverEnabled: true
                    Accessible.name: qsTr("Delete from this device")
                    contentItem: Text {
                        anchors.centerIn: parent
                        text: qsTr("Delete from this device")
                        textFormat: Text.PlainText
                        font.pixelSize: 14
                        font.weight: Font.Medium
                        color: page.colors["notice-base"]
                    }
                    background: Rectangle {
                        radius: 8
                        color: deleteLocalKeyButton.hovered ? page.colors["sidebar-item-hover"] : "transparent"
                    }
                    // R2 review 4576750806: the reference routes the danger
                    // button through the shared confirmation dialog; deleting a
                    // local key may drop the only copy of a historical key, so
                    // the dialog naming that consequence gates the action.
                    onClicked: { console.log("PROBE-DELETE-CLICK"); deleteLocalKeyConfirm.open() }
                }
                Dialog {
                    id: deleteLocalKeyConfirm
                    objectName: "encryptedChatDeleteLocalKeyConfirm"
                    onVisibleChanged: console.log("PROBE-DIALOG-VISIBLE", visible)
                    parent: Overlay.overlay
                    anchors.centerIn: parent
                    width: Math.min(360, page.width - 48)
                    modal: true
                    dim: true
                    focus: true
                    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                    Overlay.modal: Rectangle { color: "#80000000" }
                    background: Rectangle {
                        radius: 10
                        color: page.colors["bg-elevated"]
                        border.color: page.colors["border-subtle"]
                    }
                    contentItem: Column {
                        spacing: 12
                        Text {
                            objectName: "encryptedChatDeleteLocalKeyConfirmTitle"
                            width: parent.width
                            text: qsTr("Delete the saved key?")
                            textFormat: Text.PlainText
                            font.pixelSize: 14
                            font.weight: Font.DemiBold
                            wrapMode: Text.Wrap
                            color: page.colors["text-primary"]
                        }
                        Text {
                            objectName: "encryptedChatDeleteLocalKeyConfirmDescription"
                            width: parent.width
                            text: qsTr("Messages encrypted with this key will be unavailable on this device until you obtain the key again. The key in the chat and on other devices will remain unchanged.")
                            textFormat: Text.PlainText
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                            color: page.colors["text-muted"]
                        }
                        Row {
                            spacing: 8
                            layoutDirection: Qt.RightToLeft
                            Button {
                                objectName: "encryptedChatDeleteLocalKeyConfirmDelete"
                                height: 36
                                padding: 0
                                leftPadding: 14; rightPadding: 14
                                Accessible.name: qsTr("Delete from this device")
                                contentItem: Text {
                                    anchors.centerIn: parent
                                    text: qsTr("Delete from this device")
                                    textFormat: Text.PlainText
                                    font.pixelSize: 14
                                    font.weight: Font.Medium
                                    color: page.colors["notice-base"]
                                }
                                background: Rectangle {
                                    radius: 8
                                    color: parent.hovered ? page.colors["sidebar-item-hover"] : "transparent"
                                    border.width: 1
                                    border.color: page.colors["notice-base"]
                                }
                                onClicked: {
                                    deleteLocalKeyConfirm.close()
                                    page.controller.deleteChatLocalKey(page.keyUuid)
                                }
                            }
                            Button {
                                objectName: "encryptedChatDeleteLocalKeyConfirmCancel"
                                height: 36
                                padding: 0
                                leftPadding: 14; rightPadding: 14
                                Accessible.name: qsTr("Cancel")
                                contentItem: Text {
                                    anchors.centerIn: parent
                                    text: qsTr("Cancel")
                                    textFormat: Text.PlainText
                                    font.pixelSize: 14
                                    font.weight: Font.Medium
                                    color: page.colors["text-primary"]
                                }
                                background: Rectangle { radius: 8; color: page.colors["card-bg-active"] }
                                onClicked: deleteLocalKeyConfirm.close()
                            }
                        }
                    }
                }
                Text {
                    objectName: "encryptedChatLocalKeyActionError"
                    visible: page.chatEncryption && String(page.chatEncryption.actionError || "").length > 0
                    width: parent.width
                    text: page.chatEncryption ? String(page.chatEncryption.actionError) : ""
                    textFormat: Text.PlainText
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                    color: page.colors["notice-base"]
                    Accessible.role: Accessible.AlertMessage
                }
            }
        }
    }
}
