pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Account root screen of the right drawer (issue #724,
// docs/NATIVE_DESKTOP_ACCOUNT_DRAWER.md).
// Reference: workspace_ui e0194ffb667986b58a12256ce41ca5dc99b56b83
// right-panel-user-menu.ui.tsx - a flat section list with hairline dividers
// inset 16 px and muted section labels (PanelMenuRow metrics: rows px-4 py-1.5,
// ~44 px height). Theme-mode and palette controls are NOT here: the reference
// keeps them inside the Appearance screen only (right-panel-user-settings.ui.tsx).
// objectNames are carried over from the removed Main.qml accountPopup so the
// established test seams keep working.
Item {
    id: menu
    anchors.fill: parent
    // Drawer focus contract (like NativeRightDrawer.onDrawerOpenChanged):
    // the screen takes key focus when it becomes visible, so Back from a
    // nested account screen returns focus to the account root.
    onVisibleChanged: if (visible) menu.forceActiveFocus()

    required property var colors
    required property var controller
    required property var preferences
    required property bool menuOpen

    signal statusRequested()
    signal personalInfoRequested()
    signal settingsRequested()
    signal appearanceRequested()
    signal aboutRequested()
    signal diagnosticsRequested()
    signal clearCacheRequested()
    signal refreshRequested()
    signal signOutRequested()

    Flickable {
        id: accountScroll
        objectName: "accountMenuScroll"
        anchors.top: parent.top
        anchors.bottom: signOutFooter.top
        anchors.bottomMargin: 8
        anchors.left: parent.left
        anchors.right: parent.right
        clip: true
        contentWidth: width
        contentHeight: accountActions.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
    }
    NativeButton {
        id: signOutFooter
        objectName: "signOutButton"
        colors: menu.colors
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        text: qsTr("Sign out")
        onClicked: menu.signOutRequested()
    }

    ColumnLayout {
        id: accountActions
        parent: accountScroll.contentItem
        width: accountScroll.width
        spacing: 0

        // ---- Section: Description (settings.sectionDescription) ----
        ItemDelegate {
            Layout.fillWidth: true
            Layout.topMargin: 4
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.preferredHeight: 24
            padding: 0
            contentItem: Text {
                text: qsTr("Description")
                textFormat: Text.PlainText
                color: menu.colors["text-muted"]
                font.pixelSize: 12
                verticalAlignment: Text.AlignVCenter
            }
            Accessible.ignored: true
            background: Item {}
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.topMargin: 6
            Layout.bottomMargin: 6
            spacing: 8
            Image {
                Layout.preferredWidth: 32
                Layout.preferredHeight: 32
                source: "qrc:/workspace/icons/dashboard_customize.svg"
                sourceSize: Qt.size(22, 22)
                fillMode: Image.Pad
                Accessible.ignored: true
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Current server")
                    font.weight: Font.Medium
                    color: menu.colors["text-primary"]
                }
                Label {
                    objectName: "currentServerLabel"
                    Layout.fillWidth: true
                    text: menu.controller.account.serverLabel || ""
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: menu.colors["text-muted"]
                }
                Label {
                    objectName: "currentServerLogin"
                    Layout.fillWidth: true
                    text: menu.controller.account.login || ""
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: menu.colors["text-muted"]
                }
            }
        }
        NativeExternalAccountBlock {
            id: externalAccountsBlock
            Layout.fillWidth: true
            visible: menu.controller.authenticated
            directory: menu.controller.externalAccounts
            admin: menu.controller.externalProviderAdmin
            colors: menu.colors
            menuOpen: menu.menuOpen
        }
        NativeExternalProviderDialog {
            id: externalProviderDialog
            admin: menu.controller.externalProviderAdmin
            colors: menu.colors
        }
        NativeButton {
            objectName: "personalInfoAction"
            colors: menu.colors
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            text: qsTr("Personal info")
            enabled: menu.controller.authenticated
            onClicked: menu.personalInfoRequested()
        }
        NativeButton {
            objectName: "ownStatusAction"
            colors: menu.colors
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Layout.topMargin: 8
            text: qsTr("Status")
            enabled: menu.controller.authenticated
                && String(menu.controller.ownStatusContextKey || "").length > 0
            onClicked: menu.statusRequested()
        }

        // ---- Section: Settings (settings.settings) ----
        Rectangle {
            Layout.fillWidth: true
            Layout.topMargin: 12
            Layout.preferredHeight: 1
            color: menu.colors["border-subtle"]
        }
        ItemDelegate {
            Layout.fillWidth: true
            Layout.topMargin: 4
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.preferredHeight: 24
            padding: 0
            contentItem: Text {
                text: qsTr("Settings")
                textFormat: Text.PlainText
                color: menu.colors["text-muted"]
                font.pixelSize: 12
                verticalAlignment: Text.AlignVCenter
            }
            Accessible.ignored: true
            background: Item {}
        }
        ItemDelegate {
            objectName: "accountSettingsAction"
            Layout.fillWidth: true
            text: qsTr("Settings")
            onClicked: menu.settingsRequested()
            contentItem: RowLayout {
                spacing: 8
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label { text: qsTr("Settings"); color: menu.colors["text-primary"] }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Notification sound, Language, Auto sign-out")
                        color: menu.colors["text-muted"]
                        wrapMode: Text.WordWrap
                        font.pixelSize: 12
                    }
                }
                Image {
                    source: "qrc:/workspace/icons/chevron-right.svg"
                    sourceSize: Qt.size(16, 16)
                    Accessible.ignored: true
                }
            }
        }
        ItemDelegate {
            objectName: "accountAppearanceAction"
            Layout.fillWidth: true
            text: qsTr("Appearance")
            onClicked: menu.appearanceRequested()
            contentItem: RowLayout {
                spacing: 8
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label { text: qsTr("Appearance"); color: menu.colors["text-primary"] }
                    Label {
                        Layout.fillWidth: true
                        // Deviation (design doc): the reference subtitle names
                        // Folder layout / Chat list density too; the native
                        // Appearance screen carries theme settings only.
                        text: qsTr("Theme settings")
                        color: menu.colors["text-muted"]
                        wrapMode: Text.WordWrap
                        font.pixelSize: 12
                    }
                }
                Image {
                    source: "qrc:/workspace/icons/chevron-right.svg"
                    sourceSize: Qt.size(16, 16)
                    Accessible.ignored: true
                }
            }
        }

        // ---- Section: App version (settings.appVersion) ----
        Rectangle {
            Layout.fillWidth: true
            Layout.topMargin: 12
            Layout.preferredHeight: 1
            color: menu.colors["border-subtle"]
        }
        ItemDelegate {
            Layout.fillWidth: true
            Layout.topMargin: 4
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.preferredHeight: 24
            padding: 0
            contentItem: Text {
                text: qsTr("App version")
                textFormat: Text.PlainText
                color: menu.colors["text-muted"]
                font.pixelSize: 12
                verticalAlignment: Text.AlignVCenter
            }
            Accessible.ignored: true
            background: Item {}
        }
        NativeButton {
            objectName: "aboutAction"
            colors: menu.colors
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            text: qsTr("App version")
            onClicked: menu.aboutRequested()
        }
        // Issue #50: Diagnostics sits directly below the App version row
        // (right-panel-user-menu.ui.tsx:361-379, settings.connectionDiagnostics).
        NativeButton {
            objectName: "diagnosticsAction"
            colors: menu.colors
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Layout.topMargin: 8
            text: qsTr("Diagnostics")
            enabled: menu.controller.authenticated
            onClicked: menu.diagnosticsRequested()
        }
        ItemDelegate {
            objectName: "clearLocalCacheAction"
            Layout.fillWidth: true
            text: qsTr("Clear cache")
            Accessible.description: qsTr("Message cache, local settings, then reload")
            enabled: menu.controller.authenticated && !menu.controller.busy
            onClicked: menu.clearCacheRequested()
            contentItem: RowLayout {
                spacing: 8
                Image {
                    source: "qrc:/workspace/icons/delete.svg"
                    sourceSize: Qt.size(22, 22)
                    Accessible.ignored: true
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label { text: qsTr("Clear cache"); color: menu.colors["text-primary"] }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Message cache, local settings, then reload")
                        color: menu.colors["text-muted"]
                        wrapMode: Text.WordWrap
                        font.pixelSize: 12
                    }
                }
                Image {
                    source: "qrc:/workspace/icons/chevron-right.svg"
                    sourceSize: Qt.size(16, 16)
                    Accessible.ignored: true
                }
            }
        }

        // ---- Native extras kept from the removed popup (design doc): ----
        // Language and the disk-cache widget block have no reference root rows
        // today; they move unchanged so their seams and behaviour survive.
        Rectangle {
            Layout.fillWidth: true
            Layout.topMargin: 12
            Layout.preferredHeight: 1
            color: menu.colors["border-subtle"]
        }
        NativeButton {
            id: languageSetting
            objectName: "languageSetting"
            property bool expanded: false
            colors: menu.colors
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Layout.topMargin: 8
            text: qsTr("Language") + " · " + (menu.preferences.locale === "ru" ? "Русский" : "English")
            onClicked: expanded = !expanded
        }
        RowLayout {
            visible: languageSetting.expanded
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            spacing: 4
            NativeButton {
                objectName: "language-en"
                colors: menu.colors
                Layout.fillWidth: true
                text: qsTr("English")
                primary: menu.preferences.locale === "en"
                onClicked: menu.preferences.locale = "en"
            }
            NativeButton {
                objectName: "language-ru"
                colors: menu.colors
                Layout.fillWidth: true
                text: qsTr("Русский")
                primary: menu.preferences.locale === "ru"
                onClicked: menu.preferences.locale = "ru"
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.topMargin: 8
            spacing: 8
            Text {
                text: qsTr("Disk cache (GB)")
                textFormat: Text.PlainText
                color: menu.colors["text-secondary"]
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.pixelSize: 12
            }
            SpinBox {
                id: cacheLimit
                objectName: "cacheLimitGb"
                from: 1
                to: 1000
                value: menu.preferences.cacheLimitGb
                editable: true
                implicitWidth: 104
                implicitHeight: 32
                Accessible.name: qsTr("Disk cache limit in GB")
                onValueModified: menu.preferences.cacheLimitGb = value
            }
        }
        Text {
            objectName: "cacheSettingsBusy"
            visible: menu.controller.authenticated && menu.preferences.cacheBusy
            text: qsTr("Updating disk cache…")
            textFormat: Text.PlainText
            color: menu.colors["text-secondary"]
            wrapMode: Text.Wrap
            font.pixelSize: 12
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Accessible.role: Accessible.AlertMessage
        }
        Text {
            objectName: "cacheSettingsLimits"
            visible: menu.controller.authenticated
                && (menu.preferences.cacheBusy || menu.preferences.cacheAppliedLimitBytes > 0)
            text: menu.preferences.cacheAppliedLimitBytes > 0
                ? qsTr("Requested: %1 GB · Applied: %2 GB").arg(menu.preferences.cacheLimitGb).arg(menu.preferences.cacheAppliedLimitBytes / 1000000000)
                : qsTr("Requested: %1 GB").arg(menu.preferences.cacheLimitGb)
            textFormat: Text.PlainText
            color: menu.colors["text-muted"]
            wrapMode: Text.Wrap
            font.pixelSize: 12
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
        }
        Text {
            objectName: "cacheSettingsUsage"
            visible: menu.controller.authenticated && menu.preferences.cacheAppliedLimitBytes > 0
            text: qsTr("Used: %1").arg(Qt.locale(menu.preferences.locale).formattedDataSize(menu.preferences.cacheUsageBytes, 1, Locale.DataSizeSIFormat))
            textFormat: Text.PlainText
            color: menu.colors["text-muted"]
            wrapMode: Text.Wrap
            font.pixelSize: 12
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
        }
        Text {
            objectName: "cacheSettingsUnavailable"
            visible: menu.controller.authenticated && !menu.preferences.cacheBusy && !menu.preferences.cacheAvailable
            text: qsTr("Disk cache is unavailable.")
            textFormat: Text.PlainText
            color: menu.colors["text-muted"]
            wrapMode: Text.Wrap
            font.pixelSize: 12
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
        }
        Text {
            objectName: "cacheSettingsError"
            visible: menu.preferences.cacheError.length > 0
            text: menu.preferences.cacheError
            textFormat: Text.PlainText
            color: menu.colors.danger
            wrapMode: Text.Wrap
            font.pixelSize: 12
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Accessible.role: Accessible.AlertMessage
        }
        Text {
            visible: menu.preferences.error.length > 0
            text: menu.preferences.error
            textFormat: Text.PlainText
            color: menu.colors.danger
            wrapMode: Text.Wrap
            font.pixelSize: 12
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Accessible.role: Accessible.AlertMessage
        }
        NativeButton {
            objectName: "refreshButton"
            colors: menu.colors
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Layout.topMargin: 8
            Layout.bottomMargin: 8
            text: qsTr("Refresh")
            enabled: !menu.controller.busy
            onClicked: menu.refreshRequested()
        }
    }
}
