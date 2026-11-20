pragma ComponentBehavior: Bound
import QtQuick

// The message list can replace a card while its voters GET is in flight.
// Share that request and its result across delegates of the same poll revision.
Item {
    id: store
    required property var controller
    readonly property string sessionKey: controller.ownStatusContextKey
    property var polls: ({})
    property var requests: ({})
    property int change: 0
    property int serial: 0
    visible: false

    function clear() {
        const tags = Object.keys(requests)
        requests = {}
        polls = {}
        change++
        tags.forEach(tag => controller.cancelPollVoterRequest(tag))
    }
    function retire(id) {
        const row = polls[id]
        if (!row) return
        delete polls[id]
        for (const tag of Object.keys(requests)) {
            if (requests[tag].id === id) {
                delete requests[tag]
                controller.cancelPollVoterRequest(tag)
            }
        }
    }
    function ensure(id, revision, jobs) {
        if (!sessionKey || !id) return
        if (polls[id] && polls[id].revision === revision) return
        retire(id)
        // Retire requests and cached identities when a poll leaves the recent set.
        if (Object.keys(polls).length >= 50) retire(Object.keys(polls)[0])
        polls[id] = {revision:revision, queue:jobs, inFlight:0, options:{}}
        change++
        pump(id)
    }
    function pump(id) {
        const row = polls[id]
        if (!row) return
        while (row.queue.length && row.inFlight < 3) {
            const job = row.queue.shift()
            const tag = "poll-preview-" + String(++serial)
            requests[tag] = {id:id, key:job.questionId + ":" + job.optionId}
            row.inFlight++
            controller.loadPollVoters(id, job.questionId, job.optionId, "", 5, tag)
        }
    }
    function get(id, revision, key) {
        change
        const row = polls[id]
        return row && row.revision === revision ? row.options[key] || [] : []
    }
    onSessionKeyChanged: clear()
    Component.onDestruction: clear()
    Connections {
        target: store.controller
        function onPollVotersLoaded(message, q, o, voters, marker, error, tag) {
            const job = store.requests[tag]
            if (!job) return
            delete store.requests[tag]
            const row = store.polls[job.id]
            if (!row) return
            row.inFlight--
            if (!error) { row.options[job.key] = voters; store.change++ }
            store.pump(job.id)
        }
    }
}
