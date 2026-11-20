pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: dialog
    required property var colors
    required property var controller
    property string messageId: ""
    property string sourceAuthor: ""
    property string openedContext: ""
    property string openedStream: ""
    property string openedTopic: ""
    property string question: ""
    property string originalDeadline: ""
    property bool closing: false
    property bool withDeadline: false
    property bool addOptions: false
    property bool invalidDate: false
    property bool pending: false
    property bool failed: false
    readonly property string action: closing ? "close_poll" : "update_settings"

    function showFor(message, closeMode) {
        if (!message || !message.poll || !controller.canManagePoll(message.id)) return
        messageId = String(message.id)
        sourceAuthor = String(message.authorId)
        openedContext = controller.ownStatusContextKey
        openedStream = controller.selectedStream
        openedTopic = controller.selectedTopic
        closing = closeMode
        question = message.poll.questions.length ? String(message.poll.questions[0].text) : ""
        originalDeadline = String(message.poll.closesAt || "")
        withDeadline = originalDeadline.length > 0
        const date = withDeadline ? new Date(originalDeadline) : new Date()
        dateField.text = Qt.formatDate(date, "yyyy-MM-dd")
        timeField.text = Qt.formatTime(date, "HH:mm")
        addOptions = Boolean(message.poll.allowAddingOptions)
        invalidDate = false
        failed = false
        pending = false
        open()
    }
    function submit() {
        if (pending || !messageId.length) return
        const now = Date.now()
        let timestamp = ""
        if (!closing) {
            const original = new Date(originalDeadline)
            const unchanged = originalDeadline.length && Qt.formatDate(original, "yyyy-MM-dd") === dateField.text
                && Qt.formatTime(original, "HH:mm") === timeField.text
            const deadline = unchanged ? original : new Date(dateField.text + "T" + timeField.text)
            if ((originalDeadline.length && original.getTime() <= now)
                || (withDeadline && (isNaN(deadline.getTime()) || deadline.getTime() <= now
                    || Qt.formatDate(deadline, "yyyy-MM-dd") !== dateField.text
                    || Qt.formatTime(deadline, "HH:mm") !== timeField.text))) {
                invalidDate = true
                return
            }
            if (withDeadline) timestamp = deadline.toISOString()
        }
        if (!controller.canManagePoll(messageId)) { failed = true; return }
        failed = false
        pending = true
        if (closing) controller.closePoll(messageId)
        else controller.updatePollSettings(messageId, timestamp, addOptions)
    }
    function matches(message, operation) {
        return visible && pending && message === messageId && operation === action
            && openedContext === controller.ownStatusContextKey
    }
    Connections {
        target: dialog.controller
        function onPollActionSucceeded(message, operation, questionId) {
            if (dialog.matches(message, operation)) { dialog.pending = false; dialog.close() }
        }
        function onPollActionFailed(message, operation, detail, questionId) {
            if (dialog.matches(message, operation)) { dialog.pending = false; dialog.failed = true }
        }
        function onMessagesChanged() {
            if (!dialog.visible) return
            const source = dialog.controller.messages.find(row => row.id === dialog.messageId)
            if (!source || !source.poll || source.authorId !== dialog.sourceAuthor || source.poll.state.isClosed)
                dialog.close()
        }
        function onStateChanged() {
            if (dialog.visible && (dialog.controller.ownStatusContextKey !== dialog.openedContext
                || dialog.controller.selectedStream !== dialog.openedStream
                || dialog.controller.selectedTopic !== dialog.openedTopic)) dialog.close()
        }
    }
    width: Math.min(480, parent ? parent.width - 32 : 480)
    anchors.centerIn: parent
    padding: 24
    focus: true
    modal: true
    closePolicy: pending ? Popup.NoAutoClose : Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onClosed: { pending = false; messageId = ""; openedContext = "" }
    Overlay.modal: Rectangle { color: "#80000000" }
    background: Rectangle { radius: 12; color: dialog.colors["bg-elevated"]; border.color: dialog.colors["border-subtle"] }
    contentItem: ColumnLayout {
        spacing: 16
        Text {
            Layout.fillWidth: true
            objectName: "poll-management-title"
            text: dialog.closing ? qsTr("Close poll?") : qsTr("Edit poll")
            textFormat: Text.PlainText; color: dialog.colors["text-primary"]
            font.pixelSize: 18; font.weight: Font.DemiBold; wrapMode: Text.Wrap
        }
        Text {
            Layout.fillWidth: true
            text: dialog.closing ? qsTr("Participants will no longer be able to vote or change their answers.") : dialog.question
            textFormat: Text.PlainText; wrapMode: Text.Wrap
            color: dialog.colors["text-primary"]; font.pixelSize: 14
            font.weight: dialog.closing ? Font.Normal : Font.DemiBold
        }
        ColumnLayout {
            Layout.fillWidth: true
            visible: !dialog.closing
            enabled: !dialog.pending
            spacing: 6
            RowLayout {
                Layout.fillWidth: true
                Text {
                    Layout.fillWidth: true; text: qsTr("Poll closing date"); textFormat: Text.PlainText
                    color: dialog.colors["text-primary"]; font.pixelSize: 14
                }
                Button {
                    objectName: "poll-management-deadline"
                    text: dialog.withDeadline ? qsTr("Remove deadline") : qsTr("Set a deadline")
                    onClicked: { dialog.withDeadline = !dialog.withDeadline; dialog.invalidDate = false }
                }
            }
            RowLayout {
                visible: dialog.withDeadline
                Layout.fillWidth: true
                TextField {
                    id: dateField; objectName: "poll-management-date"
                    Layout.fillWidth: true
                    placeholderText: "yyyy-MM-dd"; Accessible.name: qsTr("Poll closing date")
                    onTextChanged: dialog.invalidDate = false
                }
                TextField {
                    id: timeField; objectName: "poll-management-time"
                    Layout.preferredWidth: 88; placeholderText: "HH:mm"
                    Accessible.name: qsTr("Time"); onTextChanged: dialog.invalidDate = false
                }
            }
            Text {
                objectName: "poll-management-invalid"; visible: dialog.invalidDate
                text: qsTr("Choose a future date"); textFormat: Text.PlainText
                color: dialog.colors.danger; font.pixelSize: 12; Accessible.role: Accessible.AlertMessage
            }
            CheckBox {
                objectName: "poll-management-add-options"
                text: qsTr("Allow adding options"); checked: dialog.addOptions
                onToggled: dialog.addOptions = checked
            }
            Text {
                Layout.fillWidth: true; text: qsTr("Participants can add answer options")
                textFormat: Text.PlainText; color: dialog.colors["text-secondary"]; font.pixelSize: 12; wrapMode: Text.Wrap
            }
        }
        Text {
            objectName: "poll-management-error"; Layout.fillWidth: true; visible: dialog.failed
            text: qsTr("Could not save the change. Please try again.")
            textFormat: Text.PlainText; wrapMode: Text.Wrap; color: dialog.colors.danger
            font.pixelSize: 14; Accessible.role: Accessible.AlertMessage
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            Button { text: qsTr("Cancel"); enabled: !dialog.pending; onClicked: dialog.close() }
            Button {
                objectName: "poll-management-submit"
                text: dialog.closing ? qsTr("Close poll") : qsTr("Save")
                enabled: !dialog.pending; onClicked: dialog.submit()
            }
        }
    }
}
