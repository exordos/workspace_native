pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects

// Unavailable-state section page (issue #35, docs/NATIVE_DESKTOP_MAIL_CALENDAR.md).
// Geometry measured from workspace_ui 61dacef78dab2f5d0be76f283bebf49fe29b84af
// mail-page.ui.tsx fallback branch: centered flex column gap-4 (16 px) with
// p-6 (24 px) padding, Icon "mail" size 64 at opacity 0.5 inheriting
// text-text-muted, h2 text-xl font-medium text-text-primary, body max-w-lg
// (512 px) text-center text-sm. The packaged desktop (Electron loads the app
// over file:// so the embed URL never resolves, main.ts:452) always renders
// this card, which is the state QtWebEngine is never needed for. Heading and
// body travel in as i18n strings so the same card serves the Calendar section.
FocusScope {
    id: page
    required property var colors
    property string asset: ""
    property string heading: ""
    property string body: ""
    onVisibleChanged: { if (visible) forceActiveFocus(); }

    Rectangle {
        anchors.fill: parent
        color: page.colors["bg"]
    }
    // p-6 (24 px) padding box; justify-center items-center centers the card.
    Item {
        objectName: "sectionUnavailablePadding"
        anchors.fill: parent
        anchors.margins: 24
        Column {
            id: column
            objectName: "sectionUnavailableColumn"
            anchors.centerIn: parent
            width: Math.min(512, parent.width)
            spacing: 16
            // Icon name="mail" size={64} className="opacity-50", tinted with
            // the container's text-text-muted (SVG recolor via MultiEffect,
            // the codebase pattern from NativeInboxPage.qml).
            Item {
                objectName: "sectionUnavailableGlyph"
                x: (column.width - width) / 2
                width: 64
                height: 64
                opacity: 0.5
                readonly property color tint: page.colors["text-muted"]
                Image {
                    anchors.fill: parent
                    source: page.asset.length ? "qrc:/workspace/icons/" + page.asset : ""
                    sourceSize.width: 64
                    sourceSize.height: 64
                    asynchronous: false
                    fillMode: Image.PreserveAspectFit
                    layer.enabled: true
                    layer.effect: MultiEffect {
                        colorizationColor: page.colors["text-muted"]
                        colorization: 1.0
                    }
                }
            }
            // h2 text-xl font-medium text-text-primary (key nav.mail).
            Text {
                objectName: "sectionUnavailableHeading"
                width: column.width
                horizontalAlignment: Text.AlignHCenter
                text: page.heading
                textFormat: Text.PlainText
                font.pixelSize: 20
                font.weight: Font.Medium
                color: page.colors["text-primary"]
            }
            // p max-w-lg text-center text-sm (key app.webModeUnavailable).
            Text {
                objectName: "sectionUnavailableBody"
                width: Math.min(512, column.width)
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: page.body
                textFormat: Text.PlainText
                font.pixelSize: 14
                color: page.colors["text-muted"]
            }
        }
    }
}
