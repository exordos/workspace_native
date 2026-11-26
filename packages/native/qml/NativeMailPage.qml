pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Native Mail mailbox mock (issue #879, part of #851). Replaces the
// unavailable-fallback card with the reference demo mailbox measured from
// workspace_ui e0194ff embeds/mail-placeholder.html: header + non-functional
// search affordance, 220 px folder sidebar with unread counts, thread list
// (sender / subject / snippet / time) and a reader column that shows the
// selected row's title only - the full reader is the next subtask (#880).
Item {
    id: page
    objectName: "mailPage"
    implicitWidth: 900
    implicitHeight: 700

    property var colors
    property var mail
    readonly property bool dark: colors && colors.dark === true
    readonly property color panelColor: colors["bg-elevated"]
    readonly property color pageColor: colors["bg"]
    readonly property color borderColor: colors["border-subtle"]
    readonly property color textColor: colors["text-primary"]
    readonly property color mutedColor: colors["text-muted"]

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
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // Folder sidebar (220 px column per the reference grid).
            ColumnLayout {
                Layout.preferredWidth: 220
                Layout.fillHeight: true
                Layout.margins: 14
                spacing: 6
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
            Rectangle {
                Layout.preferredWidth: 1
                Layout.fillHeight: true
                color: page.borderColor
            }

            // Thread list column.
            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: 10
                currentIndex: page.mail && page.mail.state === "loading" ? 0
                    : page.mail && page.mail.state === "error" ? 2 : 1

                Item {
                    objectName: "mailLoadingState"
                    BusyIndicator {
                        anchors.centerIn: parent
                        running: true
                    }
                }
                ScrollView {
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
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 3
                                    RowLayout {
                                        width: parent.width
                                        spacing: 6
                                        Text {
                                            objectName: "mailMessageSender-" + messageRow.row.uuid
                                            Layout.fillWidth: true
                                            text: messageRow.row.sender
                                            font.pixelSize: 12
                                            color: page.mutedColor
                                            elide: Text.ElideRight
                                        }
                                        Text {
                                            objectName: "mailMessageTime-" + messageRow.row.uuid
                                            text: messageRow.row.time
                                            font.pixelSize: 11
                                            color: page.mutedColor
                                        }
                                    }
                                    Text {
                                        objectName: "mailMessageSubject-" + messageRow.row.uuid
                                        width: parent.width
                                        text: messageRow.row.subject
                                        font.pixelSize: 14
                                        font.weight: Font.DemiBold
                                        color: page.textColor
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        objectName: "mailMessageSnippet-" + messageRow.row.uuid
                                        width: parent.width
                                        text: messageRow.row.snippet
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
                Item {
                    objectName: "mailErrorState"
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

            // Reader placeholder column: subject + sender of the selected row
            // and an empty-body note; the real reader is #880.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: 16
                spacing: 8
                Text {
                    objectName: "mailReaderLabel"
                    text: qsTr("Preview")
                    font.pixelSize: 12
                    color: page.mutedColor
                }
                Text {
                    objectName: "mailReaderTitle"
                    Layout.fillWidth: true
                    text: page.mail && page.mail.selectedMessageUuid
                        ? page.mail.messages.find(m => m.uuid === page.mail.selectedMessageUuid).subject
                        : qsTr("Select a message")
                    font.pixelSize: 18
                    color: page.textColor
                    elide: Text.ElideRight
                }
                Text {
                    objectName: "mailReaderBody"
                    Layout.fillWidth: true
                    text: qsTr("Message bodies arrive with the mail server integration.")
                    font.pixelSize: 13
                    color: page.mutedColor
                    wrapMode: Text.WordWrap
                }
                Item { Layout.fillHeight: true }
            }
        }
    }
}
