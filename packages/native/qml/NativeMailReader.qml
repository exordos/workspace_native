pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

ScrollView {
    id: reader
    required property var colors
    property var message: null
    required property var mail
    readonly property bool live: !!message && !!mail?.live
    readonly property bool canAct: live && !mail.actionBusy && mail.state === "ready"
    readonly property bool inTrash: live && mail.folders.some(row => row.id === mail.folderId && row.kind === "trash")
    readonly property bool hasTrash: live && mail.folders.some(row => row.account === message.account && row.kind === "trash")
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
        Repeater {
            model: reader.message?.attachments ?? []
            delegate: ReaderLine {
                required property string modelData
                required property int index
                objectName: "mailReaderAttachment-" + index
                text: qsTr("Attachment: %1").arg(modelData)
            }
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
                    { key: "delete", label: reader.live ? (reader.inTrash ? qsTr("Restore") : qsTr("Move to trash")) : qsTr("Delete") },
                    { key: "star", label: reader.message?.starred ? qsTr("Unstar") : qsTr("Star") }
                ]
                delegate: NativeButton {
                    required property var modelData
                    objectName: "mailReaderAction-" + modelData.key
                    width: Math.min(implicitWidth, reader.availableWidth)
                    visible: modelData.key !== "star" || reader.live
                    enabled: reader.canAct && (modelData.key === "mark" || modelData.key === "star"
                        || (modelData.key === "delete" && (reader.inTrash || (reader.hasTrash && !reader.message.queued))))
                    onClicked: {
                        const action = modelData.key === "mark" ? (reader.message.unread ? "read" : "unread")
                            : modelData.key === "star" ? (reader.message.starred ? "unstar" : "star")
                            : reader.inTrash ? "restore" : "trash"
                        reader.mail.applyMessageAction(action)
                    }
                    colors: reader.colors
                    text: modelData.label
                    Accessible.description: actionNotice.text
                }
            }
        }
        NativeSelect {
            objectName: "mailMoveFolder"
            visible: reader.live && count > 0
            enabled: reader.canAct
            width: Math.min(implicitWidth, reader.availableWidth)
            colors: reader.colors
            model: reader.live ? reader.mail.moveTargets : []
            currentIndex: -1
            placeholderText: qsTr("Move to folder")
            onActivated: index => {
                reader.mail.applyMessageAction("move", valueAt(index))
                currentIndex = -1
            }
        }
        ReaderLine {
            objectName: "mailReaderActionError"
            visible: !!reader.mail?.actionError
            text: reader.mail?.actionError ?? ""
        }
        ReaderLine {
            id: actionNotice
            objectName: "mailReaderActionNotice"
            text: reader.live ? qsTr("Reply and forward are unavailable.") : qsTr("Mail actions are unavailable in this preview.")
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
