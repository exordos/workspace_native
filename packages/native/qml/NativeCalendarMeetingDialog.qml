pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: form
    objectName: "calendarMeetingDialog"
    required property var colors
    required property var saveMeeting
    signal saved(date start, string calendar)
    signal deleteRequested(string uuid, string title)
    property string editingUuid: ""
    property string originalTitle: ""
    property string errorMessage: ""

    title: editingUuid ? qsTr("Edit meeting") : qsTr("New meeting")
    modal: true
    focus: true
    anchors.centerIn: parent
    width: Math.min(768, Math.max(1, parent.width - 32))
    height: Math.min(implicitHeight, Math.max(1, parent.height - 32))
    padding: 20
    closePolicy: Popup.CloseOnEscape
    background: Rectangle {
        radius: 12
        color: form.colors["bg-elevated"]
        border.color: form.colors["border-subtle"]
    }

    function openForDate(selectedDate, meeting) {
        editingUuid = meeting ? meeting.uuid : ""
        originalTitle = meeting ? meeting.title : ""
        subject.text = originalTitle
        people.text = meeting ? meeting.participants.join("\n") : ""
        calendarBox.currentIndex = meeting && meeting.calendar === "personal" ? 1 : 0
        const start = meeting ? new Date(meeting.startUtc) : selectedDate
        const end = meeting ? new Date(meeting.endUtc) : selectedDate
        startDate.text = Qt.formatDate(start, "yyyy-MM-dd")
        endDate.text = Qt.formatDate(end, "yyyy-MM-dd")
        startTime.text = meeting ? Qt.formatTime(start, "HH:mm") : "09:00"
        endTime.text = meeting ? Qt.formatTime(end, "HH:mm") : "10:00"
        location.text = meeting ? meeting.location : ""
        errorMessage = ""
        open()
    }

    function localDateTime(day, clock) {
        const value = day + " " + clock
        const instant = Date.fromLocaleString(Qt.locale("C"), value, "yyyy-MM-dd HH:mm")
        return !isNaN(instant.getTime()) && Qt.formatDateTime(instant, "yyyy-MM-dd HH:mm") === value
            ? instant : null
    }

    function submit() {
        if (!subject.text.trim()) {
            errorMessage = qsTr("Enter a meeting title.")
            subject.forceActiveFocus()
            return
        }
        const start = localDateTime(startDate.text, startTime.text)
        const end = localDateTime(endDate.text, endTime.text)
        if (!start || !end || end <= start) {
            errorMessage = qsTr("Enter valid dates and times. The end must be after the start.")
            return
        }
        const calendar = calendarBox.currentIndex === 0 ? "work" : "personal"
        const participants = people.text.split("\n").map(name => name.trim()).filter(name => name.length)
        if (!saveMeeting(editingUuid, {title: subject.text, calendar, start, end,
                          location: location.text, participants})) {
            errorMessage = qsTr("The sample calendar is unavailable.")
            return
        }
        saved(start, calendar)
        close()
    }
    onOpened: subject.forceActiveFocus()

    contentItem: ScrollView {
        id: scroller
        clip: true
        contentWidth: availableWidth
        implicitHeight: fields.implicitHeight
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            id: fields
            width: scroller.availableWidth
            spacing: 8
            Label {
                Layout.fillWidth: true
                text: qsTr("Sample meetings stay in this session. Participants will not receive invitations.")
                color: form.colors["text-muted"]
                wrapMode: Text.Wrap
            }
            Label { id: titleLabel; text: qsTr("Title") }
            TextField {
                id: subject
                objectName: "calendarMeetingTitle"
                Accessible.name: titleLabel.text
                Layout.fillWidth: true
                Layout.preferredHeight: 40
            }
            Label { id: participantsLabel; text: qsTr("Participants") }
            TextArea {
                id: people
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
                model: [qsTr("Work"), qsTr("Personal")]
            }
            GridLayout {
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
            Label { id: locationLabel; text: qsTr("Location") }
            TextField {
                id: location
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
            objectName: "calendarMeetingSave"
            Layout.leftMargin: 20
            Layout.bottomMargin: 16
            highlighted: true
            text: form.editingUuid ? qsTr("Save changes") : qsTr("Create meeting")
            onClicked: form.submit()
        }
        Button {
            objectName: "calendarMeetingCancel"
            Layout.bottomMargin: 16
            text: qsTr("Cancel")
            onClicked: form.reject()
        }
        Item { Layout.fillWidth: true }
        Button {
            objectName: "calendarMeetingDelete"
            visible: form.editingUuid.length > 0
            Layout.rightMargin: 20
            Layout.bottomMargin: 16
            palette.buttonText: form.colors["danger"]
            text: qsTr("Delete meeting")
            onClicked: form.deleteRequested(form.editingUuid, form.originalTitle)
        }
    }
}
