pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Activity Marked (Starred) messages page (issue #41). Presentation only:
// the controller owns the starred collection, the marker pagination and the
// message-route jump. Geometry measured from workspace_ui
// 61dacef78dab2f5d0be76f283bebf49fe29b84af:
// list container activity-message-list.ui.tsx (p-2 8 px, space-y-1 4 px);
// card activity-message-card.ui.tsx (p-3 12 px, rounded-lg 8 px, hover
// bg-card-bg, 11 px meta text-text-muted, 12 px sender text-sidebar-sender,
// 14 px two-line preview text-text-primary, hover actions opacity 0 to 1
// with 16 px icons; starred renders no unread dot slot at all).
FocusScope {
    id: page
    objectName: "nativeStarredPage"
    required property var controller
    required property var colors
    property bool loadActive: visible
    // The card forward action opens the Main.qml forward flow with [uuid]
    // (activity-page.ui.tsx:393-397 openWorkspaceForward, test 336).
    signal forwardRequested(string messageUuid)
    readonly property var starredState: controller.starredPage
    readonly property var rows: starredState.rows || []
    readonly property bool loading: Boolean(starredState.loading)
    readonly property bool error: Boolean(starredState.error)
    readonly property bool paginationError: Boolean(starredState.paginationError)
    readonly property bool canLoadMore: Boolean(starredState.canLoadMore)
    onVisibleChanged: { if (visible) forceActiveFocus(); }
    Keys.onEscapePressed: page.controller.closeStarred()

    component StarredAction: Button {
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
        objectName: "starredHeader"
        anchors.top: parent.top
        width: parent.width
        height: 54
        radius: 6
        color: page.colors["bg-elevated"]
        Text {
            anchors.left: parent.left
            anchors.leftMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Marked messages")
            color: page.colors["text-primary"]
            font.pixelSize: 14
            font.weight: Font.DemiBold
        }
        NativeIconButton {
            id: closeButton
            objectName: "starredCloseButton"
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            colors: page.colors
            asset: "close.svg"
            glyphSize: 16
            help: qsTr("Close marked messages")
            onClicked: page.controller.closeStarred()
        }
    }

    Text {
        id: loadingText
        objectName: "starredLoadingText"
        visible: page.visible && page.loading && page.rows.length === 0
        anchors.top: header.bottom
        anchors.topMargin: 16
        anchors.left: parent.left
        anchors.leftMargin: 16
        text: qsTr("Loading…")
        color: page.colors["text-muted"]
        font.pixelSize: 13
    }

    Rectangle {
        id: errorBox
        objectName: "starredErrorBox"
        visible: page.visible && page.error && page.rows.length === 0
        anchors.top: header.bottom
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
            StarredAction {
                objectName: "starredRetryButton"
                text: qsTr("Retry")
                onClicked: page.controller.retryStarred()
            }
        }
    }

    Text {
        id: emptyText
        objectName: "starredEmptyText"
        visible: page.visible && !page.loading && !page.error && page.rows.length === 0
        anchors.top: header.bottom
        anchors.topMargin: 16
        anchors.left: parent.left
        anchors.leftMargin: 16
        text: qsTr("No messages here yet...")
        color: page.colors["text-muted"]
        font.pixelSize: 13
    }

    StarredAction {
        id: refreshError
        objectName: "starredRefreshError"
        visible: page.visible && page.error && page.rows.length > 0
        anchors.top: header.bottom
        anchors.topMargin: 8
        anchors.left: parent.left
        anchors.leftMargin: 12
        text: qsTr("Could not refresh activity messages.") + " " + qsTr("Retry")
        onClicked: page.controller.retryStarred()
    }

    StarredAction {
        id: paginationError
        objectName: "starredPaginationError"
        visible: page.visible && page.paginationError
        anchors.top: refreshError.visible ? refreshError.bottom : header.bottom
        anchors.topMargin: 8
        anchors.left: parent.left
        anchors.leftMargin: 12
        text: qsTr("Could not load older messages.") + " " + qsTr("Retry")
        onClicked: page.controller.loadMoreStarred()
    }

    // Bottom-edge pagination identical to the reference: fire once at 64 px
    // from the bottom edge (ACTIVITY_BOTTOM_PAGINATION_THRESHOLD_PX), re-arm
    // after moving past 96 px (…_REARM_THRESHOLD_PX), activity-page.ui.tsx:46-48,518-555.
    // Initial scroll stays at the top: the newest-first list opens at the
    // latest messages (activity-page.ui.tsx:367-378, test 1066).
    ListView {
        id: starredList
        objectName: "starredList"
        // Stay below every visible error control: the retry controls render
        // above the list in the child order, and anchoring under the header
        // would let the first card cover and intercept the retry clicks.
        anchors.top: paginationError.visible ? paginationError.bottom
            : refreshError.visible ? refreshError.bottom : header.bottom
        anchors.topMargin: 8 // list container p-2 (activity-message-list.ui.tsx:27)
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        anchors.bottomMargin: 8
        visible: page.rows.length > 0
        clip: true
        spacing: 4 // space-y-1
        model: page.rows
        property bool paginationArmed: true
        function checkPagination() {
            if (!page.visible)
                return
            if (contentHeight <= height || contentY < 0) {
                paginationArmed = true
                // A page that cannot overflow the viewport never reaches the
                // bottom edge by scrolling, and there is no other load-more
                // control, so the marker chain continues on its own while a
                // marker remains (bounded by canLoadMore and the
                // pagination-error stop).
                if (contentHeight > 0 && page.canLoadMore && !page.paginationError)
                    page.controller.loadMoreStarred()
                return
            }
            const distance = contentHeight - height - contentY
            if (distance > 96)
                paginationArmed = true
            if (paginationArmed && distance <= 64 && page.canLoadMore && !page.loading) {
                paginationArmed = false
                page.controller.loadMoreStarred()
            }
        }
        // The delegates and the viewport can report different intermediate
        // sizes during one layout pass. Only the settled geometry can tell
        // whether a page really needs another marker.
        onVisibleChanged: if (visible) Qt.callLater(starredList.checkPagination)
        onContentYChanged: Qt.callLater(starredList.checkPagination)
        onContentHeightChanged: Qt.callLater(starredList.checkPagination)
        onHeightChanged: Qt.callLater(starredList.checkPagination)
        onCountChanged: Qt.callLater(starredList.checkPagination)
        onMovementEnded: Qt.callLater(starredList.checkPagination)

        delegate: Rectangle {
            id: card
            required property var modelData
            objectName: "starred-card-" + card.modelData.id
            width: starredList.width
            radius: 8
            color: cardHover.containsMouse ? page.colors["card-bg"] : "transparent"
            implicitHeight: 24 + cardColumn.implicitHeight
            MouseArea {
                id: cardHover
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                // The whole card button opens the message route
                // (activity-page.ui.tsx:399-406 handleWorkspaceMessageClick,
                // test 315).
                onClicked: page.controller.openStarredMessage(String(card.modelData.id))
            }
            Column {
                id: cardColumn
                anchors.left: parent.left
                anchors.leftMargin: 12
                anchors.right: actionsZone.left
                anchors.rightMargin: 8
                anchors.top: parent.top
                anchors.topMargin: 12
                spacing: 0
                Item {
                    width: parent.width
                    height: 15
                    Text {
                        id: timeLabel
                        objectName: "starred-time-" + card.modelData.id
                        anchors.left: parent.left
                        text: String(card.modelData.time)
                        color: page.colors["text-muted"]
                        font.pixelSize: 11
                    }
                    Text {
                        objectName: "starred-meta-" + card.modelData.id
                        anchors.right: parent.right
                        anchors.left: timeLabel.right
                        anchors.leftMargin: 8
                        horizontalAlignment: Text.AlignRight
                        elide: Text.ElideRight
                        text: Boolean(card.modelData.direct)
                            ? [qsTr("Private"), String(card.modelData.streamName)].filter(Boolean).join(" · ")
                            : [String(card.modelData.streamName), String(card.modelData.topicName)]
                                .filter(Boolean).join(" · ")
                        // Server-provided names never get rich-text markup
                        // treatment: no attacker-controlled link or image load.
                        textFormat: Text.PlainText
                        color: page.colors["text-muted"]
                        font.pixelSize: 11
                    }
                }
                Text {
                    objectName: "starred-author-" + card.modelData.id
                    topPadding: 2
                    text: String(card.modelData.author)
                    // Directory names are attacker-visible input: never let a
                    // crafted name be parsed as rich text (image/link fetch).
                    textFormat: Text.PlainText
                    color: page.colors["sidebar-sender"]
                    font.pixelSize: 12
                    elide: Text.ElideRight
                    width: parent.width
                }
                Text {
                    objectName: "starred-preview-" + card.modelData.id
                    topPadding: 4
                    text: String(card.modelData.preview)
                    textFormat: Text.PlainText
                    color: page.colors["text-primary"]
                    font.pixelSize: 14
                    wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    width: parent.width
                }
            }
            Item {
                id: actionsZone
                anchors.top: parent.top
                anchors.topMargin: 12
                anchors.right: parent.right
                anchors.rightMargin: 8
                width: actionsRow.width
                height: actionsRow.height
                // Hover actions (activity-message-card.ui.tsx:106-128):
                // opacity 0 to 1 with newWindow (open in chat) and send
                // (forward) 16 px icons; starred renders no unread dot.
                opacity: cardHover.containsMouse || actionsHover.containsMouse ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 150 } }
                Row {
                    id: actionsRow
                    spacing: 4
                    NativeIconButton {
                        objectName: "starred-open-" + card.modelData.id
                        colors: page.colors
                        width: 24; height: 24
                        cornerRadius: 4
                        asset: "new_window.svg"
                        glyphSize: 16
                        help: qsTr("Open in chat")
                        onClicked: page.controller.openStarredMessage(String(card.modelData.id))
                    }
                    NativeIconButton {
                        objectName: "starred-forward-" + card.modelData.id
                        colors: page.colors
                        width: 24; height: 24
                        cornerRadius: 4
                        asset: "send.svg"
                        glyphSize: 16
                        help: qsTr("Forward")
                        // Forward is disabled for non-markdown payloads
                        // (card lines 116-128: disabled when kind !== markdown).
                        enabled: Boolean(card.modelData.markdown)
                        onClicked: page.forwardRequested(String(card.modelData.id))
                    }
                }
                // Click-through hover guard above the buttons: it keeps the
                // cluster at opacity 1 while the pointer sits on a button
                // (group-hover semantics) without stealing the button clicks.
                MouseArea {
                    id: actionsHover
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.NoButton
                }
            }
        }
    }
}
