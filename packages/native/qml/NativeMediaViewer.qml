import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtMultimedia

// Full-size media viewer overlay (issue #8, docs/NATIVE_DESKTOP_MEDIA_VIEWER.md;
// video slots added by issue #203, docs/NATIVE_DESKTOP_MEDIA_VIEWER_VIDEO.md).
// Every geometry and colour value below was measured from the React reference
// at workspace_ui 61dacef78dab2f5d0be76f283bebf49fe29b84af; file citations name
// the source line.
Popup {
    id: viewerRoot
    objectName: "mediaViewer-popup"
    required property var viewer
    required property var colors
    signal requestDownload(string fileUuid)

    readonly property var hostWindow: viewerSurface.Window.window
    readonly property var currentSlot: viewer.open && viewer.index < viewer.items.length
        ? viewer.items[viewer.index] || ({}) : ({})
    readonly property bool gallery: viewer.count > 1
    property real zoom: 1
    // Issue #730: drag-to-pan state for the zoomed image, in scene pixels.
    // Raw values are write-clamped to the current bounds and the drawn offset
    // clamps again at read time (see mediaImage x/y), so bounds recalculate
    // whenever zoom or the window size changes and an axis whose picture
    // fits (bound 0) is always centered.
    property real panX: 0
    property real panY: 0
    readonly property int selectedSlot: viewer.open ? viewer.index : -1
    // Video controls-bar state (issue #731, docs/NATIVE_DESKTOP_VIDEO_PLAYBACK.md):
    // Chromium convention cloned - controls show while paused or on pointer
    // activity and auto-hide 2.5 s into playback. The timer itself lives with
    // the media area; pokeControls() is the single seam that (re)arms it.
    property bool controlsShown: true
    // Issue #784: keyboard focus inside the controls bar pins it open (a
    // focused control with a hidden bar leaves the keyboard user without any
    // visual feedback). Resolved through the window's active focus item so
    // every focusable child - both glyphs and both sliders - is covered
    // without per-control wiring.
    readonly property Item activeFocusTarget: hostWindow ? hostWindow.activeFocusItem : null
    readonly property bool controlsFocusWithin: {
        // Walk up from the focused item (QQuickItem exposes no invokable
        // ancestry test to QML) and stop at the bar or the focus scope.
        for (let item = activeFocusTarget; item !== null; item = item.parent)
            if (item === videoControls)
                return true;
        return false;
    }
    // Focusing in re-pins the bar; moving focus out restarts the countdown,
    // so "moving focus elsewhere allows normal auto-hide" (#784 acceptance).
    onControlsFocusWithinChanged: viewerRoot.pokeControls()
    // Shared gate for the reveal Shortcuts: armed only while a ready video
    // slot sits auto-hidden with focus outside the bar.
    readonly property bool controlsRevealArmed: currentSlot.state === "ready"
        && String(currentSlot.kind || "") === "video"
        && !controlsShown && !controlsFocusWithin
    function pokeControls() {
        viewerRoot.controlsShown = true;
        controlsHideTimer.restart();
    }
    // m:ss for the time label; a not-yet-known duration (0/negative) renders
    // the --:-- placeholder, exactly what the label contract expects.
    function formatTime(ms) {
        if (!(ms > 0))
            return "--:--";
        const total = Math.floor(ms / 1000);
        const secs = total % 60;
        return Math.floor(total / 60) + (secs < 10 ? ":0" : ":") + secs;
    }
    onSelectedSlotChanged: {
        zoom = 1;
        // #730: pan resets with the zoom whenever the slot changes (the close
        // -> reopen cycle also passes through -1, so a reopen resets too).
        panX = 0;
        panY = 0;
        // Stop-on-switch/close contract (issue #731): audio must never leak
        // from the previous slot while the new one stages or the session is
        // closed. Clearing the source binding is not enough - the explicit
        // stop settles the pipeline on the spot.
        videoPlayer.stop();
        viewerRoot.controlsShown = true;
        Qt.callLater(statusLabel.announceStatus);
    }

    // Issue #919 reference zoom contract (media-viewer-zoom.hook.ts):
    // clamp [MIN_SCALE=1, MAX_SCALE=5], multiplicative wheel
    // scale * exp(delta * 0.0035) anchored at the cursor, 1.25x steps for
    // the buttons and the keyboard anchored at the image center, 48 px
    // Shift+arrow keyboard pan while zoomed. The wheel unit maps Qt's
    // pixelDelta (trackpad) else angleDelta (120/notch) onto the reference
    // delta (docs/NATIVE_DESKTOP_MEDIA_VIEWER.md); video slots keep zoom
    // disabled but still consume the wheel so nothing behind scrolls.
    readonly property real minScale: 1
    readonly property real maxScale: 5
    readonly property real stepFactor: 1.25
    readonly property int keyboardPanStep: 48
    readonly property bool zoomableSlot: currentSlot.state === "ready"
        && String(currentSlot.kind || "") !== "video"
    function zoomByWheel(wheel) {
        wheel.accepted = true;
        if (!zoomableSlot)
            return;
        const delta = wheel.pixelDelta.y || wheel.angleDelta.y;
        const next = Math.max(minScale, Math.min(maxScale,
                                                 zoom * Math.exp(delta * 0.0035)));
        const applied = next / zoom;
        // Cursor anchoring: the pixel under the cursor stays under the
        // cursor. Offsets are measured from the media area center, where
        // the scaled picture is centered at pan 0.
        panX = (wheel.x - mediaArea.width / 2) * (1 - applied) + panX * applied;
        panY = (wheel.y - mediaArea.height / 2) * (1 - applied) + panY * applied;
        zoom = next; // onZoomChanged re-clamps the pan into the new bounds
    }
    // Button/keyboard step anchored at the image center: the picture center
    // keeps its on-screen offset while the scale changes.
    function zoomByStep(factor) {
        const next = Math.max(minScale, Math.min(maxScale, zoom * factor));
        const applied = next / zoom;
        panX = panX * applied;
        panY = panY * applied;
        zoom = next;
    }
    function resetZoom() {
        zoom = 1;
        panX = 0;
        panY = 0;
    }
    // Shift+arrow pan (reference): only while zoomed, write-clamped to the
    // current bounds so no hidden excess can strand the picture (#762 rule).
    function panBy(dx, dy) {
        if (!(zoom > 1))
            return;
        panX = Math.max(-mediaImage.panBoundX,
                        Math.min(mediaImage.panBoundX, panX + dx));
        panY = Math.max(-mediaImage.panBoundY,
                        Math.min(mediaImage.panBoundY, panY + dy));
    }
    // #730: when the zoom shrinks the bounds (e.g. back to fit), bring the raw
    // pan values along so a zoom-out centers the picture and a zoom-in starts
    // centered instead of resurrecting a stale edge pan. Bounds are computed
    // from the primitives here - mediaImage.panBoundX is bound to zoom and its
    // re-evaluation order against this handler is not contractual.
    onZoomChanged: {
        const boundX = Math.max(0, (mediaImage.paintedWidth * zoom - mediaArea.width) / 2);
        const boundY = Math.max(0, (mediaImage.paintedHeight * zoom - mediaArea.height) / 2);
        panX = Math.max(-boundX, Math.min(boundX, panX));
        panY = Math.max(-boundY, Math.min(boundY, panY));
    }

    // Positional model for the strip: a live viewer.items model would rebuild
    // delegates on every slot download and destroy items under a click.
    readonly property var stripModel: gallery
        ? Array.from({length: viewer.count}, (_, i) => i) : []

    // The same full-window surface is modal even before media is ready.
    // Popup owns keyboard/tab isolation and restores the previous focus item.
    parent: Overlay.overlay
    width: parent ? parent.width : 0
    height: parent ? parent.height : 0
    padding: 0
    margins: 0
    background: null
    modal: true
    dim: false
    focus: true
    closePolicy: Popup.NoAutoClose
    visible: viewer.open
    contentItem: FocusScope {
        id: viewerSurface
        objectName: "mediaViewer-overlay"
        focus: true
        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("Media viewer")
    }
    // Issue #784: Qt's tab chain skips invisible items, so once the bar has
    // auto-hidden the keyboard cannot reach it anymore. The first Tab or
    // Shift+Tab press (enabled only while a ready video slot sits hidden with
    // focus outside the bar) reveals the bar; the press is consumed as a
    // reveal gesture only in that state, and the next Tab / Shift+Tab walks
    // the focus chain normally onto the controls, where controlsFocusWithin
    // keeps the bar open. Both directions need the gesture: the reverse chain
    // skips invisible items exactly like the forward one (review round).
    Shortcut {
        enabled: viewerRoot.controlsRevealArmed
        sequence: "Tab"
        onActivated: viewerRoot.pokeControls()
    }
    Shortcut {
        enabled: viewerRoot.controlsRevealArmed
        sequence: "Shift+Tab"
        onActivated: viewerRoot.pokeControls()
    }

    // Backdrop: fixed inset-0 z-modal bg-black/90 (media-viewer-backdrop.ui.tsx:56);
    // the root click closes (media-viewer-backdrop.ui.tsx:53-64).
    MouseArea {
        objectName: "mediaViewer-backdrop"
        anchors.fill: parent
        onWheel: (wheel) => { wheel.accepted = true; }
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
            scale: viewerRoot.zoom
            width: parent.width
            height: Math.min(parent.height, hostWindow ? Math.max(0, hostWindow.height - 192) : parent.height)
            // #730: pan bounds measure the scaled PAINTED pixels against the
            // media area, so an axis whose picture fits has bound 0 and stays
            // centered, while a zoomed axis pans until either picture edge
            // reaches the area edge. The read-side clamp recalculates on every
            // zoom or window-size change, so an out-of-range pan snaps back
            // without any imperative repair.
            readonly property real panBoundX:
                Math.max(0, (paintedWidth * viewerRoot.zoom - mediaArea.width) / 2)
            readonly property real panBoundY:
                Math.max(0, (paintedHeight * viewerRoot.zoom - mediaArea.height) / 2)
            // #762 review round: a window resize shrinks the bounds through
            // mediaArea/the painted box rather than through zoom, so clamp the
            // raw pan here too. Without this the stored value keeps a hidden
            // excess after a shrink: the drawn offset clamps at read time, so
            // the picture LOOKS pinned at the new edge, but the next press
            // captures the raw value and the reverse drag must consume the
            // slack before the picture moves. The bound-change clamp keeps the
            // stored state in range at all times (onZoomChanged is the same
            // recipe for the zoom seam).
            onPanBoundXChanged: viewerRoot.panX =
                Math.max(-panBoundX, Math.min(panBoundX, viewerRoot.panX))
            onPanBoundYChanged: viewerRoot.panY =
                Math.max(-panBoundY, Math.min(panBoundY, viewerRoot.panY))
            x: (mediaArea.width - width) / 2
                + Math.max(-panBoundX, Math.min(panBoundX, viewerRoot.panX))
            y: (mediaArea.height - height) / 2
                + Math.max(-panBoundY, Math.min(panBoundY, viewerRoot.panY))
            source: viewerRoot.visible && viewerRoot.currentSlot.state === "ready"
                && String(viewerRoot.currentSlot.kind || "image") !== "video"
                ? String(viewerRoot.currentSlot.url || "") : ""
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
            parent: mediaImage
            anchors.fill: parent
            onWheel: (wheel) => viewerRoot.zoomByWheel(wheel)
            enabled: mediaImage.visible
            // #730: drag-to-pan with the primary button. mouse.x/y are
            // mediaImage-local and the image slides under the cursor while
            // panning, so local deltas drift (measured with a 60 px drag);
            // QQuickMouseEvent.sceneX is not exposed here either (NaN in
            // practice). Each event converts its local point to the scene via
            // mapToItem(null, ...), which is frame-correct under the live
            // scale transform, and the pan tracks the scene-space delta.
            property real pressSceneX: 0
            property real pressSceneY: 0
            property real panAtPressX: 0
            property real panAtPressY: 0
            property bool draggingPan: false
            onPressed: (mouse) => {
                const p = mapToItem(null, mouse.x, mouse.y);
                pressSceneX = p.x;
                pressSceneY = p.y;
                panAtPressX = viewerRoot.panX;
                panAtPressY = viewerRoot.panY;
                draggingPan = false;
            }
            onPositionChanged: (mouse) => {
                if (!pressed)
                    return;
                const p = mapToItem(null, mouse.x, mouse.y);
                const dx = p.x - pressSceneX;
                const dy = p.y - pressSceneY;
                if (!draggingPan && Math.abs(dx) + Math.abs(dy) < 6)
                    return;
                draggingPan = true;
                viewerRoot.panX = Math.max(-mediaImage.panBoundX,
                    Math.min(mediaImage.panBoundX, panAtPressX + dx));
                viewerRoot.panY = Math.max(-mediaImage.panBoundY,
                    Math.min(mediaImage.panBoundY, panAtPressY + dy));
            }
            onClicked: (mouse) => {
                // A drag-pan release is not a click: the viewer must stay open
                // even when the finger lifted over a letterbox band.
                if (draggingPan)
                    return;
                const insidePainted = mediaImage.status === Image.Ready
                    && Math.abs(mouse.x - mediaImage.width / 2) <= mediaImage.paintedWidth / 2 + 1
                    && Math.abs(mouse.y - mediaImage.height / 2) <= mediaImage.paintedHeight / 2 + 1;
                if (!insidePainted)
                    viewerRoot.viewer.close();
            }
        }
        // Ready video slots play inline (media-viewer-overlay.ui.tsx:152-161):
        // source, controls-equivalent play/pause, autoPlay and the
        // onError display-error path; the box and height cap match the image
        // (px-16 pb-28 pt-14 with calc(100vh-12rem), :126,158).
        MediaPlayer {
            id: videoPlayer
            objectName: "mediaViewer-player"
            autoPlay: true
            videoOutput: mediaVideo
            // The player has no audio sink of its own: connected only to the
            // VideoOutput, a clip's audio track is silently dropped
            // (exo-review finding A). On this Qt the binding direction is
            // MediaPlayer.audioOutput; the sink names the active slot.
            audioOutput: videoAudio
            // A bar interaction always counts as pointer activity (the buttons
            // call pokeControls() themselves); playback state changes own the
            // bar: start arms the auto-hide countdown, anything but active
            // playback pins it open (the browser behaves the same: a paused or
            // ended video always shows its controls, so a short clip can never
            // end and hide the bar before the user can act on it).
            onPlaybackStateChanged: {
                if (playbackState === MediaPlayer.PlayingState)
                    viewerRoot.pokeControls();
                else
                    viewerRoot.controlsShown = true;
            }
            source: viewerRoot.visible && viewerRoot.currentSlot.state === "ready"
                && String(viewerRoot.currentSlot.kind || "") === "video"
                ? String(viewerRoot.currentSlot.url || "") : ""
            // A staged file the decoder rejects maps onto the reference's
            // video onError -> replaceItem display-error contract
            // (media-viewer-overlay.ui.tsx:75-90). An empty source is the
            // slot not being a ready video, never a display failure.
            onErrorOccurred: (error, errorString) => {
                if (String(source).length > 0)
                    viewerRoot.viewer.markSlotDisplayError(viewerRoot.viewer.index);
            }
        }
        // One AudioOutput follows the active slot because the viewer drives a
        // single shared player (bound above via audioOutput); the objectName
        // names the slot and the mirror properties expose the wired player
        // for the window test.
        AudioOutput {
            id: videoAudio
            objectName: "mediaViewer-audio-" + viewerRoot.viewer.index
            readonly property bool playerBound: videoPlayer.audioOutput === videoAudio
            readonly property string playerObjectName: videoPlayer.objectName
            readonly property int playerPlaybackState: videoPlayer.playbackState
        }
        VideoOutput {
            id: mediaVideo
            objectName: "mediaViewer-video"
            scale: viewerRoot.zoom
            property int playbackState: videoPlayer.playbackState
            anchors.centerIn: parent
            width: parent.width
            height: Math.min(parent.height, hostWindow ? Math.max(0, hostWindow.height - 192) : parent.height)
            fillMode: VideoOutput.PreserveAspectFit
            visible: viewerRoot.visible && viewerRoot.currentSlot.state === "ready"
                && String(viewerRoot.currentSlot.kind || "") === "video"
            Accessible.ignored: true
        }
        // The reference media element consumes its clicks
        // (media-viewer-overlay.ui.tsx:159); a browser's native video controls
        // toggle play/pause on a picture click, which this reproduces.
        MouseArea {
            objectName: "mediaViewer-video-toggle"
            parent: mediaVideo
            anchors.fill: parent
            onWheel: (wheel) => viewerRoot.zoomByWheel(wheel)
            enabled: mediaVideo.visible
            onClicked: (mouse) => {
                const painted = mediaVideo.contentRect;
                const insidePainted = painted.width > 0
                    && mouse.x >= painted.x - 1 && mouse.x <= painted.x + painted.width + 1
                    && mouse.y >= painted.y - 1 && mouse.y <= painted.y + painted.height + 1;
                if (!insidePainted) {
                    viewerRoot.viewer.close();
                    return;
                }
                if (videoPlayer.playbackState === MediaPlayer.PlayingState)
                    videoPlayer.pause();
                else
                    videoPlayer.play();
            }
        }
        // Auto-hide countdown for the controls bar (issue #731): expires only
        // mid-playback and only while the pointer is not on the picture
        // (hovering pins the bar open like the browser); a paused or ended
        // clip always shows the bar, so a short clip can never end and hide
        // the controls before the user can act.
        Timer {
            id: controlsHideTimer
            objectName: "mediaViewer-controlsHideTimer"
            interval: 2500
            onTriggered: if (videoPlayer.playbackState === MediaPlayer.PlayingState
                    && !videoHover.pointing)
                viewerRoot.controlsShown = false
        }
        // Any pointer movement over the video re-shows the bar and re-arms
        // the countdown (Chromium's seek-on-move convention).
        HoverHandler {
            id: videoHover
            parent: mediaVideo
            onPointChanged: viewerRoot.pokeControls()
        }
        // Video controls bar (issue #731, docs/NATIVE_DESKTOP_VIDEO_PLAYBACK.md):
        // the reference renders browser-native <video controls>, so no custom
        // geometry exists to clone (design doc gap section) - this is NEW
        // functionality modeled on the Chromium bar: play/pause, time, seek,
        // mute and volume overlaid on the bottom 56 px of the video element
        // box (a 60%-opacity gradient, controls row 40 px, 12 px side padding,
        // 20 px glyphs, 13 px #B3FFFFFF text). It is a media-area sibling that
        // anchors onto the video box (the strip pattern), declared after the
        // video so its controls stack above the picture's click targets.
        Rectangle {
            id: videoControls
            objectName: "mediaViewer-controls"
            anchors.left: mediaVideo.left
            anchors.right: mediaVideo.right
            anchors.bottom: mediaVideo.bottom
            height: 56
            visible: (viewerRoot.controlsShown || viewerRoot.controlsFocusWithin)
                && viewerRoot.currentSlot.state === "ready"
                && String(viewerRoot.currentSlot.kind || "") === "video"
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#00000000" }
                GradientStop { position: 1.0; color: "#99000000" }
            }
            // The bar consumes its own clicks: a press on the overlay must
            // neither toggle playback (that is the picture click) nor reach
            // the letterbox fallthrough that closes the viewer.
            MouseArea {
                anchors.fill: parent
                onClicked: {}
            }
            ViewerGlyph {
                id: playGlyph
                objectName: "mediaViewer-play"
                anchors.left: parent.left
                anchors.leftMargin: 12
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 8
                readonly property bool playing: videoPlayer.playbackState === MediaPlayer.PlayingState
                asset: playing ? "pause.svg" : "play_arrow.svg"
                Accessible.name: playing ? qsTr("Pause") : qsTr("Play")
                onClicked: {
                    viewerRoot.pokeControls();
                    if (videoPlayer.playbackState === MediaPlayer.PlayingState)
                        videoPlayer.pause();
                    else
                        videoPlayer.play();
                }
            }
            Text {
                id: timeLabel
                objectName: "mediaViewer-time"
                anchors.left: playGlyph.right
                anchors.leftMargin: 8
                anchors.verticalCenter: playGlyph.verticalCenter
                color: "#B3FFFFFF"
                font.pixelSize: 13
                text: videoPlayer.duration > 0
                    ? viewerRoot.formatTime(videoPlayer.position) + " / " + viewerRoot.formatTime(videoPlayer.duration)
                    : "--:-- / --:--"
            }
            Slider {
                id: volumeBar
                objectName: "mediaViewer-volume"
                width: 80
                implicitHeight: 20
                padding: 0
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: playGlyph.verticalCenter
                from: 0
                to: 1
                enabled: !videoAudio.muted
                Accessible.name: qsTr("Volume")
                onMoved: {
                    viewerRoot.pokeControls();
                    // Qt 6 keeps volume/muted on the AudioOutput, not the
                    // MediaPlayer (assignments to videoPlayer.volume throw
                    // "Cannot assign to non-existent property").
                    videoAudio.volume = value;
                }
                background: Rectangle {
                    x: volumeBar.leftPadding
                    y: volumeBar.topPadding + volumeBar.availableHeight / 2 - height / 2
                    width: volumeBar.availableWidth
                    height: 3
                    radius: 1
                    color: "#4DFFFFFF"
                    Rectangle {
                        width: volumeBar.position * parent.width
                        height: 3
                        radius: 1
                        color: "white"
                    }
                }
                // value must follow the live volume without the drag-write
                // killing the binding (the Qt video sample's Binding-when
                // pattern), so release re-syncs and a programmatic volume
                // change still moves the handle.
                Binding {
                    target: volumeBar
                    property: "value"
                    value: videoAudio.volume
                    when: !volumeBar.pressed
                    restoreMode: Binding.RestoreBindingOrValue
                }
            }
            ViewerGlyph {
                id: muteGlyph
                objectName: "mediaViewer-mute"
                anchors.right: volumeBar.left
                anchors.rightMargin: 8
                anchors.verticalCenter: playGlyph.verticalCenter
                asset: videoAudio.muted ? "volume_off.svg" : "volume_up.svg"
                Accessible.name: videoAudio.muted ? qsTr("Unmute") : qsTr("Mute")
                onClicked: {
                    viewerRoot.pokeControls();
                    videoAudio.muted = !videoAudio.muted;
                }
            }
            Slider {
                id: seekBar
                objectName: "mediaViewer-seek"
                implicitHeight: 20
                padding: 0
                anchors.left: timeLabel.right
                anchors.leftMargin: 12
                anchors.right: muteGlyph.left
                anchors.rightMargin: 12
                anchors.verticalCenter: playGlyph.verticalCenter
                from: 0
                to: Math.max(1, videoPlayer.duration)
                Accessible.name: qsTr("Seek")
                onMoved: {
                    viewerRoot.pokeControls();
                    // Qt 6's QML MediaPlayer has no seek() - the writable
                    // position property is the seek call (WRITE setPosition).
                    videoPlayer.position = value;
                }
                background: Rectangle {
                    x: seekBar.leftPadding
                    y: seekBar.topPadding + seekBar.availableHeight / 2 - height / 2
                    width: seekBar.availableWidth
                    height: 3
                    radius: 1
                    color: "#4DFFFFFF"
                    Rectangle {
                        width: seekBar.position * parent.width
                        height: 3
                        radius: 1
                        color: "white"
                    }
                }
                Binding {
                    target: seekBar
                    property: "value"
                    value: Math.min(seekBar.to, videoPlayer.position)
                    when: !seekBar.pressed
                    restoreMode: Binding.RestoreBindingOrValue
                }
            }
        }
        Text {
            id: statusLabel
            objectName: "mediaViewer-status"
            anchors.centerIn: parent
            width: Math.min(480, mediaArea.width)
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            color: "#B3FFFFFF"
            font.pixelSize: 14
            readonly property bool isVideo: String(viewerRoot.currentSlot.kind || "") === "video"
            readonly property bool failed: viewerRoot.currentSlot.state === "load-error"
                || viewerRoot.currentSlot.state === "display-error"
            Accessible.role: failed ? Accessible.AlertMessage : Accessible.StatusBar
            Accessible.name: text
            readonly property string announcement: visible ? text : ""
            onAnnouncementChanged: Qt.callLater(announceStatus)
            function announceStatus() {
                if (announcement)
                    Accessible.announce(announcement, failed ? Accessible.Assertive : Accessible.Polite);
            }
            // Per-state labels exactly as measured
            // (media-viewer-overlay.ui.tsx:127-151).
            text: isVideo
                // mediaViewer.videoLoadFailed
                ? (viewerRoot.currentSlot.state === "load-error"
                    ? qsTr("Failed to load video")
                    // mediaViewer.videoDisplayFailed
                    : viewerRoot.currentSlot.state === "display-error"
                        ? qsTr("This video cannot be played")
                        // mediaViewer.videoLoading
                        : qsTr("Video is loading…"))
                // mediaViewer.unavailable / mediaViewer.loading
                : (viewerRoot.currentSlot.state === "load-error"
                    || viewerRoot.currentSlot.state === "display-error"
                    ? qsTr("This media cannot be displayed. You can download the file instead.")
                    : qsTr("Loading media…"))
            visible: viewerRoot.visible && viewerRoot.currentSlot.state !== "ready"
        }
        // Actionable retry for a failed attachment fetch (issue #731): the
        // reference user's equivalent is the browser's own reload affordance;
        // this re-dispatches the slot download through the model (retrySlot
        // is a no-op for anything but a load-error slot).
        AbstractButton {
            id: retryButton
            objectName: "mediaViewer-retry"
            anchors.horizontalCenter: statusLabel.horizontalCenter
            anchors.top: statusLabel.bottom
            anchors.topMargin: 10
            visible: viewerRoot.visible && viewerRoot.currentSlot.state === "load-error"
            hoverEnabled: true
            implicitWidth: Math.max(72, retryLabel.implicitWidth + 24)
            implicitHeight: 30
            Accessible.name: text
            text: qsTr("Retry")
            contentItem: Text {
                id: retryLabel
                text: retryButton.text
                color: "#B3FFFFFF"
                font.pixelSize: 13
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: 6
                color: retryButton.hovered ? "#33FFFFFF" : "#1AFFFFFF"
                border.width: 1
                border.color: "#4DFFFFFF"
            }
            onClicked: viewerRoot.viewer.retrySlot(viewerRoot.viewer.index)
        }
    }

    // Flat tinted-glyph button of the video controls bar (issue #731):
    // 24x24 hit area, 20 px white glyph, no background fill (Chromium's
    // transparent media-button chrome). Issue #793: the transport glyphs are
    // the two most-used controls of the bar yet showed no pointer feedback at
    // all, while every sibling in this popup (ViewerArrow, ViewerButton, the
    // retry chip) tints on hover. Give them the same response as the retry
    // chip of this popup (hovered #33FFFFFF, pressed one step brighter);
    // the idle state stays transparent, matching the reference's native-
    // controls chrome (media-viewer-overlay.ui.tsx <video controls>).
    component ViewerGlyph: AbstractButton {
        id: glyph
        required property string asset
        implicitWidth: 24
        implicitHeight: 24
        padding: 0
        hoverEnabled: true
        onClicked: viewerRoot.pokeControls()
        background: Rectangle {
            radius: 6
            color: glyph.down ? "#4DFFFFFF"
                : glyph.hovered ? "#33FFFFFF" : "transparent"
        }
        Image {
            anchors.centerIn: parent
            source: "qrc:/workspace/icons/" + glyph.asset
            sourceSize.width: 20
            sourceSize.height: 20
            asynchronous: false
            layer.enabled: true
            layer.effect: MultiEffect {
                colorizationColor: "white"
                colorization: 1.0
            }
        }
    }

    // Issue #919: a zoom-pill button (36 px high, glyph centered, 40% opacity
    // while disabled per the reference opacity-40).
    component ZoomButton: AbstractButton {
        implicitHeight: 36
        padding: 0
        hoverEnabled: true
        opacity: enabled ? 1.0 : 0.4
        background: Rectangle {
            radius: 6
            color: parent.hovered && parent.enabled
                   ? viewerRoot.colors["bg-elevated"] : "transparent"
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
    // "Open in new tab" is reference-browser-only (media-viewer-overlay.ui.tsx:56),
    // so the desktop viewer ships copy (issue #79) + download + close.
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
            objectName: "mediaViewer-copy"
            asset: "copy.svg"
            // showCopy renders only for image slots (media-viewer-overlay.ui.tsx:118);
            // visible actions stay unusable until the display URL exists
            // (media-viewer-actions.lib.ts:71-79).
            visible: viewerRoot.currentSlot.kind === "image"
            enabled: visible && viewerRoot.currentSlot.state === "ready"
                && String(viewerRoot.currentSlot.uuid || "").length > 0
            Accessible.name: qsTr("Copy image")
            onClicked: viewerRoot.viewer.copyCurrentImage()
        }
        ViewerButton {
            objectName: "mediaViewer-download"
            asset: "download.svg"
            // Download follows the file uuid, never the display URL: the
            // reference keeps it enabled for an image slot whose display
            // failed (media-viewer-overlay.ui.tsx:64-68 downloadEnabled —
            // the #638 HEIC render-error case). Slots that never staged
            // (load-error) and video display-errors stay gated
            // (media-viewer-actions.lib.ts:71-79).
            enabled: (viewerRoot.currentSlot.state === "ready"
                    || (viewerRoot.currentSlot.state === "display-error"
                        && viewerRoot.currentSlot.kind === "image"))
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
                    readonly property var slotData: viewerRoot.viewer.items[thumb.modelData] || ({})
                    readonly property bool isVideo: String(slotData.kind || "") === "video"
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
                    // Thumbnail still images are rendered for image slots only
                    // (media-viewer-thumbnails.ui.tsx:23).
                    Image {
                        anchors.fill: parent
                        anchors.margins: 2
                        source: !thumb.isVideo ? String(slotData.url || "") : ""
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        cache: false
                        visible: status === Image.Ready
                        Accessible.ignored: true
                    }
                    // Video slots carry a play badge: inset-0 bg-black/40 with
                    // the videos glyph at 20 px text-primary
                    // (media-viewer-thumbnails.ui.tsx:44-48).
                    Rectangle {
                        objectName: "mediaViewer-thumb-badge-" + thumb.modelData
                        visible: thumb.isVideo
                        anchors.fill: parent
                        anchors.margins: 2
                        radius: 6
                        color: "#66000000"
                        Accessible.ignored: true
                        Image {
                            anchors.centerIn: parent
                            source: "qrc:/workspace/icons/videos.svg"
                            sourceSize.width: 20
                            sourceSize.height: 20
                            asynchronous: false
                            layer.enabled: true
                            layer.effect: MultiEffect {
                                colorizationColor: viewerRoot.colors["text-primary"]
                                colorization: 1.0
                            }
                        }
                    }
                    Accessible.name: qsTr("Media %1").arg(modelData + 1)
                }
            }
        }
    }

    // Issue #919 zoom toolbar pill (media-viewer-overlay.ui.tsx): rounded-lg
    // p-1 gap-1 pill bg-bg-elevated/80 anchored bottom-4 centered (z-float);
    // "-" / reset / "+" buttons h-9 w-9 text-xl, the reset button min-w-14
    // px-1 text-sm with the rounded percentage. Rendered only for a ready
    // IMAGE slot (the video slot keeps zoom disabled); disabled buttons keep
    // their glyph at 40% opacity per the reference opacity-40.
    Rectangle {
        id: zoomBar
        objectName: "mediaViewer-zoomBar"
        visible: viewerRoot.visible && viewerRoot.zoomableSlot
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 16
        anchors.horizontalCenter: parent.horizontalCenter
        width: zoomRow.implicitWidth + 8
        height: zoomRow.implicitHeight + 8
        radius: 8
        color: viewerRoot.colors["bg-elevated"]
        opacity: 0.8
        Accessible.ignored: true
        Row {
            id: zoomRow
            anchors.centerIn: parent
            spacing: 4
            ZoomButton {
                objectName: "mediaViewer-zoomOutButton"
                implicitWidth: 36
                enabled: viewerRoot.zoom > 1
                onClicked: viewerRoot.zoomByStep(1 / viewerRoot.stepFactor)
                Accessible.name: qsTr("Zoom out")
                contentItem: Label {
                    horizontalAlignment: Label.AlignHCenter
                    verticalAlignment: Label.AlignVCenter
                    text: "-"
                    font.pixelSize: 20
                    color: viewerRoot.colors["text-primary"]
                }
            }
            ZoomButton {
                objectName: "mediaViewer-zoomResetButton"
                implicitWidth: Math.max(56, zoomPercent.implicitWidth + 8)
                onClicked: viewerRoot.resetZoom()
                Accessible.name: qsTr("Reset zoom")
                contentItem: Label {
                    id: zoomPercent
                    objectName: "mediaViewer-zoomPercent"
                    horizontalAlignment: Label.AlignHCenter
                    verticalAlignment: Label.AlignVCenter
                    text: Math.round(viewerRoot.zoom * 100) + "%"
                    font.pixelSize: 14
                    color: viewerRoot.colors["text-primary"]
                }
            }
            ZoomButton {
                objectName: "mediaViewer-zoomInButton"
                implicitWidth: 36
                enabled: viewerRoot.zoom < viewerRoot.maxScale
                onClicked: viewerRoot.zoomByStep(viewerRoot.stepFactor)
                Accessible.name: qsTr("Zoom in")
                contentItem: Label {
                    horizontalAlignment: Label.AlignHCenter
                    verticalAlignment: Label.AlignVCenter
                    text: "+"
                    font.pixelSize: 20
                    color: viewerRoot.colors["text-primary"]
                }
            }
        }
    }

    // Document-level keyboard contract (media-viewer-backdrop.ui.tsx:27-51):
    // Escape always closes; arrows navigate only with more than one item.
    Shortcut {
        sequence: "Escape"
        enabled: viewerRoot.viewer.open && viewerRoot.activeFocus
        onActivated: viewerRoot.viewer.close()
    }
    Shortcut {
        sequence: "Left"
        enabled: viewerRoot.viewer.open && viewerRoot.activeFocus && viewerRoot.gallery
        onActivated: viewerRoot.viewer.prev()
    }
    Shortcut {
        sequence: "Right"
        enabled: viewerRoot.viewer.open && viewerRoot.activeFocus && viewerRoot.gallery
        onActivated: viewerRoot.viewer.next()
    }
    // Issue #919 zoom keyboard contract: "+"/"=" in, "-" out, "0" reset,
    // Shift+arrow pans 48 px while zoomed (reference KEYBOARD_PAN_STEP).
    // Bare "Left"/"Right" above keep paging the gallery; Qt shortcut matching
    // is exact on modifiers, so Shift+arrows never page and "+" never reaches
    // a Ctrl/Meta/Alt chord - the reference ignore-rules fall out by shape.
    // The sequences use symbol characters: QKeySequence::fromString does not
    // parse the "Equal"/"Plus"/"Minus" key-name words in this Qt build
    // (they yield an empty sequence that never matches), while the symbols
    // parse to Qt::Key_Equal/Key_Plus/Key_Minus reliably (probed).
    Shortcut {
        sequence: "+"
        enabled: viewerRoot.viewer.open && viewerRoot.activeFocus && viewerRoot.zoomableSlot
        onActivated: viewerRoot.zoomByStep(viewerRoot.stepFactor)
    }
    Shortcut {
        sequence: "="
        enabled: viewerRoot.viewer.open && viewerRoot.activeFocus && viewerRoot.zoomableSlot
        onActivated: viewerRoot.zoomByStep(viewerRoot.stepFactor)
    }
    Shortcut {
        sequence: "-"
        enabled: viewerRoot.viewer.open && viewerRoot.activeFocus && viewerRoot.zoomableSlot
        onActivated: viewerRoot.zoomByStep(1 / viewerRoot.stepFactor)
    }
    Shortcut {
        sequence: "0"
        enabled: viewerRoot.viewer.open && viewerRoot.activeFocus && viewerRoot.zoomableSlot
        onActivated: viewerRoot.resetZoom()
    }
    Shortcut {
        sequence: "Shift+Left"
        enabled: viewerRoot.viewer.open && viewerRoot.activeFocus && viewerRoot.zoomableSlot
                 && viewerRoot.zoom > 1
        onActivated: viewerRoot.panBy(-viewerRoot.keyboardPanStep, 0)
    }
    Shortcut {
        sequence: "Shift+Right"
        enabled: viewerRoot.viewer.open && viewerRoot.activeFocus && viewerRoot.zoomableSlot
                 && viewerRoot.zoom > 1
        onActivated: viewerRoot.panBy(viewerRoot.keyboardPanStep, 0)
    }
    Shortcut {
        sequence: "Shift+Up"
        enabled: viewerRoot.viewer.open && viewerRoot.activeFocus && viewerRoot.zoomableSlot
                 && viewerRoot.zoom > 1
        onActivated: viewerRoot.panBy(0, -viewerRoot.keyboardPanStep)
    }
    Shortcut {
        sequence: "Shift+Down"
        enabled: viewerRoot.viewer.open && viewerRoot.activeFocus && viewerRoot.zoomableSlot
                 && viewerRoot.zoom > 1
        onActivated: viewerRoot.panBy(0, viewerRoot.keyboardPanStep)
    }
}
