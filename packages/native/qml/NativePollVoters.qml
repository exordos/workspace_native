pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: dialog
    required property var hostCard
    readonly property var controller: hostCard.controller
    readonly property var colors: hostCard.colors
    readonly property var questions: !hostCard.poll || hostCard.readOnly || hostCard.poll.isAnonymous ? []
        : hostCard.questions.filter(q => hostCard.closed || hostCard.resultsVisibility === "always"
            || hostCard.selectedFor(q).length > 0)
    // This session key includes the API generation even when the same account signs in again.
    readonly property string owner: controller ? controller.ownStatusContextKey : ""
    readonly property string identity: JSON.stringify([owner, hostCard.messageUuid])
    readonly property var pollRevision: hostCard.poll && hostCard.poll.state ? hostCard.poll.state.revision : 0
    readonly property string revision: JSON.stringify([identity, pollRevision,
        questions.map(q => [q.uuid, hostCard.stateFor(q)])])
    property string questionId: ""
    property string optionId: ""
    property bool overview: false
    property string pageRequest: ""
    property string currentPageVersion: ""
    property var items: []
    property string marker: ""
    property bool error: false
    readonly property var choices: {
        const rows = []
        for (const q of questions)
            for (const o of q.options)
                rows.push({id:o.uuid, displayLabel:o.text, questionId:q.uuid, question:q.text})
        return rows
    }
    readonly property var question: questions.find(q => q.uuid === questionId)
    readonly property var option: question ? question.options.find(o => o.uuid === optionId) : null
    readonly property int votes: option ? votesFor(question, option) : 0
    readonly property string targetVersion: JSON.stringify([identity, (hostCard.poll && hostCard.poll.state ? hostCard.poll.state.revision : 0),
        questionId, optionId, votes, option ? seedFor(question, option) : []])
    property bool initialized: false

    function votesFor(q, o) {
        return Number((hostCard.stateFor(q).optionCounts || {})[o.uuid] || 0)
    }
    function seedFor(q, o) {
        return (hostCard.stateFor(q).optionVoterPreviews || {})[o.uuid] || []
    }
    function preview(q, o) {
        if (!questions.some(item => item.uuid === q.uuid) || votesFor(q, o) <= 0) return []
        const seed = seedFor(q, o)
        return (seed.length ? seed : hostCard.previewStore.get(hostCard.messageUuid, revision,
            q.uuid + ":" + o.uuid)).slice(0, 5)
    }
    function startPreviews() {
        if (!initialized) return
        const jobs = []
        for (const q of questions) {
            for (const o of q.options) {
                if (jobs.length < 5 && votesFor(q, o) > 0 && !seedFor(q, o).length)
                    jobs.push({questionId:q.uuid, optionId:o.uuid})
            }
        }
        hostCard.previewStore.ensure(hostCard.messageUuid, revision, jobs, pollRevision)
    }
    function cancelPage() {
        const tag = pageRequest
        pageRequest = ""
        if (tag.length && controller) controller.cancelPollVoterRequest(tag)
    }
    function resetPage() {
        if (currentPageVersion === targetVersion) return
        currentPageVersion = targetVersion
        cancelPage()
        error = false
        items = option && votes ? seedFor(question, option).slice() : []
        marker = items.length && items.length < votes ? items[items.length - 1] : ""
        if (visible && !option) close()
        else if (visible && votes && !items.length) loadPage()
    }
    function loadPage() {
        if (!option || pageRequest.length) return
        error = false
        pageRequest = hostCard.newClientUuid()
        controller.loadPollVoters(hostCard.messageUuid, questionId, optionId, marker, 50, pageRequest)
    }
    function showFor(q, o, all) {
        if (!questions.some(entry => entry.uuid === q.uuid)) return
        overview = all
        questionId = q.uuid
        optionId = o.uuid
        if (!visible) open()
        else resetPage()
    }
    function showOverview() {
        const first = questions[0]
        if (!first || !first.options.length) return
        showFor(first, first.options.find(o => hostCard.selectedFor(first).includes(o.uuid)) || first.options[0], true)
    }

    Timer { id: previewUpdate; interval: 0; onTriggered: dialog.startPreviews() }
    Timer { id: pageUpdate; interval: 0; onTriggered: dialog.resetPage() }
    onRevisionChanged: if (initialized) previewUpdate.restart()
    onIdentityChanged: if (initialized && visible) close()
    onTargetVersionChanged: if (initialized && visible) pageUpdate.restart()
    Component.onCompleted: { initialized = true; previewUpdate.start() }
    Component.onDestruction: cancelPage()
    Connections {
        target: dialog.controller
        function onPollVotersLoaded(message, questionId, optionId, voters, next, failure, tag) {
            if (!tag || message !== dialog.hostCard.messageUuid) return
            if (tag === dialog.pageRequest && dialog.questionId === questionId && dialog.optionId === optionId) {
                dialog.pageRequest = ""
                dialog.error = Boolean(failure)
                if (!failure) {
                    dialog.items = Array.from(new Set(dialog.items.concat(Array.from(voters))))
                    dialog.marker = next
                }
            }
        }
    }
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(480, parent ? parent.width - 32 : 480)
    height: Math.min(parent ? parent.height - 48 : 600, contents.implicitHeight + topPadding + bottomPadding)
    padding: 24
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onOpened: resetPage()
    onClosed: { cancelPage(); currentPageVersion = ""; questionId = ""; optionId = ""; items = [] }
    background: Rectangle { radius: 12; color: dialog.colors["bg-elevated"]; border.color: dialog.colors["border-subtle"] }
    contentItem: ColumnLayout {
        id: contents
        spacing: 12
        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: qsTr("Voters"); textFormat: Text.PlainText
                font.pixelSize: 18; font.weight: Font.DemiBold; color: dialog.colors["text-primary"]
            }
            ToolButton {
                text: "×"; Accessible.name: qsTr("Close")
                onClicked: dialog.close()
            }
        }
        Text {
            Layout.fillWidth: true; visible: dialog.hostCard.questions.length > 1
            text: dialog.question ? String(dialog.question.text) : ""; textFormat: Text.PlainText
            wrapMode: Text.Wrap; font.pixelSize: 14; color: dialog.colors["text-secondary"]
        }
        NativeSelect {
            id: answers
            objectName: "poll-voters-answer"
            Layout.fillWidth: true; visible: dialog.overview; colors: dialog.colors
            placeholderText: qsTr("Answer"); model: dialog.choices
            currentIndex: dialog.choices.findIndex(o => o.questionId === dialog.questionId && o.id === dialog.optionId)
            delegate: ItemDelegate {
                id: choiceRow
                required property int index
                required property var modelData
                readonly property bool firstInGroup: index === 0 || dialog.choices[index - 1].questionId !== modelData.questionId
                width: answers.popup.availableWidth
                height: 36 + (firstInGroup ? 22 : 0)
                text: modelData.displayLabel
                contentItem: Column {
                    spacing: 4
                    Text {
                        visible: choiceRow.firstInGroup
                        text: choiceRow.modelData.question; textFormat: Text.PlainText
                        color: dialog.colors["text-secondary"]; font.pixelSize: 12
                    }
                    Text {
                        text: choiceRow.text; textFormat: Text.PlainText
                        color: dialog.colors["text-primary"]; font.pixelSize: 14
                    }
                }
                background: Rectangle {
                    radius: 8; color: choiceRow.hovered || answers.highlightedIndex === choiceRow.index
                        ? dialog.colors["card-bg-active"] : "transparent"
                }
            }
            onActivated: index => {
                const choice = dialog.choices[index]
                if (choice) {
                    dialog.questionId = choice.questionId
                    dialog.optionId = choice.id
                }
            }
        }
        Text {
            Layout.fillWidth: true; visible: !dialog.overview
            text: dialog.option ? String(dialog.option.text) : ""; textFormat: Text.PlainText
            wrapMode: Text.Wrap; font.pixelSize: 14; color: dialog.colors["text-primary"]
        }
        Text {
            Layout.fillWidth: true
            text: dialog.hostCard.participantCount(dialog.votes); textFormat: Text.PlainText
            font.pixelSize: 12; color: dialog.colors["text-secondary"]
        }
        ListView {
            id: list
            objectName: "poll-voters-list"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, Math.max(80, (dialog.parent ? dialog.parent.height : 600) - 350))
            model: dialog.items; clip: true; spacing: 8
            delegate: RowLayout {
                required property string modelData
                objectName: "poll-voter-row-" + modelData
                width: list.width; spacing: 12
                NativePollVoterAvatar {
                    id: avatar; userUuid: parent.modelData
                    Layout.preferredWidth: 32; Layout.preferredHeight: 32
                    controller: dialog.controller; colors: dialog.colors
                }
                Text {
                    Layout.fillWidth: true; text: avatar.displayName; textFormat: Text.PlainText
                    wrapMode: Text.Wrap; font.pixelSize: 14; color: dialog.colors["text-primary"]
                }
            }
            ScrollBar.vertical: ScrollBar {}
        }
        Text {
            objectName: dialog.error ? "poll-voters-error" : "poll-voters-status"
            Layout.fillWidth: true
            visible: Boolean(dialog.pageRequest) || dialog.error || !dialog.items.length
            text: dialog.pageRequest ? qsTr("Loading…") : dialog.error ? qsTr("Could not load voters. Please try again.") : qsTr("No votes yet")
            textFormat: Text.PlainText; wrapMode: Text.Wrap; font.pixelSize: 14
            Accessible.role: dialog.error ? Accessible.AlertMessage : Accessible.StatusBar
            color: dialog.colors[dialog.error ? "danger" : "text-secondary"]
        }
        Button {
            objectName: "poll-voters-more"
            visible: dialog.error || dialog.marker.length > 0
            enabled: !dialog.pageRequest
            text: dialog.error ? qsTr("Retry") : qsTr("Load more")
            onClicked: dialog.loadPage()
        }
    }
}
