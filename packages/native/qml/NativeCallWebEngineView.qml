import QtQuick
import QtWebEngine
import QtWebChannel

// Embed host for the call window (issue #162, embed stack per #160): a
// WebEngine view pointed at the meeting URL with the one-time session config
// passed as the reference-equivalent hash overrides. This view owns the real
// embed lifecycle: its load result feeds the controller back (apiLoadSucceeded
// / apiLoadFailed), so the shell never waits on an event its own creation was
// supposed to produce. The permission policy is granted once per session and
// only to the meeting origin - shell minimize/expand never recreates this
// view (the shell Loader activates exactly once), so the embed never looks
// like a new session to Jitsi (jitsi-call-permissions.lib.ts:4).
//
// The view also carries the live external-API handshake (finding 5, round 4):
// the Qt WebEngine channel exposes qt.webChannelTransport in the page, and
// two main-world user scripts (the vendored Qt WebChannel JS API plus
// workspace-call-events.js) map the frame's external_api events onto the
// JitsiEmbedBridge report slots, so the participant count and roster arrive
// from real meeting-page events instead of the reconcile poll.
Item {
    id: host
    property var controller: null
    // C++ counterpart of the handshake (NativeController.embedBridge); set
    // by the shell's Loader together with `controller`.
    property var bridge: null
    // One-time capability grant, mirrors the reference iframe allow policy
    // "camera; microphone; fullscreen; display-capture".
    property bool allowPolicyApplied: false

    function grantPolicy(origin, feature) {
        allowPolicyApplied = true
        view.grantPermission(origin, feature)
    }

    // Same message pair the reference external_api postMessage handshake
    // performs. The QML WebChannel registry requires an attached
    // WebChannel.id (C++ objects have none), so the bridge is exposed
    // through a thin method-forwarding proxy whose calls land on the
    // bridge's report/listener invokables unchanged.
    QtObject {
        id: bridgeProxy
        WebChannel.id: "workspaceCallBridge"
        function reportParticipantCount(count: int) {
            if (host.bridge)
                host.bridge.reportParticipantCount(count)
        }
        function reportRoster(names: var) {
            if (host.bridge)
                host.bridge.reportRoster(names)
        }
        function handleParticipantsJoined() {
            if (host.bridge)
                host.bridge.handleParticipantsJoined()
        }
        function handleParticipantsLeft() {
            if (host.bridge)
                host.bridge.handleParticipantsLeft()
        }
    }

    WebChannel {
        id: callEventChannel
        registeredObjects: host.bridge ? [bridgeProxy] : []
    }

    WebEngineView {
        id: view
        anchors.fill: parent
        url: host.controller ? host.controller.embedUrl() : ""
        webChannel: host.bridge ? callEventChannel : null
        onFeaturePermissionRequested: (securityOrigin, feature) => {
            // Deny-by-default allowlist: camera/microphone/fullscreen/
            // display-capture are granted ONLY to the validated meeting
            // origin, never to an arbitrary requesting frame (finding 3).
            const trusted = host.controller
                && host.controller.isMeetingOrigin(securityOrigin)
            if (trusted && (feature === WebEngineProfile.AudioCapture
                || feature === WebEngineProfile.VideoCapture
                || feature === WebEngineProfile.FullScreenFeature
                || feature === WebEngineProfile.DisplayCapture))
                host.grantPolicy(securityOrigin, feature)
            else
                view.denyPermission(securityOrigin, feature)
        }
        // The real load events of this view ARE the embed lifecycle: success
        // moves the controller to Ready (creating the embed exactly once),
        // failure to LoadFailed where retry recreates the view (finding 1).
        onLoadingChanged: (loadRequest) => {
            if (!host.controller || loadRequest.status !== WebEngineView.LoadFinishedStatus)
                return
            if (loadRequest.error.code === 0)
                host.controller.apiLoadSucceeded()
            else
                host.controller.apiLoadFailed()
        }
        settings.javascriptEnabled: true
        Component.onCompleted: {
            // Main-world scripts so the glue can read the frame's window.APP
            // and the channel transport the view injects into world 0.
            view.userScripts.collection = [
                {
                    name: "workspace-call-qwebchannel",
                    sourceUrl: "qrc:/workspace/qml/qwebchannel.js",
                    injectionPoint: WebEngineScript.DocumentCreation,
                    worldId: WebEngineScript.MainWorld,
                    runsOnSubFrames: false
                },
                {
                    name: "workspace-call-events",
                    sourceUrl: "qrc:/workspace/qml/workspace-call-events.js",
                    injectionPoint: WebEngineScript.DocumentCreation,
                    worldId: WebEngineScript.MainWorld,
                    runsOnSubFrames: false
                }
            ]
        }
    }
}
