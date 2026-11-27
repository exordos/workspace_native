pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: form
    objectName: "calendarMeetingDialog"
    required property var colors
    required property var saveMeeting
    required property var serverActions
    required property var calendars
    required property bool sample
    property string sourceCalendar: ""
    property bool moving: false
    property string createUuid: ""
    property bool retryOriginal: false
    property string submittedCalendar: ""
    property var submittedStart: null
    signal saved(date start, string calendar)
    signal deleteRequested(string uuid, string title)
    property string editingUuid: ""
    property string originalTitle: ""
    property string originalStartUtc: ""
    property string originalEndUtc: ""
    property string errorMessage: ""

    title: moving ? qsTr("Move meeting") : editingUuid ? qsTr("Edit meeting") : qsTr("New meeting")
    modal: true
    focus: true
    anchors.centerIn: parent
    width: Math.min(768, Math.max(1, parent.width - 32))
    height: Math.min(implicitHeight, Math.max(1, parent.height - 32))
    padding: 20
    closePolicy: serverActions.calendarWriting ? Popup.NoAutoClose : Popup.CloseOnEscape
    background: Rectangle {
        radius: 12
        color: form.colors["bg-elevated"]
        border.color: form.colors["border-subtle"]
    }

    function openForDate(selectedDate, meeting, moveOnly) {
        retryOriginal = false
        sourceCalendar = meeting ? meeting.calendar : ""
        moving = moveOnly === true
        createUuid = !sample && !meeting ? serverActions.newCalendarMeetingUuid() : ""
        editingUuid = meeting ? meeting.uuid : ""
        originalTitle = meeting ? meeting.title : ""
        subject.text = originalTitle
        people.text = meeting ? meeting.participants.join("\n") : ""
        const previous = meeting ? calendars.findIndex(row => row.key === meeting.calendar) : -1
        calendarBox.currentIndex = moving ? 0 : Math.max(previous, 0)
        originalStartUtc = meeting ? meeting.startUtc : ""
        originalEndUtc = meeting ? meeting.endUtc : ""
        const start = meeting ? new Date(originalStartUtc) : selectedDate
        const end = meeting ? new Date(originalEndUtc) : selectedDate
        startDate.text = Qt.formatDate(start, "yyyy-MM-dd")
        endDate.text = Qt.formatDate(end, "yyyy-MM-dd")
        startTime.text = meeting ? Qt.formatTime(start, "HH:mm") : "09:00"
        endTime.text = meeting ? Qt.formatTime(end, "HH:mm") : "10:00"
        location.text = meeting ? meeting.location : ""
        errorMessage = ""
        open()
    }

    function localDateTime(day, clock, originalUtc) {
        const value = day + " " + clock
        if (originalUtc) {
            const original = new Date(originalUtc)
            // The repeated hour at a DST transition has two valid instants.
            // A title-only edit must also preserve seconds hidden by the form.
            if (Qt.formatDateTime(original, "yyyy-MM-dd HH:mm") === value)
                return original
        }
        const instant = Date.fromLocaleString(Qt.locale("C"), value, "yyyy-MM-dd HH:mm")
        return !isNaN(instant.getTime()) && Qt.formatDateTime(instant, "yyyy-MM-dd HH:mm") === value
            ? instant : null
    }

    function submit() {
        const calendar = calendarBox.currentValue
        if (!calendar || serverActions.calendarWriting) return
        if (moving) {
            submittedStart = new Date(originalStartUtc)
            submittedCalendar = calendar
            if (!serverActions.writeCalendarMeeting("move", editingUuid, {calendar}))
                errorMessage = qsTr("This calendar operation is unavailable.")
            return
        }
        if (!subject.text.trim()) {
            errorMessage = qsTr("Enter a meeting title.")
            subject.forceActiveFocus()
            return
        }
        const start = localDateTime(startDate.text, startTime.text, originalStartUtc)
        const end = localDateTime(endDate.text, endTime.text, originalEndUtc)
        // Preserve the backend's fractional seconds when the displayed minute
        // is unchanged. Two valid original instants can be a microsecond apart.
        const startWire = start && originalStartUtc && start.getTime() === new Date(originalStartUtc).getTime()
                        ? originalStartUtc : start ? start.toISOString() : ""
        const endWire = end && originalEndUtc && end.getTime() === new Date(originalEndUtc).getTime()
                      ? originalEndUtc : end ? end.toISOString() : ""
        const keepsOriginal = !sample && startWire === originalStartUtc && endWire === originalEndUtc
        if (!start || !end || (end <= start && !keepsOriginal)) {
            errorMessage = qsTr("Enter valid dates and times. The end must be after the start.")
            return
        }
        if (!sample) {
            submittedStart = start
            submittedCalendar = calendar
            const action = editingUuid ? "edit" : "create"
            if (!serverActions.writeCalendarMeeting(action, editingUuid || createUuid,
                    {title: subject.text, calendar, startUtc: startWire, endUtc: endWire, location: location.text}))
                errorMessage = qsTr("This calendar operation is unavailable.")
            return
        }
        const participants = people.text.split("\n").map(name => name.trim()).filter(name => name.length)
        if (!saveMeeting(editingUuid, {title: subject.text, calendar, start, end,
                          location: location.text, participants})) {
            errorMessage = qsTr("The sample calendar is unavailable.")
            return
        }
        saved(start, calendar)
        close()
    }
    function completed(accepted, retryUnchanged) {
        if (!visible || submittedStart === null) return
        if (accepted) {
            saved(submittedStart, submittedCalendar)
            close()
        } else {
            retryOriginal = retryUnchanged
            errorMessage = retryUnchanged ? qsTr("Could not confirm the new meeting. Retry it unchanged or cancel and check your calendar.")
                                          : qsTr("Could not confirm the change. Try again.")
        }
    }
    onClosed: submittedStart = null
    onOpened: moving ? calendarBox.forceActiveFocus() : subject.forceActiveFocus()

    contentItem: ScrollView {
        id: scroller
        clip: true
        contentWidth: availableWidth
        implicitHeight: fields.implicitHeight
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            id: fields
            enabled: !form.serverActions.calendarWriting && !form.retryOriginal
            width: scroller.availableWidth
            spacing: 8
            Label {
                Layout.fillWidth: true
                visible: form.sample
                text: qsTr("Sample meetings stay in this session. Participants will not receive invitations.")
                color: form.colors["text-muted"]
                wrapMode: Text.Wrap
            }
            Label { id: titleLabel; visible: !form.moving; text: qsTr("Title") }
            TextField {
                id: subject
                visible: !form.moving
                objectName: "calendarMeetingTitle"
                Accessible.name: titleLabel.text
                Layout.fillWidth: true
                Layout.preferredHeight: 40
            }
            Label { id: participantsLabel; visible: form.sample; text: qsTr("Participants") }
            TextArea {
                id: people
                visible: form.sample
                objectName: "calendarMeetingParticipants"
                Accessible.name: participantsLabel.text
                placeholderText: qsTr("One name per line")
                Layout.fillWidth: true
                wrapMode: TextEdit.Wrap
                textFormat: TextEdit.PlainText
            }
            Label { id: calendarLabel; text: qsTr("Calendar") }
            ComboBox {
                id: calendarBox
                objectName: "calendarMeetingCalendar"
                Accessible.name: calendarLabel.text
                Layout.fillWidth: true
                model: form.moving ? form.calendars.filter(row => row.key !== form.sourceCalendar) : form.calendars
                textRole: "label"
                valueRole: "key"
                enabled: !form.editingUuid || form.sample || form.moving
                contentItem: Label {
                    text: calendarBox.displayText
                    textFormat: Text.PlainText
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }
                delegate: ItemDelegate {
                    required property var modelData
                    width: calendarBox.width
                    contentItem: Label {
                        text: modelData.label
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                    }
                }
            }
            GridLayout {
                visible: !form.moving
                Layout.fillWidth: true
                columns: 2
                columnSpacing: 12
                Label { id: startDateLabel; text: qsTr("Start date") }
                Label { id: startTimeLabel; text: qsTr("Start time") }
                TextField {
                    id: startDate
                    objectName: "calendarMeetingStartDate"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Accessible.name: startDateLabel.text
                    placeholderText: "YYYY-MM-DD"
                }
                TextField {
                    id: startTime
                    objectName: "calendarMeetingStartTime"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Accessible.name: startTimeLabel.text
                    placeholderText: "HH:MM"
                }
                Label { id: endDateLabel; text: qsTr("End date") }
                Label { id: endTimeLabel; text: qsTr("End time") }
                TextField {
                    id: endDate
                    objectName: "calendarMeetingEndDate"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Accessible.name: endDateLabel.text
                    placeholderText: "YYYY-MM-DD"
                }
                TextField {
                    id: endTime
                    objectName: "calendarMeetingEndTime"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Accessible.name: endTimeLabel.text
                    placeholderText: "HH:MM"
                }
            }
            Label { id: locationLabel; visible: !form.moving; text: qsTr("Location") }
            TextField {
                id: location
                visible: !form.moving
                objectName: "calendarMeetingLocation"
                Accessible.name: locationLabel.text
                Layout.fillWidth: true
            }
            Label {
                objectName: "calendarMeetingError"
                Layout.fillWidth: true
                visible: form.errorMessage.length > 0
                text: form.errorMessage
                color: form.colors["danger"]
                wrapMode: Text.Wrap
            }
        }
    }
    footer: RowLayout {
        spacing: 8
        Button {
            enabled: !form.serverActions.calendarWriting
            objectName: "calendarMeetingSave"
            Layout.leftMargin: 20
            Layout.bottomMargin: 16
            highlighted: true
            text: form.moving ? qsTr("Move meeting") : form.editingUuid ? qsTr("Save changes") : qsTr("Create meeting")
            onClicked: form.submit()
        }
        Button {
            enabled: !form.serverActions.calendarWriting
            objectName: "calendarMeetingCancel"
            Layout.bottomMargin: 16
            text: qsTr("Cancel")
            onClicked: form.reject()
        }
        Item { Layout.fillWidth: true }
        Button {
            objectName: "calendarMeetingDelete"
            visible: form.editingUuid.length > 0 && !form.moving
            enabled: !form.serverActions.calendarWriting
            Layout.rightMargin: 20
            Layout.bottomMargin: 16
            palette.buttonText: form.colors["danger"]
            text: qsTr("Delete meeting")
            onClicked: form.deleteRequested(form.editingUuid, form.originalTitle)
        }
    }
}
