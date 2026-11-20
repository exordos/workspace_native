pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs as NativeDialogs

Dialog {
    id: form
    required property var service
    required property var controller
    required property var preferences
    required property var colors
    required property string displayPlatform
    property string feedback: ""
    objectName: "supportReportDialog"
    title: qsTr("Report a problem")
    anchors.centerIn: parent
    width: Math.min(650, parent.width - 32)
    height: Math.min(720, parent.height - 32)
    modal: true
    focus: true
    Connections {
        target: form.controller
        function onSupportScopeChanged() { if (form.visible) form.close() }
    }
    onClosed: service.cancel()

    contentItem: ColumnLayout {
        spacing: 8
        Label {
            Layout.fillWidth: true
            text: qsTr("Describe what you expected, what happened, when it happened and how to reproduce it. This description is kept separate from the diagnostic file.")
            color: form.colors["text-primary"]
            wrapMode: Text.Wrap
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: 86
            TextArea {
                id: notes
                objectName: "supportDescription"
                Accessible.name: qsTr("Problem description")
                wrapMode: TextEdit.Wrap
                selectByMouse: true
            }
        }
        NativeButton {
            colors: form.colors
            text: qsTr("Copy description")
            enabled: notes.length > 0
            onClicked: form.controller.copyToClipboard(notes.text)
        }
        CheckBox {
            id: includeCounts
            objectName: "supportIncludeCounts"
            text: qsTr("Include session counts and response times")
            onToggled: { form.feedback = ""; form.service.cancel() }
        }
        NativeButton {
            objectName: "supportPrepare"
            colors: form.colors
            enabled: !form.service.busy
            text: form.service.busy ? qsTr("Preparing report…") : qsTr("Prepare report")
            onClicked: {
                form.feedback = "";
                form.service.prepare(form.displayPlatform, form.preferences.locale, form.preferences.mode,
                                     includeCounts.checked ? form.controller.diagnosticsSupportMeasurements() : ({}));
            }
        }
        Label {
            text: qsTr("Diagnostic file preview")
            color: form.colors["text-primary"]
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            TextArea {
                objectName: "supportPreview"
                Accessible.name: qsTr("Diagnostic file preview")
                text: form.service.preview
                readOnly: true
                selectByMouse: true
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.WrapAnywhere
            }
        }
        Label {
            visible: text.length > 0
            text: form.feedback
            color: form.colors["text-primary"]
            Accessible.role: Accessible.AlertMessage
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            NativeButton {
                colors: form.colors
                text: qsTr("Cancel")
                onClicked: form.close()
            }
            NativeButton {
                objectName: "supportSave"
                colors: form.colors
                text: qsTr("Save report")
                primary: true
                enabled: form.service.preview.length > 0
                onClicked: chooser.open()
            }
        }
    }
    NativeDialogs.FileDialog {
        id: chooser
        title: qsTr("Save report")
        fileMode: NativeDialogs.FileDialog.SaveFile
        defaultSuffix: "json"
        nameFilters: [qsTr("JSON files (*.json)")]
        onAccepted: form.feedback = form.service.save(selectedFile) ? qsTr("Report saved") : qsTr("Could not save the report")
    }
}
