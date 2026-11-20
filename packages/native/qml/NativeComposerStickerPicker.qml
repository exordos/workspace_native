import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: panel
    required property var colors
    property var packs: []
    property int selectedPack: 0
    readonly property var currentPack: packs[selectedPack] || ({})
    readonly property var matching: {
        const query = search.text.trim().toLocaleLowerCase();
        const candidates = query ? packs : [currentPack];
        const result = [];
        for (const pack of candidates) {
            for (const sticker of pack.stickers || []) {
                if (!query || [sticker.emoji, sticker.alt, ...(sticker.keywords || [])]
                        .some(word => String(word || "").toLocaleLowerCase().includes(query)))
                    result.push({packId: pack.id, sticker: sticker});
            }
        }
        return result;
    }
    signal picked(string markdown)
    function focusSearch() { search.forceActiveFocus(Qt.PopupFocusReason); }
    function reset() { selectedPack = 0; search.clear(); }
    ColumnLayout {
        anchors.fill: parent; anchors.margins: 8; spacing: 6
        TextField {
            id: search
            objectName: "composerStickerSearch"
            Layout.fillWidth: true
            placeholderText: qsTr("Search stickers")
            Accessible.name: placeholderText
        }
        ScrollView {
            visible: panel.packs.length > 0
            Layout.fillWidth: true
            Layout.preferredHeight: 32
            clip: true
            Row {
                Repeater {
                    model: panel.packs
                    delegate: ToolButton {
                        required property int index
                        required property var modelData
                        text: String(modelData.title || modelData.id)
                        checked: index === panel.selectedPack
                        onClicked: panel.selectedPack = index
                    }
                }
            }
        }
        Label {
            objectName: "composerStickerEmpty"
            Layout.fillWidth: true
            Layout.fillHeight: true
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            visible: !panel.matching.length
            color: panel.colors["text-muted"]
            text: panel.packs.length ? qsTr("No stickers found") : qsTr("No sticker packs installed yet.")
        }
        GridView {
            visible: panel.matching.length > 0
            Layout.fillWidth: true; Layout.fillHeight: true
            cellHeight: 80; cellWidth: 80; clip: true
            model: panel.matching
            delegate: ToolButton {
                required property var modelData
                id: cell
                readonly property var sticker: modelData.sticker
                width: 80; height: 80
                objectName: "composerSticker-" + modelData.packId + "-" + sticker.id
                Accessible.name: String(sticker.alt || sticker.emoji || "")
                contentItem: Item {
                    Image {
                        id: thumbnail
                        anchors.fill: parent
                        anchors.margins: 4
                        asynchronous: true
                        fillMode: Image.PreserveAspectFit
                        source: cell.sticker.format === "lottie" ? "" : (cell.sticker.thumbnailUrl || cell.sticker.url || "")
                    }
                    Label {
                        anchors.centerIn: parent
                        visible: thumbnail.status !== Image.Ready
                        color: panel.colors["text-primary"]
                        text: String(cell.sticker.emoji || cell.sticker.alt || "")
                        font.pixelSize: 30
                    }
                }
                onClicked: panel.picked("[sticker:" + modelData.packId + ":" + sticker.id + "](" + sticker.url + ")")
            }
        }
    }
}
