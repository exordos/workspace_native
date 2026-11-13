pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects

// Incoming call invite (issue #163). Pixel port of the reference renderer at
// workspace_ui 61dacef7: jitsi-call-incoming-large.ui.tsx (variant "large",
// the build-time default) and jitsi-call-incoming-compact.ui.tsx (variant
// "compact"). The root fills the viewport; the large variant renders the
// scrim + centered section, the compact variant the top-right toast. See
// docs/NATIVE_DESKTOP_INCOMING_CALL_UI.md for the measurement contract.
Item {
    id: presence

    required property var colors
    required property var invite
    required property string variant
    required property bool videoEnabled
    signal acceptRequested()
    signal declineRequested()
    signal toggleVideoRequested()
    // large.ui.tsx:43 closeOnBackdrop: the scrim click closes without
    // declining.
    signal dismissRequested()

    // Shell invite title (jitsi-call-shell.ui.tsx:37-40): trimmed caller
    // name, else the translated fallback; the avatar letter is its first
    // upper-case character, "?" only when even the fallback was empty.
    readonly property string inviteTitle: {
        const trimmed = String(invite.callerName || "").trim()
        return trimmed.length > 0 ? trimmed : qsTr("Participant")
    }
    readonly property string inviteAvatarLetter:
        inviteTitle.length > 0 ? inviteTitle.charAt(0).toUpperCase() : "?"
    readonly property string avatarUrl: String(invite.avatarUrl || "").trim()
    readonly property bool isLarge: variant !== "compact"

    // Large variant scrim + section (jitsi-call-incoming-large.ui.tsx:41-50).
    Rectangle {
        objectName: presence.isLarge ? "incomingCallScrim" : ""
        visible: presence.isLarge
        anchors.fill: parent
        // bg-black/55 over the full viewport.
        color: "#8c000000"
        MouseArea {
            anchors.fill: parent
            onClicked: presence.dismissRequested()
        }
    }
    Rectangle {
        id: largeCard
        objectName: presence.isLarge ? "incomingCallCard" : ""
        visible: presence.isLarge
        // 918x574 with the viewport max clamps, rounded-2xl, #000000.
        width: Math.min(918, parent.width - 32)
        height: Math.min(574, parent.height - 32)
        x: (parent.width - width) / 2
        y: (parent.height - height) / 2
        radius: 16
        color: "#000000"

        // Avatar block: 160 px, 200 px at the 640 px (sm) breakpoint,
        // bg-neutral-500, mt-6 sm:mt-6, gap-7 (large.ui.tsx:52-75).
        readonly property real avatarSize: width >= 640 ? 200 : 160
        Column {
            id: largeHeader
            anchors.top: parent.top
            anchors.topMargin: 40 // py-10 at sm
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 28 // gap-7
            Rectangle {
                objectName: "incomingCallAvatar"
                width: largeCard.avatarSize
                height: largeCard.avatarSize
                radius: width / 2
                color: "#737373" // bg-neutral-500
                NativeAvatarImage {
                    anchors.fill: parent
                    reference: presence.avatarUrl
                    loadActive: largeCard.visible && presence.avatarUrl.length > 0
                }
                Text {
                    anchors.centerIn: parent
                    visible: presence.avatarUrl.length === 0
                        && presence.inviteAvatarLetter !== "?"
                    text: presence.inviteAvatarLetter
                    color: "white"
                    font.pixelSize: largeCard.avatarSize >= 200 ? 72 : 60
                    font.weight: Font.DemiBold // text-6xl/7xl font-semibold
                }
                Image {
                    // Empty-title fallback: phone glyph 48 (large.ui.tsx:19).
                    objectName: "incomingCallAvatarPhone"
                    anchors.centerIn: parent
                    visible: presence.avatarUrl.length === 0
                        && presence.inviteAvatarLetter === "?"
                    source: "qrc:/workspace/icons/phone.svg"
                    sourceSize.width: 48
                    sourceSize.height: 48
                    layer.enabled: true
                    layer.effect: MultiEffect {
                        colorization: 1.0
                        colorizationColor: "white"
                    }
                }
            }
            Column {
                objectName: "incomingCallTextBlock"
                width: largeCard.width - 96 // px-12 at sm
                spacing: 8 // space-y-2
                Text {
                    objectName: "incomingCallTitleText"
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: presence.inviteTitle
                    color: "white"
                    font.pixelSize: 24 // text-2xl
                    font.weight: Font.DemiBold // font-semibold
                    elide: Text.ElideRight
                    maximumLineCount: 1
                }
                Text {
                    objectName: "incomingCallSubtitleText"
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("Choose how to answer the call")
                    color: "#99ffffff" // text-white/60
                    font.pixelSize: 18 // text-lg
                }
            }
        }

        // Figma button group 6176:36282: 514 x 52, gap 8, icon slot 40,
        // inner gap 6, padding 16 (large.ui.tsx:85-142). At the reference
        // card width the group is exactly 514 px (169 + 160 + 169 + 2 gaps);
        // narrower cards split the available width by those proportions so
        // the group never overflows the card (PR #257 finding 3).
        Row {
            id: largeButtons
            objectName: "incomingCallButtonRow"
            // The reference section is px-6 (48 px total) below the sm (640)
            // breakpoint and sm:px-12 above it (large.ui.tsx:48, 85); the row
            // inherits that inset, so a narrow card gives the padding back to
            // the buttons instead of squeezing them further.
            width: Math.min(514, parent.width - (parent.width >= 640 ? 96 : 48))
            height: 52
            spacing: 8
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 40 // py-10 at sm
            anchors.horizontalCenter: parent.horizontalCenter
            readonly property real baseWidthSum: 498 // 169 + 160 + 169
            readonly property real innerWidth: Math.max(0, width - spacing * 2)
            // Review finding 4561759278: once the row is narrower than the
            // reference's full-size 514 px group, proportional shrinking
            // alone leaves ~9 px of label per button and every label elides.
            // The reference keeps `truncate` there (large.ui.tsx:105,122,139)
            // because its dialog is designed for the 918 px card; the native
            // overlay must stay readable down to a 400 px window, so below
            // this threshold the icon slot drops 40->20, horizontal padding
            // 16->6, inner gap 6->4 and the label font 16->12. Worst case
            // (400 px window): buttons 103.1/97.6/103.1 px, labels
            // 103.1-36=67.1 / 97.6-36=61.6 / 67.1 px against measured advances
            // 59.7/37.3/37.6 px (QFontMetricsF, Sans 12 px) - every label
            // keeps >= 7.4 px of slack and paints without eliding. Deviation
            // documented in docs/NATIVE_DESKTOP_INCOMING_CALL_UI.md.
            readonly property bool narrowLayout: innerWidth < baseWidthSum
            readonly property real buttonPad: narrowLayout ? 6 : 16 // px-4
            readonly property real iconSlot: narrowLayout ? 20 : 40 // INCOMING_CALL_ICON_SLOT_PX
            readonly property real innerGap: narrowLayout ? 4 : 6 // gap-1.5
            readonly property int labelPixelSize: narrowLayout ? 12 : 16 // text-base

            // "With video" checkbox-role toggle.
            Rectangle {
                id: videoToggle
                objectName: "incomingCallVideoToggle"
                width: largeButtons.innerWidth * 169 / largeButtons.baseWidthSum
                height: 52
                radius: 8 // rounded-lg
                // On: bg-call-green + black text; off: outlined surface.
                color: presence.videoEnabled
                    ? presence.colors["call-green"] : presence.colors["bg"]
                border.width: presence.videoEnabled ? 0 : 1
                border.color: presence.colors["border-subtle"]
                Accessible.role: Accessible.CheckBox
                Accessible.name: qsTr("With video")
                Accessible.checked: presence.videoEnabled
                Text {
                    objectName: "incomingCallVideoToggleText"
                    text: qsTr("With video")
                    color: presence.videoEnabled ? "black" : presence.colors["text-muted"]
                    font.pixelSize: largeButtons.labelPixelSize // text-base
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: largeButtons.buttonPad
                        + largeButtons.iconSlot + largeButtons.innerGap // pad + slot + gap
                    elide: Text.ElideRight
                    width: Math.max(0, parent.width - 2 * largeButtons.buttonPad
                        - largeButtons.iconSlot - largeButtons.innerGap)
                }
                Image {
                    // The video icon renders at the full icon slot inside the
                    // button, matching the Cancel/Accept icon slots
                    // (large.ui.tsx:99-103; PR #257 finding 4).
                    objectName: "incomingCallVideoIcon"
                    source: "qrc:/workspace/icons/call_incoming_video_toggle.svg"
                    sourceSize.width: largeButtons.iconSlot
                    sourceSize.height: largeButtons.iconSlot
                    width: largeButtons.iconSlot
                    height: largeButtons.iconSlot
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: largeButtons.buttonPad
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: presence.toggleVideoRequested()
                }
            }

            // "Cancel": bg-call-red, close glyph 20 in a 40 px slot.
            Rectangle {
                id: largeDecline
                objectName: "incomingCallDeclineButton"
                property string text: qsTr("Cancel")
                width: largeButtons.innerWidth * 160 / largeButtons.baseWidthSum
                height: 52
                radius: 8
                color: presence.colors["call-red"] // #e43535 light
                Text {
                    objectName: "incomingCallDeclineText"
                    text: qsTr("Cancel")
                    color: "white"
                    font.pixelSize: largeButtons.labelPixelSize
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: largeButtons.buttonPad
                        + largeButtons.iconSlot + largeButtons.innerGap // pad + slot + gap
                    elide: Text.ElideRight
                    width: Math.max(0, parent.width - 2 * largeButtons.buttonPad
                        - largeButtons.iconSlot - largeButtons.innerGap)
                }
                Item {
                    // 40 px icon slot (INCOMING_CALL_ICON_SLOT_PX, :13);
                    // shrinks to 20 in the narrow layout above.
                    objectName: "incomingCallDeclineIconSlot"
                    width: largeButtons.iconSlot
                    height: largeButtons.iconSlot
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: largeButtons.buttonPad // px-4
                    Image {
                        objectName: "incomingCallCloseGlyph"
                        anchors.centerIn: parent
                        source: "qrc:/workspace/icons/close.svg"
                        sourceSize.width: Math.min(20, largeButtons.iconSlot) // INCOMING_CALL_CLOSE_GLYPH_PX
                        sourceSize.height: Math.min(20, largeButtons.iconSlot)
                        layer.enabled: true
                        layer.effect: MultiEffect {
                            colorization: 1.0
                            colorizationColor: "white"
                        }
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: presence.declineRequested()
                }
            }

            // "Accept": bg-call-green, phone glyph 27 in a 40 px slot.
            Rectangle {
                id: largeAccept
                objectName: "incomingCallAcceptButton"
                property string text: qsTr("Accept")
                width: largeButtons.innerWidth * 169 / largeButtons.baseWidthSum
                height: 52
                radius: 8
                color: presence.colors["call-green"] // #26c038
                Text {
                    objectName: "incomingCallAcceptText"
                    text: qsTr("Accept")
                    color: "black"
                    font.pixelSize: largeButtons.labelPixelSize
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: largeButtons.buttonPad
                        + largeButtons.iconSlot + largeButtons.innerGap // pad + slot + gap
                    elide: Text.ElideRight
                    width: Math.max(0, parent.width - 2 * largeButtons.buttonPad
                        - largeButtons.iconSlot - largeButtons.innerGap)
                }
                Item {
                    objectName: "incomingCallAcceptIconSlot"
                    width: largeButtons.iconSlot
                    height: largeButtons.iconSlot
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: largeButtons.buttonPad
                    Image {
                        objectName: "incomingCallPhoneGlyph"
                        anchors.centerIn: parent
                        source: "qrc:/workspace/icons/phone.svg"
                        sourceSize.width: Math.min(27, largeButtons.iconSlot) // INCOMING_CALL_PHONE_GLYPH_PX
                        sourceSize.height: Math.min(27, largeButtons.iconSlot)
                        layer.enabled: true
                        layer.effect: MultiEffect {
                            colorization: 1.0
                            colorizationColor: "black"
                        }
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: presence.acceptRequested()
                }
            }
        }
    }

    // Compact toast (jitsi-call-incoming-compact.ui.tsx:25-73): fixed
    // right-4 top-20, w-320, rounded-xl, bg-bg-elevated, p-3.
    Rectangle {
        id: compactToast
        objectName: presence.isLarge ? "" : "incomingCallToast"
        visible: !presence.isLarge
        width: Math.min(320, parent.width - 32)
        height: compactContent.height + 24 // p-3
        x: parent.width - width - 16 // right-4
        y: 80 // top-20
        radius: 12 // rounded-xl
        color: presence.colors["bg-elevated"]
        border.width: 1
        border.color: presence.colors["border-subtle"]
        // shadow-xl (compact.ui.tsx:26); same MultiEffect shadow pattern as
        // the create-chat dialog (Main.qml:6899-6907), software-render safe.
        readonly property bool shadowsSupported: GraphicsInfo.api
            !== GraphicsInfo.Software && GraphicsInfo.api !== GraphicsInfo.OpenVG
            && GraphicsInfo.api !== GraphicsInfo.Unknown && GraphicsInfo.api !== GraphicsInfo.Null
        layer.enabled: compactToast.visible && shadowsSupported
        layer.effect: MultiEffect {
            shadowEnabled: true
            shadowColor: "black"
            shadowOpacity: 0.16
            shadowVerticalOffset: 12
            shadowScale: 0.99
            shadowBlur: 1
        }

        Column {
            id: compactContent
            x: 12 // p-3
            y: 12
            width: parent.width - 24
            spacing: 8 // mb-2 between the header row and the button row

            Row {
                objectName: "incomingCallToastHeader"
                width: parent.width
                spacing: 8 // gap-2
                // Avatar size sm = 32 px (shared/ui/avatar/avatar.tsx:8).
                Rectangle {
                    objectName: "incomingCallToastAvatar"
                    width: 32
                    height: 32
                    radius: 16
                    color: presence.avatarUrl.length > 0
                        ? presence.colors["bg-elevated"]
                        : Qt.alpha(presence.colors["call-green"], 0.15)
                    NativeAvatarImage {
                        anchors.fill: parent
                        reference: presence.avatarUrl
                        loadActive: compactToast.visible
                            && presence.avatarUrl.length > 0
                    }
                    Image {
                        objectName: "incomingCallToastPhoneGlyph"
                        anchors.centerIn: parent
                        visible: presence.avatarUrl.length === 0
                        source: "qrc:/workspace/icons/phone.svg"
                        sourceSize.width: 16
                        sourceSize.height: 16
                        layer.enabled: true
                        layer.effect: MultiEffect {
                            colorization: 1.0
                            colorizationColor: presence.colors["call-green"]
                        }
                    }
                }
                Column {
                    objectName: "incomingCallToastColumn"
                    width: parent.width - 32 - 8
                    spacing: 0
                    Text {
                        objectName: "incomingCallToastTitleText"
                        text: qsTr("Incoming call")
                        color: presence.colors["text-primary"]
                        font.pixelSize: 14 // text-sm
                        font.weight: Font.DemiBold // font-semibold
                        elide: Text.ElideRight
                        width: parent.width
                    }
                    Text {
                        objectName: "incomingCallToastSubtitleText"
                        text: qsTr("%1 is calling").arg(presence.inviteTitle)
                        color: presence.colors["text-muted"]
                        font.pixelSize: 12 // text-xs
                        elide: Text.ElideRight
                        width: parent.width
                    }
                }
            }

            Row {
                objectName: "incomingCallToastButtons"
                anchors.right: parent.right
                spacing: 8 // gap-2
                // ghost sm Decline: transparent, text-text-muted, h-8 px-3.
                Rectangle {
                    id: compactDecline
                    objectName: "incomingCallDeclineButton"
                    property string text: qsTr("Decline")
                    width: compactDeclineText.width + 24 // px-3
                    height: 32 // button.tsx:9 h-8
                    radius: 6
                    color: "transparent"
                    Text {
                        id: compactDeclineText
                        objectName: "incomingCallDeclineText"
                        anchors.centerIn: parent
                        text: qsTr("Decline")
                        color: presence.colors["text-muted"]
                        font.pixelSize: 12 // text-xs
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: presence.declineRequested()
                    }
                }
                // filled sm Accept: bg-accent, text-on-accent, font-semibold.
                Rectangle {
                    id: compactAccept
                    objectName: "incomingCallAcceptButton"
                    property string text: qsTr("Accept")
                    width: compactAcceptText.width + 24
                    height: 32
                    radius: 6
                    color: presence.colors["accent"]
                    Text {
                        id: compactAcceptText
                        objectName: "incomingCallAcceptText"
                        anchors.centerIn: parent
                        text: qsTr("Accept")
                        color: presence.colors["on-accent"]
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: presence.acceptRequested()
                    }
                }
            }
        }
    }
}
