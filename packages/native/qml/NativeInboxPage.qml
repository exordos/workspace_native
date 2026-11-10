pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Effects

// Inbox page (issue #44). Presentation only: the controller owns grouping,
// counters, topic hydration and row navigation.
// Geometry measured from workspace_ui 61dacef78dab2f5d0be76f283bebf49fe29b84af
// inbox-page.ui.tsx: state cards m-3 rounded-xl border border-border-subtle
// bg-bg-elevated/50 px-4 py-3 text-sm (inbox-page.ui.tsx:25-26, 196-207);
// scroll container px-3 pb-3 pt-2 (inbox-page.ui.tsx:246); section heads
// text-[11px] font-semibold uppercase tracking-[0.08em] text-muted plus a
// flex-1 h-px border-border-subtle/60 hairline with gap-2 (ui.tsx:209-232);
// stream card rounded-xl bg-bg-elevated/45 border-border-subtle p-2 with a
// 28 px icon circle (16 px icon) head and rows gap-1.5 p-2.5 gap-2 with a
// 32 px circle (18 px icon), hover border-accent-soft/40 bg-bg-card
// (ui.tsx:126-188); badge h-5 min-w-5 rounded-full px-1 text-[11px]
// font-medium bg-badge-bg text-badge-text border-border-subtle (ui.tsx:119-123).
FocusScope {
    id: page
    objectName: "nativeInboxPage"
    required property var controller
    required property var colors
    readonly property var inboxState: controller.inboxPage
    readonly property var directCards: inboxState.direct || []
    readonly property var channelCards: inboxState.channels || []
    readonly property bool loading: Boolean(inboxState.loading)
    readonly property bool error: Boolean(inboxState.error)
    readonly property bool hasGroups: directCards.length + channelCards.length > 0
    onVisibleChanged: { if (visible) forceActiveFocus(); }
    Keys.onEscapePressed: page.controller.closeInbox()

    // Tinted static glyph (SVG recolored via the codebase's MultiEffect
    // pattern, Main.qml:3882).
    component InboxGlyph: Item {
        required property string asset
        required property int glyph
        required property color tint
        implicitWidth: glyph
        implicitHeight: glyph
        Image {
            id: glyphImage
            anchors.fill: parent
            source: "qrc:/workspace/icons/" + parent.asset
            sourceSize.width: parent.glyph
            sourceSize.height: parent.glyph
            asynchronous: false
            layer.enabled: true
            layer.effect: MultiEffect {
                colorizationColor: glyphImage.parent.tint
                colorization: 1.0
            }
        }
    }

    // Unread badge (ui.tsx:119-123).
    component InboxBadge: Rectangle {
        id: badge
        required property string text
        required property string name
        objectName: name
        implicitWidth: Math.max(20, badgeLabel.implicitWidth + 8)
        implicitHeight: 20
        color: page.colors["badge-bg"]
        border.width: 1
        border.color: page.colors["border-subtle"]
        radius: 10
        Text {
            id: badgeLabel
            anchors.centerIn: parent
            text: badge.text
            color: page.colors["badge-text"]
            font.pixelSize: 11
            font.weight: Font.Medium
        }
    }

    // Icon circle on bg-bg (ui.tsx:136-143, 156-163).
    component InboxIconCircle: Rectangle {
        required property int diameter
        required property int glyph
        required property string asset
        width: diameter
        height: diameter
        radius: diameter / 2
        color: page.colors["bg"]
        InboxGlyph {
            anchors.centerIn: parent
            asset: parent.asset
            glyph: parent.glyph
            tint: page.colors["text-muted"]
        }
    }

    // Loading/error/empty state card.
    component InboxStateCard: Rectangle {
        id: stateCard
        required property string body
        required property string bodyColor
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 12
        radius: 12
        border.width: 1
        border.color: page.colors["border-subtle"]
        color: Qt.alpha(page.colors["bg-elevated"], 0.5)
        implicitHeight: stateLabel.implicitHeight + 24
        Text {
            id: stateLabel
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.topMargin: 12
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            text: stateCard.body
            color: stateCard.bodyColor
            font.pixelSize: 14
            wrapMode: Text.WordWrap
        }
    }

    // Section heading: uppercase label + hairline (ui.tsx:209-232).
    component InboxSectionHead: Item {
        id: headRoot
        required property string title
        required property string name
        objectName: name
        implicitHeight: 24
        Row {
            anchors.fill: parent
            anchors.leftMargin: 4
            anchors.rightMargin: 4
            anchors.topMargin: 4
            anchors.bottomMargin: 4
            spacing: 8
            Text {
                id: headLabel
                objectName: name + "Label"
                anchors.verticalCenter: parent.verticalCenter
                text: headRoot.title
                color: page.colors["text-muted"]
                font.pixelSize: 11
                font.weight: Font.DemiBold
                font.capitalization: Font.AllUppercase
                font.letterSpacing: 0.88 // tracking-[0.08em]
            }
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - headLabel.implicitWidth - 8
                height: 1
                color: Qt.alpha(page.colors["border-subtle"], 0.6)
            }
        }
    }

    // One stream/topic card with its unread topic rows (ui.tsx:126-188).
    component InboxStreamCard: Rectangle {
        id: card
        required property var modelData
        objectName: "inbox-card-" + String(modelData.id)
        width: parent ? parent.width : 0
        radius: 12
        border.width: 1
        border.color: page.colors["border-subtle"]
        color: Qt.alpha(page.colors["bg-elevated"], 0.45)
        implicitHeight: cardColumn.implicitHeight + 16 // p-2
        Column {
            id: cardColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 8
            spacing: 6 // head mb-1.5
            Item {
                width: parent.width
                height: 36 // py-1 around the 28 px circle
                Row {
                    anchors.fill: parent
                    anchors.leftMargin: 4
                    anchors.rightMargin: 4
                    anchors.topMargin: 4
                    anchors.bottomMargin: 4
                    spacing: 8
                    InboxIconCircle {
                        anchors.verticalCenter: parent.verticalCenter
                        diameter: 28
                        glyph: 16
                        asset: Boolean(card.modelData.direct) ? "profile.svg" : "channels.svg"
                    }
                    Text {
                        objectName: "inbox-card-title-" + String(card.modelData.id)
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 28 - 8 - cardBadge.implicitWidth - 8
                        text: String(card.modelData.title)
                        color: page.colors["text-primary"]
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                    InboxBadge {
                        id: cardBadge
                        anchors.verticalCenter: parent.verticalCenter
                        text: String(card.modelData.badge)
                        name: "inbox-card-badge-" + String(card.modelData.id)
                    }
                }
            }
            Repeater {
                model: card.modelData.rows
                delegate: Rectangle {
                    id: row
                    required property var modelData
                    objectName: "inbox-row-" + String(modelData.id)
                    width: parent ? parent.width : 0
                    radius: 12
                    border.width: 1
                    border.color: rowArea.containsMouse
                        ? Qt.alpha(page.colors["accent-soft"], 0.4) : page.colors["border-subtle"]
                    color: rowArea.containsMouse ? page.colors["card-bg"]
                                                 : Qt.alpha(page.colors["bg-elevated"], 0.5)
                    implicitHeight: rowContent.implicitHeight + 20 // p-2.5
                    Behavior on color { ColorAnimation { duration: 100 } }
                    Behavior on border.color { ColorAnimation { duration: 100 } }
                    Row {
                        id: rowContent
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 10
                        spacing: 8
                        InboxIconCircle {
                            anchors.verticalCenter: parent.verticalCenter
                            diameter: 32
                            glyph: 18
                            asset: Boolean(row.modelData.direct) ? "profile.svg" : "channels.svg"
                        }
                        Column {
                            anchors.verticalCenter: parent.verticalCenter
                            width: rowContent.width - 32 - 8 - rowBadge.implicitWidth - 8
                            spacing: 2 // time mt-0.5
                            Text {
                                objectName: "inbox-row-title-" + String(row.modelData.id)
                                width: parent.width
                                text: String(row.modelData.title)
                                color: page.colors["text-primary"]
                                font.pixelSize: 14
                                font.weight: Font.Medium
                                elide: Text.ElideRight
                            }
                            Text {
                                objectName: "inbox-row-time-" + String(row.modelData.id)
                                visible: String(row.modelData.time).length > 0
                                text: String(row.modelData.time)
                                color: page.colors["text-muted"]
                                font.pixelSize: 11
                            }
                        }
                        InboxBadge {
                            id: rowBadge
                            anchors.verticalCenter: parent.verticalCenter
                            text: String(row.modelData.badge)
                            name: "inbox-row-badge-" + String(row.modelData.id)
                        }
                    }
                    MouseArea {
                        id: rowArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: page.controller.openInboxRow(
                            String(row.modelData.stream), String(row.modelData.topic))
                    }
                }
            }
        }
    }

    Rectangle {
        id: header
        objectName: "inboxHeader"
        anchors.top: parent.top
        width: parent.width
        height: 54
        radius: 6
        color: page.colors["bg-elevated"]
        Text {
            anchors.left: parent.left
            anchors.leftMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Inbox")
            color: page.colors["text-primary"]
            font.pixelSize: 14
            font.weight: Font.DemiBold
        }
        NativeIconButton {
            objectName: "inboxCloseButton"
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            colors: page.colors
            asset: "close.svg"
            glyphSize: 16
            help: qsTr("Close inbox")
            onClicked: page.controller.closeInbox()
        }
    }

    InboxStateCard {
        objectName: "inboxLoadingText"
        visible: page.visible && page.loading && !page.hasGroups
        anchors.top: header.bottom
        body: qsTr("Loading…")
        bodyColor: page.colors["text-muted"]
    }

    InboxStateCard {
        objectName: "inboxErrorBox"
        visible: page.visible && page.error && !page.hasGroups
        anchors.top: header.bottom
        body: qsTr("Failed to load inbox")
        bodyColor: page.colors["notice-base"]
    }

    InboxStateCard {
        objectName: "inboxEmptyText"
        visible: page.visible && !page.loading && !page.error && !page.hasGroups
        anchors.top: header.bottom
        body: qsTr("No unread messages")
        bodyColor: page.colors["text-muted"]
    }

    Flickable {
        id: inboxScroll
        objectName: "inboxScroll"
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        visible: page.hasGroups
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        contentHeight: inboxColumn.y + inboxColumn.implicitHeight + 12
        Column {
            id: inboxColumn
            x: 12 // px-3
            y: 8 // pt-2
            width: inboxScroll.width - 24
            spacing: 0

            Column {
                visible: page.directCards.length > 0
                width: parent.width
                spacing: 6 // space-y-1.5
                InboxSectionHead {
                    width: parent.width
                    title: qsTr("Direct messages")
                    name: "inboxSectionTitleDirect"
                }
                Repeater {
                    model: page.directCards
                    delegate: InboxStreamCard { }
                }
            }

            Column {
                visible: page.channelCards.length > 0
                width: parent.width
                topPadding: page.directCards.length > 0 ? 16 : 0 // mt-4
                spacing: 8 // space-y-2
                InboxSectionHead {
                    width: parent.width
                    title: qsTr("Channels")
                    name: "inboxSectionTitleChannels"
                }
                Repeater {
                    model: page.channelCards
                    delegate: InboxStreamCard { }
                }
            }
        }
    }
}
