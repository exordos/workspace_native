pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs as FileDialogs

Dialog {
    id: dialog
    objectName: "mailComposeDialog"
    required property var colors
    required property var mail
    property var attachments: []
    property bool recipientsVisited: false
    readonly property bool hasRecipient: /[^,;\s]/.test(recipients.text)

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(760, parent ? parent.width - 32 : 760)
    height: Math.min(686, parent ? parent.height - 32 : 686)
    padding: 20
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    onOpened: recipients.forceActiveFocus()
    onAboutToHide: chooser.close()

    function addAttachments(urls) {
        if (!opened)
            return
        attachments = attachments.concat(mail.describeAttachments(urls))
    }
    FileDialogs.FileDialog {
        id: chooser
        fileMode: FileDialogs.FileDialog.OpenFiles
        onAccepted: dialog.addAttachments(selectedFiles)
    }

    background: Rectangle {
        radius: 12
        color: dialog.colors["bg-elevated"]
        border.color: dialog.colors["border-subtle"]
    }

    contentItem: ColumnLayout {
        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("New message")
        spacing: 12
        RowLayout {
            Layout.fillWidth: true
            Text {
                text: qsTr("New message")
                color: dialog.colors["text-primary"]
                font.pixelSize: 18
                font.weight: Font.DemiBold
                Layout.fillWidth: true
            }
            NativeButton {
                objectName: "mailComposeClose"
                text: qsTr("Close")
                colors: dialog.colors
                onClicked: dialog.close()
            }
        }
        Text {
            text: qsTr("To")
            color: dialog.colors["text-secondary"]
        }
        NativeField {
            id: recipients
            objectName: "mailComposeRecipients"
            colors: dialog.colors
            Layout.fillWidth: true
            placeholderText: qsTr("Names or email addresses")
            Accessible.name: qsTr("To")
            onActiveFocusChanged: if (!activeFocus && dialog.opened) dialog.recipientsVisited = true
            onAccepted: subject.forceActiveFocus()
        }
        Text {
            objectName: "mailComposeRecipientsError"
            visible: dialog.recipientsVisited && !dialog.hasRecipient
            text: qsTr("Enter at least one recipient.")
            color: dialog.colors.danger
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
        NativeField {
            id: subject
            objectName: "mailComposeSubject"
            colors: dialog.colors
            Layout.fillWidth: true
            placeholderText: qsTr("Subject")
            Accessible.name: qsTr("Subject")
            onAccepted: body.forceActiveFocus()
        }
        RowLayout {
            Layout.fillWidth: true
            NativeButton {
                objectName: "mailComposeAttach"
                text: qsTr("Attach files")
                colors: dialog.colors
                onClicked: chooser.open()
            }
        }
        ScrollView {
            id: fileList
            visible: dialog.attachments.length > 0
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(fileRows.implicitHeight, 88)
            contentWidth: availableWidth
            clip: true
            Column {
                id: fileRows
                width: fileList.availableWidth
                Repeater {
                    model: dialog.attachments
                    delegate: RowLayout {
                        required property var modelData
                        required property int index
                        width: fileList.availableWidth
                        Text {
                            objectName: "mailComposeAttachmentName-" + parent.index
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            text: parent.modelData.name
                            textFormat: Text.PlainText
                            color: dialog.colors["text-primary"]
                            elide: Text.ElideMiddle
                        }
                        Text {
                            objectName: "mailComposeAttachmentSize-" + parent.index
                            text: parent.modelData.displaySize
                            textFormat: Text.PlainText
                            color: dialog.colors["text-secondary"]
                        }
                        NativeButton {
                            objectName: "mailComposeRemove-" + parent.index
                            text: qsTr("Remove")
                            colors: dialog.colors
                            onClicked: dialog.attachments = dialog.attachments.filter((_, i) => i !== parent.index)
                        }
                    }
                }
            }
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 48
            clip: true
            TextArea {
                id: body
                objectName: "mailComposeBody"
                textFormat: TextEdit.PlainText
                placeholderText: qsTr("Write a message…")
                Accessible.name: qsTr("Message")
                color: dialog.colors["text-primary"]
                placeholderTextColor: dialog.colors["text-muted"]
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                background: Rectangle {
                    radius: 8
                    color: dialog.colors["text-field-bg"]
                    border.color: body.activeFocus ? dialog.colors.accent : dialog.colors["border-subtle"]
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            NativeButton {
                objectName: "mailComposeSend"
                primary: true
                enabled: false
                colors: dialog.colors
                text: qsTr("Send")
                Accessible.description: unsupported.text
            }
            Text {
                id: unsupported
                objectName: "mailComposeSendNotice"
                Layout.fillWidth: true
                text: qsTr("Sending mail is unavailable in this preview.")
                wrapMode: Text.WordWrap
                color: dialog.colors["text-secondary"]
            }
        }
    }
}
