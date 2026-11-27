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
    property string uiLocale: "en"

    // Localized full date for keyboard-focus announced cell names (#925).
    // Pattern-per-language follows the Main.qml date-label precedent;
    // QLocale is not exposed as a QML type, so enum formats are not
    // reachable from QML.
    function localizedCellDate(d) {
        const pattern = uiLocale === "ru" ? "dddd, d MMMM yyyy" : "dddd, MMMM d, yyyy"
        return Qt.locale(uiLocale).toString(d, pattern)
    }

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
            // #952 round 2 (finding 4722274186): the window spans the DISPLAYED
            // matrix - a keyboard rove past the matrix edge re-anchors the month,
            // and the newly shown cells must not render without their events.
            // Duplicate requests from a selection-driven re-anchor (which fires
            // both onSelectedDateChanged and onGridAnchorChanged) are dropped by
            // the controller-side in-flight / same-loaded-window guards in
            // requestCalendarWindow, exactly as other refresh seams already.
            const gridStart = weekStart(monthGrid.gridAnchor);
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
        GridLayout {
            id: header
            objectName: "calendarHeader"
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            columns: page.width - Layout.leftMargin - Layout.rightMargin >= tabRow.implicitWidth + dateNavigation.implicitWidth + columnSpacing ? 2 : 1
            columnSpacing: 8
            rowSpacing: 0

            RowLayout {
                id: tabRow
                Layout.fillWidth: true
                Layout.preferredHeight: 48
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
            }
            RowLayout {
                id: dateNavigation
                Layout.preferredHeight: 48
                Layout.alignment: Qt.AlignRight
                spacing: 8
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
                        // #952 round 2 (finding 4722274191): same contract as the
                        // mini-calendar title - the header names the month the
                        // month grid displays, which a keyboard edge re-anchor
                        // moves independently of the selection.
                        return Qt.formatDate(monthGrid.gridAnchor, "MMMM yyyy");
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
                    // #952 round 2 (finding 4722274191): the heading names the
                    // month the cells SHOW - a rove past the matrix edge
                    // re-anchors the grid without moving the selection.
                    text: Qt.formatDate(miniGrid.gridAnchor, "MMMM yyyy")
                }
                // Stable positional model (issue pitfall): delegates read
                // live state through `page`, never through modelData identity.
                Grid {
                    id: miniGrid
                    objectName: "calendarMini"
                    columns: 7
                    spacing: 2
                    // #925: the grid is ONE tab stop. Roving keyboard focus is
                    // kept as ONE date (never 42 tab stops), arrow keys roam
                    // it, and Enter/Space runs the same selection action the
                    // pointer uses. The roving state is a date rather than a
                    // matrix index (exo-review #952 findings 4722010291 /
                    // 4722010314): selecting re-anchors focus to the selected
                    // cell, and an arrow move that leaves the 42-cell matrix
                    // re-anchors the displayed month instead of clamping.
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Grid
                    // Equivalent accessible mechanism for the roving focus
                    // (finding 4722010298): while the grid holds keyboard
                    // focus it announces the active cell's date as its name;
                    // Qt derives the focused state from the item's own
                    // activeFocus automatically.
                    Accessible.name: miniGrid.activeFocus
                        ? page.localizedCellDate(miniGrid.rovedDate) : ""
                    property var focusDate: null
                    readonly property var rovedDate:
                        miniGrid.focusDate !== null ? miniGrid.focusDate : page.selectedDate
                    readonly property date gridAnchor: {
                        page.selectedDate; // re-evaluate on selection
                        const roved = miniGrid.rovedDate;
                        const first = page.monthStart(page.selectedDate);
                        const offset = (first.getDay() + 6) % 7;
                        const idx = page.dayDiff(first, roved) + offset;
                        if (idx < 0 || idx >= 42)
                            return page.monthStart(roved);
                        return first;
                    }
                    readonly property int selectedGridIndex: {
                        miniGrid.gridAnchor; // re-evaluate when the grid re-anchors
                        const offset = (miniGrid.gridAnchor.getDay() + 6) % 7;
                        return page.dayDiff(miniGrid.gridAnchor, page.selectedDate) + offset;
                    }
                    readonly property int effectiveFocusIndex: {
                        miniGrid.gridAnchor; // re-evaluate when the grid re-anchors
                        const offset = (miniGrid.gridAnchor.getDay() + 6) % 7;
                        return page.dayDiff(miniGrid.gridAnchor, miniGrid.rovedDate) + offset;
                    }
                    readonly property int focusIndex:
                        miniGrid.focusDate === null ? -1 : miniGrid.effectiveFocusIndex
                    function moveFocus(delta)
                    {
                        miniGrid.focusDate = page.shiftDate(miniGrid.rovedDate, delta);
                    }
                    function selectFocused()
                    {
                        page.selectedDate = miniGrid.rovedDate;
                        miniGrid.focusDate = null; // re-anchor onto the selected cell
                    }
                    onActiveFocusChanged: if (!miniGrid.activeFocus)
                        miniGrid.focusDate = null
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
                                miniGrid.gridAnchor; // re-evaluate when the grid re-anchors
                                const first = miniGrid.gridAnchor;
                                const offset = (first.getDay() + 6) % 7;
                                return page.shiftDate(first, modelData - offset);
                            }
                            readonly property bool inMonth:
                                cellDate.getMonth() === miniGrid.gridAnchor.getMonth()
                            readonly property bool isToday: page.calDate(cellDate) === page.calDate(new Date())
                            readonly property bool isSelected: page.calDate(cellDate) === page.calDate(page.selectedDate)
                            readonly property bool keyboardFocused: miniGrid.activeFocus
                                && miniGrid.effectiveFocusIndex === modelData
                            Accessible.role: Accessible.Button
                            Accessible.name: page.localizedCellDate(miniCell.cellDate)
                            Accessible.selected: miniCell.isSelected
                            Accessible.onPressAction: page.selectedDate = miniCell.cellDate
                            Rectangle {
                                objectName: "miniSelectionFill"
                                anchors.fill: parent
                                radius: 6
                                color: miniCell.isSelected ? page.colors["accent"] : "transparent"
                            }
                            // The ring paints AFTER the selection fill so it
                            // stays visible over it, with a contrasting border
                            // while the focused cell is also the selected one
                            // (exo-review #952 finding 4722010307).
                            Rectangle {
                                objectName: "miniFocusRing"
                                anchors.fill: parent
                                radius: 6
                                color: "transparent"
                                border.width: 2
                                border.color: miniCell.isSelected
                                    ? page.colors["on-accent"] : page.colors["accent"]
                                visible: miniCell.keyboardFocused
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
                Layout.minimumWidth: 0
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
                Item {
                    id: monthView
                    // At narrower widths the agenda needs its own row.
                    readonly property bool stacked: width < 7 * 64 + 6 + 12 + 200
                    Grid {
                        id: monthGrid
                        objectName: "calendarMonthGrid"
                        anchors.left: parent.left
                        anchors.top: monthView.stacked ? parent.top : undefined
                        anchors.verticalCenter: monthView.stacked ? undefined : parent.verticalCenter
                        columns: 7
                        spacing: 1
                        // #925: same roving-focus keyboard contract as the
                        // mini-calendar (one tab stop, arrows roam, Enter
                        // selects), including the date-based roving state
                        // with selection re-anchor and month-edge re-anchor.
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Grid
                        Accessible.name: monthGrid.activeFocus
                            ? page.localizedCellDate(monthGrid.rovedDate) : ""
                        property var focusDate: null
                        readonly property var rovedDate:
                            monthGrid.focusDate !== null ? monthGrid.focusDate : page.selectedDate
                        readonly property date gridAnchor: {
                            page.selectedDate; // re-evaluate on selection
                            const roved = monthGrid.rovedDate;
                            const first = page.monthStart(page.selectedDate);
                            const offset = (first.getDay() + 6) % 7;
                            const idx = page.dayDiff(first, roved) + offset;
                            if (idx < 0 || idx >= 42)
                                return page.monthStart(roved);
                            return first;
                        }
                        readonly property int selectedGridIndex: {
                            monthGrid.gridAnchor; // re-evaluate when the grid re-anchors
                            const offset = (monthGrid.gridAnchor.getDay() + 6) % 7;
                            return page.dayDiff(monthGrid.gridAnchor, page.selectedDate) + offset;
                        }
                        readonly property int effectiveFocusIndex: {
                            monthGrid.gridAnchor; // re-evaluate when the grid re-anchors
                            const offset = (monthGrid.gridAnchor.getDay() + 6) % 7;
                            return page.dayDiff(monthGrid.gridAnchor, monthGrid.rovedDate) + offset;
                        }
                        readonly property int focusIndex:
                            monthGrid.focusDate === null ? -1 : monthGrid.effectiveFocusIndex
                        function moveFocus(delta)
                        {
                            monthGrid.focusDate = page.shiftDate(monthGrid.rovedDate, delta);
                        }
                        function selectFocused()
                        {
                            page.selectedDate = monthGrid.rovedDate;
                            monthGrid.focusDate = null; // re-anchor onto the selected cell
                        }
                        // #952 round 2 (finding 4722274186): keep the fetched
                        // event window synchronized with the displayed matrix
                        // when an edge rove re-anchors the month grid.
                        onGridAnchorChanged: if (page.viewMode === "month")
                            page.refreshWindow()
                        onActiveFocusChanged: if (!monthGrid.activeFocus)
                            monthGrid.focusDate = null
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
                                width: Math.max(0, Math.min(64, Math.floor((monthView.width - 6 * monthGrid.spacing) / monthGrid.columns)))
                                height: monthView.stacked ? Math.min(48, width) : 64
                                readonly property var cellDate: {
                                    monthGrid.gridAnchor; // re-evaluate when the grid re-anchors
                                    const first = monthGrid.gridAnchor;
                                    const offset = (first.getDay() + 6) % 7;
                                    return page.shiftDate(first, modelData - offset);
                                }
                                readonly property int eventCount: page.visibleEvents(
                                    page.calDate(cellDate), page.calDate(cellDate)).length
                                readonly property bool isSelected: page.calDate(cellDate) === page.calDate(page.selectedDate)
                                readonly property bool keyboardFocused: monthGrid.activeFocus
                                    && monthGrid.effectiveFocusIndex === modelData
                                Accessible.role: Accessible.Button
                                Accessible.name: page.localizedCellDate(monthCell.cellDate)
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
                        anchors.left: monthView.stacked ? parent.left : monthGrid.right
                        anchors.leftMargin: monthView.stacked ? 0 : 12
                        anchors.top: monthView.stacked ? monthGrid.bottom : parent.top
                        anchors.topMargin: monthView.stacked ? 8 : 0
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
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
