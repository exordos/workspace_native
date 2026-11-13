// Production embed glue for the call window (issue #162, review round 4).
//
// This script is injected into the meeting frame's main world at document
// creation (NativeCallWebEngineView.qml userScripts, alongside the vendored
// qwebchannel.js). The meeting page's own API service (window.APP.API - the
// exact event source external_api.js forwards in the reference client) is
// mapped onto the C++ JitsiEmbedBridge through the Qt WebEngine channel:
//
//   jitsi-call-modal-shell.hook.ts:172-210  ->  syncReads() below
//       getNumberOfParticipants()  -> bridge.reportParticipantCount()
//       getParticipantsInfo()      -> bridge.reportRoster() (take semantics)
//   jitsi-participant-count.hook.ts:19-33   ->  the two listener bindings
//       api.on("participantJoined") -> bridge.handleParticipantsJoined()
//       api.on("participantLeft")   -> bridge.handleParticipantsLeft()
//
// Like the reference, an event never carries the truth itself: it only
// triggers a re-read of the live reads, and the controller's focus-aware
// timer keeps reconciling against the same bridge values.
(function () {
    "use strict";
    if (window.__workspaceCallEventsGlue)
        return;
    window.__workspaceCallEventsGlue = true;

    function pollUntil(ready, then) {
        if (ready()) {
            then();
            return;
        }
        setTimeout(function () { pollUntil(ready, then); }, 50);
    }

    // The in-page API service appears with the meet bundle; external_api's
    // own iframe handshake waits the same way.
    function api() { return window.APP ? window.APP.API : null; }

    pollUntil(function () {
        return window.QWebChannel && window.qt && window.qt.webChannelTransport;
    }, function () {
        new QWebChannel(window.qt.webChannelTransport, function (channel) {
            var bridge = channel.objects.workspaceCallBridge;
            if (!bridge)
                return;

            function syncReads() {
                var a = api();
                if (!a)
                    return;
                try {
                    var count = a.getNumberOfParticipants
                        ? a.getNumberOfParticipants() : undefined;
                    if (typeof count === "number")
                        bridge.reportParticipantCount(count);
                } catch (e) {}
                try {
                    if (a.getParticipantsInfo) {
                        var info = a.getParticipantsInfo() || [];
                        var names = [];
                        for (var i = 0; i < info.length; ++i) {
                            names.push(info[i].displayName
                                || info[i].displayname || "Participant");
                        }
                        bridge.reportRoster(names);
                    }
                } catch (e) {}
            }

            pollUntil(api, function () {
                var a = api();
                a.addEventListener("videoConferenceJoined", syncReads);
                a.addEventListener("participantJoined", function () {
                    syncReads();
                    bridge.handleParticipantsJoined();
                });
                a.addEventListener("participantLeft", function () {
                    syncReads();
                    bridge.handleParticipantsLeft();
                });
                // Reference onApiReady performs the first reads immediately.
                syncReads();
            });
        });
    });
})();
