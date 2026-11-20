import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: panel
    required property var colors
    property bool creating: false
    property bool unsupported: false
    property string initialContent
    function reset(draft) {
        initialContent = draft; creating = false; unsupported = false;
        filter.clear(); title.clear(); body.text = draft;
    }
    width: Math.min(360, parent.width - 16); height: Math.min(380, parent.height - 16)
    padding: 12; focus: true; modal: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onOpened: filter.forceActiveFocus()
    background: Rectangle { radius: 8; color: panel.colors["bg-elevated"]; border.color: panel.colors["border-subtle"] }
    contentItem: ColumnLayout {
        spacing: 8
        Accessible.role: Accessible.Dialog; Accessible.name: qsTr("Saved snippets")
        Label { Layout.fillWidth: true; color: panel.colors["text-primary"]; text: panel.creating ? qsTr("Create new saved snippet") : qsTr("Saved snippets") }
        TextField {
            id: filter; objectName: "composerSnippetFilter"; visible: !panel.creating
            Layout.fillWidth: true; placeholderText: qsTr("Filter"); Accessible.name: qsTr("Filter snippets")
        }
        Label {
            objectName: "composerSnippetEmpty"; visible: !panel.creating; Layout.fillWidth: true; Layout.fillHeight: true
            color: panel.colors["text-muted"]; text: qsTr("No saved snippets")
        }
        Button {
            objectName: "composerSnippetCreate"; visible: !panel.creating; text: qsTr("Create new saved snippet")
            onClicked: { panel.creating = true; title.forceActiveFocus(); }
        }
        TextField {
            id: title; objectName: "composerSnippetTitle"; visible: panel.creating; Layout.fillWidth: true
            placeholderText: qsTr("Snippet title"); Accessible.name: placeholderText
        }
        TextArea {
            id: body; visible: panel.creating; Layout.fillWidth: true; Layout.fillHeight: true
            placeholderText: qsTr("Snippet content"); Accessible.name: placeholderText
            wrapMode: TextEdit.Wrap; textFormat: TextEdit.PlainText
        }
        Label {
            objectName: "composerSnippetUnsupported"; visible: panel.creating && panel.unsupported
            Layout.fillWidth: true; wrapMode: Text.Wrap; color: panel.colors["text-muted"]
            Accessible.role: Accessible.StatusBar; text: qsTr("Saved snippets are not connected yet.")
        }
        RowLayout {
            visible: panel.creating; Layout.alignment: Qt.AlignRight
            Button { text: qsTr("Cancel"); onClicked: { panel.reset(panel.initialContent); filter.forceActiveFocus(); } }
            Button {
                objectName: "composerSnippetSave"; text: qsTr("Save")
                enabled: title.text.trim().length > 0 && body.text.trim().length > 0
                onClicked: panel.unsupported = true
            }
        }
    }
}
