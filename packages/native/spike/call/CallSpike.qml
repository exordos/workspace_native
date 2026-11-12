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

    WebEngineView {
        id: callView
        anchors.fill: parent
        url: spikeStartUrl
        Component.onCompleted: forceActiveFocus()
        // Qt 6.10 permission API: native counterpart of Electron's
        // setPermissionRequestHandler (workspace_ui main.ts:978). Spike
        // policy is DENY BY DEFAULT: the view can load remote https content
        // (meet.jit.si), so a blanket grant would let any remote frame
        // capture devices. Grants are limited to (a) media capture from the
        // spike's own app://spike page or the Jitsi origins the spike loads,
        // and (b) display capture from app://spike only (the F1 gesture
        // probe in probe.html is its only caller). Production gates per
        // active-call frame/origin like isAllowedJitsiDisplayCaptureRequest
        // (main.ts:996).
        onPermissionRequested: (permission) => {
            const typeName = permission.permissionType
            const origin = permission.origin.toString().replace(/\/+$/, "")
            console.log("PERM_REQUEST type=" + typeName + " origin=" + origin)
            const fromProbe = (origin === "app://spike")
            const fromJitsi = (origin === "https://meet.jit.si")
            // Numeric values of QWebEnginePermission::PermissionType from
            // Qt 6.10.2 qwebenginepermission.h: 0 Unsupported,
            // 1 MediaAudioCapture, 2 MediaVideoCapture, 3 MediaAudioVideoCapture,
            // 4 DesktopVideoCapture, 5 DesktopAudioVideoCapture, 6 MouseLock,
            // 7 Notifications, 8 Geolocation, 9 ClipboardReadWrite, 10 LocalFontsAccess.
            // (Scoped QML enum lookup proved unreliable in the built app,
            // so compare the documented numbers.)
            const t = Number(typeName)
            const mediaCapture = (t >= 1 && t <= 3)   // audio/video/audio+video capture
            const desktopCapture = (t === 4 || t === 5) // display capture (F1 gesture probe only)
            if ((mediaCapture && (fromProbe || fromJitsi))
                    || (desktopCapture && fromProbe)) {
                permission.grant()
                console.log("PERM_GRANTED type=" + typeName + " origin=" + origin)
            } else {
                permission.deny()
                console.log("PERM_DENIED type=" + typeName + " origin=" + origin)
            }
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
}
