pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects

// Issue #217 (docs/NATIVE_DESKTOP_QUOTE_MEDIA_THUMBNAILS.md): the leading media
// thumbnail of a reply or quote frame. Reference shape is
// `min-h-12 w-12 shrink-0 self-stretch overflow-hidden rounded-md
// bg-bg-elevated` with an object-cover image
// (messenger-workspace-media-thumbnail.ui.tsx:150-160 at workspace_ui
// 230e02b8). The element is decorative: the quoted author line already names the
// frame for assistive technology.
//
// Loading goes through the bounded authenticated manager: exactly one lease is
// held while the frame is active, and `status` follows the reference state
// machine. A failed load reports "failed" so the caller keeps the text-only
// layout, and a video reports "ready" with the media viewer play badge because
// the image manager never decodes video sources.
Rectangle {
    id: thumb
    required property var colors
    property var images: nativeMessageImages
    property string urn: ""
    property string kind: "image"
    property bool active: true

    property string imageUrl: ""
    property bool imageReady: false
    property bool imageFailed: false
    property var lease: null
    property int refreshTicket: 0

    readonly property string status: kind === "video" ? "ready"
        : imageFailed ? "failed" : imageReady ? "ready" : "loading"
    readonly property bool wantsLease: active && visible && images !== null
        && kind !== "video" && urn.length > 0
    readonly property bool maskingSupported: GraphicsInfo.api !== GraphicsInfo.Software
        && GraphicsInfo.api !== GraphicsInfo.OpenVG && GraphicsInfo.api !== GraphicsInfo.Unknown
        && GraphicsInfo.api !== GraphicsInfo.Null

    implicitWidth: 48
    implicitHeight: 48
    width: 48
    radius: 6
    color: colors["bg-elevated"]
    clip: true
    Accessible.ignored: true

    function updateState() {
        const held = thumb.lease
        if (!held || held.manager !== images || held.urn !== urn) return
        const nextUrl = images.imageUrl(urn)
        const nextFailed = images.failed(urn)
        if (imageFailed !== nextFailed) imageFailed = nextFailed
        if (imageUrl !== nextUrl) imageUrl = nextUrl
    }
    function refresh() {
        const ticket = ++thumb.refreshTicket
        const previous = thumb.lease
        thumb.lease = null
        const wanted = thumb.wantsLease ? images : null
        if (!wanted) {
            if (imageUrl.length > 0) imageUrl = ""
            if (imageReady) imageReady = false
            if (imageFailed) imageFailed = false
            if (previous) previous.manager.release(previous.token)
            return
        }
        if (previous && previous.manager === wanted && previous.urn === urn && !imageFailed) {
            thumb.lease = previous
            updateState()
            return
        }
        if (previous) previous.manager.release(previous.token)
        if (imageReady) imageReady = false
        if (imageFailed) imageFailed = false
        const token = wanted.acquire(urn)
        // The urn can change synchronously inside acquire()'s network kick-off.
        if (ticket !== thumb.refreshTicket || !thumb.wantsLease) {
            if (token) wanted.release(token)
            return
        }
        thumb.lease = token ? {manager: wanted, urn: urn, token: token} : null
        updateState()
    }

    onUrnChanged: refresh()
    onKindChanged: refresh()
    onActiveChanged: refresh()
    onVisibleChanged: refresh()
    onImagesChanged: refresh()
    Component.onCompleted: refresh()
    Component.onDestruction: {
        ++thumb.refreshTicket
        const previous = thumb.lease
        thumb.lease = null
        if (previous) previous.manager.release(previous.token)
    }
    Connections {
        target: thumb.images
        enabled: thumb.images !== null
        function onScopeChanged() { thumb.refresh() }
        function onRevisionChanged() { thumb.updateState() }
    }

    // A video reference keeps the media viewer badge instead of a decoded frame.
    Rectangle {
        objectName: thumb.objectName + "-badge"
        visible: thumb.kind === "video"
        anchors.fill: parent
        radius: thumb.radius
        color: "#66000000"
        Accessible.ignored: true
        Image {
            anchors.centerIn: parent
            source: "qrc:/workspace/icons/videos.svg"
            sourceSize.width: 20
            sourceSize.height: 20
            asynchronous: false
            Accessible.ignored: true
        }
    }

    Image {
        id: thumbImage
        objectName: thumb.objectName + "-image"
        anchors.fill: parent
        source: thumb.imageUrl
        asynchronous: true
        cache: false
        smooth: true
        fillMode: Image.PreserveAspectCrop
        visible: thumb.kind !== "video" && thumb.imageUrl.length > 0
        onStatusChanged: if (status === Image.Ready) thumb.imageReady = true
        layer.enabled: visible && status === Image.Ready && thumb.maskingSupported
        layer.effect: MultiEffect {
            autoPaddingEnabled: false
            maskEnabled: true
            maskSource: thumbMask
        }
        Accessible.ignored: true
    }
    Rectangle {
        id: thumbMask
        width: thumbImage.width
        height: thumbImage.height
        radius: thumb.radius
        color: "white"
        visible: false
        layer.enabled: thumbImage.layer.enabled
        Accessible.ignored: true
    }
}
