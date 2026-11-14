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
    objectName: "encryptedChatInfoLayer"
    anchors.fill: parent
    clip: true

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

    Flickable {
        id: pageScroll
        objectName: "protectedChatScroll"
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: Math.max(infoFlow.y + infoFlow.height, settingsFlow.y + settingsFlow.height) + 16
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
