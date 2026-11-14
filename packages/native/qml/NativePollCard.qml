pragma ComponentBehavior: Bound
import QtQuick

// Poll card rendered inside a message bubble (issue #216,
// docs/NATIVE_DESKTOP_POLLS_BUBBLE.md). Geometry and click semantics are
// measured against workspace_ui 230e02b8b0273dee556a406746d122180a68b3e7:
// workspace-message-poll.css (336 px column, 6 px gaps, 28 px rows, 2 px
// result bars), workspace-message-poll.ui.tsx (subtitle/summary/status rules)
// and workspace-message-poll-question.ui.tsx (percent formula, selectability,
// correct marks, inline add option). Voting intents are forwarded to the
// controller; in-flight locks release on their own request completion
// (pollActionSucceeded / pollActionFailed), and a poll_state revision change
// only retires stale error text.
Item {
    id: card
    objectName: "poll-card-" + messageUuid

    required property var poll
    required property string messageUuid
    property var colors: ({})
    property var controller: null
    property bool readOnly: false
    // Right inset reserved on the card's final line for the bubble time
    // (workspace-message-poll.css:152-156 reserves 48 px).
    property real timeInset: 48
    property string timeText: ""

    signal voteRequested(string questionUuid, string optionUuid, bool selected)
    signal addOptionRequested(string questionUuid, string optionUuid, string optionText)
    // The embed host wires voteRequested straight to the controller; the card
    // tracks the in-flight state itself (the reference locks the question
    // before the request leaves, question.ui:190-195).
    onVoteRequested: questionUuid => card.markPending(questionUuid, "vote")

    // Transient local state, keyed by question uuid. In-flight locks release
    // on their own request completion (pollActionSucceeded / pollActionFailed
    // below); a poll_state revision change only retires stale error text.
    // Drafts and add attempts live on the card, keyed by question, so an
    // unrelated revision or a delegate re-incubation never loses them.
    // A pending entry records the pending request's action ("vote" or
    // "add_poll_option") as well as the question, so a failure can release
    // exactly the lock its own request set (exo-review 4571961640).
    property var pendingQuestions: ({})
    property var questionErrors: ({})
    property var addErrors: ({})
    property var addAttempts: ({})
    property var addDrafts: ({})
    property int seenRevision: 0
    property bool deadlinePassed: false

    readonly property var questions: poll && poll.questions ? poll.questions : []
    readonly property bool closed: Boolean(poll && poll.state ? poll.state.isClosed : false) || deadlinePassed
    readonly property bool pollMultiple: Boolean(poll && poll.multipleAnswers)
    readonly property bool allowRevote: Boolean(poll && poll.allowRevote)
    readonly property bool allowAddingOptions: Boolean(poll && poll.allowAddingOptions)
    readonly property string resultsVisibility: String(poll ? poll.resultsVisibility : "")
    readonly property string closesAt: String(poll ? poll.closesAt : "")
    readonly property string closedAt: String(poll ? poll.closedAt : "")
    readonly property int pollVoterCount: Number(poll && poll.state ? poll.state.voterCount : 0)
    readonly property bool quiz: questions.some(question =>
        selectedFor(question).length > 0 && (question.correctOptionUuids || []).length > 0)
    readonly property bool summaryVisible: resultsVisibility === "always" || closed
        || (questions.length > 0 && questions.every(question => selectedFor(question).length > 0))
    readonly property string subtitleText: {
        if (closed)
            return qsTr("Poll closed")
        const parts = [poll && poll.isAnonymous ? qsTr("Anonymous poll") : qsTr("Public poll"),
            pollMultiple ? qsTr("Choose multiple options") : qsTr("Choose one option")]
        if (questions.length > 1)
            parts.push(qsTr("Questions: %1").arg(questions.length))
        return parts.join(" \u00b7 ")
    }
    readonly property var stateDate: closed ? (closedAt.length ? closedAt : closesAt) : closesAt
    readonly property string statusDateText: {
        if (!stateDate.length || !controller)
            return ""
        const label = controller.formatPollDate(stateDate)
        if (!label.length)
            return ""
        return closed ? qsTr("Closed %1").arg(label) : qsTr("Until %1").arg(label)
    }
    // Reference parity, repo idiom for plurals without an English catalog:
    // one qsTr per source form (same as the channel-info member counts).
    readonly property string summaryText: summaryVisible
        ? ((pollVoterCount === 1 ? qsTr("%n participant", "", pollVoterCount)
            : qsTr("%n participants", "", pollVoterCount))
        + (quiz ? " \u00b7 " + qsTr("You voted") : ""))
        : qsTr("Results after voting")

    function selectedFor(question) {
        const votes = poll && poll.state ? poll.state.myVotes || [] : []
        for (let i = 0; i < votes.length; ++i)
            if (votes[i].questionUuid === question.uuid)
                return votes[i].optionUuids || []
        return []
    }
    function stateFor(question) {
        const rows = poll && poll.state ? poll.state.questions || [] : []
        for (let i = 0; i < rows.length; ++i)
            if (rows[i].questionUuid === question.uuid)
                return rows[i]
        return { voterCount: 0, optionCounts: {} }
    }
    function canAct() {
        return !readOnly && !closed
    }
    // Reference canSelectPollOption (workspace-message-poll-question.ui.tsx:32-40).
    function canSelect(question, optionUuid) {
        const selected = selectedFor(question)
        if (allowRevote || selected.length === 0)
            return true
        return pollMultiple && !selected.includes(optionUuid)
    }
    function armDeadline() {
        const raw = closesAt
        const at = raw.length ? Date.parse(raw) : NaN
        if (!Number.isFinite(at)) {
            card.deadlinePassed = false
            deadlineTimer.stop()
            return
        }
        const delta = at - Date.now()
        card.deadlinePassed = delta <= 0
        if (delta > 0) {
            // Reference caps the timer at the setTimeout limit (hook:14).
            deadlineTimer.interval = Math.min(delta, 2147483647)
            deadlineTimer.restart()
        } else {
            deadlineTimer.stop()
        }
    }
    function markPending(questionUuid, action) {
        const next = Object.assign({}, card.pendingQuestions)
        next[questionUuid] = action
        card.pendingQuestions = next
        const errors = Object.assign({}, card.questionErrors)
        delete errors[questionUuid]
        card.questionErrors = errors
    }
    // Qt.createUuid is not available in shipped QML scopes (the same reason
    // the controller generates ids), so build a v4-shaped uuid here and keep
    // it with the attempt: retrying the same draft text replays the
    // documented idempotent uuid+text add instead of creating a duplicate
    // option (exo-review 4571525932).
    function newClientUuid() {
        return "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx".replace(/[xy]/g, marker => {
            const r = Math.floor(Math.random() * 16)
            return (marker === "x" ? r : ((r & 0x3) | 0x8)).toString(16)
        })
    }
    function setAddDraft(questionUuid, text) {
        if (String(card.addDrafts[questionUuid] || "") === String(text))
            return
        card.addDrafts = Object.assign({}, card.addDrafts, {[questionUuid]: String(text)})
    }

    // Success is observed through the poll_state revision change (the response
    // merged by the controller); failures arrive via pollActionFailed for this
    // message. Both completions identify their own question: the vote and add
    // bodies carry question_uuid, so a lock belongs to exactly one request
    // (a stale add attempt on another question can never release a vote's
    // lock, exo-review 4571961640).
    Connections {
        target: card.controller
        enabled: card.controller !== null
        function onPollActionFailed(messageUuid, action, detail, questionUuid) {
            if (messageUuid !== card.messageUuid)
                return
            if (action !== "vote" && action !== "add_poll_option")
                return
            // Release only the failed request's own lock: the pending entry
            // records which action set it, so an add failure cannot clear a
            // vote lock on the same or another question (the stale add
            // attempt map is a retry cache, not a pending marker).
            if (card.pendingQuestions[questionUuid] !== action)
                return
            const pending = Object.assign({}, card.pendingQuestions)
            delete pending[questionUuid]
            card.pendingQuestions = pending
            if (action === "vote") {
                const errors = Object.assign({}, card.questionErrors)
                errors[questionUuid] = qsTr("Could not save the change. Please try again.")
                card.questionErrors = errors
            } else {
                const errors = Object.assign({}, card.addErrors)
                errors[questionUuid] = qsTr("Could not add the option. Try again.")
                card.addErrors = errors
            }
        }
        // Own-request completion (exo-review 4571525925): this request's own
        // response was merged into the poll_state, so exactly its lock - and
        // for adds, its attempt and draft - retires. An unrelated revision
        // can never permit a duplicate concurrent vote.
        function onPollActionSucceeded(messageUuid, action, questionUuid) {
            if (messageUuid !== card.messageUuid || !questionUuid.length)
                return
            if (action !== "vote" && action !== "add_poll_option")
                return
            const pending = Object.assign({}, card.pendingQuestions)
            delete pending[questionUuid]
            card.pendingQuestions = pending
            if (action === "add_poll_option") {
                const attempts = Object.assign({}, card.addAttempts)
                delete attempts[questionUuid]
                card.addAttempts = attempts
                const errors = Object.assign({}, card.addErrors)
                delete errors[questionUuid]
                card.addErrors = errors
                // The reference clears the input only after its own add was
                // accepted (connected ui:143-150, question.ui:77-80).
                card.setAddDraft(questionUuid, "")
            }
        }
    }

    onPollChanged: {
        const revision = Number(poll && poll.state ? poll.state.revision : 0)
        if (revision !== card.seenRevision) {
            card.seenRevision = revision
            // Only stale error text retires on a revision: in-flight locks
            // belong to their own request and release via the completion
            // signals above, so an unrelated poll revision (another
            // participant's vote, another question's response) can never
            // permit a duplicate concurrent vote (exo-review 4571525925).
            card.questionErrors = ({})
            card.addErrors = ({})
        }
        card.armDeadline()
    }
    Component.onCompleted: {
        card.seenRevision = Number(poll && poll.state ? poll.state.revision : 0)
        card.armDeadline()
    }

    Timer {
        id: deadlineTimer
        repeat: false
        onTriggered: card.armDeadline()
    }

    Accessible.role: Accessible.Grouping
    Accessible.name: card.poll && card.poll.isAnonymous ? qsTr("Anonymous poll") : qsTr("Public poll")

    implicitWidth: cardColumn.implicitWidth
    implicitHeight: cardColumn.height

    Column {
        id: cardColumn
        // Reference parity is 336 px inside the 360 px bubble, but Main.qml
        // caps the bubble at 88 % of the available message width, so narrow
        // windows hand the card less than 336 px: the column follows the
        // available card width instead of overflowing the bubble
        // (exo-review 4571525936).
        width: card.width > 0 ? Math.min(336, card.width) : 336
        spacing: 6

        Repeater {
            model: card.questions
            delegate: Column {
                id: questionBlock
                required property var modelData
                required property int index
                readonly property var question: modelData
                readonly property var counts: card.stateFor(question)
                readonly property var selected: card.selectedFor(question)
                readonly property bool answered: selected.length > 0
                readonly property bool results: card.resultsVisibility === "always" || card.closed || answered
                readonly property bool busy: Boolean(card.pendingQuestions[question.uuid])
                readonly property bool canAct: card.canAct()
                readonly property string errorText: String(card.questionErrors[question.uuid] || "")
                readonly property var correctUuids: question.correctOptionUuids || []
                readonly property bool anyCorrect: correctUuids.length > 0
                width: cardColumn.width
                spacing: 6
                Accessible.name: questionBlock.busy
                    ? qsTr("Saving answer…") : String(question.text)

                // The reference separates questions with 12 px margins around a
                // 1 px subtle border (question.ui:268). Column positioners
                // count margins, so 6 px margin + 6 px spacing = 12 px.
                Rectangle {
                    visible: questionBlock.index > 0
                    width: parent.width
                    height: 1
                    color: card.colors["border-subtle"]
                    anchors.topMargin: 6
                    anchors.bottomMargin: 6
                }

                Text {
                    objectName: "poll-question-title-" + questionBlock.index + "-" + card.messageUuid
                    width: parent.width
                    text: String(question.text)
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.pixelSize: 14
                    font.weight: Font.Medium
                    lineHeightMode: Text.FixedHeight
                    lineHeight: 17
                    color: card.colors["text-primary"]
                }
                Text {
                    visible: questionBlock.index === 0
                    objectName: "poll-subtitle-" + card.messageUuid
                    width: parent.width
                    text: card.subtitleText
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.pixelSize: 12
                    lineHeightMode: Text.FixedHeight
                    lineHeight: 15
                    color: card.colors["text-secondary"]
                }

                Column {
                    width: parent.width
                    spacing: 6

                    Repeater {
                        model: question.options || []
                        delegate: Item {
                            id: optionRow
                            required property var modelData
                            required property int index
                            readonly property var option: modelData
                            property bool selected: questionBlock.selected.includes(option.uuid)
                            property bool correct: questionBlock.answered
                                && questionBlock.correctUuids.includes(option.uuid)
                            property bool wrong: selected && questionBlock.correctUuids.length > 0 && !correct
                            readonly property int votes: Number(questionBlock.counts.optionCounts
                                ? questionBlock.counts.optionCounts[option.uuid] || 0 : 0)
                            readonly property int percent: Number(questionBlock.counts.voterCount) > 0
                                ? Math.round(votes / Number(questionBlock.counts.voterCount) * 100) : 0
                            readonly property bool selectable: card.canSelect(questionBlock.question, option.uuid)
                            // During an options-model churn the delegate incubates
                            // before it is parented; guard the size bindings.
                            width: parent ? parent.width : 0
                            height: optionRowInner.height + 8
                            // Reference parity: disabled when !canAct || busy || !canSelect
                            // (workspace-message-poll-question.ui.tsx:190-195).
                            enabled: !readOnly && !card.closed && !questionBlock.busy && selectable && card.canAct()
                            Accessible.role: Accessible.Button
                            Accessible.name: String(option.text)
                            Accessible.checkable: true
                            Accessible.checked: selected
                            // Keyboard parity with the reference button element
                            // (exo-review 4571525935): reachable via Tab,
                            // activated by Return/Space just like a click.
                            focusPolicy: Qt.TabFocus | Qt.ClickFocus
                            activeFocusOnTab: true
                            function activate() {
                                card.voteRequested(questionBlock.question.uuid,
                                    optionRow.option.uuid, !optionRow.selected)
                            }
                            Keys.onPressed: event => {
                                if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                                    || event.key === Qt.Key_Enter) {
                                    event.accepted = true
                                    optionRow.activate()
                                }
                            }

                            Item {
                                id: optionRowInner
                                width: parent.width
                                height: Math.max(28, optionText.height)

                                Row {
                                    id: optionChoice
                                    width: parent.width
                                    spacing: 8

                                    Rectangle {
                                        id: optionMarker
                                        width: 16
                                        height: 16
                                        anchors.verticalCenter: optionText.verticalCenter
                                        radius: 8
                                        color: optionRow.selected || optionRow.correct
                                            ? (optionRow.correct ? card.colors["indicator-green"] : card.colors["accent"])
                                            : "transparent"
                                        border.width: optionRow.selected || optionRow.correct ? 0 : 1
                                        border.color: optionRow.correct ? card.colors["indicator-green"] : card.colors["accent"]
                                        opacity: optionRow.enabled || questionBlock.busy ? 1 : 0.4
                                        Text {
                                            anchors.centerIn: parent
                                            visible: optionRow.selected || optionRow.correct
                                            text: "\u2713"
                                            textFormat: Text.PlainText
                                            font.pixelSize: 11
                                            color: "white"
                                        }
                                    }
                                    Text {
                                        id: optionText
                                        objectName: "poll-option-text-" + questionBlock.index + "-" + optionRow.index + "-" + card.messageUuid
                                        width: Math.max(24, parent.width - optionMarker.width - 8
                                            - (optionAggregate.visible ? optionAggregate.implicitWidth + 8 : 0))
                                        text: String(option.text)
                                        textFormat: Text.PlainText
                                        wrapMode: Text.Wrap
                                        font.pixelSize: 14
                                        lineHeightMode: Text.FixedHeight
                                        lineHeight: 17
                                        color: optionRow.correct ? card.colors["indicator-green"]
                                            : (optionRow.wrong ? card.colors["danger"]
                                                : (optionRow.selected ? card.colors["accent"]
                                                    : card.colors["text-primary"]))
                                    }
                                    Text {
                                        id: optionAggregate
                                        objectName: "poll-aggregate-" + questionBlock.index + "-" + optionRow.index + "-" + card.messageUuid
                                        visible: questionBlock.results
                                        text: String(optionRow.percent) + "% \u00b7 " + String(optionRow.votes)
                                        textFormat: Text.PlainText
                                        font.pixelSize: 14
                                        color: card.colors["text-secondary"]
                                    }
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: optionRow.activate()
                                }
                            }
                            objectName: "poll-option-" + questionBlock.index + "-" + optionRow.index + "-" + card.messageUuid

                            // Result bar: always present (2 px after 6 px), the
                            // fill carries the percent width (css:106-110).
                            Item {
                                anchors.top: optionRowInner.bottom
                                width: parent.width
                                height: 8
                                Rectangle {
                                    id: optionBar
                                    objectName: "poll-bar-" + questionBlock.index + "-" + optionRow.index + "-" + card.messageUuid
                                    property int percent: optionRow.percent
                                    y: 6
                                    width: questionBlock && questionBlock.results
                                        ? parent.width * Math.min(100, Math.max(0, optionRow.percent)) / 100 : 0
                                    height: 2
                                    radius: 1
                                    color: optionRow.correct || (optionRow.selected && !optionRow.wrong)
                                        ? card.colors["accent"] : card.colors["text-secondary"]
                                }
                            }
                        }
                    }
                }

                Item {
                    visible: questionBlock.anyCorrect
                    width: parent.width
                    height: 15
                    Text {
                        visible: questionBlock.answered
                        objectName: "poll-correct-" + questionBlock.index + "-" + card.messageUuid
                        text: "\u2713 " + qsTr("Correct answer: %1").arg(
                            (question.options || []).filter(option =>
                                questionBlock.correctUuids.includes(option.uuid))
                                .map(option => String(option.text)).join(", "))
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        font.pixelSize: 12
                        lineHeightMode: Text.FixedHeight
                        lineHeight: 15
                        color: card.colors["indicator-green"]
                    }
                }

                Text {
                    visible: questionBlock.errorText.length > 0
                    objectName: "poll-question-error-" + questionBlock.index + "-" + card.messageUuid
                    width: parent.width - card.timeInset
                    text: questionBlock.errorText
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.pixelSize: 12
                    color: card.colors["danger"]
                    Accessible.role: Accessible.AlertMessage
                }

                Text {
                    visible: card.questions.length > 1 && questionBlock.results
                    objectName: "poll-question-status-" + questionBlock.index + "-" + card.messageUuid
                    width: parent.width
                    text: qsTr("Participants: %1").arg(Number(questionBlock.counts.voterCount))
                    textFormat: Text.PlainText
                    font.pixelSize: 12
                    color: card.colors["text-secondary"]
                }

                Column {
                    id: addColumn
                    visible: card.allowAddingOptions && card.canAct()
                        && (question.options || []).length < 20
                    width: parent.width
                    spacing: 4
                    // Draft and attempt live on the card keyed by question
                    // (exo-review 4571525928): an unrelated poll revision or
                    // a delegate re-incubation keeps the unsubmitted text;
                    // only this draft's own accepted add clears it (see
                    // onPollActionSucceeded).
                    readonly property string addText: String(card.addDrafts[question.uuid] || "")
                    property var attempt: card.addAttempts[question.uuid] || null
                    property bool addPending: Boolean(card.pendingQuestions[question.uuid])
                    property string addError: String(card.addErrors[question.uuid] || "")

                    Row {
                        spacing: 8
                        Rectangle {
                            width: 16
                            height: 16
                            anchors.verticalCenter: parent.verticalCenter
                            radius: 8
                            border.width: 1
                            border.color: addInput.activeFocus ? card.colors["accent"] : card.colors["text-secondary"]
                            color: "transparent"
                        }
                        Item {
                            width: addInput.width
                            height: addInput.height
                            Text {
                                anchors.fill: parent
                                visible: addColumn.addText.length === 0 && !addInput.textEdited
                                text: qsTr("Add an answer option\u2026")
                                textFormat: Text.PlainText
                                verticalAlignment: Text.AlignVCenter
                                font.pixelSize: 14
                                color: card.colors["text-secondary"]
                            }
                            TextInput {
                                id: addInput
                                objectName: "poll-add-option-" + questionBlock.index + "-" + card.messageUuid
                                property bool textEdited: false
                                width: cardColumn.width - 16 - 8
                                height: 20
                                text: addColumn.addText
                                onTextChanged: {
                                    // A dying re-incubated delegate must not
                                    // touch the card's draft map.
                                    if (question)
                                        card.setAddDraft(question.uuid, text)
                                    textEdited = text.length > 0
                                }
                                maximumLength: 200
                                enabled: !addColumn.addPending
                                clip: true
                                font.pixelSize: 14
                                color: card.colors["text-primary"]
                                selectionColor: card.colors["accent"]
                                Accessible.name: qsTr("Add an answer option\u2026")
                                Accessible.description: qsTr("Press Enter to add an answer. Shift+Enter inserts a new line.")
                                function submit(event) {
                                    if (event.modifiers !== Qt.NoModifier || addInput.inputMethodComposing)
                                        return
                                    event.accepted = true
                                    const text = addColumn.addText.trim()
                                    if (!text || addColumn.addPending)
                                        return
                                    // One generated uuid per attempt text (exo-review
                                    // 4571525932): a retry of the same text replays
                                    // the documented idempotent uuid+text add.
                                    let attempt = addColumn.attempt
                                    if (!attempt || attempt.text !== text
                                        || !attempt.uuid || attempt.uuid.length === 0) {
                                        attempt = { uuid: card.newClientUuid(), text: text }
                                        card.addAttempts = Object.assign({}, card.addAttempts,
                                            {[question.uuid]: attempt})
                                    }
                                    const pending = Object.assign({}, card.pendingQuestions)
                                    pending[question.uuid] = "add_poll_option"
                                    card.pendingQuestions = pending
                                    const errors = Object.assign({}, card.addErrors)
                                    delete errors[question.uuid]
                                    card.addErrors = errors
                                    card.addOptionRequested(question.uuid, attempt.uuid, attempt.text)
                                }
                                Keys.onReturnPressed: event => submit(event)
                                Keys.onEnterPressed: event => submit(event)
                            }
                        }
                    }
                    Text {
                        visible: addColumn.addError.length > 0
                        objectName: "poll-add-error-" + questionBlock.index + "-" + card.messageUuid
                        text: addColumn.addError
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        font.pixelSize: 12
                        color: card.colors["danger"]
                        Accessible.role: Accessible.AlertMessage
                    }
                }
            }
        }

        // The summary and status rows follow the questions in the column
        // flow: they occupy their own lines and count into the card height
        // instead of rendering at the card origin over the poll top
        // (exo-review 4571525921).
        Item {
            id: summaryRow
            objectName: "poll-summary-row-" + card.messageUuid
            width: cardColumn.width - ((card.statusDateText.length > 0 || card.readOnly) ? 0 : card.timeInset)
            height: Math.max(15, summaryTextItem.height)
            Accessible.role: Accessible.Grouping
            Accessible.name: qsTr("Vote counts")
            Text {
                id: summaryTextItem
                objectName: "poll-summary-" + card.messageUuid
                width: parent.width
                text: card.summaryText
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                font.pixelSize: 12
                lineHeightMode: Text.FixedHeight
                lineHeight: 15
                color: card.colors["text-secondary"]
            }
        }

        Item {
            id: statusRow
            objectName: "poll-status-row-" + card.messageUuid
            visible: card.statusDateText.length > 0 || card.readOnly
            width: cardColumn.width - card.timeInset
            height: Math.max(16, statusDateText.height)
            Accessible.role: Accessible.Grouping
            Accessible.name: qsTr("Poll status")
            Row {
                spacing: 8
                Text {
                    id: statusDateText
                    objectName: "poll-status-date-" + card.messageUuid
                    visible: card.statusDateText.length > 0
                    text: card.statusDateText
                    textFormat: Text.PlainText
                    font.pixelSize: 12
                    color: card.colors["text-secondary"]
                }
                Text {
                    objectName: "poll-status-readonly-" + card.messageUuid
                    visible: card.readOnly
                    text: qsTr("Read only")
                    textFormat: Text.PlainText
                    font.pixelSize: 12
                    color: card.colors["text-secondary"]
                }
            }
        }
    }
}
