// Throwaway spike window for #160 (docs/NATIVE_DESKTOP_CALL_STACK.md).
import QtQuick
import QtQuick.Window
import QtWebEngine

Window {
    id: spikeWindow
    width: 1000
    height: 700
    visible: true
    title: "Workspace native call spike (throwaway, #160)"

    // Capture requests waiting for explicit user approval: one entry per
    // pending QWebEnginePermission (a page may ask audio and video as two
    // separate requests; they are confirmed one at a time, in order).
    property var pendingPerms: []

    function deviceName(t) {
        if (t === 1) return "microphone"
        if (t === 2) return "camera"
        if (t === 3) return "microphone and camera"
        if (t === 5) return "screen and audio"
        return "screen" // 4 desktop video capture
    }

    function showNextConsentRequest() {
        if (spikeWindow.pendingPerms.length === 0) {
            consentOverlay.visible = false
            callView.forceActiveFocus()
            return
        }
        const p = spikeWindow.pendingPerms[0]
        consentLabel.text = "Allow " + p.device + " access for " + p.origin + "?"
        consentOverlay.visible = true
        denyButton.forceActiveFocus()   // Deny is the default choice
        console.log("PERM_DIALOG_SHOWN type=" + p.type + " origin=" + p.origin)
        // Log the Allow button center in window coordinates so automated
        // runs can click it via xdotool without guessing the layout.
        const xy = allowButton.mapToItem(spikeWindow.contentItem,
            allowButton.width / 2, allowButton.height / 2)
        console.log("PERM_DIALOG_ALLOW_XY x=" + Math.round(xy.x)
                    + " y=" + Math.round(xy.y))
    }

    function resolveConsent(allow) {
        if (spikeWindow.pendingPerms.length === 0)
            return
        const p = spikeWindow.pendingPerms.shift()
        if (allow) {
            p.permission.grant()
            console.log("PERM_USER_GRANT type=" + p.type + " origin=" + p.origin)
        } else {
            p.permission.deny()
            console.log("PERM_USER_DENY type=" + p.type + " origin=" + p.origin)
        }
        spikeWindow.showNextConsentRequest()
    }

    WebEngineView {
        id: callView
        anchors.fill: parent
        url: spikeStartUrl
        Component.onCompleted: forceActiveFocus()
        // Qt 6.10 permission API: native counterpart of Electron's
        // setPermissionRequestHandler (workspace_ui main.ts:978). Spike
        // policy is DENY BY DEFAULT + EXPLICIT USER CONSENT: the view can
        // load remote https content (meet.jit.si), so a blanket grant would
        // let any remote frame capture devices. Non-capture requests and
        // requests from origins outside the spike allowlist (the probe page
        // app://spike, the Jitsi origin the spike loads) are denied
        // immediately, no dialog. Capture requests from allowlisted origins
        // are NEVER granted automatically: they surface the confirmation
        // overlay below and wait for an explicit Allow click (Deny focused
        // by default). The only way to skip the dialog is the documented
        // per-run env opt-in WORKSPACE_NATIVE_SPIKE_AUTOGRANT=1
        // (spike/call/main.cpp warns once at startup when active; intended
        // for headless probe automation only). Production gates per
        // active-call frame/origin like isAllowedJitsiDisplayCaptureRequest
        // (main.ts:996).
        onPermissionRequested: (permission) => {
            const typeName = permission.permissionType
            const origin = permission.origin.toString().replace(/\/+$/, "")
            console.log("PERM_REQUEST type=" + typeName + " origin=" + origin)
            const fromProbe = (origin === "app://spike")
            const fromJitsi = (origin === "https://meet.jit.si")
            // Numeric values of QWebEnginePermission::PermissionType from
            // the installed Qt 6.10.2 qwebenginepermission.h: 0 Unsupported,
            // 1 MediaAudioCapture, 2 MediaVideoCapture, 3 MediaAudioVideoCapture,
            // 4 DesktopVideoCapture, 5 DesktopAudioVideoCapture, 6 MouseLock,
            // 7 Notifications, 8 Geolocation, 9 ClipboardReadWrite, 10 LocalFontsAccess.
            // (Scoped QML enum lookup proved unreliable in the built app,
            // so compare the documented numbers. Display capture is 4/5 on
            // this Qt; 6 is MouseLock.)
            const t = Number(typeName)
            const mediaCapture = (t >= 1 && t <= 3)   // audio/video/audio+video capture
            const desktopCapture = (t === 4 || t === 5) // display capture (F1 gesture probe only)
            const allowlistedCapture = (mediaCapture && (fromProbe || fromJitsi))
                    || (desktopCapture && fromProbe)
            if (!allowlistedCapture) {
                permission.deny()
                console.log("PERM_DENIED type=" + typeName + " origin=" + origin)
                return
            }
            if (spikeAutoGrant) {
                permission.grant()
                console.log("PERM_AUTOGRANT type=" + typeName + " origin=" + origin)
                return
            }
            // Wait for the user: keep the request pending until the overlay
            // resolves it. QWebEnginePermission stays valid while referenced.
            spikeWindow.pendingPerms.push({ permission: permission,
                type: typeName, origin: origin, device: spikeWindow.deviceName(t) })
            if (!consentOverlay.visible)
                spikeWindow.showNextConsentRequest()
        }
        onJavaScriptConsoleMessage: (level, message, lineNumber, sourceID) =>
            console.log("PAGE> " + message)
        onLoadingChanged: loadRequest => {
            if (loadRequest.status === WebEngineView.LoadFailedStatus)
                console.log("LOAD_FAILED " + loadRequest.url + " " + loadRequest.errorString)
            if (loadRequest.status === WebEngineView.LoadSucceededStatus)
                console.log("LOAD_OK " + loadRequest.url)
        }
    }

    // Explicit-consent overlay for capture requests from allowlisted
    // origins. Plain QtQuick items only (no extra QML module dependency in
    // the spike). Deny has the default focus; Tab moves to Allow; Return/
    // Enter activates the focused button; Escape denies.
    Rectangle {
        id: consentOverlay
        anchors.fill: parent
        visible: false
        color: "#B3000000"
        z: 100

        Rectangle {
            id: consentPanel
            anchors.centerIn: parent
            width: 540
            height: consentColumn.height + 44
            radius: 8
            color: "#1e1e1e"
            border.color: "#666666"
            border.width: 1

            Column {
                id: consentColumn
                anchors.centerIn: parent
                spacing: 20

                Text {
                    id: consentLabel
                    width: 480
                    anchors.horizontalCenter: parent.horizontalCenter
                    horizontalAlignment: Text.AlignHCenter
                    color: "#f0f0f0"
                    font.pixelSize: 16
                    wrapMode: Text.WordWrap
                }

                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 16

                    Rectangle {
                        id: allowButton
                        width: 140
                        height: 38
                        radius: 5
                        color: activeFocus ? "#2e7d32" : "#1b5e20"
                        border.color: activeFocus ? "#a5d6a7" : "#4caf50"
                        border.width: 1
                        Text {
                            anchors.centerIn: parent
                            text: "Allow"
                            color: "white"
                            font.pixelSize: 15
                        }
                        Keys.onReturnPressed: spikeWindow.resolveConsent(true)
                        Keys.onEnterPressed: spikeWindow.resolveConsent(true)
                        Keys.onEscapePressed: spikeWindow.resolveConsent(false)
                        Keys.onTabPressed: denyButton.forceActiveFocus()
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onPressed: allowButton.forceActiveFocus()
                            onClicked: spikeWindow.resolveConsent(true)
                        }
                    }

                    Rectangle {
                        id: denyButton
                        width: 140
                        height: 38
                        radius: 5
                        color: activeFocus ? "#b71c1c" : "#7f0000"
                        border.color: activeFocus ? "#ef9a9a" : "#e57373"
                        border.width: 1
                        Text {
                            anchors.centerIn: parent
                            text: "Deny"
                            color: "white"
                            font.pixelSize: 15
                        }
                        Keys.onReturnPressed: spikeWindow.resolveConsent(false)
                        Keys.onEnterPressed: spikeWindow.resolveConsent(false)
                        Keys.onEscapePressed: spikeWindow.resolveConsent(false)
                        Keys.onTabPressed: allowButton.forceActiveFocus()
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onPressed: denyButton.forceActiveFocus()
                            onClicked: spikeWindow.resolveConsent(false)
                        }
                    }
                }
            }
        }
    }
}
