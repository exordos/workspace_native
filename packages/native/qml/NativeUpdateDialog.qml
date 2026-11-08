import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog
    required property var updater
    required property var colors
    readonly property string phase: updater ? updater.state : "idle"
    readonly property bool working: phase === "checking" || phase === "downloading" || phase === "installing"
    objectName: "nativeUpdateDialog"
    width: Math.min(448, parent ? parent.width - 32 : 448)
    x: parent ? (parent.width - width) / 2 : 0
    y: parent ? (parent.height - height) / 2 : 0
    padding: 24
    modal: true
    title: qsTr("Workspace updates")
    closePolicy: working ? Popup.NoAutoClose : Popup.CloseOnEscape
    background: Rectangle {
        radius: 10
        color: dialog.colors["bg-elevated"]
        border.width: 1
        border.color: dialog.colors["border-subtle"]
    }
    header: Label {
        text: dialog.title
        color: dialog.colors["text-primary"]
        font.pixelSize: 20
        font.bold: true
        padding: 24
        bottomPadding: 0
        textFormat: Text.PlainText
    }
    contentItem: ColumnLayout {
        spacing: 16
        Label {
            Layout.fillWidth: true
            text: qsTr("Current version: %1").arg(dialog.updater ? dialog.updater.currentVersion : "")
            textFormat: Text.PlainText
            color: dialog.colors["text-primary"]
            wrapMode: Text.Wrap
        }
        Label {
            objectName: "nativeUpdateStatus"
            Layout.fillWidth: true
            text: {
                switch (dialog.phase) {
                case "checking": return qsTr("Checking for updates…");
                case "current": return qsTr("You are using the latest version.");
                case "available": return qsTr("Version %1 is available.").arg(dialog.updater.availableVersion);
                case "downloading": return qsTr("Downloading update… %1%").arg(Math.round(dialog.updater.progress * 100));
                case "downloaded": return qsTr("The update is downloaded and verified.");
                case "installing": return qsTr("Installing update…");
                case "ready": return qsTr("The update is installed. Restart to use it.");
                case "error": return dialog.updater.error;
                default: return qsTr("Check GitHub for a new version.");
                }
            }
            textFormat: Text.PlainText
            color: dialog.colors["text-primary"]
            wrapMode: Text.Wrap
        }
        ProgressBar {
            objectName: "nativeUpdateProgress"
            Layout.fillWidth: true
            visible: dialog.working
            indeterminate: dialog.phase !== "downloading"
            from: 0; to: 1
            value: dialog.updater ? dialog.updater.progress : 0
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            NativeButton {
                objectName: "nativeUpdateClose"
                colors: dialog.colors
                text: qsTr("Close")
                enabled: !dialog.working
                onClicked: dialog.close()
            }
            NativeButton {
                objectName: "nativeUpdateAction"
                colors: dialog.colors
                primary: true
                enabled: !dialog.working && !!dialog.updater
                text: dialog.phase === "available" ? qsTr("Download update")
                    : dialog.phase === "downloaded" ? qsTr("Install update")
                    : dialog.phase === "ready" ? qsTr("Restart now") : qsTr("Check again")
                onClicked: {
                    if (dialog.phase === "available") dialog.updater.download();
                    else if (dialog.phase === "downloaded") dialog.updater.install();
                    else if (dialog.phase === "ready") dialog.updater.restart();
                    else dialog.updater.check();
                }
            }
        }
    }
}
