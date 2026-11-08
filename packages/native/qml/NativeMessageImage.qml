import QtQuick
import QtQuick.Effects

// Authenticated loading belongs to the bounded manager. This view holds one
// demand only while its current body is near the real message viewport.
Item {
    id: photo
    required property var colors
    property var images: nativeMessageImages
    property var viewport: null
    property string reference: ""
    property string label: ""
    property real hintWidth: 0
    property real hintHeight: 0
    property bool bodyActive: true
    property bool initialized: false
    property var leaseState: null
    property int refreshTicket: 0
    property var viewState: ({})
    signal opened()

    readonly property bool currentState: viewState.images === images && viewState.reference === reference
        && Number(viewState.scope) === Number(images ? images.scopeRevision : 0)
    // Output observers may replace the scope/reference synchronously. Publish
    // the URL imperatively so their callback never re-enters its QML binding.
    property string imageUrl: ""
    readonly property bool failed: currentState && Boolean(viewState.failed)
    readonly property real naturalWidth: currentState && Number(viewState.width) > 0 ? Number(viewState.width)
        : hintWidth > 0 && hintHeight > 0 ? hintWidth : 240
    readonly property real naturalHeight: currentState && Number(viewState.height) > 0 ? Number(viewState.height)
        : hintWidth > 0 && hintHeight > 0 ? hintHeight : 180
    implicitWidth: naturalWidth * Math.min(1, 180 / naturalHeight)
    implicitHeight: naturalHeight * Math.min(1, 180 / naturalHeight)
    height: implicitWidth > 0 ? implicitHeight * Math.min(1, width / implicitWidth) : 0

    readonly property real viewportY: {
        if (!viewport) return NaN
        let node = photo, offset = 0, depth = 0
        // Read the real ancestor geometry, so ListView reuse, body growth and
        // scrolling all invalidate this binding; mapToItem alone is not a
        // dependency on the ancestors' positions.
        while (node && node !== viewport.contentItem && depth++ < 32) {
            offset += node.y
            node = node.parent
        }
        return node === viewport.contentItem ? offset - viewport.contentY : NaN
    }
    readonly property bool loadActive: bodyActive && viewport
        && Number.isFinite(viewportY) && viewportY + height >= -640 && viewportY <= viewport.height + 640

    function updateState() {
        const ticket = refreshTicket
        const manager = images, urn = reference, alive = initialized
        const held = leaseState
        const wanted = alive && manager && held && held.images === manager && held.reference === urn ? manager : null
        const info = wanted ? wanted.info(urn) : ({})
        const scope = manager ? manager.scopeRevision : 0
        const same = viewState.images === manager && viewState.reference === urn && Number(viewState.scope) === Number(scope)
        // Keep measured layout dimensions across an ordinary viewport release.
        // Resetting them to the unknown reservation could oscillate at an edge.
        const nextUrl = wanted ? wanted.imageUrl(urn) : ""
        const next = { images: manager, reference: urn, scope: scope,
            width: Number(info.width) || (same ? Number(viewState.width) || 0 : 0),
            height: Number(info.height) || (same ? Number(viewState.height) || 0 : 0),
            failed: wanted ? wanted.failed(urn) : false }
        if (!same || next.width !== viewState.width || next.height !== viewState.height
                || next.failed !== viewState.failed)
            viewState = next
        // Geometry/failure notifications can start a newer refresh too. Its
        // state owns the view; the older call must not publish its stale URL.
        if (ticket !== refreshTicket || manager !== images || urn !== reference || alive !== initialized
                || held !== leaseState || Number(scope) !== Number(manager ? manager.scopeRevision : 0)) return
        if (imageUrl !== nextUrl) imageUrl = nextUrl
    }
    function refresh() {
        if (!initialized) return
        const ticket = ++refreshTicket
        if (ticket !== refreshTicket) return
        const previous = leaseState
        leaseState = null
        if (ticket === refreshTicket) updateState()
        if (previous) previous.images.release(previous.token)
        if (ticket !== refreshTicket) return
        const wanted = visible && loadActive ? images : null
        if (!wanted) return
        const urn = reference
        const token = wanted.acquire(urn)
        if (ticket !== refreshTicket || wanted !== images || urn !== reference || !visible || !loadActive) {
            if (token) wanted.release(token)
            return
        }
        leaseState = token ? { images: wanted, reference: urn, token: token } : null
        updateState()
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
        updateState()
        if (previous) previous.images.release(previous.token)
    }
    Connections {
        target: photo.images
        function onScopeChanged() { photo.refresh() }
        function onRevisionChanged() { photo.updateState() }
    }
    Rectangle {
        anchors.fill: parent
        color: photo.imageUrl.length ? "transparent" : photo.colors["card-bg"]
        radius: 6
    }
    Image {
        id: image
        anchors.fill: parent
        source: photo.imageUrl
        asynchronous: true
        cache: false
        smooth: true
        fillMode: Image.PreserveAspectFit
        visible: photo.imageUrl.length > 0
        readonly property bool maskingSupported: GraphicsInfo.api !== GraphicsInfo.Software
            && GraphicsInfo.api !== GraphicsInfo.OpenVG && GraphicsInfo.api !== GraphicsInfo.Unknown && GraphicsInfo.api !== GraphicsInfo.Null
        layer.enabled: visible && status === Image.Ready && maskingSupported
        layer.effect: MultiEffect {
            autoPaddingEnabled: false
            maskEnabled: true
            maskSource: roundedMask
        }
        Accessible.ignored: true
    }
    Rectangle {
        id: roundedMask
        width: image.width; height: image.height
        radius: 4
        color: "white"
        visible: false
        layer.enabled: image.layer.enabled
        Accessible.ignored: true
    }
    MouseArea {
        anchors.fill: parent
        // Saving the original attachment does not require a decoded preview.
        // The existing caller validates the owned route before opening Save.
        enabled: photo.loadActive && photo.reference.length > 0
        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: photo.opened()
    }
    NativeButton {
        colors: photo.colors
        anchors.centerIn: parent
        visible: photo.failed
        enabled: photo.loadActive && photo.leaseState !== null
        text: qsTr("Retry")
        onClicked: if (photo.images && photo.loadActive && photo.leaseState) photo.images.retry(photo.reference)
    }
    Accessible.role: Accessible.Graphic
    Accessible.name: label
}
