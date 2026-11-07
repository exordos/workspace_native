pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Item {
    id: picker
    required property var choices
    property var suggested: []
    property bool dark: false
    property bool active: false
    property bool allowed: true
    property string skin: "neutral"
    readonly property int emojiCount: presentation.emojiCount
    readonly property string searchAnnouncement: emojiCount === 0 ? qsTr("No results found") : emojiCount === 1 ? qsTr("1 result found. Use up and down arrow keys to navigate.") : qsTr("%1 results found. Use up and down arrow keys to navigate.").arg(emojiCount)
    onEmojiCountChanged: if (active && search.text.length > 0)
        search.Accessible.announce(searchAnnouncement)
    readonly property string textFamily: "Arial"
    readonly property string emojiFamily: {
        const installed = Qt.fontFamilies();
        const preferred = ["Segoe UI Emoji", "Segoe UI Symbol", "Segoe UI", "Apple Color Emoji", "Twemoji Mozilla", "Noto Color Emoji", "EmojiOne Color", "Android Emoji"];
        for (let i = 0; i < preferred.length; ++i)
            if (installed.indexOf(preferred[i]) >= 0)
                return preferred[i];
        return "";
    }
    // Linux Noto bitmap metrics differ between Chromium and Qt. Measured
    // colored ink is 32x31px in the source's 40px cells; Qt36 matches it.
    readonly property int emojiPixelSize: Qt.platform.os === "linux" && emojiFamily === "Noto Color Emoji" ? 36 : 30
    readonly property color surface: dark ? "#222222" : "#ffffff"
    readonly property color foreground: dark ? "#c0c0c0" : "#858585"
    readonly property color highlight: dark ? "#c0c0c0" : "#007aeb"
    readonly property color hoverColor: dark ? "#f6363636" : "#e5f0fa"
    readonly property color focusColor: dark ? "#474747" : "#e0f0ff"
    readonly property var categoryIds: ["suggested", "smileys_people", "animals_nature", "food_drink", "travel_places", "activities", "objects", "symbols", "flags"]
    readonly property var categoryNames: [qsTr("Frequently Used"), qsTr("Smileys & People"), qsTr("Animals & Nature"), qsTr("Food & Drink"), qsTr("Travel & Places"), qsTr("Activities"), qsTr("Objects"), qsTr("Symbols"), qsTr("Flags")]
    readonly property var skinIds: ["neutral", "1f3fb", "1f3fc", "1f3fd", "1f3fe", "1f3ff"]
    readonly property var skinNames: [qsTr("Default skin tone"), qsTr("Light skin tone"), qsTr("Medium-light skin tone"), qsTr("Medium skin tone"), qsTr("Medium-dark skin tone"), qsTr("Dark skin tone")]
    readonly property var skinColors: ["#ffd225", "#ffdfbd", "#e9c197", "#c88e62", "#a86637", "#60463a"]
    readonly property var byName: {
        const result = {};
        for (let i = 0; i < choices.length; ++i)
            result[choices[i].emojiName] = choices[i];
        return result;
    }
    readonly property var bases: choices.filter(entry => entry.pickerEligible && entry.pickerBase === entry.emojiName).sort((a, b) => a.pickerOrder - b.pickerOrder)
    readonly property var presentation: buildPresentation()
    property int activeCategory: 1
    property bool skinsExpanded: false
    signal chosen(string emojiName)
    implicitWidth: 320
    implicitHeight: 360

    function reset() {
        search.text = "";
        skin = "neutral";
        skinsExpanded = false;
        variations.close();
        activeCategory = suggested.length ? 0 : 1;
        grid.positionViewAtBeginning();
    }

    function focusSearch() {
        search.forceActiveFocus(Qt.PopupFocusReason);
    }

    function variant(base) {
        if (skin === "neutral")
            return base;
        const codepoint = String.fromCodePoint(parseInt(skin, 16));
        const variants = base.pickerVariants || [];
        for (let i = 0; i < variants.length; ++i) {
            const candidate = byName[variants[i]];
            if (candidate && String(candidate.pickerUnicode).indexOf(codepoint) >= 0)
                return candidate;
        }
        return base;
    }

    function select(entry) {
        if (allowed && entry)
            chosen(entry.emojiName);
    }

    function buildPresentation() {
        if (!active)
            return {
                cells: [],
                headings: [],
                emojiCount: 0
            };
        const cells = [], headings = [];
        let count = 0;
        const needle = search.text.trim().toLocaleLowerCase().replace(/^:+|:+$/g, "").replace(/[_-]/g, " ");
        for (let category = 0; category < categoryIds.length; ++category) {
            let entries = [];
            if (category === 0) {
                if (needle.length)
                    continue;
                entries = suggested.map((entry, index) => ({
                            entry: byName[entry.name],
                            count: Number(entry.count),
                            index: index
                        })).filter(entry => entry.entry && entry.entry.pickerEligible).sort((a, b) => b.count - a.count || a.index - b.index).slice(0, 14).map(entry => entry.entry);
            } else {
                entries = bases.filter(entry => entry.pickerCategory === categoryIds[category]);
                if (needle.length)
                    entries = entries.filter(entry => {
                        const words = [entry.emojiName].concat(entry.aliases || [], entry.pickerSearch || []).join(" ").toLocaleLowerCase().replace(/[_-]/g, " ");
                        return String(entry.pickerUnicode).indexOf(needle) >= 0 || needle.split(/\s+/).every(word => words.indexOf(word) >= 0);
                    });
                entries = entries.map(entry => variant(entry));
            }
            if (!entries.length)
                continue;
            headings.push({
                index: cells.length,
                category: category
            });
            for (let col = 0; col < 7; ++col)
                cells.push({
                    kind: "heading",
                    category: category,
                    first: col === 0
                });
            for (let i = 0; i < entries.length; ++i) {
                cells.push({
                    kind: "emoji",
                    category: category,
                    entry: entries[i]
                });
                ++count;
            }
            while (cells.length % 7)
                cells.push({
                    kind: "space",
                    category: category
                });
        }
        return {
            cells: cells,
            headings: headings,
            emojiCount: count
        };
    }

    function goToCategory(category) {
        search.text = "";
        for (let i = 0; i < presentation.headings.length; ++i) {
            const heading = presentation.headings[i];
            if (heading.category === category) {
                grid.positionViewAtIndex(heading.index, GridView.Beginning);
                activeCategory = category;
                return;
            }
        }
    }

    function visibleCategory() {
        let result = suggested.length ? 0 : 1;
        const row = Math.floor(Math.max(0, grid.contentY) / grid.cellHeight) * 7;
        for (let i = 0; i < presentation.headings.length; ++i) {
            if (presentation.headings[i].index > row)
                break;
            result = presentation.headings[i].category;
        }
        activeCategory = result;
    }

    function moveChoice(direction) {
        const cells = presentation.cells;
        if (!cells.length)
            return;
        let next = grid.currentIndex;
        for (let i = 0; i < cells.length; ++i) {
            next = (next + direction + cells.length) % cells.length;
            if (cells[next].kind === "emoji") {
                grid.currentIndex = next;
                grid.positionViewAtIndex(next, GridView.Contain);
                grid.forceActiveFocus(Qt.TabFocusReason);
                return;
            }
        }
    }

    function openVariations(base, anchor) {
        const parent = byName[base.pickerBase];
        if (!parent || !parent.pickerVariants.length)
            return false;
        variations.entries = [parent].concat(parent.pickerVariants.map(name => byName[name])).slice(0, 6);
        const point = anchor.mapToItem(picker, anchor.width / 2, 0);
        variations.x = 15;
        variations.below = point.y - variations.height < header.height;
        variations.y = Math.max(header.height, Math.min(picker.height - variations.height - 4, variations.below ? point.y + anchor.height + 4 : point.y - variations.height - 4));
        variations.pointerX = Math.max(4, Math.min(variations.width - 29, point.x - variations.x - 18));
        variations.open();
        return true;
    }

    Rectangle {
        anchors.fill: parent
        color: picker.surface
        border.color: picker.dark ? "#151617" : "#e7e7e7"
        radius: 8
    }
    Item {
        id: header
        x: 1
        y: 1
        height: 130
        width: parent.width - 2
        Item {
            x: 10
            y: 15
            width: parent.width - 20
            height: 40
            TextField {
                id: search
                objectName: "reactionSearch"
                width: parent.width - (picker.skinsExpanded ? 168 : 28)
                height: 40
                leftPadding: 30
                rightPadding: 30
                topPadding: 0
                bottomPadding: 0
                maximumLength: 128
                font.family: picker.textFamily
                font.pixelSize: 15
                color: picker.foreground
                placeholderTextColor: picker.foreground
                placeholderText: qsTr("Search")
                selectByMouse: true
                Accessible.name: qsTr("Search reactions")
                background: Rectangle {
                    color: picker.dark ? (search.activeFocus ? "#000000" : "#333333") : "#f6f6f6"
                    radius: 8
                    border.width: search.activeFocus ? 1 : 0
                    border.color: picker.highlight
                }
                onTextChanged: grid.positionViewAtBeginning()
                Keys.onDownPressed: {
                    grid.currentIndex = -1;
                    picker.moveChoice(1);
                }
                Item {
                    x: 8
                    y: 10
                    width: 20
                    height: 20
                    clip: true
                    Image {
                        source: "qrc:/workspace/data/emoji_picker_search.svg"
                        width: 20
                        height: 40
                        y: picker.dark ? -20 : 0
                    }
                }
                ToolButton {
                    visible: search.text.length > 0
                    x: parent.width - 26
                    y: 10
                    width: 20
                    height: 20
                    padding: 0
                    Accessible.name: qsTr("Clear search")
                    background: null
                    contentItem: Item {
                        clip: true
                        Image {
                            source: "qrc:/workspace/data/emoji_picker_clear.svg"
                            width: 20
                            height: 80
                            y: picker.dark ? -40 : 0
                        }
                    }
                    onClicked: {
                        search.text = "";
                        search.forceActiveFocus();
                    }
                }
            }
            ToolButton {
                objectName: "reactionSkinToneToggle"
                x: parent.width - 28
                y: 6
                width: 28
                height: 28
                padding: 0
                Accessible.name: qsTr("Choose skin tone")
                background: Rectangle {
                    color: parent.visualFocus ? picker.focusColor : "transparent"
                    radius: 4
                }
                contentItem: Item {
                    Rectangle {
                        width: 15
                        height: 15
                        anchors.centerIn: parent
                        color: picker.skinColors[picker.skinIds.indexOf(picker.skin)]
                        radius: 4
                        border.color: "#555555"
                    }
                }
                onClicked: picker.skinsExpanded = !picker.skinsExpanded
            }
            Rectangle {
                visible: picker.skinsExpanded
                width: 168
                height: 28
                x: parent.width - width
                y: 6
                z: 3
                color: picker.surface
                Row {
                    layoutDirection: Qt.RightToLeft
                    Repeater {
                        model: picker.skinIds
                        delegate: ToolButton {
                            id: skinChoice
                            required property string modelData
                            required property int index
                            objectName: "reaction-skin-" + modelData
                            width: 28
                            height: 28
                            padding: 0
                            Accessible.name: picker.skinNames[index]
                            Accessible.checkable: true
                            Accessible.checked: picker.skin === modelData
                            background: Rectangle {
                                color: parent.visualFocus ? picker.focusColor : "transparent"
                                radius: 4
                            }
                            contentItem: Item {
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: 15
                                    height: 15
                                    radius: 4
                                    color: picker.skinColors[skinChoice.index]
                                    border.color: "#555555"
                                    scale: picker.skin === skinChoice.modelData ? 1.3 : 1
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: 1
                                        radius: 3
                                        color: "transparent"
                                        border.color: picker.surface
                                    }
                                }
                            }
                            onClicked: {
                                picker.skin = modelData;
                                picker.skinsExpanded = false;
                                search.forceActiveFocus(Qt.OtherFocusReason);
                            }
                        }
                    }
                }
            }
        }
        Row {
            enabled: search.text.length === 0
            opacity: enabled ? 1 : 0.3
            x: 10
            y: 85
            width: parent.width - 20
            spacing: (width - 9 * 30) / 8
            Repeater {
                model: picker.categoryIds
                delegate: ToolButton {
                    id: categoryButton
                    required property string modelData
                    required property int index
                    readonly property bool selected: picker.activeCategory === index && search.text.length === 0
                    readonly property int spriteColumn: index === 0 ? 8 : index - 1
                    objectName: "reaction-category-" + modelData
                    width: 30
                    height: 30
                    padding: 0
                    Accessible.name: picker.categoryNames[index]
                    Accessible.checkable: true
                    Accessible.checked: selected
                    ToolTip.visible: hovered
                    ToolTip.text: picker.categoryNames[index]
                    background: Rectangle {
                        color: categoryButton.visualFocus ? picker.focusColor : "transparent"
                        radius: 4
                    }
                    contentItem: Item {
                        clip: true
                        Image {
                            source: "qrc:/workspace/data/emoji_picker_navigation.svg"
                            sourceSize: Qt.size(300, 120)
                            width: 300
                            height: 120
                            x: -categoryButton.spriteColumn * 30
                            y: -(picker.dark ? (categoryButton.selected ? 3 : 2) : (categoryButton.selected ? 1 : 0)) * 30
                        }
                    }
                    onClicked: picker.goToCategory(index)
                }
            }
        }
    }
    GridView {
        id: grid
        objectName: "reactionGrid"
        property int emojiCount: picker.emojiCount
        x: 11
        y: header.y + header.height
        width: parent.width - 22
        height: parent.height - y
        clip: true
        cellWidth: 40
        cellHeight: 40
        cacheBuffer: 80
        reuseItems: true
        model: picker.presentation.cells
        onContentYChanged: picker.visibleCategory()
        ScrollBar.vertical: ScrollBar {
            policy: ScrollBar.AsNeeded
        }
        Keys.onLeftPressed: picker.moveChoice(-1)
        Keys.onRightPressed: picker.moveChoice(1)
        Keys.onUpPressed: picker.moveChoice(-7)
        Keys.onDownPressed: picker.moveChoice(7)
        Keys.onReturnPressed: {
            const entry = picker.presentation.cells[currentIndex];
            if (entry && entry.kind === "emoji")
                picker.select(entry.entry);
        }
        delegate: Item {
            id: cell
            required property var modelData
            required property int index
            width: grid.cellWidth
            height: 40
            z: modelData.kind === "heading" ? 1 : 0
            Text {
                objectName: cell.modelData.kind === "heading" && cell.modelData.first ? "reaction-heading-" + picker.categoryIds[cell.modelData.category] : ""
                visible: cell.modelData.kind === "heading" && cell.modelData.first
                width: grid.width
                height: 40
                text: picker.categoryNames[cell.modelData.category]
                textFormat: Text.PlainText
                font.family: picker.textFamily
                font.pixelSize: 16
                font.weight: Font.Bold
                verticalAlignment: Text.AlignVCenter
                color: picker.foreground
            }
            Button {
                id: choice
                readonly property var entry: cell.modelData.entry || null
                property bool held: false
                objectName: entry ? "emoji-choice-" + entry.emojiName : ""
                visible: cell.modelData.kind === "emoji"
                enabled: picker.allowed && visible
                width: 40
                height: 40
                x: 0
                padding: 5
                hoverEnabled: true
                activeFocusOnTab: false
                Accessible.name: entry ? String((entry.pickerSearch || []).slice(-1)[0] || entry.emojiName) : ""
                background: Rectangle {
                    color: choice.visualFocus || (grid.activeFocus && grid.currentIndex === cell.index) ? picker.focusColor : choice.hovered ? picker.hoverColor : "transparent"
                    radius: 8
                }
                contentItem: Text {
                    text: choice.entry ? choice.entry.pickerUnicode : ""
                    textFormat: Text.PlainText
                    font.family: picker.emojiFamily
                    font.pixelSize: picker.emojiPixelSize
                    color: picker.foreground
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                onPressed: held = false
                onClicked: if (!held)
                    picker.select(entry)
                Timer {
                    interval: 500
                    running: choice.down && !choice.held
                    onTriggered: if (choice.entry)
                        choice.held = picker.openVariations(choice.entry, choice)
                }
            }
        }
        Rectangle {
            visible: grid.contentY > 0 && picker.presentation.cells.length > 0
            x: 0
            y: grid.contentY
            width: grid.width
            height: 40
            z: 2
            color: picker.dark ? "#e6222222" : "#e6ffffff"
            Text {
                anchors.fill: parent
                text: picker.categoryNames[picker.activeCategory]
                textFormat: Text.PlainText
                color: picker.foreground
                font.family: picker.textFamily
                font.pixelSize: 16
                font.weight: Font.Bold
                verticalAlignment: Text.AlignVCenter
                Accessible.ignored: true
            }
        }
        Accessible.description: search.text.length > 0 ? picker.searchAnnouncement : ""
    }
    Popup {
        id: variations
        objectName: "reactionVariations"
        property var entries: []
        property real pointerX: 0
        property bool below: false
        width: picker.width - 30
        height: 45
        padding: 5
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onOpened: if (variationRepeater.count)
            variationRepeater.itemAt(0).forceActiveFocus(Qt.PopupFocusReason)
        background: Rectangle {
            color: picker.surface
            radius: 3
            border.color: picker.dark ? "#151617" : "#e7e7e7"
            Item {
                x: variations.pointerX
                y: variations.below ? -14 : parent.height - 1
                width: 25
                height: 15
                clip: true
                rotation: variations.below ? 180 : 0
                Image {
                    source: "qrc:/workspace/data/emoji_picker_triangle.svg"
                    width: 50
                    height: 15
                    x: picker.dark ? -25 : 0
                }
            }
        }
        contentItem: Row {
            spacing: (width - variations.entries.length * 40) / Math.max(1, variations.entries.length - 1)
            Repeater {
                id: variationRepeater
                model: variations.entries
                delegate: Button {
                    id: variationChoice
                    required property var modelData
                    objectName: "emoji-variation-" + modelData.emojiName
                    width: 40
                    height: 40
                    padding: 5
                    enabled: picker.allowed
                    hoverEnabled: true
                    Accessible.name: String(modelData.emojiName)
                    background: Rectangle {
                        radius: 8
                        color: parent.visualFocus ? picker.focusColor : parent.hovered ? picker.hoverColor : "transparent"
                    }
                    contentItem: Text {
                        text: variationChoice.modelData.pickerUnicode
                        textFormat: Text.PlainText
                        font.family: picker.emojiFamily
                        font.pixelSize: picker.emojiPixelSize
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    onClicked: {
                        variations.close();
                        picker.select(modelData);
                    }
                }
            }
        }
    }
}
