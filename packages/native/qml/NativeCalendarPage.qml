pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Calendar section page shell (issue #881, Part of #851,
// docs/NATIVE_DESKTOP_CALENDAR.md). UI phase: deterministic fixture data is
// fed through `events`; the live adapter is #852. The reference app ships no
// functional calendar screen (embed placeholder only), so layout follows the
// cached Figma frame names 18693:32760 (day), 18693:32323 (week),
// 18693:33825 (month); concrete metrics are marked pending-Figma until the
// Figma token is restored.
FocusScope {
    id: page
    objectName: "nativeCalendarPage"

    required property var colors
    property var events: []
    property date selectedDate: new Date()
    property string viewMode: "day"
    property bool loading: false
    property bool error: false
    property var activeCalendars: ["work", "personal"]

    signal visibleWindowChanged(string from, string to)
    signal retryRequested()

    readonly property int dayMs: 86400000

    function calDate(d) {
        return Qt.formatDate(d, "yyyy-MM-dd");
    }
    function shiftDate(d, days) {
        const r = new Date(d.getTime());
        r.setDate(r.getDate() + days);
        return r;
    }
    function monthStart(d) {
        return new Date(d.getFullYear(), d.getMonth(), 1);
    }
    function weekStart(d) {
        const wd = (d.getDay() + 6) % 7; // Monday first
        return shiftDate(d, -wd);
    }
    function visibleEvents(fromDate, toDate) {
        const out = [];
        for (const ev of events) {
            if (activeCalendars.indexOf(String(ev.calendar)) < 0)
                continue;
            if (String(ev.day) >= fromDate && String(ev.day) <= toDate)
                out.push(ev);
        }
        out.sort((a, b) => String(a.start).localeCompare(String(b.start)));
        return out;
    }
    function dayEvents() {
        const day = calDate(selectedDate);
        return visibleEvents(day, day);
    }
    // #925: whole-day difference between two dates (UTC math, DST-proof),
    // used by the grids to map a date to its cell index.
    function dayDiff(from, to) {
        return Math.round((Date.UTC(to.getFullYear(), to.getMonth(), to.getDate())
                          - Date.UTC(from.getFullYear(), from.getMonth(), from.getDate())) / 86400000);
    }
    function refreshWindow() {
        if (viewMode === "day")
            visibleWindowChanged(calDate(selectedDate), calDate(selectedDate));
        else if (viewMode === "week")
            visibleWindowChanged(calDate(weekStart(selectedDate)),
                                 calDate(shiftDate(weekStart(selectedDate), 6)));
        else {
            // Issue #911: the month grid renders a fixed 42-cell matrix that
            // starts at the leading week of the month, so the requested window
            // must span exactly those 42 cells - the leading/trailing rows
            // show adjacent-month days and events on them are real content.
            const gridStart = weekStart(monthStart(selectedDate));
            visibleWindowChanged(calDate(gridStart),
                                 calDate(shiftDate(gridStart, 41)));
        }
    }
    onSelectedDateChanged: refreshWindow()
    onViewModeChanged: refreshWindow()

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- header: view tabs + date navigation (pending-Figma metrics) ----
        RowLayout {
            id: header
            objectName: "calendarHeader"
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            spacing: 8

            Repeater {
                id: viewTabs
                objectName: "calendarViewTabs"
                model: [{ key: "day", label: qsTr("Day") },
                        { key: "week", label: qsTr("Week") },
                        { key: "month", label: qsTr("Month") }]
                delegate: Button {
                    id: viewTab
                    objectName: "viewTab" + modelData.key.charAt(0).toUpperCase()
                                + modelData.key.slice(1)
                    required property var modelData
                    text: modelData.label
                    checkable: true
                    checked: page.viewMode === modelData.key
                    onClicked: page.viewMode = modelData.key
                }
            }

            Item { Layout.fillWidth: true }

            Button {
                objectName: "calendarPrev"
                text: "<"
                onClicked: {
                    if (page.viewMode === "day")
                        page.selectedDate = page.shiftDate(page.selectedDate, -1);
                    else if (page.viewMode === "week")
                        page.selectedDate = page.shiftDate(page.selectedDate, -7);
                    else
                        page.selectedDate = new Date(page.selectedDate.getFullYear(),
                                                     page.selectedDate.getMonth() - 1, 1);
                }
            }
            Label {
                id: dateTitle
                objectName: "calendarDateTitle"
                color: page.colors["text-primary"]
                text: {
                    if (page.viewMode === "day")
                        return Qt.formatDate(page.selectedDate, "dd.MM.yyyy");
                    if (page.viewMode === "week")
                        return Qt.formatDate(page.weekStart(page.selectedDate), "dd.MM")
                               + " - "
                               + Qt.formatDate(page.shiftDate(page.weekStart(page.selectedDate), 6), "dd.MM.yyyy");
                    return Qt.formatDate(page.selectedDate, "MMMM yyyy");
                }
            }
            Button {
                objectName: "calendarNext"
                text: ">"
                onClicked: {
                    if (page.viewMode === "day")
                        page.selectedDate = page.shiftDate(page.selectedDate, 1);
                    else if (page.viewMode === "week")
                        page.selectedDate = page.shiftDate(page.selectedDate, 7);
                    else
                        page.selectedDate = new Date(page.selectedDate.getFullYear(),
                                                     page.selectedDate.getMonth() + 1, 1);
                }
            }
            Button {
                objectName: "calendarToday"
                text: qsTr("Today")
                onClicked: page.selectedDate = new Date()
            }
        }

        // ---- state cards (feed-page card pattern; pending-Figma) ----
        Label {
            objectName: "calendarLoading"
            visible: page.loading && !page.error
            text: qsTr("Loading...")
            color: page.colors["text-muted"]
            padding: 12
        }
        Column {
            objectName: "calendarError"
            visible: page.error
            spacing: 8
            Label {
                text: qsTr("Could not load the calendar.")
                color: page.colors["text-primary"]
                padding: 12
            }
            Button {
                objectName: "calendarRetry"
                text: qsTr("Retry")
                onClicked: page.retryRequested()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12

            // ---- left rail: mini-calendar + visibility filters ----
            ColumnLayout {
                Layout.preferredWidth: 240
                Layout.maximumWidth: 240
                Layout.fillHeight: true
                Layout.margins: 12
                spacing: 8

                Label {
                    objectName: "miniCalendarTitle"
                    color: page.colors["text-primary"]
                    text: Qt.formatDate(page.selectedDate, "MMMM yyyy")
                }
                // Stable positional model (issue pitfall): delegates read
                // live state through `page`, never through modelData identity.
                Grid {
                    id: miniGrid
                    objectName: "calendarMini"
                    columns: 7
                    spacing: 2
                    // #925: the grid is ONE tab stop. Roving keyboard focus is
                    // kept as an index into the 42-cell matrix (never 42 tab
                    // stops), arrow keys roam it, and Enter/Space runs the
                    // same selection action the pointer uses.
                    activeFocusOnTab: true
                    property int focusIndex: -1
                    readonly property int selectedGridIndex: {
                        page.selectedDate; // re-evaluate on selection
                        const first = page.monthStart(page.selectedDate);
                        const offset = (first.getDay() + 6) % 7;
                        return page.dayDiff(first, page.selectedDate) + offset;
                    }
                    readonly property int effectiveFocusIndex:
                        focusIndex >= 0 ? focusIndex : selectedGridIndex
                    function moveFocus(delta)
                    {
                        const next = miniGrid.effectiveFocusIndex + delta;
                        if (next >= 0 && next < miniRepeater.count)
                            miniGrid.focusIndex = next;
                    }
                    function selectFocused()
                    {
                        const cell = miniRepeater.itemAt(miniGrid.effectiveFocusIndex);
                        if (cell)
                            page.selectedDate = cell.cellDate;
                    }
                    onActiveFocusChanged: if (!miniGrid.activeFocus)
                        miniGrid.focusIndex = -1
                    Keys.onPressed: (event) => {
                        switch (event.key) {
                        case Qt.Key_Left:  miniGrid.moveFocus(-1); break;
                        case Qt.Key_Right: miniGrid.moveFocus(1); break;
                        case Qt.Key_Up:    miniGrid.moveFocus(-7); break;
                        case Qt.Key_Down:  miniGrid.moveFocus(7); break;
                        case Qt.Key_Return:
                        case Qt.Key_Enter:
                        case Qt.Key_Space: miniGrid.selectFocused(); break;
                        default: return;
                        }
                        event.accepted = true;
                    }
                    Repeater {
                        id: miniRepeater
                        objectName: "miniCells"
                        model: Array.from({ length: 42 }, (_, i) => i)
                        delegate: Item {
                            id: miniCell
                            required property int modelData
                            width: 30
                            height: 30
                            readonly property var cellDate: {
                                page.selectedDate; // re-evaluate on selection
                                const first = page.monthStart(page.selectedDate);
                                const offset = (first.getDay() + 6) % 7;
                                return page.shiftDate(first, modelData - offset);
                            }
                            readonly property bool inMonth: cellDate.getMonth() === page.selectedDate.getMonth()
                            readonly property bool isToday: page.calDate(cellDate) === page.calDate(new Date())
                            readonly property bool isSelected: page.calDate(cellDate) === page.calDate(page.selectedDate)
                            readonly property bool keyboardFocused: miniGrid.activeFocus
                                && miniGrid.effectiveFocusIndex === modelData
                            Accessible.role: Accessible.Button
                            Accessible.name: Qt.formatDate(miniCell.cellDate, Qt.locale(), QLocale.LongFormat)
                            Accessible.selected: miniCell.isSelected
                            Accessible.onPressAction: page.selectedDate = miniCell.cellDate
                            Rectangle {
                                objectName: "miniFocusRing"
                                anchors.fill: parent
                                radius: 6
                                color: "transparent"
                                border.width: 2
                                border.color: page.colors["accent"]
                                visible: miniCell.keyboardFocused
                            }
                            Rectangle {
                                anchors.fill: parent
                                radius: 6
                                color: miniCell.isSelected ? page.colors["accent"] : "transparent"
                            }
                            Label {
                                anchors.centerIn: parent
                                text: miniCell.cellDate.getDate()
                                color: miniCell.isSelected ? page.colors["on-accent"]
                                                           : (miniCell.inMonth ? page.colors["text-primary"]
                                                                              : page.colors["text-muted"])
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: page.selectedDate = miniCell.cellDate
                            }
                        }
                    }
                }
                Repeater {
                    id: filterRows
                    objectName: "calendarFilters"
                    model: [
                        { key: "work", label: qsTr("Work") },
                        { key: "personal", label: qsTr("Personal") }
                    ]
                    delegate: CheckBox {
                        objectName: "calendarFilter" + modelData.key.charAt(0).toUpperCase()
                                      + modelData.key.slice(1)
                        required property var modelData
                        text: modelData.label
                        checked: page.activeCalendars.indexOf(modelData.key) >= 0
                        // onToggled, NOT onCheckedChanged: the checked binding reads
                        // activeCalendars, so a data-driven re-check that wrote the
                        // array back (as onCheckedChanged did) re-entered the binding
                        // and Qt froze the projection with "Binding loop detected for
                        // property checked" (CI #904: the warning poisoned every
                        // window case in the shard). Only user toggles mutate the
                        // filter now, and only on a real membership change.
                        onToggled: {
                            const cur = page.activeCalendars.slice();
                            const idx = cur.indexOf(modelData.key);
                            if (checked && idx < 0) cur.push(modelData.key);
                            else if (!checked && idx >= 0) cur.splice(idx, 1);
                            else return;
                            page.activeCalendars = cur;
                        }
                    }
                }
                Item { Layout.fillHeight: true }
            }

            // ---- main pane: active view ----
            StackLayout {
                id: mainPane
                objectName: "calendarMainPane"
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: page.viewMode === "day" ? 0 : (page.viewMode === "week" ? 1 : 2)

                // Day view: agenda for the selected date.
                ListView {
                    id: agendaList
                    objectName: "calendarAgenda"
                    clip: true
                    model: page.dayEvents()
                    delegate: ItemDelegate {
                        id: agendaRow
                        required property var modelData
                        width: agendaList.width
                        contentItem: Row {
                            spacing: 8
                            Label {
                                text: modelData.start + " - " + modelData.end
                                color: page.colors["text-muted"]
                            }
                            Label {
                                text: modelData.title
                                color: page.colors["text-primary"]
                            }
                        }
                    }
                    Label {
                        objectName: "calendarEmpty"
                        visible: agendaList.count === 0 && !page.loading && !page.error
                        anchors.centerIn: parent
                        text: qsTr("No events on this day.")
                        color: page.colors["text-muted"]
                    }
                }

                // Week view: seven day columns with event chips.
                Row {
                    id: weekGrid
                    objectName: "calendarWeekGrid"
                    spacing: 1
                    Repeater {
                        objectName: "weekColumns"
                        model: Array.from({ length: 7 }, (_, i) => i)
                        delegate: Column {
                            id: weekColumn
                            required property int modelData
                            width: (weekGrid.width - 6) / 7
                            spacing: 2
                            readonly property var cellDate: {
                                page.selectedDate;
                                return page.shiftDate(page.weekStart(page.selectedDate), modelData);
                            }
                            Label {
                                color: page.colors["text-muted"]
                                text: Qt.formatDate(weekColumn.cellDate, "dd")
                            }
                            Repeater {
                                model: page.visibleEvents(
                                    page.calDate(weekColumn.cellDate),
                                    page.calDate(weekColumn.cellDate))
                                delegate: Label {
                                    required property var modelData
                                    objectName: "weekChip-" + modelData.uuid
                                    text: modelData.start + " " + modelData.title
                                    color: page.colors["text-primary"]
                                }
                            }
                        }
                    }
                }

                // Month view: 6x7 grid with event dots + agenda pane.
                RowLayout {
                    spacing: 12
                    Grid {
                        id: monthGrid
                        objectName: "calendarMonthGrid"
                        columns: 7
                        spacing: 1
                        // #925: same roving-focus keyboard contract as the
                        // mini-calendar (one tab stop, arrows roam, Enter
                        // selects).
                        activeFocusOnTab: true
                        property int focusIndex: -1
                        readonly property int selectedGridIndex: {
                            page.selectedDate; // re-evaluate on selection
                            const first = page.monthStart(page.selectedDate);
                            const offset = (first.getDay() + 6) % 7;
                            return page.dayDiff(first, page.selectedDate) + offset;
                        }
                        readonly property int effectiveFocusIndex:
                            focusIndex >= 0 ? focusIndex : selectedGridIndex
                        function moveFocus(delta)
                        {
                            const next = monthGrid.effectiveFocusIndex + delta;
                            if (next >= 0 && next < monthRepeater.count)
                                monthGrid.focusIndex = next;
                        }
                        function selectFocused()
                        {
                            const cell = monthRepeater.itemAt(monthGrid.effectiveFocusIndex);
                            if (cell)
                                page.selectedDate = cell.cellDate;
                        }
                        onActiveFocusChanged: if (!monthGrid.activeFocus)
                            monthGrid.focusIndex = -1
                        Keys.onPressed: (event) => {
                            switch (event.key) {
                            case Qt.Key_Left:  monthGrid.moveFocus(-1); break;
                            case Qt.Key_Right: monthGrid.moveFocus(1); break;
                            case Qt.Key_Up:    monthGrid.moveFocus(-7); break;
                            case Qt.Key_Down:  monthGrid.moveFocus(7); break;
                            case Qt.Key_Return:
                            case Qt.Key_Enter:
                            case Qt.Key_Space: monthGrid.selectFocused(); break;
                            default: return;
                            }
                            event.accepted = true;
                        }
                        Repeater {
                            id: monthRepeater
                            objectName: "monthCells"
                            model: Array.from({ length: 42 }, (_, i) => i)
                            delegate: Rectangle {
                                id: monthCell
                                required property int modelData
                                width: 64
                                height: 64
                                readonly property var cellDate: {
                                    page.selectedDate;
                                    const first = page.monthStart(page.selectedDate);
                                    const offset = (first.getDay() + 6) % 7;
                                    return page.shiftDate(first, modelData - offset);
                                }
                                readonly property int eventCount: page.visibleEvents(
                                    page.calDate(cellDate), page.calDate(cellDate)).length
                                readonly property bool isSelected: page.calDate(cellDate) === page.calDate(page.selectedDate)
                                readonly property bool keyboardFocused: monthGrid.activeFocus
                                    && monthGrid.effectiveFocusIndex === modelData
                                Accessible.role: Accessible.Button
                                Accessible.name: Qt.formatDate(monthCell.cellDate, Qt.locale(), QLocale.LongFormat)
                                Accessible.selected: monthCell.isSelected
                                Accessible.onPressAction: page.selectedDate = monthCell.cellDate
                                color: monthCell.eventCount > 0 ? page.colors["bg-elevated"] : "transparent"
                                radius: 4
                                Rectangle {
                                    objectName: "monthFocusRing"
                                    anchors.fill: parent
                                    radius: 4
                                    color: "transparent"
                                    border.width: 2
                                    border.color: page.colors["accent"]
                                    visible: monthCell.keyboardFocused
                                }
                                Label {
                                    anchors.top: parent.top
                                    anchors.left: parent.left
                                    anchors.margins: 4
                                    text: monthCell.cellDate.getDate()
                                    color: page.colors["text-primary"]
                                }
                                Row {
                                    anchors.bottom: parent.bottom
                                    anchors.left: parent.left
                                    anchors.margins: 4
                                    spacing: 2
                                    Repeater {
                                        model: monthCell.eventCount > 3 ? 3 : monthCell.eventCount
                                        Rectangle { width: 6; height: 6; radius: 3; color: page.colors["accent"] }
                                    }
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: page.selectedDate = monthCell.cellDate
                                }
                            }
                        }
                    }
                    ListView {
                        objectName: "calendarMonthAgenda"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: page.dayEvents()
                        delegate: Label {
                            required property var modelData
                            text: modelData.start + " " + modelData.title
                            color: page.colors["text-primary"]
                        }
                    }
                }
            }
        }
    }
}
