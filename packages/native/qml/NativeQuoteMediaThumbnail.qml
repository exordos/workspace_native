pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects

// Issue #217 (docs/NATIVE_DESKTOP_QUOTE_MEDIA_THUMBNAILS.md): the leading media
// thumbnail of a reply or quote frame. Reference shape is
// `min-h-12 w-12 shrink-0 self-stretch overflow-hidden rounded-md
// bg-bg-elevated` with an object-cover image
// (messenger-workspace-media-thumbnail.ui.tsx:150-160 at workspace_ui
// 230e02b8). While loading or failed the element is decorative and the media
// label stays in the frame text; once ready the label leaves the text and the
// box itself carries the media description for assistive technology.
//
// Loading goes through the bounded authenticated manager: exactly one lease is
// held while the frame demands a preview, and `mediaStatus` follows the
// reference state machine (the name avoids shadowing QQuickItem::status). A
// failed load reports "failed", hides the box and keeps the text-only layout,
// and a video reports "ready" with the media viewer play badge because the
// image manager never decodes video sources.
Rectangle {
    id: thumb
    required property var colors
    property var images: nativeMessageImages
    property string urn: ""
    property string kind: "image"
    property bool active: true

    property string imageUrl: ""
    property bool imageFailed: false
    // Caller-driven visibility. The component folds its own failure into
    // `visible` (below), so no caller binding ever reads `mediaStatus` to
    // drive visibility: a visible <- mediaStatus edge writes `visible` from
    // inside mediaStatus propagation, which Qt reports as a binding loop in
    // virtualized delegates.
    property bool demand: true
    // Latched per urn: a failed load must keep reporting "failed" for the same
    // urn even when an intermediate refresh() drops the in-flight failure, so
    // the frame stays text-only instead of re-showing an empty box mid-retry
    // (issue #217 exo-review).
    property string failedLoadUrn: ""
    property var lease: null
    property int refreshTicket: 0

    // The manager only publishes a URL after a verified decode, so a non-empty
    // URL is the native equivalent of the reference `ready` status.
    readonly property string mediaStatus: kind === "video" ? "ready"
        : imageFailed || (urn.length > 0 && failedLoadUrn === urn)
            ? "failed" : imageUrl.length > 0 ? "ready" : "loading"
    // A failed preview hides the box so the frame restores the text-only
    // layout and the media label stays in the excerpt (reference returns
    // null on error).
    visible: demand && mediaStatus !== "failed"
    readonly property bool wantsLease: active && demand && images !== null
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
    // The frame body drops the media label once the picture is ready, so from
    // that moment the box itself carries the media description for assistive
    // technology (issue #217 exo-review). While loading or failed the label
    // stays in the visible text and the box remains decorative.
    Accessible.role: Accessible.Graphic
    Accessible.name: thumb.mediaStatus === "ready"
        ? (thumb.kind === "video" ? qsTr("Video") : qsTr("Image")) : ""
    Accessible.ignored: thumb.mediaStatus !== "ready"

    function updateState() {
        const held = thumb.lease
        if (!held || held.manager !== images || held.urn !== urn) return
        const nextUrl = images.imageUrl(urn)
        const nextFailed = images.failed(urn)
        if (imageFailed !== nextFailed) imageFailed = nextFailed
        if (nextFailed) failedLoadUrn = urn
        else if (failedLoadUrn === urn) failedLoadUrn = ""
        if (imageUrl !== nextUrl) imageUrl = nextUrl
    }
    function refresh() {
        const ticket = ++thumb.refreshTicket
        const previous = thumb.lease
        thumb.lease = null
        const wanted = thumb.wantsLease ? images : null
        if (!wanted) {
            // Losing demand releases the lease but keeps the load state, so a
            // demand round-trip restores the same preview without a reload
            // flash and a failure latch is never rewritten from inside the
            // propagation that hid the box.
            if (previous) previous.manager.release(previous.token)
            return
        }
        if (previous && previous.manager === wanted && previous.urn === urn && !imageFailed) {
            thumb.lease = previous
            updateState()
            return
        }
        if (previous) previous.manager.release(previous.token)
        if (imageFailed) imageFailed = false
        if (failedLoadUrn === urn) failedLoadUrn = ""
        const token = wanted.acquire(urn)
        // The urn can change synchronously inside acquire()'s network kick-off.
        if (ticket !== thumb.refreshTicket || !thumb.wantsLease) {
            if (token) wanted.release(token)
            return
        }
        thumb.lease = token ? {manager: wanted, urn: urn, token: token} : null
        updateState()
    }

    // Demand can flip through any of the inputs below, and a QML binding only
    // re-evaluates the flag itself, so the lease work keys off `wantsLease` and
    // keeps the individual handlers for the same-value edge cases. `visible`
    // is deliberately NOT an input: it is derived from mediaStatus, and a
    // refresh() on its change would clear the failure state mid-propagation
    // and re-acquire the failed file in a loop.
    onWantsLeaseChanged: refresh()
    onUrnChanged: refresh()
    onKindChanged: refresh()
    onActiveChanged: refresh()
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
        onStatusChanged: if (status === Image.Error) {
            thumb.imageFailed = true; thumb.failedLoadUrn = thumb.urn
        }
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
