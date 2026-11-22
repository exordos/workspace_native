import QtQuick
import QtQuick.Controls

// Post-install update confirmation toast (#634). Consume the pending marker
// once at start; a match shows the success toast exactly once (reference
// useInstalledAppUpdateToast). Mismatch/corrupt values clear silently.
Popup {
    id: toast
    objectName: "updateInstalledToast"
    required property var updater
    required property var colors
    z: 10000
    x: (parent ? parent.width : 0) - width - 16
    y: 16
    width: parent ? Math.min(384, parent.width - 32) : 384
    leftPadding: 16; rightPadding: 16; topPadding: 12; bottomPadding: 12
    closePolicy: Popup.NoAutoClose
    modal: false
    focus: false
    background: Rectangle {
        radius: 8
        color: Qt.rgba(toast.colors["call-green"].r, toast.colors["call-green"].g,
            toast.colors["call-green"].b, 0.1)
        border.color: Qt.rgba(toast.colors["call-green"].r, toast.colors["call-green"].g,
            toast.colors["call-green"].b, 0.4)
    }
    contentItem: Text {
        id: toastText
        objectName: "updateInstalledText"
        wrapMode: Text.WordWrap
        color: toast.colors["text-primary"]
        font.pixelSize: 13
    }
    Timer {
        objectName: "updateInstalledProbe"
        interval: 0
        onTriggered: {
            const installed = toast.updater ? toast.updater.consumeInstalledUpdate() : ""
            if (installed.length > 0) {
                toastText.text = qsTr("Update %1 installed successfully").arg(installed)
                toast.open()
                updateInstalledCloser.restart()
            }
        }
        Component.onCompleted: start()
    }
    Timer {
        id: updateInstalledCloser
        objectName: "updateInstalledCloser"
        interval: 4000
        onTriggered: toast.close()
    }
}
