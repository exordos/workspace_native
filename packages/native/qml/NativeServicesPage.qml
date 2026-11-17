pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects

// Services placeholder hub (issue #45, docs/NATIVE_DESKTOP_SERVICES.md).
// Presentation only: the reference ships a fully static page with no
// backend calls (workspace_ui 61dacef78dab2f5d0be76f283bebf49fe29b84af
// pages/services/services-page.ui.tsx, routes /services and
// /all-services at app-route-definitions.tsx:166-167,203-204). The
// reference top bar keeps the Services button behind a build flag
// (top-bar.test.tsx:153: no services nav button by default), so native
// renders no visible section entry either; the whitelisted deeplink and
// the section state are the entry surfaces. Geometry measured from the
// same source: header px-4 py-4 border-b border-border-subtle bg-card-bg;
// h1 text-lg font-semibold; card grid gap-3 with 1/2/3 columns at the
// 768/1280 window-width bands (the reference grid is grid-cols-1
// md:grid-cols-2 xl:grid-cols-3); card min-h-[170px] rounded-xl p-4 with
// justify-between (content top, chip bottom); 44 px icon tile rounded-lg
// with a 22 px text-muted icon; "Coming soon" chip mt-4 rounded-md
// px-2.5 py-1 text-xs; bottom-right bg-accent button (px-3 py-1.5
// text-sm rounded-lg) navigating to the messenger root, which the native
// app renders as the chat section home.
FocusScope {
    id: page
    objectName: "nativeServicesPage"
    required property var colors
    // The native window width drives the column bands (viewport analog of
    // the reference Tailwind media queries); bound from Main.qml.
    required property real viewportWidth
    signal goToChats()
    onVisibleChanged: { if (visible) forceActiveFocus(); }

    // Reference breakpoints: md = 768 px, xl = 1280 px (Tailwind defaults).
    readonly property int gridColumns: viewportWidth >= 1280 ? 3
        : viewportWidth >= 768 ? 2 : 1
    readonly property int cardGap: 12

    // Fixed reference card order (services-page.ui.tsx:13-50) with the
    // settings.servicesPlaceholder* strings (en.json:1098-1109 /
    // ru.json:1110-1121). The icon column is the qrc asset name; the
    // reference businessCenter icon ships as business_center.svg.
    readonly property var cardModel: [
        { id: "knowledge-base", asset: "files.svg",
          title: qsTr("Knowledge base"),
          hint: qsTr("Internal docs and team playbooks.") },
        { id: "people-directory", asset: "group.svg",
          title: qsTr("People directory"),
          hint: qsTr("Team structure, contacts, and roles.") },
        { id: "approvals", asset: "handshake.svg",
          title: qsTr("Approvals"),
          hint: qsTr("Requests and approval workflows.") },
        { id: "docs", asset: "folder.svg",
          title: qsTr("Document vault"),
          hint: qsTr("Contracts, templates, and shared files.") },
        { id: "automation", asset: "business_center.svg",
          title: qsTr("Automation"),
          hint: qsTr("Routine process automation and bots.") },
        { id: "integrations", asset: "links.svg",
          title: qsTr("Integrations"),
          hint: qsTr("Connections with external SaaS systems.") }
    ]

    Rectangle {
        objectName: "servicesPageBackground"
        anchors.fill: parent
        color: page.colors["bg"]
    }

    // Header: border-b border-border-subtle bg-card-bg px-4 py-4.
    Rectangle {
        id: headerBg
        objectName: "servicesHeaderBg"
        x: 0
        y: 0
        width: page.width
        height: 16 + servicesHeading.height + 4 + servicesPlannedHint.height + 16
        color: page.colors["card-bg"]
    }
    Rectangle {
        objectName: "servicesHeaderBorder"
        x: 0
        y: headerBg.height
        width: page.width
        height: 1
        color: page.colors["border-subtle"]
    }
    // h1 text-lg font-semibold text-text-primary (nav.services).
    Text {
        id: servicesHeading
        objectName: "servicesHeading"
        x: 16
        y: 16
        text: qsTr("Services")
        textFormat: Text.PlainText
        font.pixelSize: 18
        font.weight: Font.DemiBold
        color: page.colors["text-primary"]
    }
    // Sub: mt-1 text-sm text-text-muted (settings.servicesPlannedHint).
    Text {
        id: servicesPlannedHint
        objectName: "servicesPlannedHint"
        x: 16
        y: servicesHeading.y + servicesHeading.height + 4
        width: page.width - 32
        text: qsTr("Services will be available here. Integrations are being prepared.")
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
        font.pixelSize: 14
        color: page.colors["text-muted"]
    }

    // Body section p-4 gap-3: the scrolling card grid takes the remaining
    // height (flex-1 overflow-auto), the button row sits at the bottom.
    Flickable {
        id: gridFlick
        objectName: "servicesGrid"
        x: 16
        y: headerBg.height + 1 + 16
        width: page.width - 32
        height: Math.max(0, page.height - y
            - 16 - chatButtonRect.height - page.cardGap)
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        contentWidth: width
        contentHeight: Math.max(gridInner.height + 4, height) // pb-1

        // Card grid: grid-cols gap-3, column count from the window width.
        Item {
            id: gridInner
            objectName: "servicesGridInner"
            width: gridFlick.width
            height: gridLayout.heights.length && gridLayout.ys.length
                ? gridLayout.ys[gridLayout.ys.length - 1]
                    + gridLayout.heights[gridLayout.heights.length - 1]
                : 0

            Repeater {
                id: cardsRepeater
                model: page.cardModel
                delegate: Rectangle {
                    id: card
                    required property var modelData
                    required property int index
                    objectName: "servicesCard-" + modelData.id
                    property string cardId: modelData.id
                    property Item titleItem: cardTitle
                    property Item hintItem: cardHint
                    property Item chipItem: chipRect
                    // Grid position and height are assigned by relayout().
                    width: 100
                    height: 170
                    radius: 12 // rounded-xl
                    color: page.colors["card-bg"]
                    border.width: 1
                    border.color: page.colors["border-subtle"]

                    // Icon tile: inline-flex h-11 w-11 rounded-lg border
                    // border-border-subtle bg-bg, icon size 22 text-muted.
                    Rectangle {
                        id: iconTile
                        objectName: "servicesIcon-" + card.cardId
                        x: 16 // p-4
                        y: 16
                        width: 44 // h-11 w-11
                        height: 44
                        radius: 8 // rounded-lg
                        color: page.colors["bg"]
                        border.width: 1
                        border.color: page.colors["border-subtle"]
                        readonly property string asset: card.modelData.asset
                        Image {
                            id: iconGlyph
                            objectName: "servicesGlyph-" + card.cardId
                            anchors.centerIn: parent
                            width: 22
                            height: 22
                            source: "qrc:/workspace/icons/" + iconTile.asset
                            sourceSize.width: 22
                            sourceSize.height: 22
                            asynchronous: false
                            fillMode: Image.PreserveAspectFit
                            layer.enabled: true
                            layer.effect: MultiEffect {
                                colorizationColor: page.colors["text-muted"]
                                colorization: 1.0
                            }
                            readonly property color tint: page.colors["text-muted"]
                        }
                    }
                    // h2 text-sm font-medium text-text-primary; space-y-3
                    // below the icon tile.
                    Text {
                        id: cardTitle
                        objectName: "servicesTitle-" + card.cardId
                        x: 16
                        y: iconTile.y + iconTile.height + 12 // space-y-3
                        width: card.width - 32
                        text: card.modelData.title
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        font.pixelSize: 14
                        font.weight: Font.Medium
                        color: page.colors["text-primary"]
                    }
                    // Hint: mt-1 text-sm text-text-muted.
                    Text {
                        id: cardHint
                        objectName: "servicesHint-" + card.cardId
                        x: 16
                        y: cardTitle.y + cardTitle.height + 4 // mt-1
                        width: card.width - 32
                        text: card.modelData.hint
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        font.pixelSize: 14
                        color: page.colors["text-muted"]
                    }
                    // Chip: mt-4 inline-flex w-fit rounded-md border
                    // border-border-subtle bg-bg px-2.5 py-1 text-xs
                    // text-text-muted; justify-between bottom-anchors it.
                    Rectangle {
                        id: chipRect
                        objectName: "servicesChip-" + card.cardId
                        x: 16
                        y: card.height - 16 - height
                        width: chipText.width + 20 // px-2.5
                        height: chipText.height + 8 // py-1
                        radius: 6 // rounded-md
                        color: page.colors["bg"]
                        border.width: 1
                        border.color: page.colors["border-subtle"]
                        Text {
                            id: chipText
                            objectName: "servicesChipText-" + card.cardId
                            x: 10
                            y: 4
                            text: qsTr("Coming soon")
                            textFormat: Text.PlainText
                            font.pixelSize: 12 // text-xs
                            color: page.colors["text-muted"]
                        }
                    }
                }
            }
        }
    }

    // Button row: flex justify-end (services-page.ui.tsx:98-106). The
    // button navigates to the messenger root (reference openMessenger),
    // which is the chat section in the native shell.
    Rectangle {
        id: chatButtonRect
        objectName: "servicesChatButton"
        x: page.width - 16 - width
        y: page.height - 16 - height
        width: chatButtonText.width + 24 // px-3
        height: chatButtonText.height + 12 // py-1.5
        radius: 8 // rounded-lg
        color: page.colors["accent"]
        opacity: buttonHover.hovered ? 0.9 : 1.0 // hover:opacity-90
        // Keyboard access (review PR #380): the reference renders a real
        // <button>, so the control is a focusable button-like element with
        // Enter/Space activation - the house pattern from review #170
        // (SessionSwitcher rows). Presentation stays exactly as measured.
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: chatButtonText.text
        function activate() { page.goToChats() }
        Keys.onReturnPressed: (event) => {
            event.accepted = true;
            chatButtonRect.activate();
        }
        Keys.onSpacePressed: (event) => {
            event.accepted = true;
            chatButtonRect.activate();
        }
        Accessible.onPressAction: chatButtonRect.activate()
        Text {
            id: chatButtonText
            objectName: "servicesChatButtonText"
            x: 12
            y: 6
            text: qsTr("Chats & channels")
            textFormat: Text.PlainText
            font.pixelSize: 14 // text-sm
            color: page.colors["on-accent"]
        }
        HoverHandler { id: buttonHover }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: chatButtonRect.activate()
        }
    }

    // True once the deferred relayout passes have applied stable row
    // heights (tests poll this to read settled grid geometry).
    property bool gridSettled: false

    // Grid layout: CSS grid emulation - equal columns, gap-3 rows/cols,
    // every card in a row stretches to the tallest card of that row,
    // minimum 170 px (min-h-[170px]). Recomputed on width/band changes.
    QtObject {
        id: gridLayout
        property var heights: []
        property var ys: []
        property int passes: 0
        property int stableCount: 0
    }

    function relayout() {
        gridLayout.passes = 0
        gridLayout.stableCount = 0
        gridSettled = false
        const cols = gridColumns
        const gridWidth = gridFlick.width
        if (gridWidth <= 0 || cardsRepeater.count === 0)
            return
        const colWidth = (gridWidth - (cols - 1) * cardGap) / cols
        const cards = []
        for (let i = 0; i < cardsRepeater.count; ++i)
            cards.push(cardsRepeater.itemAt(i))
        for (const card of cards)
            if (card) card.width = colWidth
        // Wrapped text heights settle during polish; the deferred pass runs
        // after polish so the row heights are measured against the final
        // column width, and it re-schedules itself while measurements move.
        Qt.callLater(relayoutFinal)
    }
    function relayoutFinal() {
        const cols = gridColumns
        const gridWidth = gridFlick.width
        if (gridWidth <= 0 || cardsRepeater.count === 0)
            return
        const colWidth = (gridWidth - (cols - 1) * cardGap) / cols
        const rowCount = Math.ceil(cardsRepeater.count / cols)
        const rowHeights = new Array(rowCount).fill(170)
        for (let i = 0; i < cardsRepeater.count; ++i) {
            const card = cardsRepeater.itemAt(i)
            if (!card) continue
            card.width = colWidth
            // p-4 + 44 tile + space-y-3 + title + mt-1 + hint + mt-4 chip
            // + p-4 (natural content height; min 170 applied per row).
            const contentHeight = 16 + 44 + 12
                + card.titleItem.height + 4 + card.hintItem.height
                + 16 + card.chipItem.height + 16
            const row = Math.floor(i / cols)
            rowHeights[row] = Math.max(rowHeights[row], contentHeight)
        }
        const ys = []
        let y = 0
        for (let row = 0; row < rowCount; ++row) {
            ys.push(y)
            y += rowHeights[row] + cardGap
        }
        // The previous pass' measurements may have been taken before the
        // text polish; keep re-running (bounded) until they settle.
        const previous = gridLayout.heights
        let stable = previous.length === rowHeights.length
        if (stable) {
            for (let row = 0; row < rowHeights.length; ++row)
                if (Math.abs(previous[row] - rowHeights[row]) > 0.01) { stable = false; break }
        }
        gridLayout.heights = rowHeights
        gridLayout.ys = ys
        for (let i = 0; i < cardsRepeater.count; ++i) {
            const card = cardsRepeater.itemAt(i)
            if (!card) continue
            const col = i % cols
            const row = Math.floor(i / cols)
            card.x = col * (colWidth + cardGap)
            card.y = ys[row]
            card.height = rowHeights[row]
        }
        if (!stable)
            gridLayout.stableCount = 0
        else
            gridLayout.stableCount += 1
        // A measurement can equal the previous pass one polish round before
        // the final font metrics settle; demand two stable passes so the
        // applied row heights are the polished ones.
        if (gridLayout.stableCount < 2 && gridLayout.passes < 10) {
            gridLayout.passes += 1
            Qt.callLater(relayoutFinal)
        }
        gridSettled = gridLayout.stableCount >= 2
    }
    Component.onCompleted: Qt.callLater(relayout)
    onViewportWidthChanged: relayout()
    Connections {
        target: gridFlick
        function onWidthChanged() { page.relayout() }
    }
    onGridColumnsChanged: relayout()
}
