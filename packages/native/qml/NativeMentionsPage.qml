pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Activity Mentions page (issue #42). Presentation only: the controller owns
// the collections, the unread-mentions index and the realtime merge.
// Geometry measured from workspace_ui 61dacef78dab2f5d0be76f283bebf49fe29b84af:
// filter bar activity-page.ui.tsx:655-682 (h-7 pills, rounded-md, gap-0.5);
// card layout activity-message-card.ui.tsx (8 px dot slot mt-1.5, 12 px p-3,
// 11 px meta, 12 px sender, 14 px two-line preview).
FocusScope {
    id: page
    objectName: "nativeMentionsPage"
    required property var controller
    required property var colors
    property bool loadActive: visible
    readonly property var mentionsState: controller.mentionsPage
    readonly property var rows: mentionsState.rows || []
    readonly property string filter: String(mentionsState.filter || "all")
    readonly property bool loading: Boolean(mentionsState.loading)
    readonly property bool error: Boolean(mentionsState.error)
    readonly property bool paginationError: Boolean(mentionsState.paginationError)
    readonly property bool canLoadMore: Boolean(mentionsState.canLoadMore)
    onVisibleChanged: { if (visible) forceActiveFocus(); }
    Keys.onEscapePressed: page.controller.closeMentions()

    component MentionAction: Button {
        id: action
        implicitWidth: contentItem.implicitWidth + 16
        implicitHeight: contentItem.implicitHeight + 8
        leftPadding: 8; rightPadding: 8
        topPadding: 4; bottomPadding: 4
        font.pixelSize: 11
        hoverEnabled: true
        contentItem: Text {
            text: action.text
            textFormat: Text.PlainText
            color: page.colors["text-primary"]
            font: action.font
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: 4
            color: action.hovered ? page.colors["card-bg"] : "transparent"
            border.width: action.visualFocus ? 1 : 0
            border.color: page.colors.accent
        }
    }

    Rectangle {
        id: header
        objectName: "mentionsHeader"
        anchors.top: parent.top
        width: parent.width
        height: 54
        radius: 6
        color: page.colors["bg-elevated"]
        Text {
            anchors.left: parent.left
            anchors.leftMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Mentions")
            color: page.colors["text-primary"]
            font.pixelSize: 14
            font.weight: Font.DemiBold
        }
        NativeIconButton {
            id: closeButton
            objectName: "mentionsCloseButton"
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            colors: page.colors
            asset: "close.svg"
            glyphSize: 16
            help: qsTr("Close mentions")
            onClicked: page.controller.closeMentions()
        }
    }

    // Segmented All/Unread filter. Reference: bar px-3 py-2 with a
    // border-border-subtle bottom rule; inline-flex gap-0.5 rounded-lg bg-bg
    // p-0.5 pills; buttons h-7 px-3 text-xs font-medium rounded-md.
    Rectangle {
        id: filterBar
        objectName: "mentionsFilterBar"
        anchors.top: header.bottom
        width: parent.width
        height: 8 + 28 + 8
        color: "transparent"
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: page.colors["border-subtle"]
        }
        Rectangle {
            id: pillGroup
            objectName: "mentionsFilterGroup"
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            height: 28 + 4
            radius: 8
            color: page.colors["bg"]
            Accessible.role: Accessible.Grouping
            Accessible.name: qsTr("Filter mentions")
            Row {
                x: 2; y: 2
                spacing: 2
                Repeater {
                    model: [
                        { id: "all", label: qsTr("All") },
                        { id: "unread", label: qsTr("Unread") }
                    ]
                    delegate: Button {
                        id: pill
                        required property var modelData
                        objectName: "mentionsFilter" + modelData.id.charAt(0).toUpperCase()
                            + modelData.id.slice(1) + "Button"
                        checkable: true
                        checked: page.filter === modelData.id
                        Accessible.ignored: false
                        hoverEnabled: true
                        height: 28
                        leftPadding: 12; rightPadding: 12
                        font.pixelSize: 12
                        font.weight: Font.Medium
                        contentItem: Text {
                            text: pill.modelData.label
                            textFormat: Text.PlainText
                            color: pill.checked ? page.colors["text-primary"]
                                                : page.colors["text-muted"]
                            font: pill.font
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: 6
                            color: pill.checked ? page.colors["card-bg"]
                                : pill.hovered ? page.colors["card-bg-active"] : "transparent"
                        }
                        onClicked: page.controller.selectMentionsFilter(modelData.id)
                    }
                }
            }
        }
    }

    Text {
        id: loadingText
        objectName: "mentionsLoadingText"
        visible: page.visible && page.loading && page.rows.length === 0
        anchors.top: filterBar.bottom
        anchors.topMargin: 16
        anchors.left: parent.left
        anchors.leftMargin: 16
        text: qsTr("Loading…")
        color: page.colors["text-muted"]
        font.pixelSize: 13
    }

    Rectangle {
        id: errorBox
        objectName: "mentionsErrorBox"
        visible: page.visible && page.error && page.rows.length === 0
        anchors.top: filterBar.bottom
        anchors.topMargin: 12
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        radius: 8
        border.width: 1
        border.color: page.colors["border-subtle"]
        color: "transparent"
        implicitHeight: errorColumn.implicitHeight + 24
        Column {
            id: errorColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 12; anchors.rightMargin: 12
            spacing: 8
            Text {
                text: qsTr("Could not load activity messages.")
                color: page.colors["notice-base"]
                font.pixelSize: 13
                wrapMode: Text.WordWrap
                width: parent.width
            }
            MentionAction {
                objectName: "mentionsRetryButton"
                text: qsTr("Retry")
                onClicked: page.controller.retryMentions()
            }
        }
    }

    Text {
        id: emptyText
        objectName: "mentionsEmptyText"
        visible: page.visible && !page.loading && !page.error && page.rows.length === 0
        anchors.top: filterBar.bottom
        anchors.topMargin: 16
        anchors.left: parent.left
        anchors.leftMargin: 16
        text: page.filter === "unread" ? qsTr("No unread mentions") : qsTr("No mentions yet")
        color: page.colors["text-muted"]
        font.pixelSize: 13
    }

    MentionAction {
        id: refreshError
        objectName: "mentionsRefreshError"
        visible: page.visible && page.error && page.rows.length > 0
        anchors.top: filterBar.bottom
        anchors.topMargin: 8
        anchors.left: parent.left
        anchors.leftMargin: 12
        text: qsTr("Could not refresh activity messages.") + " " + qsTr("Retry")
        onClicked: page.controller.retryMentions()
    }

    MentionAction {
        id: paginationError
        objectName: "mentionsPaginationError"
        visible: page.visible && page.paginationError
        anchors.top: refreshError.visible ? refreshError.bottom : filterBar.bottom
        anchors.topMargin: 8
        anchors.left: parent.left
        anchors.leftMargin: 12
        text: qsTr("Could not load older messages.") + " " + qsTr("Retry")
        onClicked: page.controller.loadMoreMentions()
    }

    // Bottom-edge pagination identical to the reference: fire once at 64 px
    // from the bottom edge (ACTIVITY_BOTTOM_PAGINATION_THRESHOLD_PX), re-arm
    // after moving past 96 px (…_REARM_THRESHOLD_PX), activity-page.ui.tsx:46-48,518-555.
    ListView {
        id: mentionList
        objectName: "mentionList"
        anchors.top: paginationError.visible ? paginationError.bottom : filterBar.bottom
        anchors.topMargin: 4
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        visible: page.rows.length > 0
        clip: true
        spacing: 0
        model: page.rows
        property bool paginationArmed: true
        function checkPagination() {
            if (contentHeight <= height || contentY < 0) { paginationArmed = true; return }
            const distance = contentHeight - height - contentY
            if (distance > 96)
                paginationArmed = true
            if (paginationArmed && distance <= 64 && page.canLoadMore && !page.loading) {
                paginationArmed = false
                page.controller.loadMoreMentions()
            }
        }
        onContentYChanged: mentionList.checkPagination()
        onHeightChanged: mentionList.checkPagination()
        onCountChanged: Qt.callLater(() => mentionList.checkPagination())
        onMovementEnded: mentionList.checkPagination()

        delegate: Rectangle {
            id: card
            required property var modelData
            objectName: "mention-card-" + modelData.id
            width: mentionList.width
            color: card.modelData.unread ? page.colors["card-bg"] : "transparent"
            radius: 8
            implicitHeight: 24 + cardColumn.implicitHeight
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: page.controller.openTopic(card.modelData.topic, card.modelData.stream)
            }
            // Unread dot slot: size-2 slot at mt-1.5 next to an 8 px
            // rounded-full dot tinted sidebar-unread
            // (activity-message-card.ui.tsx:70-77).
            Rectangle {
                id: dotSlot
                objectName: "mention-unread-dot-" + card.modelData.id
                width: 8; height: 8
                radius: 4
                color: page.colors["sidebar-unread"]
                visible: Boolean(card.modelData.unread)
                anchors.left: parent.left
                anchors.leftMargin: 12
                anchors.top: parent.top
                anchors.topMargin: 6 + 12
                Accessible.name: qsTr("Unread mention")
            }
            Column {
                id: cardColumn
                anchors.left: dotSlot.right
                anchors.leftMargin: 8
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.top: parent.top
                anchors.topMargin: 12
                spacing: 0
                Item {
                    width: parent.width
                    height: 15
                    Text {
                        id: timeLabel
                        objectName: "mention-time-" + card.modelData.id
                        anchors.left: parent.left
                        text: String(card.modelData.time)
                        color: page.colors["text-muted"]
                        font.pixelSize: 11
                    }
                    Text {
                        objectName: "mention-meta-" + card.modelData.id
                        anchors.right: parent.right
                        anchors.left: timeLabel.right
                        anchors.leftMargin: 8
                        horizontalAlignment: Text.AlignRight
                        elide: Text.ElideRight
                        text: Boolean(card.modelData.direct)
                            ? [qsTr("Private"), String(card.modelData.streamName)].filter(Boolean).join(" · ")
                            : [String(card.modelData.streamName), String(card.modelData.topicName)]
                                .filter(Boolean).join(" · ")
                        color: page.colors["text-muted"]
                        font.pixelSize: 11
                    }
                }
                Text {
                    objectName: "mention-author-" + card.modelData.id
                    topPadding: 2
                    text: String(card.modelData.author)
                    color: page.colors["sidebar-sender"]
                    font.pixelSize: 12
                    font.weight: card.modelData.unread ? Font.Medium : Font.Normal
                    elide: Text.ElideRight
                    width: parent.width
                }
                Text {
                    objectName: "mention-preview-" + card.modelData.id
                    topPadding: 4
                    text: String(card.modelData.preview)
                    color: page.colors["text-primary"]
                    font.pixelSize: 14
                    wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    width: parent.width
                    font.weight: card.modelData.unread ? Font.Medium : Font.Normal
                }
            }
        }
    }
}
