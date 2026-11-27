pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

ScrollView {
    id: reader
    required property var colors
    property var message: null
    clip: true
    contentWidth: availableWidth
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
    onMessageChanged: if (contentItem) contentItem.contentY = 0

    Column {
        width: reader.availableWidth
        spacing: 8

        Text {
            objectName: "mailReaderTitle"
            width: reader.availableWidth
            text: reader.message ? reader.message.subject : qsTr("Select a message")
            color: reader.colors["text-primary"]
            font.pixelSize: 18
            font.weight: Font.DemiBold
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
        }
        ReaderLine {
            objectName: "mailReaderFrom"
            text: qsTr("From: %1").arg(reader.message ? reader.message.sender : "")
        }
        ReaderLine {
            objectName: "mailReaderTo"
            visible: reader.message !== null && reader.message.recipients.length > 0
            text: qsTr("To: %1").arg(reader.message ? reader.message.recipients.join(", ") : "")
        }
        ReaderLine {
            objectName: "mailReaderDate"
            text: qsTr("Date: %1").arg(reader.message ? reader.message.date || reader.message.time : "")
        }
        Rectangle {
            visible: reader.message !== null
            width: reader.availableWidth
            implicitHeight: 1
            color: reader.colors["border-subtle"]
        }
        ReaderLine {
            objectName: "mailReaderBody"
            text: reader.message ? (reader.message.body ?? qsTr("Message body unavailable")) : ""
            font.pixelSize: 14
            color: reader.colors["text-primary"]
        }
        Flow {
            visible: reader.message !== null
            width: reader.availableWidth
            spacing: 4
            Repeater {
                model: [
                    { key: "reply", label: qsTr("Reply") },
                    { key: "forward", label: qsTr("Forward") },
                    { key: "mark", label: reader.message && reader.message.unread ? qsTr("Mark as read") : qsTr("Mark as unread") },
                    { key: "delete", label: qsTr("Delete") }
                ]
                delegate: NativeButton {
                    required property var modelData
                    objectName: "mailReaderAction-" + modelData.key
                    width: Math.min(implicitWidth, reader.availableWidth)
                    enabled: false
                    colors: reader.colors
                    text: modelData.label
                    Accessible.description: actionNotice.text
                }
            }
        }
        ReaderLine {
            id: actionNotice
            objectName: "mailReaderActionNotice"
            text: qsTr("Mail actions are unavailable in this preview.")
        }
    }

    component ReaderLine: Text {
        visible: reader.message !== null
        width: reader.availableWidth
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: reader.colors["text-muted"]
        font.pixelSize: 13
    }
}
