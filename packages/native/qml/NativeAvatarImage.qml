import QtQuick

// This child preserves the existing circle, border and fallback geometry.
// The manager owns bounded decoded thumbnails; Qt's separate URL cache is off.
Item {
    id: photo
    property var images: nativeAvatars
    property string reference: ""
    property bool loadActive: true
    property bool initialized: false
    property var leaseState: null
    property int refreshTicket: 0
    property string imageUrl: ""
    readonly property bool replacesFallback: imageUrl.length > 0 && image.status !== Image.Error

    // Publish the pure manager result without a QML binding: an output observer
    // may synchronously change the scope/reference while this notification runs.
    function updateUrl() {
        const held = leaseState
        const next = initialized && images && held && held.images === images ? images.imageUrl(reference) : ""
        if (imageUrl !== next) imageUrl = next
    }
    function refresh() {
        if (!initialized) return
        const ticket = ++refreshTicket
        if (ticket !== refreshTicket) return
        const previous = leaseState
        leaseState = null
        updateUrl()
        if (previous) previous.images.release(previous.token)
        if (ticket !== refreshTicket) return
        const wanted = visible && loadActive ? images : null
        if (!wanted) return
        const urn = reference
        const acquired = wanted.acquire(urn)
        // acquire/release can synchronously publish a new scope. A nested refresh
        // owns its newer lease; the outer call may only release its own result.
        if (ticket !== refreshTicket || wanted !== images || urn !== reference || !visible || !loadActive) {
            if (acquired) wanted.release(acquired)
            return
        }
        leaseState = acquired ? { images: wanted, token: acquired } : null
        updateUrl()
    }
    onImagesChanged: refresh()
    onReferenceChanged: refresh()
    onVisibleChanged: refresh()
    onLoadActiveChanged: refresh()
    Component.onCompleted: { initialized = true; refresh() }
    Component.onDestruction: {
        initialized = false
        ++refreshTicket
        const previous = leaseState
        leaseState = null
        updateUrl()
        if (previous) previous.images.release(previous.token)
    }
    Connections {
        target: photo.images
        function onScopeChanged() { photo.refresh() }
        function onRevisionChanged() { photo.updateUrl() }
    }
    Image {
        id: image
        x: 1; y: 1
        width: Math.max(0, photo.width - 2)
        height: Math.max(0, photo.height - 2)
        source: photo.imageUrl
        asynchronous: true
        cache: false
        smooth: true
        visible: photo.replacesFallback
        Accessible.ignored: true
    }
    Accessible.ignored: true
}
