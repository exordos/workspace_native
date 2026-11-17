pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects

// Recent-calls section page (issue #37, docs/NATIVE_DESKTOP_CALLS_RECENT_EMPTY.md).
// Presentation only: the empty state is the only state the reference ships
// (workspace_ui 61dacef78dab2f5d0be76f283bebf49fe29b84af pages/calls/
// calls-page.ui.tsx), and it issues no history request on mount
// (calls-page.test.tsx). Geometry measured from that source: page root
// "flex max-h-full min-h-0 w-full min-w-0 flex-1 flex-col overflow-hidden"
// (full width, no max-w-narrow-page cap, asserted by calls-page.test.tsx:
// 22-36); header row "flex items-center gap-2 px-4 pb-2 pt-3" with Icon
// "phone" size 18 in text-text-muted and h2 "text-sm font-semibold
// text-text-primary" (i18n call.recentCalls); state card
// "m-3 rounded-xl border border-border-subtle bg-bg-elevated/50 px-4 py-3
// text-sm" in text-text-muted (i18n call.noRecentCalls). The reference
// top-bar Calls chip sits behind the TOP_BAR_CALLS_NAV build gate
// (top-bar.lib.ts:89-91); the native build settings do not introduce that
// gate, so no top-bar entry is rendered and the section state reached
// through the whitelisted /calls deep link is the entry surface.
FocusScope {
    id: page
    objectName: "nativeCallsPage"
    required property var colors
    onVisibleChanged: { if (visible) forceActiveFocus(); }

    Rectangle {
        anchors.fill: parent
        color: page.colors["bg"]
    }
    // Header row: flex items-center gap-2 px-4 pb-2 pt-3. The row carries
    // the 16/12 top-left origin and the 8 px gap; the 8 px bottom padding
    // shows up as the extra card top margin asserted by the window test.
    // Row children position their y manually (no anchors across Row).
    Row {
        id: header
        objectName: "callsHeaderRow"
        x: 16 // px-4
        y: 12 // pt-3
        width: page.width - 32 // px-4 on both sides
        spacing: 8 // gap-2
        // Icon name="phone" size={18} className="text-text-muted" (SVG
        // recolor via MultiEffect, the NativeSectionUnavailablePage pattern).
        Item {
            id: phoneGlyph
            objectName: "callsHeaderGlyph"
            width: 18
            height: 18
            y: (header.height - height) / 2 // items-center
            readonly property string asset: "phone.svg"
            readonly property color tint: page.colors["text-muted"]
            Image {
                anchors.fill: parent
                source: "qrc:/workspace/icons/" + phoneGlyph.asset
                sourceSize.width: 18
                sourceSize.height: 18
                asynchronous: false
                fillMode: Image.PreserveAspectFit
                layer.enabled: true
                layer.effect: MultiEffect {
                    colorizationColor: phoneGlyph.tint
                    colorization: 1.0
                }
            }
        }
        // h2 text-sm font-semibold text-text-primary (i18n call.recentCalls).
        Text {
            id: heading
            objectName: "callsHeading"
            text: qsTr("Recent Jitsi calls")
            textFormat: Text.PlainText
            font.pixelSize: 14
            font.weight: Font.DemiBold
            color: page.colors["text-primary"]
            y: (header.height - height) / 2 // items-center
        }
    }
    // State card: m-3 rounded-xl border border-border-subtle
    // bg-bg-elevated/50 px-4 py-3 text-sm, the same convention as the Feed
    // state cards (NativeFeedPage.qml FeedStateCard): the content anchors
    // 16/12 inside the border box. The 20 px top offset is the header's
    // pb-2 (8) plus the card's m-3 (12).
    Rectangle {
        id: card
        objectName: "callsEmptyCard"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 12 // m-3
        anchors.rightMargin: 12
        anchors.top: header.bottom
        anchors.topMargin: 20
        radius: 12 // rounded-xl
        border.width: 1
        border.color: page.colors["border-subtle"]
        color: Qt.alpha(page.colors["bg-elevated"], 0.5)
        implicitHeight: emptyText.implicitHeight + 24 // py-3
        height: implicitHeight
        Text {
            id: emptyText
            objectName: "callsEmptyText"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.topMargin: 12 // py-3
            anchors.leftMargin: 16 // px-4
            anchors.rightMargin: 16
            text: qsTr("No recent Jitsi calls yet")
            textFormat: Text.PlainText
            font.pixelSize: 14 // text-sm
            color: page.colors["text-muted"]
            wrapMode: Text.WordWrap
        }
    }
}
