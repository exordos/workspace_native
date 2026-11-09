pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Presentation only: the owner-bound controller supplies a bounded page and
// owns synchronization, exact-UUID navigation and conflict/deletion actions.
FocusScope {
    id: page
    objectName: "nativeDraftsPage"
    required property var controller
    required property var colors
    property bool loadActive: visible
    readonly property var state: controller.draftPage
    readonly property var rows: state.rows || []
    readonly property bool loading: Boolean(state.loading)
    readonly property string error: String(state.error || "")
    onVisibleChanged: { if (visible) forceActiveFocus(); }
    Keys.onEscapePressed: page.controller.closeDrafts()

    function statusText(row) {
        switch (String(row.phase)) {
        case "local": return qsTr("Waiting to save");
        case "saving": return qsTr("Saving");
        case "failed": return Boolean(row.consumed) ? qsTr("Could not delete") : qsTr("Could not save");
        case "conflict": return qsTr("Conflict: remote version was kept");
        case "deleting": return qsTr("Deleting");
        default: return "";
        }
    }

    component DraftAction: Button {
        id: action
        property bool destructive: false
        implicitWidth: contentItem.implicitWidth + 16
        implicitHeight: contentItem.implicitHeight + 8
        leftPadding: 8; rightPadding: 8
        topPadding: 4; bottomPadding: 4
        font.pixelSize: 11
        hoverEnabled: true
        contentItem: Text {
            text: action.text
            textFormat: Text.PlainText
            color: page.colors[action.destructive ? "danger" : "text-primary"]
            opacity: action.enabled ? 1 : 0.5
            font: action.font
            lineHeightMode: Text.FixedHeight
            lineHeight: 16.5
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: 4
            color: action.hovered ? page.colors[action.destructive ? "bg-elevated" : "card-bg"]
                : action.destructive ? "transparent" : page.colors["bg-elevated"]
            border.width: action.visualFocus ? 1 : 0
            border.color: page.colors.accent
        }
    }

    Rectangle {
        id: header
        objectName: "draftsHeader"
        anchors.top: parent.top
        width: parent.width
        height: 54
        radius: 6
        color: page.colors["bg-elevated"]
        Text {
            anchors.left: parent.left
            anchors.leftMargin: 20
            anchors.right: headerActions.left
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Drafts")
            textFormat: Text.PlainText
            font.pixelSize: 14
            font.weight: Font.DemiBold
            color: page.colors["text-primary"]
            elide: Text.ElideRight
        }
        Row {
            id: headerActions
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
            NativeButton {
                objectName: "draftsRefreshButton"
                colors: page.colors
                implicitHeight: 32
                verticalPadding: 6
                font.pixelSize: 12
                text: qsTr("Refresh")
                enabled: !page.loading
                onClicked: page.controller.refreshDrafts()
            }
            NativeIconButton {
                objectName: "draftsCloseButton"
                colors: page.colors
                width: 32; height: 32
                glyphSize: 18
                asset: "close.svg"
                help: qsTr("Close drafts")
                onClicked: page.controller.closeDrafts()
            }
        }
    }

    Text {
        id: loadError
        objectName: "draftsLoadError"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        anchors.topMargin: 12
        visible: page.rows.length > 0 && page.error.length > 0
        height: visible ? implicitHeight : 0
        text: page.error
        textFormat: Text.PlainText
        font.pixelSize: 12
        color: page.colors["text-muted"]
        wrapMode: Text.Wrap
        Accessible.role: Accessible.AlertMessage
    }

    Text {
        objectName: "draftsEmptyState"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.margins: 16
        visible: page.rows.length === 0
        text: page.loading ? qsTr("Loading…") : page.error.length ? page.error : qsTr("No drafts yet")
        textFormat: Text.PlainText
        font.pixelSize: 14
        color: page.colors["text-muted"]
        wrapMode: Text.Wrap
        Accessible.role: page.error.length ? Accessible.AlertMessage : Accessible.StatusBar
    }

    ListView {
        id: list
        objectName: "draftsList"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: page.rows.length > 0 && page.error.length > 0 ? loadError.bottom : header.bottom
        anchors.bottom: pagination.visible ? pagination.top : parent.bottom
        leftMargin: 12; rightMargin: 12
        topMargin: 12; bottomMargin: 12
        spacing: 8
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: page.rows
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        delegate: Rectangle {
            id: card
            required property var modelData
            readonly property string draftId: String(modelData.id)
            readonly property bool consumed: Boolean(modelData.consumed)
            readonly property bool conflict: Boolean(modelData.conflict) || String(modelData.phase) === "conflict"
            readonly property bool retryDeletion: consumed && String(modelData.phase) === "failed"
            readonly property bool busy: Boolean(modelData.busy)
            readonly property bool bottomActions: conflict || retryDeletion
            readonly property real bottomPadding: bottomActions ? Math.max(32, conflictActions.implicitHeight + 8) : 0
            readonly property bool actionFocus: openAction.activeFocus || deleteAction.activeFocus
            objectName: "draft-card-" + draftId
            width: Math.max(0, list.width - list.leftMargin - list.rightMargin)
            height: Math.max(34, content.implicitHeight + bottomPadding) + 26
            radius: 10
            readonly property color surface: page.colors["bg-elevated"]
            color: hover.hovered ? page.colors["card-bg"] : Qt.rgba(surface.r, surface.g, surface.b, 0.4)
            border.color: page.colors["border-subtle"]
            HoverHandler { id: hover }

            Rectangle {
                x: 13; y: 15
                width: 32; height: 32; radius: 16
                color: !Boolean(card.modelData.direct) && String(card.modelData.avatarColor || "").length
                    ? card.modelData.avatarColor : page.colors.bg
                border.color: page.colors["border-subtle"]
                NativeAvatarImage {
                    id: photo
                    anchors.fill: parent
                    reference: Boolean(card.modelData.direct) ? String(card.modelData.avatarUrl || "") : ""
                    loadActive: page.loadActive && page.visible
                }
                Text {
                    anchors.centerIn: parent
                    visible: !photo.replacesFallback
                    text: String(card.modelData.initial || card.modelData.streamName || "").slice(0, 1)
                    textFormat: Text.PlainText
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    color: page.colors["text-primary"]
                }
            }

            Button {
                id: openBody
                objectName: "draft-open-" + card.draftId
                x: 57; y: 13
                width: Math.max(0, card.width - x - 13 - (card.consumed ? 0 : rowActions.width + 12))
                height: content.implicitHeight + card.bottomPadding
                padding: 0
                enabled: !card.consumed
                hoverEnabled: true
                Accessible.name: qsTr("Edit draft") + ": " + String(card.modelData.streamName) + " · " + String(card.modelData.topicName)
                onClicked: page.controller.openDraft(card.draftId)
                background: Rectangle {
                    color: "transparent"
                    radius: 4
                    border.width: openBody.visualFocus ? 1 : 0
                    border.color: page.colors.accent
                }
                contentItem: Column {
                    id: content
                    spacing: 4
                    RowLayout {
                        width: parent.width
                        height: 20.5
                        spacing: 6
                        Rectangle {
                            implicitWidth: typeLabel.implicitWidth + 12
                            implicitHeight: 20.5
                            radius: 4
                            color: page.colors["bg-elevated"]
                            Text {
                                id: typeLabel
                                anchors.centerIn: parent
                                text: Boolean(card.modelData.direct) ? qsTr("Direct chat") : qsTr("Channel")
                                textFormat: Text.PlainText
                                color: page.colors["text-muted"]
                                font.pixelSize: 11
                                lineHeightMode: Text.FixedHeight; lineHeight: 16.5
                            }
                        }
                        Text {
                            Layout.maximumWidth: parent.width * 0.3
                            text: String(card.modelData.streamName || "")
                            textFormat: Text.PlainText
                            color: page.colors["text-muted"]
                            font.pixelSize: 11
                            lineHeightMode: Text.FixedHeight; lineHeight: 16.5
                            elide: Text.ElideRight
                        }
                        Text {
                            text: "·"
                            textFormat: Text.PlainText
                            color: page.colors["text-muted"]
                            font.pixelSize: 11
                        }
                        Text {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            text: String(card.modelData.topicName || "")
                            textFormat: Text.PlainText
                            color: page.colors["text-muted"]
                            font.pixelSize: 11
                            lineHeightMode: Text.FixedHeight; lineHeight: 16.5
                            elide: Text.ElideRight
                        }
                        Text {
                            text: String(card.modelData.time || "")
                            textFormat: Text.PlainText
                            color: page.colors["text-muted"]
                            font.pixelSize: 11
                            lineHeightMode: Text.FixedHeight; lineHeight: 16.5
                        }
                    }
                    Text {
                        objectName: "draft-preview-" + card.draftId
                        width: parent.width
                        text: String(card.modelData.preview || "")
                        textFormat: Text.PlainText
                        color: page.colors["text-primary"]
                        font.pixelSize: 14
                        lineHeightMode: Text.FixedHeight; lineHeight: 20
                        wrapMode: Text.Wrap
                        maximumLineCount: 3
                        elide: Text.ElideRight
                    }
                    Text {
                        objectName: "draft-status-" + card.draftId
                        width: parent.width
                        visible: String(card.modelData.phase) !== "saved"
                        text: page.statusText(card.modelData)
                        textFormat: Text.PlainText
                        color: page.colors["text-muted"]
                        font.pixelSize: 11
                        lineHeightMode: Text.FixedHeight; lineHeight: 16.5
                        wrapMode: Text.Wrap
                        Accessible.role: Accessible.StatusBar
                    }
                }
            }

            Row {
                id: rowActions
                x: card.width - width - 13; y: 13
                height: 24
                spacing: 4
                visible: !card.consumed
                opacity: hover.hovered || card.actionFocus ? 1 : 0
                NativeIconButton {
                    id: openAction
                    objectName: "draft-edit-" + card.draftId
                    colors: page.colors
                    asset: "new_window.svg"
                    width: 24; height: 24; glyphSize: 16
                    cornerRadius: 4
                    help: qsTr("Edit draft")
                    enabled: !card.consumed
                    onClicked: page.controller.openDraft(card.draftId)
                }
                NativeIconButton {
                    id: deleteAction
                    objectName: "draft-delete-" + card.draftId
                    visible: !card.conflict
                    colors: page.colors
                    asset: "delete.svg"
                    width: 24; height: 24; glyphSize: 16
                    cornerRadius: 4
                    help: qsTr("Delete draft")
                    iconColor: hovered ? page.colors.danger : page.colors["text-muted"]
                    enabled: !card.busy
                    onClicked: page.controller.deleteDraft(card.draftId)
                }
            }

            Flow {
                id: conflictActions
                x: 48
                y: card.height - 13 - height
                width: Math.max(0, card.width - x - 13)
                spacing: 6
                visible: card.bottomActions
                DraftAction {
                    objectName: "draft-use-server-" + card.draftId
                    text: qsTr("Use server version")
                    visible: card.conflict
                    enabled: !card.busy
                    onClicked: page.controller.resolveDraft(card.draftId, "server")
                }
                DraftAction {
                    objectName: "draft-keep-local-" + card.draftId
                    text: qsTr("Keep my version")
                    visible: card.conflict
                    enabled: !card.busy
                    onClicked: page.controller.resolveDraft(card.draftId, "local")
                }
                DraftAction {
                    objectName: "draft-delete-server-" + card.draftId
                    text: qsTr("Delete draft")
                    visible: card.conflict
                    destructive: true
                    enabled: !card.busy
                    onClicked: page.controller.resolveDraft(card.draftId, "delete")
                }
                DraftAction {
                    objectName: "draft-retry-delete-" + card.draftId
                    text: qsTr("Retry deletion")
                    visible: card.retryDeletion
                    enabled: !card.busy
                    onClicked: page.controller.deleteDraft(card.draftId)
                }
            }
        }
    }

    Row {
        id: pagination
        objectName: "draftsPagination"
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 12
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: 8
        visible: Boolean(page.state.canPrevious) || Boolean(page.state.canNext)
        NativeButton {
            objectName: "draftsPreviousButton"
            colors: page.colors
            text: qsTr("Previous")
            enabled: Boolean(page.state.canPrevious) && !page.loading
            onClicked: page.controller.previousDraftPage()
        }
        NativeButton {
            objectName: "draftsNextButton"
            colors: page.colors
            text: qsTr("Next")
            enabled: Boolean(page.state.canNext) && !page.loading
            onClicked: page.controller.nextDraftPage()
        }
    }
}
