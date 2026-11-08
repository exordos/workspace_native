pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One app-scoped instance stays mounted when hidden. Disclosure never drives
// transport or installation; NativeUpdater remains the sole operation owner.
FocusScope {
    id: drawer
    required property var updater
    required property var colors
    property bool canGoBack: false
    signal closeRequested()
    signal backRequested()
    width: 315
    clip: true
    objectName: "nativeAboutDrawer"
    z: 2

    readonly property string phase: updater ? String(updater.state) : "idle"
    readonly property string targetVersion: updater ? String(updater.availableVersion || "") : ""
    readonly property bool busy: phase === "checking" || phase === "downloading" || phase === "installing"
    readonly property bool installPhase: phase === "downloaded" || phase === "installing" || phase === "ready"
    readonly property bool canInstall: !!updater && phase === "downloaded" && targetVersion.length > 0 && installTarget.length === 0
    readonly property bool canCheck: !!updater && !busy && !installPhase
    readonly property bool indicatorVisible: !!updater && ["available", "downloading", "downloaded", "installing", "ready"].indexOf(phase) >= 0
    readonly property string indicatorLabel: phase === "available"
        ? qsTr("Version %1 available").arg(targetVersion || "?")
        : phase === "downloading"
            ? qsTr("Downloading update… %1%").arg(Math.round(Number(updater.progress || 0) * 100))
            : installPhase ? qsTr("Update ready to install") : qsTr("Application update")

    property bool policyReady: false
    property var policyUpdater: null
    property string downloadTarget: ""
    property var installUpdater: null
    property string installTarget: ""

    function clearInstallIntent() {
        installUpdater = null;
        installTarget = "";
    }
    function synchronizePolicy() {
        if (!policyReady)
            return;
        const source = updater;
        if (!source) {
            policyUpdater = null;
            downloadTarget = "";
            clearInstallIntent();
            return;
        }
        if (policyUpdater !== source) {
            policyUpdater = source;
            downloadTarget = "";
            clearInstallIntent();
        }
        const state = String(source.state);
        const version = String(source.availableVersion || "");
        if (state === "idle" || state === "checking" || state === "current" || state === "error") {
            downloadTarget = "";
            clearInstallIntent();
            return;
        }
        // A new target never inherits a previous target's installation consent.
        if (installTarget.length && (installUpdater !== source || installTarget !== version))
            clearInstallIntent();
        if (state === "available" && version.length && downloadTarget !== version) {
            // Fence before the synchronous changed() emitted by download().
            downloadTarget = version;
            source.download();
            return;
        }
        if (state === "ready" && installUpdater === source && installTarget.length && installTarget === version) {
            // Consume consent before restartRequested can synchronously reenter.
            clearInstallIntent();
            source.restart();
        }
    }
    function activateUpdateAction() {
        const source = updater;
        if (!source)
            return;
        if (canInstall) {
            installUpdater = source;
            installTarget = targetVersion;
            source.install();
        } else if (canCheck) {
            source.check();
        }
    }
    onUpdaterChanged: synchronizePolicy()
    Component.onCompleted: {
        policyReady = true;
        synchronizePolicy();
    }
    Connections {
        target: drawer.updater
        function onChanged() { drawer.synchronizePolicy(); }
    }
    Keys.onEscapePressed: event => {
        event.accepted = true;
        drawer.closeRequested();
    }

    Rectangle {
        anchors.fill: parent
        color: drawer.colors["bg-elevated"]
        radius: 6
        // This dock consumes input inside its bounds only; the rest of the
        // window stays usable, including while the updater is working.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onWheel: event => { event.accepted = true; }
        }
    }
    Item {
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        anchors.topMargin: 8
        anchors.bottomMargin: 20
        Item {
            id: header
            width: parent.width
            height: 40
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                anchors.topMargin: 4
                anchors.bottomMargin: 4
                spacing: 8
                NativeIconButton {
                    objectName: "nativeAboutBack"
                    visible: drawer.canGoBack
                    colors: drawer.colors
                    Layout.preferredWidth: 32; Layout.preferredHeight: 32
                    width: 32; height: 32; glyphSize: 16
                    asset: "chevron-right.svg"
                    rotation: 180
                    help: qsTr("Back")
                    iconColor: hovered ? drawer.colors["text-primary"] : drawer.colors["text-muted"]
                    background: Rectangle {
                        radius: 6
                        color: parent.hovered ? drawer.colors["card-bg-active"] : "transparent"
                        border.width: parent.visualFocus ? 1 : 0
                        border.color: drawer.colors.accent
                    }
                    onClicked: drawer.backRequested()
                }
                Text {
                    objectName: "nativeAboutTitle"
                    Layout.fillWidth: true
                    text: qsTr("App version")
                    textFormat: Text.PlainText
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                    Layout.preferredHeight: 24
                    lineHeightMode: Text.FixedHeight; lineHeight: 24
                    verticalAlignment: Text.AlignVCenter
                    color: drawer.colors["text-primary"]
                    elide: Text.ElideRight
                    Accessible.role: Accessible.Heading
                }
                NativeIconButton {
                    objectName: "nativeUpdateClose"
                    colors: drawer.colors
                    Layout.preferredWidth: 32; Layout.preferredHeight: 32
                    width: 32; height: 32; glyphSize: 16
                    asset: "close.svg"
                    help: qsTr("Close")
                    iconColor: hovered ? drawer.colors["text-primary"] : drawer.colors["text-muted"]
                    background: Rectangle {
                        radius: 6
                        color: parent.hovered ? drawer.colors["card-bg-active"] : "transparent"
                        border.width: parent.visualFocus ? 1 : 0
                        border.color: drawer.colors.accent
                    }
                    onClicked: drawer.closeRequested()
                }
            }
        }
        ScrollView {
            id: scroll
            objectName: "nativeAboutScroll"
            anchors.top: header.bottom
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            clip: true
            padding: 0
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            Column {
                width: scroll.availableWidth
                spacing: 12
                leftPadding: 16
                rightPadding: 16
                topPadding: 12
                bottomPadding: 12
                Rectangle {
                    objectName: "nativeAboutVersionCard"
                    width: parent.width - 32
                    implicitHeight: versionRow.height + 34
                    radius: 10
                    color: drawer.colors["card-bg"]
                    border.width: 1
                    border.color: drawer.colors["border-subtle"]
                    Row {
                        id: versionRow
                        x: 17; y: 17
                        width: parent.width - 34
                        height: Math.max(36, versionText.implicitHeight)
                        spacing: 12
                        Rectangle {
                            width: 36; height: 36; radius: 18
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.alignWhenCentered: false
                            color: drawer.colors["bg-elevated"]
                            NativeIconButton {
                                anchors.centerIn: parent
                                colors: drawer.colors; asset: "info.svg"
                                width: 18; height: 18; glyphSize: 18
                                enabled: false; opacity: 1; focusPolicy: Qt.NoFocus
                                icon.color: drawer.colors.accent
                                background: Item {}
                                Accessible.ignored: true
                            }
                        }
                        Column {
                            id: versionText
                            width: parent.width - 48
                            spacing: 2
                            Text {
                                width: parent.width
                                text: qsTr("App version")
                                textFormat: Text.PlainText
                                color: drawer.colors["text-primary"]
                                font.pixelSize: 14; font.weight: Font.Medium
                                lineHeightMode: Text.FixedHeight; lineHeight: 20
                                height: Math.max(20, lineCount * 20)
                                verticalAlignment: Text.AlignVCenter
                                wrapMode: Text.Wrap
                            }
                            Text {
                                objectName: "nativeAboutCurrentVersionText"
                                width: parent.width
                                text: qsTr("Current version: %1").arg(drawer.updater ? drawer.updater.currentVersion : "")
                                textFormat: Text.PlainText
                                color: drawer.colors["text-muted"]
                                font.pixelSize: 12
                                lineHeightMode: Text.FixedHeight; lineHeight: 16
                                height: Math.max(16, lineCount * 16)
                                verticalAlignment: Text.AlignVCenter
                                wrapMode: Text.Wrap
                            }
                        }
                    }
                }
                Rectangle {
                    objectName: "nativeAboutUpdateCard"
                    width: parent.width - 32
                    implicitHeight: updateContent.implicitHeight + 34
                    radius: 10
                    color: drawer.colors["card-bg"]
                    border.width: 1
                    border.color: drawer.colors["border-subtle"]
                    Accessible.role: Accessible.Grouping
                    Accessible.name: qsTr("Application update")
                    Column {
                        id: updateContent
                        x: 17; y: 17
                        width: parent.width - 34
                        spacing: 12
                        Row {
                            width: parent.width
                            height: Math.max(36, updateText.implicitHeight)
                            spacing: 12
                            Rectangle {
                                width: 36; height: 36; radius: 18
                                color: drawer.colors["bg-elevated"]
                                NativeIconButton {
                                    anchors.centerIn: parent
                                    colors: drawer.colors; asset: "download.svg"
                                    width: 18; height: 18; glyphSize: 18
                                    enabled: false; opacity: 1; focusPolicy: Qt.NoFocus
                                    icon.color: drawer.colors.accent
                                    background: Item {}
                                    Accessible.ignored: true
                                }
                            }
                            Column {
                                id: updateText
                                width: parent.width - 48
                                spacing: 2
                                Text {
                                    width: parent.width
                                    text: qsTr("Application update")
                                    textFormat: Text.PlainText
                                    color: drawer.colors["text-primary"]
                                    font.pixelSize: 14; font.weight: Font.Medium
                                    lineHeightMode: Text.FixedHeight; lineHeight: 20
                                    height: Math.max(20, lineCount * 20)
                                    verticalAlignment: Text.AlignVCenter
                                    wrapMode: Text.Wrap
                                }
                                Text {
                                    objectName: "nativeUpdateStatus"
                                    width: parent.width
                                    text: {
                                        switch (drawer.phase) {
                                        case "checking": return qsTr("Checking for updates…");
                                        case "available": return qsTr("Version %1 available").arg(drawer.targetVersion || "?");
                                        case "downloading": return qsTr("Downloading update… %1%").arg(Math.round(Number(drawer.updater.progress || 0) * 100));
                                        case "downloaded":
                                        case "installing": return qsTr("Update ready to install");
                                        case "ready": return qsTr("The update is installed. Restart to use it.");
                                        case "current": return qsTr("You're up to date");
                                        case "error": return String(drawer.updater.error || "").trim().length ? drawer.updater.error : qsTr("Failed to check updates");
                                        default: return qsTr("Check whether a new version is available");
                                        }
                                    }
                                    textFormat: Text.PlainText
                                    color: drawer.colors["text-muted"]
                                    font.pixelSize: 12
                                    lineHeightMode: Text.FixedHeight; lineHeight: 16
                                    height: Math.max(16, lineCount * 16)
                                    verticalAlignment: Text.AlignVCenter
                                    wrapMode: Text.Wrap
                                }
                            }
                        }
                        Column {
                            width: parent.width
                            spacing: 8
                            Text {
                                width: parent.width
                                visible: drawer.phase === "downloaded" || drawer.phase === "installing"
                                text: qsTr("Installation may take up to a minute. The application will restart automatically.")
                                textFormat: Text.PlainText
                                color: drawer.colors["text-muted"]
                                font.pixelSize: 12
                                lineHeightMode: Text.FixedHeight; lineHeight: 16
                                height: Math.max(16, lineCount * 16)
                                verticalAlignment: Text.AlignVCenter
                                wrapMode: Text.Wrap
                            }
                            Button {
                                id: action
                                objectName: "nativeUpdateAction"
                                width: parent.width
                                implicitHeight: 32
                                horizontalPadding: 12
                                verticalPadding: 0
                                hoverEnabled: true
                                text: drawer.installPhase ? qsTr("Install and restart") : qsTr("Check for updates")
                                enabled: drawer.canInstall || drawer.canCheck
                                onClicked: drawer.activateUpdateAction()
                                contentItem: Text {
                                    text: action.text
                                    textFormat: Text.PlainText
                                    font.pixelSize: 12
                                    font.weight: Font.DemiBold
                                    color: drawer.colors["on-accent"]
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                    wrapMode: Text.NoWrap
                                    elide: Text.ElideRight
                                }
                                background: Item {
                                    Rectangle {
                                        anchors.fill: parent; anchors.margins: -2
                                        visible: action.visualFocus
                                        radius: 8; color: drawer.colors.bg
                                    }
                                    Rectangle {
                                        anchors.fill: parent; anchors.margins: -4
                                        visible: action.visualFocus
                                        radius: 10; color: "transparent"
                                        border.width: 2; border.color: drawer.colors.accent
                                    }
                                    Rectangle {
                                        anchors.fill: parent
                                        radius: 6; color: drawer.colors.accent
                                        opacity: action.enabled ? (action.down ? 0.8 : action.hovered ? 0.9 : 1) : 1
                                    }
                                }
                                opacity: enabled ? 1 : 0.5
                            }
                        }
                    }
                }
            }
        }
    }
}
