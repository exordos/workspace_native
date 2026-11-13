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
// workspace-call-events.js) speak the pinned Jitsi external-API wire
// contract of the frame and map its events onto the JitsiEmbedBridge report
// slots, so the participant count and roster arrive from real meeting-page
// events instead of the reconcile poll.
Item {
    id: host
    property var controller: null
    // C++ counterpart of the handshake (NativeController.embedBridge); set
    // by the shell's Loader together with `controller`.
    property var bridge: null
    // One-time capability grant, mirrors the reference iframe allow policy
    // "camera; microphone; fullscreen; display-capture".
    property bool allowPolicyApplied: false

    // #250: loadingChanged delivers QWebEngineLoadingInfo, whose LoadStatus
    // values are LoadStartedStatus 0 / LoadStoppedStatus 1 /
    // LoadSucceededStatus 2 / LoadFailedStatus 3 - verified against the
    // installed Qt 6.10.2 header
    // /usr/include/x86_64-linux-gnu/qt6/QtWebEngineCore/qwebengineloadinginfo.h:35-40.
    // The earlier comparison compared against a constant that does not
    // exist in this runtime: no real load event ever reached the controller.
    readonly property int loadStatusSucceeded: 2
    readonly property int loadStatusFailed: 3
    // #251: Qt 6.10 delivers capture gates as permissionRequested with a
    // QWebEnginePermission (origin + permissionType + grant()/deny()) -
    // verified against the installed Qt 6.10.2 header
    // /usr/include/x86_64-linux-gnu/qt6/QtWebEngineCore/qwebenginepermission.h:47-59:
    // Unsupported 0 / MediaAudioCapture 1 / MediaVideoCapture 2 /
    // MediaAudioVideoCapture 3 / DesktopVideoCapture 4. The legacy
    // WebEngineProfile feature enums and the grant/deny view methods used
    // before do not exist in this runtime.
    readonly property int permissionMediaAudioCapture: 1
    readonly property int permissionMediaVideoCapture: 2
    readonly property int permissionMediaAudioVideoCapture: 3

    function grantPolicy(permission) {
        allowPolicyApplied = true
        permission.grant()
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
        // #251 (with the #237 contract preserved): deny-by-default allowlist
        // - microphone/camera capture (alone or combined) is granted ONLY to
        // the validated meeting origin, never to an arbitrary requesting
        // frame; every other request (including display capture, whose
        // source picker is #87's scope, and notifications) is denied through
        // the real Qt permission object. Fullscreen arrives separately on
        // fullScreenRequested, not as a capture permission.
        onPermissionRequested: (permission) => {
            const trusted = host.controller
                && host.controller.isMeetingOrigin(permission.origin)
            const type = permission.permissionType
            if (trusted && (type === host.permissionMediaAudioCapture
                || type === host.permissionMediaVideoCapture
                || type === host.permissionMediaAudioVideoCapture))
                host.grantPolicy(permission)
            else
                permission.deny()
        }
        onFullScreenRequested: (request) => {
            // The reference iframe allow policy grants fullscreen to the
            // meeting document; Qt routes it through its own request object.
            if (host.controller && host.controller.isMeetingOrigin(request.origin))
                request.accept()
            else
                request.reject()
        }
        // The real load events of this view ARE the embed lifecycle: success
        // moves the controller to Ready (creating the embed exactly once),
        // failure to LoadFailed where the shell offers retry (finding 1,
        // fixed for the real Qt status constants in #250; the controller
        // ignores both callbacks unless the state is Loading, so late
        // in-session navigations cannot downgrade a live call).
        onLoadingChanged: (loadRequest) => {
            if (!host.controller)
                return
            if (loadRequest.status === host.loadStatusSucceeded)
                host.controller.apiLoadSucceeded()
            else if (loadRequest.status === host.loadStatusFailed)
                host.controller.apiLoadFailed()
        }
        // #263: the shell keeps this single embed surface alive across the
        // LoadFailed retry state (one active embed loader in every phase of
        // expand/PiP), so retry cannot rely on Loader re-instantiation.
        // When the controller moves LoadFailed -> Loading, the live view
        // re-requests the meeting URL itself and its own load events drive
        // the state on, exactly as a recreated view would. The previous
        // state is captured in the handler (not bound), so the edge survives
        // the same signal that re-evaluates bindings.
        property int priorEmbedState: 0
        Connections {
            target: host.controller
            function onEmbedStateChanged() {
                if (!host.controller)
                    return
                const now = host.controller.embedState
                if (view.priorEmbedState === 3 && now === 1) {
                    const target = host.controller.embedUrl()
                    if (target.length > 0 && target === view.url.toString())
                        view.reload()
                    else
                        view.load(target)
                }
                view.priorEmbedState = now
            }
        }
        settings.javascriptEnabled: true
        Component.onCompleted: {
            // Main-world scripts so the glue sees exactly the frame's own
            // window (where the external-API postMessage transport lives)
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
