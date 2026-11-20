pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

// Project feed page (issue #38, docs/NATIVE_DESKTOP_FEED.md). Presentation
// only: the controller owns the fetch, marker pagination and row projection.
// Geometry measured from workspace_ui 61dacef78dab2f5d0be76f283bebf49fe29b84af
// feed-page.ui.tsx: header ChatChannelHeader with feed.title (line 339-344);
// state cards m-3 rounded-xl border border-border-subtle bg-bg-elevated/50
// px-4 py-3 text-sm (FEED_STATE_CARD_CLASS line 58-59), loading/empty
// text-text-muted and error text-notice-base (lines 346-353); list container
// px-3 pt-2 pb-3 with space-y-2 and overscroll contain (line 356); row
// rounded-xl border-border-subtle bg-bg-elevated/60 p-2.5 with an 8 px gap to
// the action cluster (FEED_ROW_CLASS line 60-61); time pill rounded-full bg-bg
// px-2 py-0.5 10 px font-medium text-text-muted and context pill same geometry
// text-text-secondary truncate (lines 386-392); sender line mt-1 text-xs
// text-sidebar-sender (line 393); summary mt-1.5 line-clamp-2 rounded-lg
// bg-bg/70 px-2.5 py-2 text-sm leading-snug text-text-primary (line 398);
// action cluster bg-bg/60 rounded-lg p-1 opacity 0.7 to 1.0 on row hover with
// two 16 px icon buttons (lines 405-423).
// Pagination contract (feed-scroll.lib.ts, feed-page.ui.tsx:55-57, 177-208):
// older page fires at scrollTop <= 64 px, one request until re-armed past
// 96 px, never while one is in flight; near-bottom threshold 80 px drives the
// floating scroll-to-bottom button; the initial load anchors at the bottom and
// a prepended page restores scrollTop += delta(scrollHeight)
// (feed-scroll.lib.ts:16-31, scroll-prepend-anchor.lib.ts).
FocusScope {
    id: page
    objectName: "nativeFeedPage"
    required property var controller
    required property var colors
    readonly property var feedState: controller.feedPage
    readonly property var feedRows: feedState.rows || []
    readonly property bool loading: Boolean(feedState.loading)
    readonly property bool loadingMore: Boolean(feedState.loadingMore)
    readonly property bool error: Boolean(feedState.error)
    readonly property bool hasRows: feedRows.length > 0
    // Scroll-prepend anchor (scroll-prepend-anchor.lib.ts): captured before an
    // older page is requested, applied when the response has merged in.
    property real restoreY: -1
    property real restoreHeight: -1
    property bool prevLoadingMore: false
    property bool armed: true
    property bool initialAnchored: false
    // True while the settle timer is still walking the viewport to the bottom;
    // the top-pagination trigger must not fire from these programmatic moves.
    property bool anchorRunning: false
    onVisibleChanged: {
        if (visible)
            forceActiveFocus()
        else {
            restoreY = -1
            restoreHeight = -1
        }
    }
    Keys.onEscapePressed: page.controller.closeFeed()

    // feedState re-emits with controller.stateChanged; this is where an older
    // page landing restores the visual position (prepend anchor).
    onFeedStateChanged: {
        if (!page.visible) {
            page.restoreY = -1
            page.restoreHeight = -1
        }
        const more = Boolean(controller.feedPage.loadingMore)
        if (!page.prevLoadingMore) {
            page.prevLoadingMore = more
            return
        }
        if (more)
            return
        page.prevLoadingMore = false
        if (page.restoreHeight < 0)
            return
        // Prepend restore: keep the visual position stable while the merged
        // older rows incubate and polish their heights.
        prependRestoreTimer.ticks = 0
        prependRestoreTimer.lastHeight = -1
        prependRestoreTimer.restart()
    }

    Timer {
        id: prependRestoreTimer
        property int ticks: 0
        property real lastHeight: -1
        interval: 30
        repeat: true
        triggeredOnStart: true
        onTriggered: {
            if (page.restoreHeight < 0) {
                stop()
                return
            }
            const grown = feedList.contentHeight - page.restoreHeight
            ++ticks
            const settled = feedList.contentHeight == lastHeight
            lastHeight = feedList.contentHeight
            if (grown > 0)
                feedList.contentY = Math.max(0, page.restoreY + grown)
            if ((grown > 0 && settled) || ticks > 24) {
                page.restoreY = -1
                page.restoreHeight = -1
                stop()
            }
        }
    }

    // Same card geometry as the inbox page state cards (FEED_STATE_CARD_CLASS:
    // m-3 rounded-xl border-subtle bg-elevated/50 px-4 py-3 text-sm). The
    // error card additionally offers the retry action (exo-review: the card
    // displayed the failure but gave no way to recover through the UI).
    component FeedStateCard: Rectangle {
        id: stateCard
        required property string body
        required property string bodyColor
        property string actionLabel: ""
        property string actionName: ""
        signal actionTriggered()
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 12 // m-3
        radius: 12 // rounded-xl
        border.width: 1
        border.color: page.colors["border-subtle"]
        color: Qt.alpha(page.colors["bg-elevated"], 0.5)
        implicitHeight: stateLabel.implicitHeight + 24 // py-3
            + (actionButton.visible ? actionButton.height + 8 : 0)
        Text {
            id: stateLabel
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.topMargin: 12
            anchors.leftMargin: 16 // px-4
            anchors.rightMargin: 16
            text: stateCard.body
            color: stateCard.bodyColor
            font.pixelSize: 14 // text-sm
            wrapMode: Text.WordWrap
        }
        NativeButton {
            id: actionButton
            objectName: stateCard.actionName
            anchors.top: stateLabel.bottom
            anchors.topMargin: 8
            anchors.left: parent.left
            anchors.leftMargin: 16
            colors: page.colors
            text: stateCard.actionLabel
            visible: stateCard.actionLabel.length > 0
            implicitHeight: 32
            onClicked: stateCard.actionTriggered()
        }
    }

    // rounded-full bg-bg px-2 py-0.5 pill (time: 10 px font-medium
    // text-text-muted; context: same geometry, text-text-secondary, truncate;
    // feed-page.ui.tsx:386-392).
    component FeedPill: Rectangle {
        id: pill
        required property string body
        required property string bodyColor
        property string labelName: ""
        property real maxWidth: -1
        radius: height / 2 // rounded-full
        color: page.colors["bg"]
        implicitWidth: maxWidth >= 0 ? maxWidth : pillLabel.implicitWidth + 16 // px-2
        implicitHeight: pillLabel.implicitHeight + 4 // py-0.5
        width: implicitWidth
        height: implicitHeight
        Text {
            id: pillLabel
            objectName: pill.labelName
            x: 8
            y: (parent.height - height) / 2
            width: pill.maxWidth >= 0
                ? Math.max(0, pill.maxWidth - 16) : undefined
            text: pill.body
            textFormat: Text.PlainText
            color: pill.bodyColor
            font.pixelSize: 10 // text-[10px]
            font.weight: Font.Medium
            elide: Text.ElideRight
        }
    }

    Rectangle {
        id: header
        objectName: "feedHeader"
        anchors.top: parent.top
        width: parent.width
        height: 54
        radius: 6
        color: page.colors["bg-elevated"]
        Text {
            anchors.left: parent.left
            anchors.leftMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("All messages")
            color: page.colors["text-primary"]
            font.pixelSize: 14
            font.weight: Font.DemiBold
        }
        NativeIconButton {
            objectName: "feedCloseButton"
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            colors: page.colors
            asset: "close.svg"
            glyphSize: 16
            help: qsTr("Close feed")
            onClicked: page.controller.closeFeed()
        }
    }

    FeedStateCard {
        objectName: "feedLoadingText"
        visible: page.visible && page.loading && !page.hasRows
        anchors.top: header.bottom
        body: qsTr("Loading…")
        bodyColor: page.colors["text-muted"]
    }

    FeedStateCard {
        objectName: "feedErrorBox"
        visible: page.visible && page.error && !page.hasRows
        anchors.top: header.bottom
        body: qsTr("Failed to load messages")
        bodyColor: page.colors["notice-base"]
        actionLabel: qsTr("Retry")
        actionName: "feedRetryButton"
        onActionTriggered: page.controller.retryFeed()
    }

    FeedStateCard {
        objectName: "feedEmptyText"
        visible: page.visible && !page.loading && !page.error && !page.hasRows
        anchors.top: header.bottom
        body: qsTr("No messages yet")
        bodyColor: page.colors["text-muted"]
    }

    ListView {
        id: feedList
        objectName: "feedList"
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        visible: page.hasRows
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        // Keep a whole page of rows instantiated so the content height
        // converges before the initial bottom anchor settles (the reference
        // DOM list measures all rows at once).
        cacheBuffer: 8000
        spacing: 8 // space-y-2
        leftMargin: 12 // px-3
        rightMargin: 12
        topMargin: 8 // pt-2
        bottomMargin: 12 // pb-3
        // Stable positional model: delegates read the live row through the
        // page accessor so an in-flight click never sees a destroyed delegate.
        model: Array.from({length: page.feedRows.length}, function (_, i) { return i })
        // Near-bottom 80 px gate for the floating button
        // (FEED_BOTTOM_THRESHOLD_PX, feed-page.ui.tsx:56).
        readonly property bool nearBottom:
            contentHeight - (contentY + height) <= 80
        property bool glide: false
        Behavior on contentY {
            enabled: feedList.glide
            NumberAnimation { duration: 200; easing.type: Easing.OutCubic }
        }
        Timer {
            id: glideTimer
            interval: 230
            onTriggered: {
                feedList.glide = false
                if (page.visible && page.hasRows) page.anchorToBottom()
            }
        }

        // Top pagination: one request at <= 64 px until re-armed past 96 px
        // (feed-scroll.lib.ts:7-24, feed-page.ui.tsx:210-235).
        onContentYChanged: {
            if (contentY > 96)
                page.armed = true
            const state = controller.feedPage
            if (page.armed && !page.anchorRunning && contentY <= 64
                && Boolean(state.canLoadMore) && !Boolean(state.loadingMore)) {
                page.armed = false
                page.restoreY = feedList.contentY
                page.restoreHeight = feedList.contentHeight
                page.controller.loadMoreFeed()
            }
        }

        delegate: Rectangle {
            id: feedRow
            required property var modelData
            readonly property var row: page.feedRows[Number(modelData)] || ({})
            readonly property string rowId: String(row.id || "")
            objectName: "feed-row-" + rowId
            width: ListView.view.width
            height: rowColumn.implicitHeight + 20 // p-2.5
            radius: 12 // rounded-xl
            border.width: 1
            border.color: page.colors["border-subtle"]
            color: Qt.alpha(page.colors["bg-elevated"], 0.6) // bg-bg-elevated/60

            Row {
                anchors.fill: parent
                anchors.margins: 10 // p-2.5
                spacing: 8 // gap-2

                Column {
                    id: rowColumn
                    width: parent.width - actionsCluster.width - 8
                    spacing: 0
                    Row {
                        id: pillsRow
                        width: rowColumn.width
                        spacing: 8 // justify-between gap-2
                        FeedPill {
                            id: timePill
                            labelName: "feed-time-" + feedRow.rowId
                            body: String(feedRow.row.time || "")
                            bodyColor: page.colors["text-muted"]
                        }
                        FeedPill {
                            labelName: "feed-context-" + feedRow.rowId
                            body: String(feedRow.row.context || "")
                            bodyColor: page.colors["text-secondary"]
                            maxWidth: pillsRow.width - timePill.width - 8
                        }
                    }
                    Text {
                        objectName: "feed-sender-" + feedRow.rowId
                        topPadding: 4 // mt-1
                        width: rowColumn.width
                        text: String(feedRow.row.sender || "")
                        // Sender, context and preview are attacker-controllable
                        // message fields: never let automatic format detection
                        // interpret them as rich text (exo-review finding on the
                        // preview Text; the reference renders the clamped plain
                        // summary text, never markdown/HTML).
                        textFormat: Text.PlainText
                        color: page.colors["sidebar-sender"] // text-xs text-sidebar-sender
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                    Item {
                        width: rowColumn.width
                        height: 6 // mt-1.5
                    }
                    Rectangle {
                        id: summaryBlock
                        width: rowColumn.width
                        radius: 8 // rounded-lg
                        color: Qt.alpha(page.colors["bg"], 0.7) // bg-bg/70
                        implicitHeight: summaryLabel.height + 16 // py-2
                        Text {
                            id: summaryLabel
                            objectName: "feed-summary-" + feedRow.rowId
                            x: 10 // px-2.5
                            y: 8 // py-2
                            width: summaryBlock.width - 20
                            text: String(feedRow.row.preview || "")
                            // The preview is attacker-controlled message
                            // content: render it plain so automatic format
                            // detection can never load a remote image or parse
                            // markup from it (exo-review finding; the reference
                            // feeds the same clamped plain summary text).
                            textFormat: Text.PlainText
                            color: page.colors["text-primary"]
                            font.pixelSize: 14 // text-sm
                            lineHeight: 1.375 // leading-snug
                            lineHeightMode: Text.ProportionalHeight
                            maximumLineCount: 2 // line-clamp-2
                            wrapMode: Text.WordWrap
                            elide: Text.ElideRight
                        }
                    }
                }

                // Row actions cluster: bg-bg/60 rounded-lg p-1 opacity 0.7 ->
                // 1.0 on row hover (feed-page.ui.tsx:405-423). The forward flow
                // does not exist on master yet, so the button stays gated
                // pending roadmap issue #7 (0.10.0 multiple forward).
                Rectangle {
                    id: actionsCluster
                    width: actionsRow.width + 8 // p-1
                    height: actionsRow.height + 8
                    radius: 8 // rounded-lg
                    color: Qt.alpha(page.colors["bg"], 0.6)
                    opacity: rowHover.containsMouse ? 1.0 : 0.7
                    Row {
                        id: actionsRow
                        anchors.centerIn: parent
                        spacing: 4 // gap-1
                        NativeIconButton {
                            objectName: "feed-open-" + feedRow.rowId
                            colors: page.colors
                            asset: "new_window.svg"
                            glyphSize: 16
                            help: qsTr("Open in chat")
                            onClicked: page.controller.openFeedRow(
                                String(feedRow.row.stream || ""), String(feedRow.row.topic || ""))
                        }
                        NativeIconButton {
                            objectName: "feed-forward-" + feedRow.rowId
                            colors: page.colors
                            asset: "send.svg"
                            glyphSize: 16
                            help: qsTr("Forward")
                            enabled: false // gated until issue #7 ships the forward flow
                        }
                    }
                }
            }

            MouseArea {
                id: rowHover
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                // Row click routes to the message topic
                // (feed-page.ui.tsx:263 pin at feed-page.ui.test.tsx).
                onClicked: page.controller.openFeedRow(
                    String(feedRow.row.stream || ""), String(feedRow.row.topic || ""))
            }
        }
    }

    // Initial load anchors at the newest row (feed-page.ui.tsx:161-172).
    // The anchor is re-applied while delegates incubate so a long first page
    // still lands at the bottom once its content height is known.
    onHasRowsChanged: {
        if (!page.hasRows) {
            page.initialAnchored = false
            return
        }
        if (!page.initialAnchored) {
            page.initialAnchored = true
            page.anchorToBottom()
        }
    }

    function anchorToBottom() {
        page.anchorRunning = true
        initialAnchorTimer.ticks = 0
        initialAnchorTimer.ticks_lastHeight = -1
        initialAnchorTimer.restart()
    }

    // The first page anchors at the bottom; delegates incubate in passes, so
    // the anchor is re-applied until the settled content height reaches the
    // bottom (bounded to a few frames).
    Timer {
        id: initialAnchorTimer
        property int ticks: 0
        interval: 30
        repeat: true
        triggeredOnStart: true
        onTriggered: {
            // positionViewAtEnd resolves asynchronously against incubating
            // delegate heights, so the anchor writes contentY directly to the
            // current end position and re-applies while the height settles.
            const endY = Math.max(0, feedList.contentHeight - feedList.height)
            feedList.contentY = endY
            const settled = feedList.contentHeight == ticks_lastHeight
            ticks_lastHeight = feedList.contentHeight
            ++ticks
            if (!page.hasRows || !page.visible || ticks > 24
                || (settled && Math.abs(feedList.contentY - endY) <= 1)) {
                page.anchorRunning = false
                stop()
            }
        }
        property real ticks_lastHeight: -1
    }

    // FloatingLoadingOverlay while loading more (feed-page.ui.tsx:426).
    BusyIndicator {
        objectName: "feedLoadingMoreOverlay"
        visible: page.loadingMore
        running: visible
        anchors.top: feedList.top
        anchors.topMargin: 8
        anchors.horizontalCenter: feedList.horizontalCenter
    }

    // FloatingScrollToBottomButton: hidden within 80 px of the bottom,
    // smooth-scrolls on click (feed-page.ui.tsx:427, 306-311).
    Rectangle {
        id: scrollBottomButton
        objectName: "feedScrollToBottom"
        visible: page.hasRows && !feedList.nearBottom
        width: 40
        height: 40
        radius: 20
        color: page.colors["bg-elevated"]
        border.width: 1
        border.color: page.colors["border-subtle"]
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 16
        Image {
            anchors.centerIn: parent
            source: "qrc:/workspace/icons/chevron-down.svg"
            sourceSize.width: 18
            sourceSize.height: 18
            asynchronous: false
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                // The user's jump supersedes the pending prepend anchor.
                prependRestoreTimer.stop()
                page.restoreY = -1
                page.restoreHeight = -1
                feedList.glide = true
                glideTimer.restart()
                // Direct write (not positionViewAtEnd) so the final position
                // is exact against the settled content height.
                feedList.contentY = Math.max(0, feedList.contentHeight - feedList.height)
            }
        }
    }
}
