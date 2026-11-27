pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Calendar event detail card (issue #907, Part of #882,
// docs/NATIVE_DESKTOP_CALENDAR.md "Event detail"). Opened by clicking an
// event row in the day agenda or a week chip; read-only in the UI phase.
// The reference app ships no functional calendar screen (embed placeholder),
// so metrics are pending-Figma and follow native token conventions; the
// close/footer chrome follows the existing native dialog pattern.
//
// Time-zone rule (issue contract): the stored instants are UTC
// (startUtc/endUtc); the card renders them converted to the client zone via
// the JS Date pipeline (new Date parses the ISO instant, Qt.format* prints in
// the local zone), so DST boundaries come out right with no offset math.
Rectangle {
    id: detail
    objectName: "calendarEventDetail"
    radius: 12
    color: detail.colors["bg-elevated"]
    border.color: detail.colors["border-subtle"]
    border.width: 1
    implicitWidth: Math.min(420, Math.max(320, implicitChildrenWidth))
    implicitHeight: contentColumn.implicitHeight + 32

    required property var colors
    property var event: ({})
    signal closed()

    // Fallback width when the rows are all short: keep a readable card.
    readonly property real implicitChildrenWidth: 320

    function isAllDay(ev) {
        return ev.allDay === true
    }
    function instantText(ev) {
        if (isAllDay(ev))
            return Qt.formatDate(new Date(String(ev.day) + "T00:00:00"), "ddd, dd.MM.yyyy")
        const rawStart = String(ev.startUtc || "")
        if (rawStart.length === 0)
            return Qt.formatDate(new Date(String(ev.day) + "T00:00:00"), "ddd, dd.MM.yyyy")
                   + " " + String(ev.start) + " - " + String(ev.end)
        const start = new Date(rawStart)
        const text = Qt.formatDate(start, "ddd, dd.MM.yyyy") + " "
                   + Qt.formatTime(start, "HH:mm")
        const rawEnd = String(ev.endUtc || "")
        if (rawEnd.length === 0)
            return text
        const end = new Date(rawEnd)
        if (Qt.formatDate(end, "yyyy-MM-dd") === Qt.formatDate(start, "yyyy-MM-dd"))
            return text + " - " + Qt.formatTime(end, "HH:mm")
        return text + " - " + Qt.formatDate(end, "dd.MM HH:mm")
    }

    ColumnLayout {
        id: contentColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 16
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label {
                objectName: "calendarDetailTitle"
                // #879 lesson: a fill child beside a sibling needs the zero
                // minimum or its implicit width wins and elide never engages.
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: String(detail.event.title || "") .length > 0
                      ? String(detail.event.title) : qsTr("(untitled)")
                color: detail.colors["text-primary"]
                font.pixelSize: 16
                font.bold: true
                elide: Text.ElideEnd
            }
            Button {
                objectName: "calendarDetailClose"
                text: "\u00d7"
                flat: true
                onClicked: detail.closed()
            }
        }

        Label {
            objectName: "calendarDetailTime"
            Layout.fillWidth: true
            text: detail.instantText(detail.event)
            color: detail.colors["text-muted"]
        }

        // Location row: the contract asks for a graceful empty state.
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Label {
                objectName: "calendarDetailLocationLabel"
                text: qsTr("Location")
                color: detail.colors["text-muted"]
                font.pixelSize: 12
            }
            Label {
                objectName: "calendarDetailLocation"
                Layout.fillWidth: true
                text: String(detail.event.location || "").length > 0
                      ? String(detail.event.location) : qsTr("No location specified")
                color: String(detail.event.location || "").length > 0
                       ? detail.colors["text-primary"] : detail.colors["text-muted"]
                wrapMode: Text.WordWrap
            }
        }

        // Participants: first three names, the rest collapse to "+N more".
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Label {
                objectName: "calendarDetailParticipantsHeader"
                text: qsTr("Participants")
                color: detail.colors["text-muted"]
                font.pixelSize: 12
            }
            Repeater {
                objectName: "calendarDetailParticipantRows"
                model: {
                    const people = detail.event.participants || []
                    return people.slice(0, 3)
                }
                delegate: Label {
                    required property var modelData
                    required property int index
                    objectName: "calendarDetailParticipant-" + index
                    Layout.fillWidth: true
                    text: String(modelData)
                    color: detail.colors["text-primary"]
                    elide: Text.ElideEnd
                }
            }
            Label {
                objectName: "calendarDetailParticipantsOverflow"
                visible: (detail.event.participants || []).length > 3
                text: qsTr("+%1 more").arg((detail.event.participants || []).length - 3)
                color: detail.colors["text-muted"]
            }
        }
    }
}
