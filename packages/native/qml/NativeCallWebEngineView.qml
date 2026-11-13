import QtQuick
import QtWebEngine

// Embed host for the call window (issue #162, embed stack per #160): a
// WebEngine view pointed at the meeting URL with the one-time session config
// passed as the reference-equivalent hash overrides. This view owns the real
// embed lifecycle: its load result feeds the controller back (apiLoadSucceeded
// / apiLoadFailed), so the shell never waits on an event its own creation was
// supposed to produce. The permission policy is granted once per session and
// only to the meeting origin - shell minimize/expand never recreates this
// view (the shell Loader activates exactly once), so the embed never looks
// like a new session to Jitsi (jitsi-call-permissions.lib.ts:4).
Item {
    id: host
    property var controller: null
    // One-time capability grant, mirrors the reference iframe allow policy
    // "camera; microphone; fullscreen; display-capture".
    property bool allowPolicyApplied: false

    function grantPolicy(origin, feature) {
        allowPolicyApplied = true
        view.grantPermission(origin, feature)
    }

    WebEngineView {
        id: view
        anchors.fill: parent
        url: host.controller ? host.controller.embedUrl() : ""
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
    }
}
