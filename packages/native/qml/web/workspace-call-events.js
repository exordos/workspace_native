// Production embed glue for the call window (issue #162; rewritten for #252).
//
// This script is injected into the meeting frame's main world at document
// creation (NativeCallWebEngineView.qml userScripts, alongside the vendored
// qwebchannel.js). The previous version subscribed on the meeting page's
// internal API service object and called addEventListener /
// getNumberOfParticipants / getParticipantsInfo on it. None of those methods
// exist there (jitsi-meet modules/API/API.js at c5bf9948a5d), so init threw
// and no count or roster ever arrived (#252).
//
// The embed decision (docs/NATIVE_DESKTOP_CALL_STACK.md) is the Jitsi IFrame
// External API contract. The meeting page delivers that contract over the
// transport of @jitsi/js-utils 6.3.2 (modules/transport + postis), which
// external_api.js instantiates on the parent side: JSON-string postMessage
// envelopes {postis:true, scope:"jitsi_meet_external_api_<id>",
// method:"__ready__"|"message", params}, where a "message" param is the
// Transport envelope {type:"event", data:{name, ...fields}} and the scope id
// comes from the frame URL hash (#jitsi_meet_external_api_id=<id>). The
// frame sends to window.parent; in this embed the meeting document IS the
// top-level document, so window.parent is this same window and this glue
// plays the parent-side peer of exactly that wire contract - the same
// participant bookkeeping external_api.js performs (_setupListeners switch:
// video-conference-joined / participant-joined / participant-left /
// display-name-change / email-change), then maps it onto the C++
// JitsiEmbedBridge through the Qt WebChannel handshake kept from #237:
//
//   external_api.getNumberOfParticipants()  -> bridge.reportParticipantCount
//   external_api.getParticipantsInfo()      -> bridge.reportRoster (take)
//   on("participantJoined") -> bridge.handleParticipantsJoined()
//   on("participantLeft")   -> bridge.handleParticipantsLeft()
(function () {
    "use strict";
    if (window.__workspaceCallEventsGlue)
        return;
    window.__workspaceCallEventsGlue = true;

    // --- parent side of the pinned external-API transport (postis wire) ---
    var scope = null;
    var idMatch = /jitsi_meet_external_api_id=(\d+)/.exec(window.location.hash || "");
    if (idMatch)
        scope = "jitsi_meet_external_api_" + idMatch[1];
    var myId = Date.now() + "-" + Math.random();
    var wireReady = false;
    var wireBuffer = [];
    function wire(method, params) {
        try {
            window.postMessage(JSON.stringify(
                { postis: true, scope: scope, method: method, params: params }), "*");
        } catch (e) {}
    }
    function wireSend(params) {
        if (wireReady) wire("message", params);
        else if (wireBuffer.length < 500) wireBuffer.push(params);
    }
    var readyTimer = window.setInterval(function () { wire("__ready__", myId); }, 50);
    window.addEventListener("message", function (event) {
        if (typeof event.data !== "string")
            return;
        var data;
        try { data = JSON.parse(event.data); } catch (e) { return; }
        if (!data || data.postis !== true || (data.scope || null) !== scope)
            return;
        if (data.method === "__ready__") {
            if (data.params === myId) {
                window.clearInterval(readyTimer);
                wireReady = true;
                for (var i = 0; i < wireBuffer.length; ++i) wire("message", wireBuffer[i]);
                wireBuffer = [];
            } else {
                wire("__ready__", data.params);
            }
            return;
        }
        if (data.method === "message" && data.params && data.params.type === "event")
            onEvent(data.params.data || {});
    });

    // --- the participant state external_api.js mirrors from these events ---
    var participants = {};
    var count = 0;
    var bridge = null; // set once the WebChannel handshake completes
    function report(joined, left) {
        if (!bridge)
            return; // wire events arriving earlier only update state below
        bridge.reportParticipantCount(getNumberOfParticipants());
        var info = getParticipantsInfo();
        var names = [];
        for (var i = 0; i < info.length; ++i) {
            names.push(info[i].displayName
                || info[i].formattedDisplayName || "Participant");
        }
        bridge.reportRoster(names);
        if (joined)
            bridge.handleParticipantsJoined();
        else if (left)
            bridge.handleParticipantsLeft();
    }
    function onEvent(data) {
        var name = data.name;
        if (name === "video-conference-joined") {
            // Same switch as external_api.js _setupListeners: the local
            // participant is created by the join event, then every remote
            // participant by its own participant-joined event.
            delete participants.local;
            participants[data.id] = {
                participantId: data.id,
                displayName: data.displayName || "",
                formattedDisplayName: data.formattedDisplayName || ""
            };
            ++count;
            report(false, false);
        } else if (name === "participant-joined") {
            var joined = participants[data.id] = participants[data.id] || { participantId: data.id };
            joined.displayName = data.displayName;
            joined.formattedDisplayName = data.formattedDisplayName;
            ++count;
            report(true, false);
        } else if (name === "participant-left") {
            --count;
            delete participants[data.id];
            report(false, true);
        } else if (name === "display-name-change") {
            var renamed = participants[data.id];
            if (renamed) renamed.displayName = data.displayname;
            report(false, false);
        } else if (name === "email-change") {
            var emailed = participants[data.id];
            if (emailed) emailed.email = data.email;
        } else if (name === "video-conference-left") {
            participants = {};
            count = 0;
            report(false, false);
        }
    }
    function getNumberOfParticipants() {
        return count;
    }
    function getParticipantsInfo() {
        var info = [];
        for (var id in participants) {
            var p = participants[id];
            p.participantId = id;
            info.push(p);
        }
        return info;
    }

    function pollUntil(ready, then) {
        if (ready()) {
            then();
            return;
        }
        setTimeout(function () { pollUntil(ready, then); }, 50);
    }

    // The Qt WebEngine channel handshake from #237, unchanged: wait for the
    // injected transport and open the WebChannel. Whatever the wire already
    // delivered while the channel was opening is replayed by the report
    // below, so a fast conference join cannot be lost.
    pollUntil(function () {
        return window.QWebChannel && window.qt && window.qt.webChannelTransport;
    }, function () {
        new QWebChannel(window.qt.webChannelTransport, function (channel) {
            bridge = channel.objects.workspaceCallBridge;
            if (!bridge)
                return;
            // Reference onApiReady performs the first reads immediately.
            report(false, false);
        });
    });
})();
