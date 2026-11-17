pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Effects

// Open Source Licenses browser page (issue #46,
// docs/NATIVE_DESKTOP_LICENSES.md). Pixel-perfect port of the reference
// licenses-page.ui.tsx measured at workspace_ui 61dacef78dab2f5d0be76f283bebf49fe29b84af:
// page px-6 py-4; title text-xl font-semibold (licenses.title), subtitle
// text-sm text-muted (licenses.subtitle), 36x36 rounded-lg close button with
// Icon close 18 (common.close); top-8 license-type chips rounded-full
// px-3 py-1 text-xs font-medium with toggle-off and Reset chip
// (licenses.reset); search w-full max-w-md rounded-lg bg-text-field-bg
// px-4 py-2 text-sm (licenses.searchPlaceholder) matching name OR publisher
// case-insensitively; counter text-xs (licenses.packagesOf); 4-column table
// with sticky header and border-t rows; package names are accent external
// links only when a repository URL exists. The dataset is the native client's
// own third-party dependencies, generated at build time into
// qrc:/workspace/licenses/licenses.json by scripts/collect-native-licenses.py
// - never the reference app's npm dataset.
FocusScope {
    id: page
    objectName: "nativeLicensesPage"
    required property var colors

    // Close handoff (reference close + tests lines 47 and 56): history back
    // when available, else the resolved messenger route (native chat home).
    property bool canGoBack: false
    signal backRequested()
    signal messengerHomeRequested()
    // External link handoff: Main.qml routes the URL through the same
    // QDesktopServices path used by markdown links (Main.qml openExternal).
    signal externalLinkRequested(string url)

    function close() {
        if (page.canGoBack)
            page.backRequested();
        else
            page.messengerHomeRequested();
    }

    property var entries: []
    property string search: ""
    property string filterLicense: ""

    // licenseTypes: count descending, first-seen order breaks ties (the
    // reference Map + stable sort); the row slice is .slice(0, 8).
    readonly property var licenseTypes: {
        const counts = [], seen = {};
        for (const entry of entries) {
            if (seen[entry.license] === undefined) {
                seen[entry.license] = counts.length;
                counts.push({ license: entry.license, count: 0, order: counts.length });
            }
            counts[seen[entry.license]].count += 1;
        }
        // Count descending with an explicit first-seen tie-break: never
        // depend on sort stability across JavaScript engines.
        return counts.sort((a, b) => b.count - a.count || a.order - b.order).slice(0, 8);
    }
    readonly property var filtered: {
        const q = page.search.toLowerCase().trim();
        return entries.filter(e => {
            if (page.filterLicense && e.license !== page.filterLicense)
                return false;
            if (q && !e.name.toLowerCase().includes(q)
                && !e.publisher.toLowerCase().includes(q))
                return false;
            return true;
        });
    }

    // Dataset handoff: Main.qml passes the JSON string that
    // NativeController reads from the build-time Qt resource
    // (QML XMLHttpRequest refuses local file GETs by policy).
    property string datasetJson: ""
    onDatasetJsonChanged: {
        try {
            const parsed = JSON.parse(datasetJson)
            page.entries = Array.isArray(parsed) ? parsed : []
        } catch (error) {
            page.entries = []
        }
    }

    onVisibleChanged: { if (visible) forceActiveFocus(); }

    Rectangle {
        anchors.fill: parent
        color: page.colors["bg"]
    }

    // Header row: mb-4 flex items-start justify-between gap-3 inside px-6
    // py-4. Row children position their y manually (no anchors in Row).
    Row {
        id: header
        objectName: "licensesHeaderRow"
        x: 24 // px-6
        y: 16 // py-4
        width: page.width - 48
        spacing: 12 // gap-3
        Column {
            id: headerText
            width: header.width - 36 - 12 // minus close button and gap
            // h1 text-xl font-semibold text-text-primary (licenses.title).
            Text {
                id: title
                objectName: "licensesTitle"
                width: parent.width
                text: qsTr("Open Source Licenses")
                textFormat: Text.PlainText
                font.pixelSize: 20
                font.weight: Font.DemiBold
                color: page.colors["text-primary"]
                elide: Text.ElideRight
                Accessible.role: Accessible.Heading
            }
            // Subtitle mt-1 text-sm text-text-muted (licenses.subtitle).
            Text {
                id: subtitle
                objectName: "licensesSubtitle"
                width: parent.width
                topPadding: 4 // mt-1
                text: qsTr("Workspace uses %1 open source packages").arg(page.entries.length)
                textFormat: Text.PlainText
                font.pixelSize: 14
                color: page.colors["text-muted"]
                elide: Text.ElideRight
            }
        }
        // Close button: grid h-9 w-9 rounded-lg text-text-muted
        // hover:bg-card-bg-active hover:text-text-primary (common.close).
        AbstractButton {
            id: closeButton
            objectName: "licensesClose"
            width: 36 // h-9 w-9
            height: 36
            hoverEnabled: true
            Accessible.name: qsTr("Close")
            Accessible.role: Accessible.Button
            onClicked: page.close()
            background: Rectangle {
                radius: 8 // rounded-lg
                color: closeButton.hovered ? page.colors["card-bg-active"] : "transparent"
            }
            Item {
                width: 18; height: 18 // Icon size 18
                anchors.centerIn: parent
                Image {
                    anchors.fill: parent
                    source: "qrc:/workspace/icons/close.svg"
                    sourceSize.width: 18
                    sourceSize.height: 18
                    asynchronous: false
                    fillMode: Image.PreserveAspectFit
                    layer.enabled: true
                    layer.effect: MultiEffect {
                        colorizationColor: closeButton.hovered
                            ? page.colors["text-primary"] : page.colors["text-muted"]
                        colorization: 1.0
                    }
                }
            }
        }
    }

    // Filter chips: mb-4 flex flex-wrap gap-2 (top 8 types by count).
    Flow {
        id: chipRow
        objectName: "licensesChipRow"
        anchors.top: header.bottom
        anchors.topMargin: 16 // mb-4
        anchors.left: parent.left
        anchors.leftMargin: 24
        anchors.right: parent.right
        anchors.rightMargin: 24
        spacing: 8 // gap-2

        Repeater {
            model: page.licenseTypes
            AbstractButton {
                id: chip
                required property var modelData
                objectName: "licensesChip-" + modelData.license
                property string labelText: modelData.license + " (" + modelData.count + ")"
                property bool selected: page.filterLicense === modelData.license
                hoverEnabled: true
                onClicked: page.filterLicense = selected ? "" : modelData.license
                Accessible.name: labelText
                contentItem: Text {
                    text: chip.labelText
                    textFormat: Text.PlainText
                    font.pixelSize: 12 // text-xs
                    font.weight: Font.Medium // font-medium
                    color: chip.selected ? page.colors["on-accent"]
                        : page.colors["text-secondary"]
                }
                background: Rectangle {
                    radius: height / 2 // rounded-full
                    color: chip.selected ? page.colors["accent"]
                        : chip.hovered ? page.colors["card-bg-active"]
                        : page.colors["card-bg"]
                }
                leftPadding: 12 // px-3
                rightPadding: 12
                topPadding: 4 // py-1
                bottomPadding: 4
            }
        }
        // Reset chip while filtering: rounded-full bg-card-bg px-3 py-1
        // text-xs text-text-muted hover:bg-card-bg-active (licenses.reset).
        AbstractButton {
            id: resetChip
            objectName: "licensesResetChip"
            visible: page.filterLicense.length > 0
            hoverEnabled: true
            onClicked: page.filterLicense = ""
            Accessible.name: qsTr("Reset")
            contentItem: Text {
                text: qsTr("Reset")
                textFormat: Text.PlainText
                font.pixelSize: 12
                color: page.colors["text-muted"]
            }
            background: Rectangle {
                radius: height / 2
                color: resetChip.hovered ? page.colors["card-bg-active"]
                    : page.colors["card-bg"]
            }
            leftPadding: 12
            rightPadding: 12
            topPadding: 4
            bottomPadding: 4
        }
    }

    // Search input: mb-4 w-full max-w-md rounded-lg bg-text-field-bg px-4
    // py-2 text-sm, placeholder licenses.searchPlaceholder.
    TextField {
        id: search
        objectName: "licensesSearch"
        anchors.top: chipRow.bottom
        anchors.topMargin: 16 // mb-4
        anchors.left: parent.left
        anchors.leftMargin: 24
        width: Math.min(448, parent.width - 48) // max-w-md
        placeholderText: qsTr("Search packages…")
        onTextChanged: page.search = text
        color: page.colors["text-primary"]
        font.pixelSize: 14 // text-sm
        leftPadding: 16 // px-4
        rightPadding: 16
        topPadding: 8 // py-2
        bottomPadding: 8
        background: Rectangle {
            radius: 8 // rounded-lg
            color: page.colors["text-field-bg"]
        }
    }

    // Counter line: mb-2 text-xs text-muted (licenses.packagesOf).
    Text {
        id: counter
        objectName: "licensesCounter"
        anchors.top: search.bottom
        anchors.topMargin: 16
        anchors.left: parent.left
        anchors.leftMargin: 24
        text: qsTr("%1 of %2 packages").arg(page.filtered.length).arg(page.entries.length)
        textFormat: Text.PlainText
        font.pixelSize: 12 // text-xs
        color: page.colors["text-muted"]
    }

    // Sticky header: bg-bg text-xs text-muted, columns pb-2 pr-4 (thead).
    Item {
        id: tableTop
        objectName: "licensesHeaderRow"
        anchors.top: counter.bottom
        anchors.topMargin: 8 // mb-2
        anchors.left: parent.left
        anchors.leftMargin: 24
        anchors.right: parent.right
        anchors.rightMargin: 24
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 16 // py-4
        clip: true
        property real nameWidth: width * 0.4 - 16
        property real versionWidth: width * 0.2 - 16
        property real licenseWidth: width * 0.2 - 16
        property real authorWidth: width * 0.2
        Rectangle { // thead background (sticky top-0 bg-bg)
            width: parent.width
            height: headRow.height
            color: page.colors["bg"]
        }
        Row {
            id: headRow
            objectName: "licensesColumnHeaders"
            Text { text: qsTr("Package"); width: tableTop.nameWidth; rightPadding: 16; bottomPadding: 8
                font.pixelSize: 12; color: page.colors["text-muted"] }
            Text { text: qsTr("Version"); width: tableTop.versionWidth; rightPadding: 16; bottomPadding: 8
                font.pixelSize: 12; color: page.colors["text-muted"] }
            Text { text: qsTr("License"); width: tableTop.licenseWidth; rightPadding: 16; bottomPadding: 8
                font.pixelSize: 12; color: page.colors["text-muted"] }
            Text { text: qsTr("Author"); width: tableTop.authorWidth; bottomPadding: 8
                font.pixelSize: 12; color: page.colors["text-muted"] }
        }
        // Table body: rows border-t border-border-subtle py-2 (tbody). The
        // list uses positional indices so delegates stay stable when the
        // filtered view changes under clicks.
        ListView {
            objectName: "licensesList"
            anchors.top: headRow.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            clip: true
            model: page.filtered.length
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Rectangle {
                id: row
                required property int index
                readonly property var entry: page.filtered[index]
                objectName: "licensesRow-" + index
                width: ListView.view.width
                height: 33 // py-2 above and below a 14 px line + 1 px border
                color: "transparent"
                Rectangle { // border-t border-border-subtle
                    width: parent.width
                    height: 1
                    color: page.colors["border-subtle"]
                }
                // Package cell: accent link when repository != null, else
                // text-primary plain text (text-sm).
                Text {
                    id: nameCell
                    objectName: "licensesName-" + index
                    x: 0
                    y: 8 // py-2
                    width: tableTop.nameWidth
                    rightPadding: 16
                    text: row.entry.name
                    textFormat: Text.PlainText
                    font.pixelSize: 14
                    color: row.entry.repository.length > 0
                        ? page.colors["accent"] : page.colors["text-primary"]
                    elide: Text.ElideRight
                    Accessible.name: text
                    Accessible.role: Accessible.StaticText
                }
                MouseArea {
                    anchors.fill: nameCell
                    enabled: row.entry.repository.length > 0
                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    hoverEnabled: true
                    onClicked: page.externalLinkRequested(row.entry.repository)
                }
                Text {
                    objectName: "licensesVersion-" + index
                    x: tableTop.nameWidth + 16 // pr-4
                    y: 8
                    width: tableTop.versionWidth - 16
                    text: row.entry.version
                    textFormat: Text.PlainText
                    font.pixelSize: 14
                    color: page.colors["text-muted"]
                    elide: Text.ElideRight
                }
                // License cell chip: rounded bg-card-bg px-2 py-0.5 text-xs
                // text-text-secondary.
                Rectangle {
                    objectName: "licensesLicense-" + index
                    x: tableTop.nameWidth + tableTop.versionWidth + 32
                    y: 6
                    width: licenseCell.implicitWidth + 16 // px-2
                    height: licenseCell.implicitHeight + 4 // py-0.5
                    radius: 4 // rounded
                    color: page.colors["card-bg"]
                    Text {
                        id: licenseCell
                        anchors.centerIn: parent
                        text: row.entry.license
                        textFormat: Text.PlainText
                        font.pixelSize: 12 // text-xs
                        color: page.colors["text-secondary"]
                    }
                }
                Text {
                    objectName: "licensesAuthor-" + index
                    x: tableTop.nameWidth + tableTop.versionWidth + tableTop.licenseWidth + 48
                    y: 8
                    width: tableTop.authorWidth - 48 > 0 ? tableTop.authorWidth - 48 : 60
                    text: row.entry.publisher
                    textFormat: Text.PlainText
                    font.pixelSize: 14
                    color: page.colors["text-muted"]
                    elide: Text.ElideRight
                }
            }
        }
    }
}
