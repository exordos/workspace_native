import QtQuick
import QtQuick.Controls
import QtQuick.Effects

// Full-size media viewer overlay (issue #8, docs/NATIVE_DESKTOP_MEDIA_VIEWER.md).
// Every geometry and colour value below was measured from the React reference
// at workspace_ui 61dacef78dab2f5d0be76f283bebf49fe29b84af; file citations name
// the source line. The viewer covers image media only; video slots and wheel
// zoom are follow-ups recorded in the design doc.
Item {
    id: viewerRoot
    objectName: "mediaViewer-overlay"
    required property var viewer
    required property var colors
    signal requestDownload(string fileUuid)

    readonly property var hostWindow: Window.window
    readonly property var currentSlot: viewer.open && viewer.index < viewer.items.length
        ? viewer.items[viewer.index] || ({}) : ({})
    readonly property bool gallery: viewer.count > 1
    // Positional model for the strip: a live viewer.items model would rebuild
    // delegates on every slot download and destroy items under a click.
    readonly property var stripModel: gallery
        ? Array.from({length: viewer.count}, (_, i) => i) : []

    visible: viewer.open
    Accessible.role: Accessible.Dialog
    Accessible.name: qsTr("Media viewer")

    // Backdrop: fixed inset-0 z-modal bg-black/90 (media-viewer-backdrop.ui.tsx:56);
    // the root click closes (media-viewer-backdrop.ui.tsx:53-64).
    MouseArea {
        objectName: "mediaViewer-backdrop"
        anchors.fill: parent
        onClicked: viewerRoot.viewer.close()
    }
    Rectangle {
        anchors.fill: parent
        color: "#E6000000"
    }

    // Media area: px-16 pb-28 pt-14 (media-viewer-overlay.ui.tsx:126) with the
    // image capped at 100% - 192 px height (media-viewer-overlay.ui.tsx:158,167).
    Item {
        id: mediaArea
        anchors.fill: parent
        anchors.leftMargin: 64
        anchors.rightMargin: 64
        anchors.topMargin: 56
        anchors.bottomMargin: 112

        Image {
            id: mediaImage
            objectName: "mediaViewer-image"
            anchors.centerIn: parent
            width: parent.width
            height: Math.min(parent.height, hostWindow ? Math.max(0, hostWindow.height - 192) : parent.height)
            source: viewerRoot.visible && viewerRoot.currentSlot.state === "ready" ? String(viewerRoot.currentSlot.url || "") : ""
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            cache: false
            smooth: true
            visible: status === Image.Ready
            Accessible.ignored: true
        }
        // The reference media element stopPropagation()s clicks
        // (media-viewer-overlay.ui.tsx:159,169), so the backdrop must not see
        // them. The reference <img> box equals its painted pixels
        // (object-contain), so clicks landing in the PreserveAspectFit
        // letterbox bands behave like backdrop clicks and close the viewer.
        MouseArea {
            anchors.fill: mediaImage
            onClicked: (mouse) => {
                const insidePainted = mediaImage.status !== Image.Ready
                    || (Math.abs(mouse.x - mediaImage.width / 2) <= mediaImage.paintedWidth / 2 + 1
                        && Math.abs(mouse.y - mediaImage.height / 2) <= mediaImage.paintedHeight / 2 + 1);
                if (!insidePainted)
                    viewerRoot.viewer.close();
            }
        }
        Text {
            anchors.centerIn: parent
            width: Math.min(480, mediaArea.width)
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            color: "#B3FFFFFF"
            font.pixelSize: 14
            text: viewerRoot.currentSlot.state === "error"
                // mediaViewer.unavailable
                ? qsTr("This media cannot be displayed. You can download the file instead.")
                : qsTr("Loading media…")
            visible: viewerRoot.visible && viewerRoot.currentSlot.state !== "ready"
        }
    }

    // Prev/next: fixed top-1/2 h-12 w-12 -translate-y-1/2 rounded-full
    // bg-black/50 hover:bg-black/70, left-4/right-4, chevron size 24, left
    // rotated 180; only rendered when items.length > 1
    // (media-viewer-controls.ui.tsx:6-7, media-viewer-overlay.ui.tsx:55,173).
    component ViewerArrow: AbstractButton {
        id: arrow
        required property bool pointsRight
        implicitWidth: 48
        implicitHeight: 48
        padding: 0
        hoverEnabled: true
        onClicked: pointsRight ? viewerRoot.viewer.next() : viewerRoot.viewer.prev()
        background: Rectangle {
            radius: 24
            color: arrow.hovered ? "#B3000000" : "#80000000"
        }
        Item {
            anchors.centerIn: parent
            width: 24
            height: 24
            rotation: arrow.pointsRight ? 0 : 180
            Image {
                anchors.fill: parent
                source: "qrc:/workspace/icons/chevron-right.svg"
                sourceSize.width: 24
                sourceSize.height: 24
                asynchronous: false
                layer.enabled: true
                layer.effect: MultiEffect {
                    colorizationColor: "white"
                    colorization: 1.0
                }
            }
        }
        Accessible.name: arrow.pointsRight ? qsTr("Next") : qsTr("Previous")
    }
    ViewerArrow {
        objectName: "mediaViewer-prev"
        visible: viewerRoot.gallery
        anchors.left: parent.left
        anchors.leftMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        pointsRight: false
    }
    ViewerArrow {
        objectName: "mediaViewer-next"
        visible: viewerRoot.gallery
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        pointsRight: true
    }

    // Toolbar: absolute right-4 top-4 z-float gap-2 (media-viewer-toolbar.ui.tsx:52),
    // buttons 40x40 rounded-lg bg-bg-elevated/80 hover:bg-bg-elevated text-text-primary
    // glyph 20, disabled opacity 0.4 (media-viewer-toolbar.ui.tsx:7-8,79-95).
    // "Open in new tab" is reference-browser-only (media-viewer-overlay.ui.tsx:56)
    // and copy-image is issue #79, so the desktop viewer ships download + close.
    Row {
        objectName: "mediaViewer-toolbar"
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.top: parent.top
        anchors.topMargin: 16
        spacing: 8
        component ViewerButton: AbstractButton {
            id: button
            required property string asset
            property color glyphTint: viewerRoot.colors["text-primary"]
            implicitWidth: 40
            implicitHeight: 40
            padding: 0
            hoverEnabled: true
            background: Rectangle {
                radius: 6
                color: button.enabled && button.hovered
                    ? viewerRoot.colors["bg-elevated"]
                    : Qt.alpha(viewerRoot.colors["bg-elevated"], 0.8)
            }
            opacity: enabled ? 1.0 : 0.4
            Image {
                anchors.centerIn: parent
                source: "qrc:/workspace/icons/" + button.asset
                sourceSize.width: 20
                sourceSize.height: 20
                asynchronous: false
                layer.enabled: true
                layer.effect: MultiEffect {
                    colorizationColor: button.glyphTint
                    colorization: 1.0
                }
            }
        }
        ViewerButton {
            objectName: "mediaViewer-download"
            asset: "download.svg"
            // Actions are unusable until the slot display URL exists
            // (media-viewer-actions.lib.ts:71-79).
            enabled: viewerRoot.currentSlot.state === "ready"
                && String(viewerRoot.currentSlot.uuid || "").length > 0
            Accessible.name: qsTr("Download")
            onClicked: viewerRoot.requestDownload(String(viewerRoot.currentSlot.uuid))
        }
        ViewerButton {
            objectName: "mediaViewer-close"
            asset: "close.svg"
            Accessible.name: qsTr("Close")
            onClicked: viewerRoot.viewer.close()
        }
    }

    // Thumbnail strip: fixed inset-x-0 bottom-0 border-t border-white/10
    // bg-black/70 px-4 py-3 with the counter "current / total" 12 px
    // text-white/70 mb-2 and 56x56 rounded-lg border-2 thumbnails
    // (media-viewer-thumbnails.ui.tsx:78-90). Hidden for a single item (:75).
    Rectangle {
        id: strip
        objectName: "mediaViewer-strip"
        visible: viewerRoot.gallery
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        color: "#B3000000"
        // py-3 top + counter + mb-2 + thumbnails + scroller pb-1 + py-3 bottom,
        // font-derived counter height included via the Text item itself.
        height: stripColumn.implicitHeight + 12 + 4 + 12
        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 1
            color: "#1AFFFFFF"
        }
        Column {
            id: stripColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.topMargin: 12
            spacing: 8
            Text {
                id: counter
                objectName: "mediaViewer-counter"
                anchors.horizontalCenter: parent.horizontalCenter
                color: "#B3FFFFFF"
                font.pixelSize: 12
                text: (viewerRoot.viewer.index + 1) + " / " + viewerRoot.viewer.count
            }
            ListView {
                id: thumbs
                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.min(parent.width, contentWidth)
                height: 56
                orientation: ListView.Horizontal
                spacing: 8
                model: viewerRoot.stripModel
                boundsBehavior: Flickable.StopAtBounds
                interactive: contentWidth > width
                currentIndex: viewerRoot.viewer.index
                onCurrentIndexChanged: if (viewerRoot.gallery) positionViewAtIndex(currentIndex, ListView.Center)
                delegate: AbstractButton {
                    id: thumb
                    required property int modelData
                    objectName: "mediaViewer-thumb-" + modelData
                    readonly property bool isActive: modelData === viewerRoot.viewer.index
                    implicitWidth: 56
                    implicitHeight: 56
                    padding: 0
                    onClicked: viewerRoot.viewer.goTo(modelData)
                    background: Rectangle {
                        radius: 6
                        color: viewerRoot.colors["bg-elevated"]
                        border.width: 2
                        border.color: thumb.isActive ? viewerRoot.colors["accent"] : "transparent"
                        opacity: thumb.isActive ? 1.0 : 0.7
                    }
                    Image {
                        anchors.fill: parent
                        anchors.margins: 2
                        source: String((viewerRoot.viewer.items[thumb.modelData] || {}).url || "")
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        cache: false
                        visible: status === Image.Ready
                        Accessible.ignored: true
                    }
                    Accessible.name: qsTr("Media %1").arg(modelData + 1)
                }
            }
        }
    }

    // Document-level keyboard contract (media-viewer-backdrop.ui.tsx:27-51):
    // Escape always closes; arrows navigate only with more than one item.
    Shortcut {
        sequence: "Escape"
        enabled: viewerRoot.viewer.open
        onActivated: viewerRoot.viewer.close()
    }
    Shortcut {
        sequence: "Left"
        enabled: viewerRoot.viewer.open && viewerRoot.gallery
        onActivated: viewerRoot.viewer.prev()
    }
    Shortcut {
        sequence: "Right"
        enabled: viewerRoot.viewer.open && viewerRoot.gallery
        onActivated: viewerRoot.viewer.next()
    }
}
