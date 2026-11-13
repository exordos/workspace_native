pragma ComponentBehavior: Bound
import QtQuick
import QtQml
import QtQuick.Controls
import QtQuick.Layouts

// Issue #215: create-poll dialog cloned from the Reference
// features/workspace-message-poll/create-poll-dialog.ui.tsx at sha
// 230e02b8b0273dee556a406746d122180a68b3e7 (docs/NATIVE_DESKTOP_POLL_CREATION.md).
Popup {
    id: dialog
    objectName: "pollCreateDialog"
    required property var colors
    required property var controller
    property int sectionCount: 1
    property bool settingsOpen: false
    property bool pending: false
    property bool sendFailed: false
    property bool validationFailed: false
    property bool creationUnavailable: false
    property bool multipleAnswers: false
    property bool anonymous: false
    property bool allowRevote: false
    property bool allowAddingOptions: false
    property int resultsIndex: 0
    property bool deadlineEnabled: false
    property string deadlineDate: ""
    property string deadlineTime: ""
    // Validity fan-in: every mutation that can change Create's enabled state
    // bumps this counter so formValid re-evaluates deterministically.
    property int revision: 0
    readonly property bool formLocked: pending || sendFailed
    readonly property bool deadlineFuture: {
        revision
        const stamp = deadlineDate.length > 0 && deadlineTime.length > 0
            ? new Date(deadlineDate + "T" + deadlineTime) : null
        return stamp !== null && !isNaN(stamp.getTime()) && stamp.getTime() > Date.now()
    }
    readonly property string deadlineIso: deadlineEnabled && deadlineFuture
        ? new Date(deadlineDate + "T" + deadlineTime).toISOString() : ""
    function sectionsInOrder() {
        return questionsContainer.children.filter(function (child) {
            return child.objectName === "pollQuestionSection"
        })
    }
    readonly property bool formValid: {
        revision
        if (sectionCount <= 0) return false
        const sections = sectionsInOrder()
        if (sections.length !== sectionCount) return false
        for (const section of sections) {
            if (!section.sectionReady) return false
        }
        return !(deadlineEnabled && !deadlineFuture)
    }
    readonly property string alertText: sendFailed
        ? qsTr("Could not confirm the poll was sent. Retry to safely send the same poll.")
        : validationFailed
            ? qsTr("The poll was rejected. Check its questions and settings, then try again.")
            : creationUnavailable ? qsTr("Poll creation is not enabled on this server.") : ""
    function openForCreate() {
        resetAll()
        open()
    }
    function resetAll() {
        sectionCount = 1
        settingsOpen = false
        pending = false
        sendFailed = false
        validationFailed = false
        creationUnavailable = false
        multipleAnswers = false
        anonymous = false
        allowRevote = false
        allowAddingOptions = false
        resultsIndex = 0
        deadlineEnabled = false
        deadlineDate = ""
        deadlineTime = ""
        revision++
    }
    function addQuestion() {
        sectionCount++
        revision++
    }
    function removeSection(index) {
        // Reference removes one question row; the survivors shift up.
        const sections = sectionsInOrder()
        for (let walk = index; walk + 1 < sectionCount; ++walk) {
            const from = sections[walk + 1]
            const to = sections[walk]
            if (from && to) to.restore(from.snapshot())
        }
        sectionCount--
        revision++
    }
    function moveSection(index, offset) {
        const target = index + offset
        if (target < 0 || target >= sectionCount) return
        const sections = sectionsInOrder()
        const left = sections[index]
        const right = sections[target]
        if (!left || !right) return
        const carry = left.snapshot()
        left.restore(right.snapshot())
        right.restore(carry)
        revision++
    }
    function submit() {
        if (pending || creationUnavailable || !formValid) return
        validationFailed = false
        sendFailed = false
        const rows = []
        const sections = sectionsInOrder()
        if (sections.length !== sectionCount) return
        for (const section of sections) {
            rows.push({text: section.questionText.trim(), options: section.answerRows()})
        }
        pending = true
        revision++
        controller.sendPoll(rows, multipleAnswers, anonymous, allowRevote,
            resultsIndex === 1 ? "always" : "after_vote", allowAddingOptions,
            deadlineEnabled && deadlineFuture ? deadlineIso : "")
    }
    function reopenAfterSend() {
        pending = false
        close()
    }
    Connections {
        target: dialog.controller
        function onPollActionFailed(messageUuid, action, detail) {
            if (!dialog.visible || action !== "create") return
            dialog.pending = false
            if (detail === "http-409")
                dialog.creationUnavailable = true
            else if (detail === "http-400" || detail === "http-422")
                dialog.validationFailed = true
            else
                dialog.sendFailed = true
            dialog.revision++
        }
        function onMessageSent(id, stream, topic) {
            if (dialog.visible && dialog.pending) dialog.reopenAfterSend()
        }
    }
    width: Math.min(600, parent && parent.width > 0 ? parent.width - 48 : 600)
    readonly property int formBudget: parent && parent.height > 0 ? parent.height - 48 : 760
    modal: true
    Overlay.modal: Rectangle { color: "#80000000" }
    // A pending create is an uncertain send: dismissing it would strand the
    // attempt, so close gestures are disabled while the request is in flight.
    closePolicy: dialog.pending
        ? Popup.NoAutoClose
        : Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onClosed: dialog.resetAll()
    background: Rectangle {
        radius: 10
        color: dialog.colors["bg-elevated"]
        border.color: dialog.colors["border-subtle"]
    }
    padding: 24
    topPadding: 24
    bottomPadding: 0
    contentItem: ColumnLayout {
        spacing: 0
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 26
            spacing: 12
            Text {
                objectName: "pollFormTitle"
                Layout.fillWidth: true
                text: qsTr("Create poll")
                textFormat: Text.PlainText
                color: dialog.colors["text-primary"]
                font.pixelSize: 16
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
        }
        Item { Layout.preferredHeight: 4 }
        Text {
            objectName: "pollFormAlert"
            Layout.fillWidth: true
            visible: dialog.alertText.length > 0
            text: dialog.alertText
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: dialog.colors["danger"]
            font.pixelSize: 13
        }
        Item { Layout.preferredHeight: 8 }
        Flickable {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(200,
                Math.min(formBody.implicitHeight, dialog.formBudget - 170))
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            contentHeight: formBody.implicitHeight
            ColumnLayout {
                id: formBody
                width: parent.width
                spacing: 12
                enabled: !dialog.formLocked
                Text {
                    text: qsTr("The default server limit is 20 questions and 20 answers per question.")
                    textFormat: Text.PlainText
                    color: dialog.colors["text-secondary"]
                    font.pixelSize: 12
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }
                ColumnLayout {
                    id: questionsContainer
                    objectName: "pollQuestionsContainer"
                    Layout.fillWidth: true
                    spacing: 12
                }
                Instantiator {
                    id: questionRepeater
                    objectName: "pollQuestionRepeater"
                    model: dialog.sectionCount
                    delegate: PollQuestionSection {
                        required property int index
                        objectName: "pollQuestionSection"
                        Layout.fillWidth: true
                        sectionIndex: index
                        sectionTotal: questionRepeater.count
                        dialogRef: dialog
                        Component.onCompleted: parent = questionsContainer
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 16
                    NativeButton {
                        objectName: "pollAddQuestionButton"
                        colors: dialog.colors
                        text: "\uFF0B " + qsTr("Add question")
                        onClicked: dialog.addQuestion()
                    }
                    Item { Layout.fillWidth: true }
                    NativeButton {
                        objectName: "pollSettingsButton"
                        colors: dialog.colors
                        text: qsTr("Poll settings") + (dialog.settingsOpen ? " \u2303" : " \u203A")
                        onClicked: dialog.settingsOpen = !dialog.settingsOpen
                    }
                }
                ColumnLayout {
                    visible: dialog.settingsOpen
                    spacing: 12
                    Layout.fillWidth: true
                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: dialog.colors["border-subtle"]
                    }
                    PollToggle {
                        colors: dialog.colors
                        label: qsTr("Multiple answers")
                        hint: qsTr("Participants can select more than one answer for each question")
                        checked: dialog.multipleAnswers
                        trackName: "pollMultipleToggle"
                        knobName: "pollMultipleKnob"
                        onToggled: dialog.multipleAnswers = !dialog.multipleAnswers
                    }
                    PollToggle {
                        colors: dialog.colors
                        label: qsTr("Anonymous poll")
                        hint: qsTr("Hide voters")
                        checked: dialog.anonymous
                        objectName: "pollAnonymousToggle"
                        onToggled: dialog.anonymous = !dialog.anonymous
                    }
                    PollToggle {
                        colors: dialog.colors
                        label: qsTr("Allow revoting")
                        hint: qsTr("Votes can be changed or withdrawn")
                        checked: dialog.allowRevote
                        objectName: "pollRevoteToggle"
                        onToggled: dialog.allowRevote = !dialog.allowRevote
                    }
                    PollToggle {
                        colors: dialog.colors
                        label: qsTr("Allow adding options")
                        hint: qsTr("Participants can add answer options")
                        checked: dialog.allowAddingOptions
                        objectName: "pollAddOptionsToggle"
                        onToggled: dialog.allowAddingOptions = !dialog.allowAddingOptions
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12
                        Text {
                            text: qsTr("Results")
                            textFormat: Text.PlainText
                            color: dialog.colors["text-primary"]
                            font.pixelSize: 14
                            font.weight: Font.DemiBold
                        }
                        Item { Layout.fillWidth: true }
                        ComboBox {
                            objectName: "pollResultsCombo"
                            Layout.preferredWidth: 220
                            model: [qsTr("After voting"), qsTr("Always")]
                            currentIndex: dialog.resultsIndex
                            onActivated: dialog.resultsIndex = currentIndex
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Text {
                            text: qsTr("Poll closing date")
                            textFormat: Text.PlainText
                            color: dialog.colors["text-primary"]
                            font.pixelSize: 14
                            font.weight: Font.DemiBold
                        }
                        RowLayout {
                            spacing: 8
                            TextField {
                                objectName: "pollDeadlineDateField"
                                visible: dialog.deadlineEnabled
                                Layout.preferredWidth: 130
                                placeholderText: "yyyy-MM-dd"
                                text: dialog.deadlineDate
                                onTextChanged: { dialog.deadlineDate = text; dialog.revision++ }
                            }
                            TextField {
                                objectName: "pollDeadlineTimeField"
                                visible: dialog.deadlineEnabled
                                Layout.preferredWidth: 90
                                placeholderText: "HH:mm"
                                text: dialog.deadlineTime
                                onTextChanged: { dialog.deadlineTime = text; dialog.revision++ }
                            }
                        }
                        Text {
                            objectName: "pollDeadlineInvalidLabel"
                            visible: dialog.deadlineEnabled && !dialog.deadlineFuture
                            text: qsTr("Choose a future date")
                            textFormat: Text.PlainText
                            color: dialog.colors["danger"]
                            font.pixelSize: 12
                        }
                        NativeButton {
                            objectName: "pollDeadlineAction"
                            colors: dialog.colors
                            text: dialog.deadlineEnabled ? qsTr("Remove deadline") : qsTr("Set a deadline")
                            onClicked: {
                                dialog.deadlineEnabled = !dialog.deadlineEnabled
                                if (!dialog.deadlineEnabled) {
                                    dialog.deadlineDate = ""
                                    dialog.deadlineTime = ""
                                }
                                dialog.revision++
                            }
                        }
                    }
                }
            }
        }
        Item { Layout.preferredHeight: 16 }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 72
            color: dialog.colors["bg-elevated"]
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: 1
                color: dialog.colors["border-subtle"]
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 0
                anchors.rightMargin: 0
                spacing: 8
                Item { Layout.fillWidth: true }
                NativeButton {
                    objectName: "pollCancelButton"
                    colors: dialog.colors
                    text: qsTr("Cancel")
                    onClicked: dialog.close()
                }
                NativeButton {
                    objectName: "pollCreateButton"
                    colors: dialog.colors
                    text: qsTr("Create")
                    primary: true
                    enabled: dialog.formValid && !dialog.pending && !dialog.creationUnavailable
                    onClicked: dialog.submit()
                }
            }
        }
    }

    // Reference PollToggle (poll-form-controls.ui.tsx): 30x18 rounded track
    // (border-subtle / accent) with a 14px white knob inset 2px, shifted 12px
    // when checked; clicking the visible label toggles the switch.
    component PollToggle: RowLayout {
        id: toggle
        required property var colors
        required property string label
        required property string hint
        property bool checked: false
        property string trackName: ""
        property string knobName: ""
        signal toggled()
        spacing: 16
        Layout.fillWidth: true
        Item {
            Layout.fillWidth: true
            implicitHeight: toggleLabel.implicitHeight
            ColumnLayout {
                id: toggleLabel
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: 2
                Text {
                    text: toggle.label
                    textFormat: Text.PlainText
                    color: toggle.colors["text-primary"]
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                }
                Text {
                    text: toggle.hint
                    textFormat: Text.PlainText
                    color: toggle.colors["text-secondary"]
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: toggle.toggled()
            }
        }
        Item {
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: 4
            Layout.preferredWidth: 30
            Layout.preferredHeight: 18
            Rectangle {
                objectName: toggle.trackName
                anchors.fill: parent
                radius: 9
                color: toggle.checked ? toggle.colors["accent"] : toggle.colors["border-subtle"]
            }
            Rectangle {
                objectName: toggle.knobName
                width: 14
                height: 14
                radius: 7
                color: "#ffffff"
                x: 2 + (toggle.checked ? 12 : 0)
                y: 2
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: toggle.toggled()
            }
        }
    }

    // Reference QuestionEditor/OptionEditor rules: question max 1000, answer
    // max 200, a trailing empty answer row is auto-appended whenever every row
    // is filled, and move/remove affordances appear for non-empty rows only.
    component PollQuestionSection: ColumnLayout {
        id: section
        required property int sectionIndex
        required property int sectionTotal
        required property var dialogRef
        property string questionText: ""
        property int optionsVersion: 0
        readonly property int filledOptions: {
            optionsVersion
            let count = 0
            for (let walk = 0; walk < optionModel.count; ++walk)
                if (optionModel.get(walk).text.trim().length > 0) count++
            return count
        }
        readonly property bool sectionReady: questionText.trim().length > 0 && filledOptions >= 2
        spacing: 8
        function rowsArray() {
            const rows = []
            for (let walk = 0; walk < optionModel.count; ++walk)
                rows.push({text: optionModel.get(walk).text, correct: optionModel.get(walk).correct})
            return rows
        }
        function answerRows() {
            const rows = []
            for (let walk = 0; walk < optionModel.count; ++walk) {
                const row = optionModel.get(walk)
                if (row.text.trim().length > 0)
                    rows.push({text: row.text.trim(), correct: row.correct})
            }
            return rows
        }
        function rebuild(rows) {
            optionModel.clear()
            for (const row of rows) optionModel.append(row)
            section.optionsVersion++
            dialogRef.revision++
        }
        function ensureTrailingEmpty() {
            let allFilled = optionModel.count > 0
            for (let walk = 0; walk < optionModel.count; ++walk)
                if (optionModel.get(walk).text.trim().length === 0) allFilled = false
            if (allFilled) {
                optionModel.append({text: "", correct: false})
                section.optionsVersion++
            }
            dialogRef.revision++
        }
        function moveRow(index, target) {
            if (target < 0 || target >= optionModel.count) return
            const rows = section.rowsArray()
            const carry = rows[index]
            rows.splice(index, 1)
            rows.splice(target, 0, carry)
            section.rebuild(rows)
        }
        function removeRow(index) {
            const rows = section.rowsArray()
            rows.splice(index, 1)
            let allFilled = rows.length > 0
            for (const row of rows)
                if (row.text.trim().length === 0) allFilled = false
            if (allFilled) rows.push({text: "", correct: false})
            section.rebuild(rows)
        }
        function snapshot() {
            return {text: section.questionText, rows: section.rowsArray()}
        }
        function restore(state) {
            section.questionText = state.text
            questionField.text = state.text
            section.rebuild(state.rows)
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Text {
                text: qsTr("Question") + " " + (sectionIndex + 1)
                textFormat: Text.PlainText
                color: dialogRef.colors["text-muted"]
                font.pixelSize: 12
                font.weight: Font.DemiBold
            }
            Item { Layout.fillWidth: true }
            Row {
                spacing: 4
                visible: sectionTotal > 1
                NativeButton {
                    objectName: "pollQuestionUpButton"
                    colors: dialogRef.colors
                    text: "\u2191"
                    implicitWidth: 32
                    enabled: sectionIndex > 0
                    onClicked: dialogRef.moveSection(sectionIndex, -1)
                }
                NativeButton {
                    objectName: "pollQuestionDownButton"
                    colors: dialogRef.colors
                    text: "\u2193"
                    implicitWidth: 32
                    enabled: sectionIndex < sectionTotal - 1
                    onClicked: dialogRef.moveSection(sectionIndex, 1)
                }
                NativeButton {
                    objectName: "pollQuestionRemoveButton"
                    colors: dialogRef.colors
                    text: "\u00D7"
                    implicitWidth: 32
                    onClicked: dialogRef.removeSection(sectionIndex)
                }
            }
        }
        ColumnLayout {
            spacing: 4
            Layout.fillWidth: true
            Text {
                text: qsTr("Question")
                textFormat: Text.PlainText
                color: dialogRef.colors["text-primary"]
                font.pixelSize: 14
                font.weight: Font.DemiBold
            }
            TextField {
                objectName: "pollQuestionField"
                Layout.fillWidth: true
                placeholderText: qsTr("Enter a question")
                Component.onCompleted: text = section.questionText
                onTextChanged: {
                    if (text.length > 1000) { text = text.slice(0, 1000); return }
                    section.questionText = text
                    dialogRef.revision++
                }
            }
        }
        ColumnLayout {
            spacing: 2
            Text {
                text: qsTr("Answer options")
                textFormat: Text.PlainText
                color: dialogRef.colors["text-primary"]
                font.pixelSize: 14
                font.weight: Font.DemiBold
            }
            Text {
                text: qsTr("Mark correct answers if needed")
                textFormat: Text.PlainText
                color: dialogRef.colors["text-secondary"]
                font.pixelSize: 12
            }
        }
        ColumnLayout {
            id: optionsContainer
            Layout.fillWidth: true
            spacing: 2
        }
        Instantiator {
            id: optionRepeater
            model: optionModel
            delegate: RowLayout {
                required property int index
                objectName: "pollAnswerRow"
                spacing: 6
                Layout.fillWidth: true
                Component.onCompleted: parent = optionsContainer
                TextField {
                    id: answerInput
                    objectName: "pollAnswerField"
                    Layout.fillWidth: true
                    placeholderText: qsTr("Add an answer\u2026")
                    Component.onCompleted: text = optionModel.get(index).text
                    onTextChanged: {
                        if (text.length > 200) { text = text.slice(0, 200); return }
                        optionModel.set(index, {text: text, correct: optionModel.get(index).correct})
                        section.optionsVersion++
                        section.ensureTrailingEmpty()
                    }
                }
                CheckBox {
                    objectName: "pollAnswerCorrectCheck"
                    visible: answerInput.text.trim().length > 0
                    checked: section.optionsVersion, optionModel.get(index).correct
                    Accessible.name: qsTr("Mark as correct")
                    onToggled: {
                        optionModel.set(index, {text: answerInput.text, correct: checked})
                        section.optionsVersion++
                    }
                }
                Row {
                    spacing: 4
                    visible: answerInput.text.trim().length > 0
                    NativeButton {
                        objectName: "pollAnswerUpButton"
                        colors: dialogRef.colors
                        text: "\u2191"
                        implicitWidth: 32
                        enabled: index > 0
                        onClicked: section.moveRow(index, index - 1)
                    }
                    NativeButton {
                        objectName: "pollAnswerDownButton"
                        colors: dialogRef.colors
                        text: "\u2193"
                        implicitWidth: 32
                        enabled: index < optionModel.count - 1
                        onClicked: section.moveRow(index, index + 1)
                    }
                    NativeButton {
                        objectName: "pollAnswerRemoveButton"
                        colors: dialogRef.colors
                        text: "\u00D7"
                        implicitWidth: 32
                        onClicked: section.removeRow(index)
                    }
                }
            }
        }
        ListModel {
            id: optionModel
            Component.onCompleted: append({text: "", correct: false})
        }
        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: dialogRef.colors["border-subtle"]
        }
    }
}
