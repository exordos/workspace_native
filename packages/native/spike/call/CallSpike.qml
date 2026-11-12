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
        // Qt 6.10 permission API: native counterpart of Electron's
        // setPermissionRequestHandler (workspace_ui main.ts:978). Spike
        // policy: grant the reference allow-policy set (camera; microphone;
        // fullscreen; display-capture). Production gates per active-call
        // frame/origin like isAllowedJitsiDisplayCaptureRequest (main.ts:996).
        onPermissionRequested: (permission) => {
            console.log("PERM_REQUEST type=" + permission.permissionType
                        + " origin=" + permission.origin)
            permission.grant()
            console.log("PERM_GRANTED type=" + permission.permissionType)
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
