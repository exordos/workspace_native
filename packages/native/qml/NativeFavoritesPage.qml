pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

// Favorites self-chat page states (issue #70, docs/NATIVE_DESKTOP_FAVORITES_SELF_CHAT.md).
// Reference contract (workspace_ui 61dacef78dab2f5d0be76f283bebf49fe29b84af):
// pages/chat/chat-page.ui.tsx:21-72 renders ChatFavoritesHeader plus, when
// ready, the conversation itself; this page only carries the loading and error
// states - the ready conversation is the ordinary conversation column with the
// favorites header presentation.
//   loading: Spinner size sm + t("app.loading") centered in a p-4 flex row
//            with gap-2 and text-sm text-text-muted (chat-page.ui.tsx:50-54).
//   error:   max-w-sm rounded-lg border border-border-subtle p-4 text-center
//            card with t("favorites.loadError") in text-sm text-notice-base
//            and a mt-3 px-3 py-1.5 text-sm text-accent t("app.retry") button
//            (chat-page.ui.tsx:56-65).
// The header is ChatFavoritesHeader (chat-header-favorites.ui.tsx:5-10): the
// shared 54 px elevated chat header with only the h1 title, text-base
// font-semibold text-text-primary - no call/search/panel actions.
FocusScope {
    id: page
    objectName: "nativeFavoritesPage"
    required property var colors
    required property var controller
    onVisibleChanged: { if (visible) forceActiveFocus() }
    Keys.onEscapePressed: page.controller.closeFavorites()

    Rectangle {
        id: header
        objectName: "favoritesStateHeader"
        anchors.top: parent.top
        width: parent.width
        height: 54
        radius: 6
        color: page.colors["bg-elevated"]
        Text {
            objectName: "favoritesStateTitle"
            anchors.left: parent.left
            anchors.leftMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Favorites")
            textFormat: Text.PlainText
            color: page.colors["text-primary"]
            font.pixelSize: 16
            font.weight: Font.DemiBold
            elide: Text.ElideRight
            width: Math.min(implicitWidth, parent.width - 40)
        }
    }

    // The loading branch: a small spinner and the app.loading label, centered
    // in the remaining column (flex items-center justify-center p-4 gap-2).
    Row {
        id: favoritesLoading
        objectName: "favoritesLoading"
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        visible: page.controller.favoritesStatus === "loading"
        spacing: 8
        BusyIndicator {
            objectName: "favoritesLoadingSpinner"
            running: favoritesLoading.visible
            width: 20
            height: 20
            anchors.verticalCenter: parent.verticalCenter
        }
        Text {
            objectName: "favoritesLoadingText"
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Loading…")
            textFormat: Text.PlainText
            font.pixelSize: 14
            color: page.colors["text-muted"]
        }
    }

    // The error branch: the bordered favorites.loadError card with the
    // app.retry action. The reference shows this copy regardless of the
    // transport detail, so no controller error text is rendered here.
    Rectangle {
        id: favoritesErrorCard
        objectName: "favoritesErrorCard"
        visible: page.controller.favoritesStatus === "error"
        anchors.centerIn: parent
        width: Math.min(384, parent.width - 32)
        height: favoritesErrorContent.implicitHeight + 32
        radius: 8
        border.width: 1
        border.color: page.colors["border-subtle"]
        color: "transparent"
        Column {
            id: favoritesErrorContent
            objectName: "favoritesErrorContent"
            anchors.centerIn: parent
            spacing: 12
            Text {
                objectName: "favoritesErrorText"
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("Could not open Favorites.")
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: 14
                color: page.colors["notice-base"]
            }
            Button {
                id: retryButton
                objectName: "favoritesRetryButton"
                anchors.horizontalCenter: parent.horizontalCenter
                topPadding: 6
                bottomPadding: 6
                leftPadding: 12
                rightPadding: 12
                hoverEnabled: true
                onClicked: page.controller.retryFavorites()
                contentItem: Text {
                    text: qsTr("Try again")
                    textFormat: Text.PlainText
                    font.pixelSize: 14
                    color: page.colors.accent
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: 6
                    color: retryButton.hovered ? page.colors["card-bg-active"] : "transparent"
                }
            }
        }
    }
}
