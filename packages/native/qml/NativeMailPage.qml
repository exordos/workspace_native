pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Native Mail mailbox mock (issue #879, part of #851). Replaces the
// unavailable-fallback card with the reference demo mailbox measured from
// workspace_ui e0194ff embeds/mail-placeholder.html: header + non-functional
// search affordance, 220 px folder sidebar with unread counts, thread list
// (sender / subject / snippet / time) and a reader for the selected message.
Item {
    id: page
    objectName: "mailPage"
    implicitWidth: 900
    implicitHeight: 700

    property var colors
    property var mail
    property bool dark: false
    readonly property color panelColor: colors["bg-elevated"]
    readonly property color pageColor: colors["bg"]
    readonly property color borderColor: colors["border-subtle"]
    readonly property color textColor: colors["text-primary"]
    readonly property color mutedColor: colors["text-muted"]

    Loader {
        id: composeLoader
        active: false
        sourceComponent: NativeMailComposeDialog {
            colors: page.colors
            mail: page.mail
            onClosed: composeLoader.active = false
        }
        onLoaded: item.open()
    }
    onVisibleChanged: if (!visible && composeLoader.item) composeLoader.item.close()
    Connections {
        target: page.mail
        function onOwnerRetired() {
            if (composeLoader.item)
                composeLoader.item.close()
            composeLoader.active = false
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Header: title + search affordance (placeholder min-width 260,
        // radius 10, padding 8/12, 13 px per the reference).
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 16
            spacing: 10
            Text {
                objectName: "mailPageTitle"
                text: qsTr("Mail")
                font.pixelSize: 23
                color: page.textColor
            }
            Item { Layout.fillWidth: true }
            Rectangle {
                objectName: "mailSearchField"
                Layout.preferredWidth: Math.max(260, 0)
                Layout.preferredHeight: 33
                radius: 10
                color: page.dark ? page.panelColor : "#f8faff"
                border.color: page.borderColor
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    text: qsTr("Search mail...")
                    font.pixelSize: 13
                    color: page.mutedColor
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: page.borderColor
        }

        RowLayout {
            id: columnsRow
            objectName: "mailColumnsRow"
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // Folder sidebar (220 px column per the reference grid).
            // Plain Item wrapper: a fillWidth delegate inside a layout-typed
            // RowLayout child hoists onto the wrapper level in Qt 6.10 and
            // starves the sibling columns.
            Item {
                Layout.preferredWidth: 220
                Layout.fillHeight: true
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 6
                    NativeButton {
                        objectName: "mailComposeButton"
                        Layout.fillWidth: true
                        text: qsTr("Compose")
                        colors: page.colors
                        primary: true
                        onClicked: {
                            if (composeLoader.item)
                                composeLoader.item.open()
                            else
                                composeLoader.active = true
                        }
                    }
                    Repeater {
                        id: folderList
                        objectName: "mailFolderList"
                        // Stable positional model (NativeFeedPage pattern): a
                        // rebound Repeater keeps delegates and re-binds
                        // modelData, so rows index the live list instead.
                        model: page.mail
                            ? Array.from({length: page.mail.folders.length}, function (_, i) { return i })
                            : []
                        delegate: Rectangle {
                            id: folderRow
                            required property var modelData
                            readonly property var row: (page.mail && page.mail.folders[Number(modelData)]) || ({})
                            readonly property bool active: page.mail
                                && page.mail.folderId === row.id
                            objectName: "mailFolderRow-" + row.id
                            Layout.fillWidth: true
                            implicitHeight: 34
                            radius: 9
                            color: active ? page.colors["accent-soft"] : "transparent"
                            border.color: active ? page.colors["accent"] : "transparent"
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: page.mail.selectFolder(folderRow.row.id)
                            }
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 6
                                Text {
                                    objectName: "mailFolderTitle-" + folderRow.row.id
                                    Layout.fillWidth: true
                                    text: folderRow.row.title
                                    textFormat: Text.PlainText
                                    font.pixelSize: 13
                                    font.weight: folderRow.active ? Font.DemiBold : Font.Normal
                                    color: folderRow.active
                                        ? (page.dark ? page.textColor : "#2347a7")
                                        : page.textColor
                                    elide: Text.ElideRight
                                }
                                Text {
                                    objectName: "mailFolderUnread-" + folderRow.row.id
                                    visible: folderRow.row.unread > 0
                                    text: String(folderRow.row.unread)
                                    font.pixelSize: 12
                                    font.weight: Font.DemiBold
                                    color: page.colors["on-accent"]
                                    Accessible.name: folderRow.row.title
                                }
                            }
                        }
                    }
                    Item { Layout.fillHeight: true }
                }
            }
            Rectangle {
                Layout.preferredWidth: 1
                Layout.fillHeight: true
                color: page.borderColor
            }

            // Thread list column. The reference grid is 220px | 1fr | 1fr;
            // A plain Item carries the equal fill hints: a layout-typed
            // RowLayout child with explicit preferredWidth 0 mis-splits the
            // leftover in Qt 6.10 (first fill column swallows everything,
            // the sibling freezes at its implicit width - measured 76/121 px).
            // The list also stays OUT of a StackLayout: delegates incubated
            // while an ancestor is hidden are never laid out by the layout
            // engine (thread rows stuck 0 px wide), so busy/error are overlays.
            Item {
                Layout.fillWidth: true
                Layout.preferredWidth: 0
                Layout.minimumWidth: 0
                Layout.fillHeight: true
                ScrollView {
                    objectName: "mailThreadColumn"
                    anchors.fill: parent
                    anchors.margins: 10
                    clip: true
                    ColumnLayout {
                        objectName: "mailMessageList"
                        width: parent ? parent.width : 0
                        spacing: 8
                        Repeater {
                            model: page.mail
                                ? Array.from({length: page.mail.messages.length}, function (_, i) { return i })
                                : []
                            delegate: Rectangle {
                                id: messageRow
                                required property var modelData
                                readonly property var row: (page.mail && page.mail.messages[Number(modelData)]) || ({})
                                readonly property bool selected: page.mail
                                    && page.mail.selectedMessageUuid === row.uuid
                                objectName: "mailMessageRow-" + row.uuid
                                Layout.fillWidth: true
                                implicitHeight: messageColumn.implicitHeight + 20
                                radius: 10
                                color: selected ? page.colors["card-bg-active"] : page.panelColor
                                border.color: selected ? page.colors["accent"] : page.borderColor
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: page.mail.selectMessage(messageRow.row.uuid)
                                }
                                Column {
                                    id: messageColumn
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 12
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 3
                                    RowLayout {
                                        width: parent.width
                                        spacing: 6
                                        Text {
                                            objectName: "mailMessageSender-" + messageRow.row.uuid
                                            Layout.fillWidth: true
                                            text: messageRow.row.sender
                                            textFormat: Text.PlainText
                                            font.pixelSize: 12
                                            color: page.mutedColor
                                            elide: Text.ElideRight
                                        }
                                        Text {
                                            objectName: "mailMessageTime-" + messageRow.row.uuid
                                            text: messageRow.row.time
                                            textFormat: Text.PlainText
                                            font.pixelSize: 11
                                            color: page.mutedColor
                                        }
                                    }
                                    Text {
                                        objectName: "mailMessageSubject-" + messageRow.row.uuid
                                        width: parent.width
                                        text: messageRow.row.subject
                                        textFormat: Text.PlainText
                                        font.pixelSize: 14
                                        font.weight: Font.DemiBold
                                        color: page.textColor
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        objectName: "mailMessageSnippet-" + messageRow.row.uuid
                                        width: parent.width
                                        text: messageRow.row.snippet
                                        textFormat: Text.PlainText
                                        font.pixelSize: 12
                                        color: page.mutedColor
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                        }
                        Item {
                            objectName: "mailEmptyState"
                            visible: page.mail && page.mail.state === "ready"
                                && page.mail.messages.length === 0
                            Layout.fillWidth: true
                            Layout.topMargin: 40
                            implicitHeight: emptyText.implicitHeight
                            Text {
                                id: emptyText
                                objectName: "mailEmptyStateText"
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: qsTr("No messages in this folder")
                                font.pixelSize: 13
                                color: page.mutedColor
                            }
                        }
                    }
                }
                Rectangle {
                    objectName: "mailLoadingState"
                    anchors.fill: parent
                    visible: page.mail && page.mail.state === "loading"
                    color: page.pageColor
                    BusyIndicator {
                        anchors.centerIn: parent
                        running: parent.visible
                    }
                }
                Rectangle {
                    objectName: "mailErrorState"
                    anchors.fill: parent
                    visible: page.mail && page.mail.state === "error"
                    color: page.pageColor
                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 10
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: qsTr("Couldn't load mail")
                            font.pixelSize: 14
                            color: page.textColor
                        }
                        Button {
                            objectName: "mailRetryButton"
                            Layout.alignment: Qt.AlignHCenter
                            text: qsTr("Retry")
                            onClicked: if (page.mail) page.mail.refresh()
                        }
                    }
                }
            }
            Rectangle {
                Layout.preferredWidth: 1
                Layout.fillHeight: true
                color: page.borderColor
            }

            Item {
                Layout.fillWidth: true
                Layout.preferredWidth: 0
                Layout.minimumWidth: 0
                Layout.fillHeight: true
                NativeMailReader {
                    anchors.fill: parent
                    anchors.margins: 16
                    colors: page.colors
                    message: (page.mail && page.mail.messages.find(
                        row => row.uuid === page.mail.selectedMessageUuid)) || null
                }
            }
        }
    }
}
