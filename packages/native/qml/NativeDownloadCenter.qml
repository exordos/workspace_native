import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

Item {
    id: center
    required property var controller
    required property var colors
    readonly property var entries: controller.downloadEntries
    readonly property int activeCount: entries.filter(row => row.status === "starting" || row.status === "downloading").length
    readonly property bool canClear: entries.some(row => row.status !== "starting" && row.status !== "downloading")
    visible: entries.length > 0
    implicitWidth: 36
    implicitHeight: 36
    onVisibleChanged: if (!visible) panel.close()

    function bytes(value) {
        if (!Number.isFinite(value) || value <= 0) return "0 B";
        const units = ["B", "KB", "MB", "GB"];
        let index = 0;
        while (value >= 1024 && index < units.length - 1) { value /= 1024; ++index; }
        return value.toFixed(index === 0 ? 0 : value >= 10 ? 1 : 2) + " " + units[index];
    }
    function status(entry) {
        if (entry.retrying || entry.status === "starting") return qsTr("Starting...");
        if (entry.status === "downloaded") return qsTr("Ready");
        if (entry.status === "error") return qsTr("Failed");
        if (entry.totalBytes > 0)
            return qsTr("%1% - %2 / %3").arg(Math.min(100, Math.round(entry.receivedBytes / entry.totalBytes * 100)))
                .arg(bytes(entry.receivedBytes)).arg(bytes(entry.totalBytes));
        return qsTr("Downloading... %1").arg(bytes(entry.receivedBytes));
    }
    Connections {
        target: center.controller
        function onDownloadDuplicateRequested() { pulse.restart(); }
    }
    Timer { id: pulse; interval: 380 }
    NativeIconButton {
        id: trigger
        objectName: "downloadCenterButton"
        anchors.fill: parent
        colors: center.colors
        asset: "files.svg"
        glyphSize: 20
        help: qsTr("Open downloads")
        scale: pulse.running ? 1.1 : 1
        onClicked: panel.visible ? panel.close() : panel.open()
        Rectangle {
            objectName: "downloadActiveBadge"
            visible: center.activeCount > 0
            x: parent.width - width + 4; y: -4
            width: Math.max(16, badge.implicitWidth + 8); height: 16; radius: 8
            color: center.colors.accent
            Text {
                id: badge
                anchors.centerIn: parent
                text: center.activeCount
                font.pixelSize: 10; font.weight: Font.DemiBold
                color: center.colors["on-accent"]
            }
        }
    }
    Popup {
        id: panel
        objectName: "downloadCenterPanel"
        x: center.width - width
        y: 44
        width: 320
        padding: 1
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            radius: 12
            color: center.colors["card-bg"]
            border.width: 1; border.color: center.colors["border-subtle"]
            layer.enabled: true
            layer.effect: MultiEffect { shadowEnabled: true; shadowBlur: 0.5; shadowOpacity: 0.2; shadowVerticalOffset: 4 }
        }
        contentItem: Column {
            Accessible.role: Accessible.Dialog
            Accessible.name: qsTr("Downloads")
            spacing: 0
            Item {
                width: parent.width; height: 37
                Text {
                    x: 12; anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Downloads")
                    font.pixelSize: 14; font.weight: Font.Medium
                    color: center.colors["text-primary"]
                }
                Button {
                    objectName: "downloadClear"
                    anchors.right: parent.right; anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Clear")
                    enabled: center.canClear
                    padding: 0
                    implicitHeight: 28
                    Accessible.name: text
                    contentItem: Text {
                        text: parent.text; font.pixelSize: 12
                        color: center.colors["text-muted"]
                        opacity: parent.enabled ? 1 : 0.4
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: "transparent"; radius: 4
                        border.width: parent.visualFocus ? 1 : 0; border.color: center.colors.accent
                    }
                    onClicked: center.controller.clearDownloads()
                }
            }
            Rectangle { width: parent.width; height: 1; color: center.colors["border-subtle"] }
            ListView {
                id: rows
                objectName: "downloadRows"
                width: parent.width
                height: Math.min(256, contentHeight + 8)
                topMargin: 4; bottomMargin: 4
                clip: true
                model: center.entries
                ScrollBar.vertical: ScrollBar {}
                delegate: Item {
                    id: row
                    required property var modelData
                    readonly property bool active: modelData.status === "starting" || modelData.status === "downloading"
                    readonly property bool failed: modelData.status === "error"
                    readonly property string statusText: center.status(modelData)
                    width: rows.width; height: 56
                    Rectangle {
                        anchors.fill: parent; anchors.margins: 4; radius: 8
                        color: main.hovered ? center.colors["bg-elevated"] : "transparent"
                    }
                    Button {
                        id: main
                        objectName: (row.failed ? "downloadRetry-" : "downloadOpen-") + row.modelData.id
                        x: 8; y: 4; width: row.width - actions.width - 20; height: 48
                        padding: 4
                        enabled: !row.active && !row.modelData.retrying
                        hoverEnabled: true
                        Accessible.name: (row.failed ? qsTr("Retry download") : qsTr("Open file")) + " " + row.modelData.fileName
                        onClicked: row.failed ? center.controller.retryDownload(row.modelData.id) : center.controller.openDownload(row.modelData.id)
                        background: Rectangle {
                            color: "transparent"; radius: 4
                            border.width: main.visualFocus ? 1 : 0; border.color: center.colors.accent
                        }
                        contentItem: Column {
                            spacing: 2
                            Text {
                                width: parent.width
                                text: row.modelData.fileName; textFormat: Text.PlainText
                                elide: Text.ElideRight; font.pixelSize: 14; font.weight: Font.Medium
                                color: center.colors["text-primary"]
                            }
                            Text {
                                objectName: "downloadStatus-" + row.modelData.id
                                width: parent.width
                                text: row.statusText; textFormat: Text.PlainText
                                elide: Text.ElideRight; font.pixelSize: 12
                                color: row.failed ? center.colors["notice-base"] : center.colors["text-muted"]
                                Accessible.role: Accessible.StaticText
                                Accessible.ignored: row.active
                                Accessible.name: row.modelData.fileName + ": " + row.statusText
                                Accessible.description: row.modelData.error
                                ProgressBar {
                                    objectName: "downloadProgress-" + row.modelData.id
                                    anchors.fill: parent
                                    visible: row.active
                                    from: 0; to: 100
                                    value: row.modelData.totalBytes > 0 ? Math.min(100, row.modelData.receivedBytes / row.modelData.totalBytes * 100) : 0
                                    indeterminate: row.modelData.totalBytes <= 0
                                    background: Item {}
                                    contentItem: Item {}
                                    Accessible.name: row.modelData.fileName + ": " + row.statusText
                                }
                            }
                        }
                    }
                    Row {
                        id: actions
                        anchors.right: parent.right; anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 4
                        NativeIconButton {
                            objectName: "downloadReveal-" + row.modelData.id
                            visible: row.modelData.status === "downloaded"
                            width: 26; height: 26; glyphSize: 18
                            colors: center.colors; asset: "folder_open.svg"
                            help: qsTr("Show in folder") + " " + row.modelData.fileName
                            onClicked: center.controller.revealDownload(row.modelData.id)
                        }
                        NativeIconButton {
                            objectName: (row.active ? "downloadCancel-" : "downloadRemove-") + row.modelData.id
                            width: 22; height: 26; glyphSize: 14
                            colors: center.colors; asset: "close.svg"
                            help: (row.active ? qsTr("Cancel download") : qsTr("Remove download")) + " " + row.modelData.fileName
                            onClicked: row.active ? center.controller.cancelDownload(row.modelData.id) : center.controller.dismissDownload(row.modelData.id)
                        }
                    }
                }
            }
        }
    }
}
