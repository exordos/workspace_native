import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: dialog
    objectName: "cachedMessageSearch"
    required property var controller
    required property var colors
    property bool openedResult: false
    readonly property bool cacheAvailable: controller.cachedSearchAvailable
    onCacheAvailableChanged: if (visible && query.text.trim().length) debounce.restart()
    readonly property string owner: controller.ownerKey
    onOwnerChanged: if (visible) close()
    parent: Overlay.overlay
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    width: parent ? Math.min(520, parent.width - 32) : 520
    height: parent ? Math.min(420, parent.height - 40) : 420
    padding: 20
    modal: true
    dim: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onOpened: { openedResult = false; query.forceActiveFocus() }
    onClosed: {
        debounce.stop()
        query.clear()
        controller.clearCachedSearch()
    }
    Overlay.modal: Rectangle { color: "#80000000" }
    background: Rectangle {
        radius: 10
        color: dialog.colors["bg-elevated"]
        border.color: dialog.colors["border-subtle"]
    }
    function move(delta) {
        if (!results.count) return
        results.currentIndex = Math.max(0, Math.min(results.count - 1, results.currentIndex + delta))
        results.positionViewAtIndex(results.currentIndex, ListView.Contain)
    }
    function openCurrent() {
        const row = controller.cachedSearchResults[results.currentIndex]
        if (row && controller.openCachedSearchResult(row.id)) { openedResult = true; close() }
    }
    Timer {
        id: debounce
        interval: 180
        onTriggered: if (dialog.visible) dialog.controller.searchCachedMessages(query.text)
    }
    contentItem: ColumnLayout {
        objectName: "cachedMessageSearchContent"
        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("Search cached messages")
        spacing: 10
        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: qsTr("Search cached messages")
                color: dialog.colors["text-primary"]
                font.pixelSize: 18
                font.bold: true
            }
            NativeIconButton {
                colors: dialog.colors
                help: qsTr("Close")
                asset: "close.svg"
                onClicked: dialog.close()
            }
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("Searches history saved on this device for the selected account and project.")
            wrapMode: Text.WordWrap
            color: dialog.colors["text-secondary"]
            font.pixelSize: 13
        }
        NativeField {
            id: query
            objectName: "cachedMessageSearchQuery"
            Layout.fillWidth: true
            colors: dialog.colors
            placeholderText: qsTr("Search message text")
            Accessible.name: placeholderText
            maximumLength: 256
            onTextChanged: {
                dialog.controller.clearCachedSearch()
                if (text.trim().length && dialog.visible) debounce.restart()
                else debounce.stop()
            }
            Keys.onDownPressed: dialog.move(1)
            Keys.onUpPressed: dialog.move(-1)
            onAccepted: dialog.openCurrent()
        }
        Text {
            objectName: "cachedMessageSearchStatus"
            Layout.fillWidth: true
            visible: text.length > 0
            text: !query.text.trim().length ? qsTr("Enter text to search.")
                : !dialog.cacheAvailable || dialog.controller.cachedSearchState === "unavailable" ? qsTr("The local message cache is unavailable.")
                : dialog.controller.cachedSearchState !== "ready" ? qsTr("Searching…")
                : !results.count ? qsTr("No matches in cached messages.")
                : results.count >= 50 ? qsTr("Showing 50 matches. Refine your search for more specific results.") : ""
            color: dialog.colors["text-secondary"]
            wrapMode: Text.WordWrap
            font.pixelSize: 13
        }
        ListView {
            id: results
            objectName: "cachedMessageSearchResults"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: dialog.controller.cachedSearchResults
            onCountChanged: currentIndex = count ? 0 : -1
            ScrollBar.vertical: ScrollBar {}
            delegate: ItemDelegate {
                required property var modelData
                required property int index
                objectName: "cachedSearchResult-" + modelData.id
                width: ListView.view.width
                height: contentItem.implicitHeight + 14
                hoverEnabled: true
                Accessible.name: modelData.location + ". " + modelData.text
                background: Rectangle {
                    radius: 6
                    color: parent.hovered || results.currentIndex === parent.index
                        ? dialog.colors["card-bg-active"] : "transparent"
                }
                contentItem: Column {
                    spacing: 3
                    Text {
                        width: parent.width
                        text: modelData.text
                        textFormat: Text.PlainText
                        maximumLineCount: 2
                        wrapMode: Text.Wrap
                        elide: Text.ElideRight
                        color: dialog.colors["text-primary"]
                    }
                    Text {
                        width: parent.width
                        text: modelData.location + " · " + new Date(modelData.created).toLocaleString(Qt.locale(), Locale.ShortFormat)
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: dialog.colors["text-muted"]
                        font.pixelSize: 12
                    }
                }
                onClicked: { results.currentIndex = index; dialog.openCurrent() }
            }
        }
    }
}
