pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Item {
    id: week
    objectName: "calendarWeekGrid"
    required property var colors
    required property string localeName
    required property var dates
    required property var eventsForDate
    signal eventActivated(var event)
    readonly property real gutter: 42
    readonly property real columnWidth: Math.max(1, (width - gutter) / 7)
    readonly property real hourHeight: Math.max(44, (height - header.height) / 10)
    readonly property var days: dates.map(date => arrange(eventsForDate(date), Qt.formatDate(date, "yyyy-MM-dd")))
    readonly property int firstHour: Math.min(8, ...days.map(day => day.timed.length
        ? Math.floor(day.timed[0].start / 60) : 8))

    function minutes(time) {
        const parts = String(time).split(":")
        return Math.max(0, Math.min(1440, (Number(parts[0]) || 0) * 60 + (Number(parts[1]) || 0)))
    }

    // A chain of overlapping intervals shares a column count. Adjacent meetings
    // reuse the full day width as soon as the previous group has ended.
    function arrange(events, date) {
        const allDay = []
        const timed = []
        for (const event of events) {
            if (event.allDay) {
                allDay.push(event)
                continue
            }
            const start = event.day < date ? 0 : minutes(event.start)
            const endDate = event.endUtc ? Qt.formatDate(new Date(event.endUtc), "yyyy-MM-dd")
                                         : (event.lastDay || event.day)
            const end = endDate > date ? 1440 : minutes(event.end)
            timed.push({event, start, end: Math.max(start + 1, end), lane: 0, lanes: 1})
        }
        timed.sort((a, b) => a.start - b.start || b.end - a.end)
        let first = 0
        while (first < timed.length) {
            const ends = []
            let last = first
            let groupEnd = timed[first].end
            while (last < timed.length && (last === first || timed[last].start < groupEnd)) {
                const item = timed[last++]
                const free = ends.findIndex(end => end <= item.start)
                item.lane = free < 0 ? ends.length : free
                ends[item.lane] = item.end
                groupEnd = Math.max(groupEnd, item.end)
            }
            for (let i = first; i < last; ++i)
                timed[i].lanes = ends.length
            first = last
        }
        return {timed, allDay}
    }

    component EventChip: Label {
        id: chip
        required property var event
        objectName: "weekChip-" + event.uuid
        text: event.title
        textFormat: Text.PlainText
        elide: Text.ElideRight
        clip: true
        leftPadding: 4
        rightPadding: 2
        topPadding: 2
        verticalAlignment: Text.AlignTop
        color: week.colors["text-primary"]
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: event.allDay ? text : event.start + " – " + event.end + " " + text
        Accessible.onPressAction: week.eventActivated(chip.event)
        Keys.onSpacePressed: week.eventActivated(chip.event)
        Keys.onReturnPressed: week.eventActivated(chip.event)
        background: Rectangle {
            radius: 3
            color: week.colors["accent-soft"]
            border.color: chip.activeFocus ? week.colors["text-primary"] : week.colors["accent"]
            border.width: chip.activeFocus ? 2 : 1
        }
        FontMetrics { id: chipMetrics; font: chip.font }
        Label {
            objectName: "weekTimeRange"
            visible: !chip.event.allDay && chip.height >= chipMetrics.height + 16
            anchors { left: parent.left; right: parent.right; top: parent.top; topMargin: chipMetrics.height + 3 }
            leftPadding: chip.leftPadding
            rightPadding: chip.rightPadding
            text: chip.event.start + " – " + chip.event.end
            textFormat: Text.PlainText
            elide: Text.ElideRight
            font.pixelSize: 10
            color: week.colors["text-muted"]
        }
        TapHandler { onTapped: week.eventActivated(chip.event) }
    }

    Row {
        id: header
        x: week.gutter
        Repeater {
            model: 7
            delegate: Column {
                id: heading
                required property int index
                width: week.columnWidth
                Label {
                    width: parent.width
                    height: 26
                    leftPadding: 4
                    verticalAlignment: Text.AlignVCenter
                    text: Qt.locale(week.localeName).toString(week.dates[heading.index], "ddd d")
                    color: week.colors["text-muted"]
                    elide: Text.ElideRight
                }
                Repeater {
                    model: week.days[heading.index].allDay
                    delegate: EventChip {
                        required property var modelData
                        event: modelData
                        width: heading.width - 2
                        height: 20
                    }
                }
            }
        }
    }

    Flickable {
        id: scroll
        objectName: "calendarWeekTimeline"
        anchors { top: header.bottom; bottom: parent.bottom; left: parent.left; right: parent.right }
        clip: true
        contentWidth: width
        contentHeight: week.hourHeight * 24
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        function showWorkingDay() {
            contentY = Math.min(week.firstHour * week.hourHeight, Math.max(0, contentHeight - height))
        }
        onVisibleChanged: if (visible) Qt.callLater(showWorkingDay)
        Connections { target: week; function onFirstHourChanged() { scroll.showWorkingDay() } }
        Repeater {
            model: 24
            delegate: Item {
                required property int index
                y: index * week.hourHeight
                width: scroll.width
                height: week.hourHeight
                Rectangle { x: week.gutter; width: parent.width - x; height: 1; color: week.colors["border-subtle"] }
                Label {
                    width: week.gutter - 3
                    text: String(parent.index).padStart(2, "0") + ":00"
                    font.pixelSize: 11
                    color: week.colors["text-muted"]
                }
            }
        }
        Row {
            x: week.gutter
            Repeater {
                objectName: "weekColumns"
                model: 7
                delegate: Item {
                    id: column
                    required property int index
                    width: week.columnWidth
                    height: scroll.contentHeight
                    Rectangle { width: 1; height: parent.height; color: week.colors["border-subtle"] }
                    Repeater {
                        model: week.days[column.index].timed
                        delegate: EventChip {
                            required property var modelData
                            event: modelData.event
                            x: 2 + modelData.lane * column.width / modelData.lanes
                            y: modelData.start * week.hourHeight / 60
                            width: Math.max(1, column.width / modelData.lanes - 3)
                            height: Math.max(1, (modelData.end - modelData.start) * week.hourHeight / 60 - 1)
                        }
                    }
                }
            }
        }
    }
}
