pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Popup {
    id: panel
    required property Item historyViewport
    required property Item windowContent
    required property var colors
    required property string language

    property Item trigger: null
    property bool openedFromChip: false
    property string instantText: ""
    readonly property var instantDate: instantText.length ? new Date(instantText) : null
    readonly property bool validInstant: instantDate != null && !isNaN(instantDate.getTime())
    readonly property int selectedDay: panel.validInstant ? instantDate.getDate() : 0
    readonly property string monthCaption: panel.validInstant
        ? Qt.locale(panel.language).toString(instantDate, "MMMM yyyy") : ""
    // Leading 0 cells are blank grid slots before the first day.
    readonly property var calendarDays: {
        if (!panel.validInstant)
            return []
        const firstWeekday = Qt.locale(panel.language).firstDayOfWeek
        const monthStart = new Date(instantDate.getFullYear(), instantDate.getMonth(), 1)
        const daysInMonth = new Date(instantDate.getFullYear(), instantDate.getMonth() + 1, 0).getDate()
        // Week alignment comes from the month's first weekday, not the
        // selected day (review finding 4638145277).
        const leading = ((monthStart.getDay() + 6) % 7 - (firstWeekday - 1) + 7) % 7
        const cells = []
        for (let index = 0; index < leading; ++index)
            cells.push(0)
        for (let day = 1; day <= daysInMonth; ++day)
            cells.push(day)
        return cells
    }

    // localTime() of the reference date-time-picker.lib.ts:17-21: HH:mm, plus
    // seconds only when the stored instant has non-zero seconds/ms (:75).
    function clockText(instant) {
        const date = new Date(String(instant || ""))
        if (isNaN(date.getTime()))
            return ""
        const pad = part => String(part).padStart(2, "0")
        const parts = [pad(date.getHours()), pad(date.getMinutes())]
        if (date.getSeconds() !== 0 || date.getMilliseconds() !== 0)
            parts.push(pad(date.getSeconds()))
        return parts.join(":")
    }

    function showInstant(instant, anchor) {
        const value = String(instant || "")
        if (!value.length || isNaN(new Date(value).getTime()))
            return
        panel.instantText = value
        panel.openedFromChip = !!anchor
        panel.trigger = anchor || null
        panel.open()
    }

    // The history uses translated items. Reading each position makes this
    // binding follow row layout and the Flickable's moving content item.
    function offsetFrom(item, ancestor) {
        let x = 0, y = 0
        for (let node = item; node && node !== ancestor; node = node.parent) {
            x += node.x
            y += node.y
        }
        return Qt.point(x, y)
    }
    readonly property point triggerInHistory: trigger
        ? offsetFrom(trigger, panel.historyViewport) : Qt.point(0, 0)
    // A virtualized row can disappear before the deferred visibility check.
    // Timestamp links can open the same panel without a chip.
    readonly property bool triggerOffscreen: openedFromChip
        && (!trigger || triggerInHistory.y >= panel.historyViewport.height || triggerInHistory.y + trigger.height <= 0)
    onTriggerOffscreenChanged: {
        if (triggerOffscreen)
            Qt.callLater(function() {
                if (panel.visible && panel.triggerOffscreen) {
                    panel.trigger = null
                    panel.close()
                }
            })
    }

    // Mirrors computeFloatingPickerPosition (floating-panel-position.lib.ts:1-46):
    // 8 px viewport margin, 8 px anchor gap, centered, prefer above, clamp.
    // The panel binds x/y to this so every open recomputes from the trigger.
    readonly property point panelPosition: {
        // Touching the instant keeps the position reactive to each open.
        if (!instantText.length)
            return Qt.point(8, Math.max(8, panel.windowContent.height - panel.implicitHeight - 8))
        const margin = 8
        const gap = 8
        const panelWidth = panel.width
        const panelHeight = panel.height > 0 ? panel.height : panel.implicitHeight
        const anchor = panel.trigger
        if (anchor == null)
            return Qt.point(margin, Math.max(margin, panel.windowContent.height - panelHeight - margin))
        const listOrigin = offsetFrom(panel.historyViewport, panel.parent)
        const origin = Qt.point(listOrigin.x + triggerInHistory.x, listOrigin.y + triggerInHistory.y)
        const maxLeft = Math.max(margin, panel.windowContent.width - panelWidth - margin)
        const x = Math.min(Math.max(margin, origin.x + anchor.width / 2 - panelWidth / 2), maxLeft)
        const topAbove = origin.y - panelHeight - gap
        if (topAbove >= margin)
            return Qt.point(x, topAbove)
        return Qt.point(x, Math.min(Math.max(margin, origin.y + anchor.height + gap),
            Math.max(margin, panel.windowContent.height - panelHeight - margin)))
    }

    objectName: "messageTimePanel"
    // Bug #644: the popup item itself carries no accessible interface, so focus
    // parked on it left Linux assistive technology without a focused node while
    // the viewer was open. Accessible focus goes to the role=dialog node below
    // (declared by the content), and onClosed hands it back to the trigger.
    property Item accessibleDialog: null
    width: 371
    padding: 0
    modal: false
    dim: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onOpened: {
        if (accessibleDialog)
            accessibleDialog.forceActiveFocus(Qt.PopupFocusReason)
    }
    // Reference panel style: maxHeight calc(100dvh - 16px) (:67).
    height: Math.max(0, Math.min(implicitHeight, panel.windowContent.height - 16))
    x: panel.panelPosition.x
    y: panel.panelPosition.y
    background: Rectangle {
        radius: 12
        color: panel.colors["bg-elevated"]
        border.color: panel.colors["border-subtle"]
    }
    onClosed: {
        const anchor = panel.trigger
        panel.openedFromChip = false
        panel.trigger = null
        if (anchor != null)
            anchor.forceActiveFocus(Qt.OtherFocusReason)
    }
    contentItem: Flickable {
        objectName: "messageTimePanelScroll"
        // Review finding 4638145296: when the viewport cap (reference
        // maxHeight calc(100dvh - 16px), :67) shortens the panel, the
        // calendar/time content stays reachable through this viewport.
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        contentHeight: messageTimePanelContent.implicitHeight
        implicitHeight: messageTimePanelContent.implicitHeight
        implicitWidth: messageTimePanelContent.implicitWidth
        Column {
            id: messageTimePanelContent
            objectName: "messageTimePanelContent"
            x: 16
            width: parent.width - 32
            spacing: 0
            // role=dialog named by dateTimePicker.label (reference dialog contract).
            Accessible.role: Accessible.Dialog
            Accessible.name: qsTranslate("Main", "Date and time")
            // Bug #644: this node is the accessible focus target while the
            // viewer is open, so AT-SPI reports the named dialog as focused
            // instead of nothing. Bound to the open state so the hidden panel
            // never competes for focus with the composer.
            focus: panel.visible
            Accessible.focusable: true
            Component.onCompleted: panel.accessibleDialog = messageTimePanelContent
            Text {
                objectName: "messageTimePanelDate"
                topPadding: 14
                bottomPadding: 6
                text: panel.monthCaption
                textFormat: Text.PlainText
                font.pixelSize: 14
                font.weight: Font.Medium
                color: panel.colors["text-primary"]
                // Review finding 4638145299: a screen reader must read the
                // month caption, the highlighted day and the time value.
                Accessible.role: Accessible.StaticText
                Accessible.name: panel.monthCaption
            }
            Grid {
                objectName: "messageTimePanelWeekdays"
                columns: 7
                rowSpacing: 2
                Accessible.ignored: true
                Repeater {
                    model: 7
                    delegate: Text {
                        required property int index
                        width: 48
                        horizontalAlignment: Text.AlignHCenter
                        text: Qt.locale(panel.language).dayName(((index + Qt.locale(panel.language).firstDayOfWeek - 1) % 7) + 1, Locale.NarrowFormat)
                        textFormat: Text.PlainText
                        font.pixelSize: 12
                        color: panel.colors["text-muted"]
                        Accessible.ignored: true
                    }
                }
            }
            Grid {
                objectName: "messageTimePanelGrid"
                columns: 7
                rowSpacing: 2
                Accessible.ignored: true
                Repeater {
                    model: panel.calendarDays
                    delegate: Item {
                        id: messageTimeDayCell
                        required property var modelData
                        required property int index
                        width: 48
                        height: 30
                        Accessible.ignored: true
                        Rectangle {
                            anchors.centerIn: parent
                            width: 34
                            height: 26
                            radius: 6
                            visible: Number(messageTimeDayCell.modelData) === panel.selectedDay
                            color: panel.colors["accent"]
                        }
                        Text {
                            anchors.centerIn: parent
                            objectName: Number(messageTimeDayCell.modelData) === 0 ? ""
                                : Number(messageTimeDayCell.modelData) === panel.selectedDay ? "messageTimePanelSelectedDay"
                                : "messageTimePanelDay-" + Number(messageTimeDayCell.modelData)
                            visible: Number(messageTimeDayCell.modelData) !== 0
                            text: Number(messageTimeDayCell.modelData) === 0 ? "" : String(Number(messageTimeDayCell.modelData))
                            textFormat: Text.PlainText
                            font.pixelSize: 13
                            color: Number(messageTimeDayCell.modelData) === panel.selectedDay
                                ? panel.colors["bg-elevated"] : panel.colors["text-primary"]
                            // Only the highlighted day is announced (finding
                            // 4638145299); blank and unselected cells stay out
                            // of the accessibility tree.
                            Accessible.ignored: Number(messageTimeDayCell.modelData) !== panel.selectedDay
                            Accessible.role: Accessible.StaticText
                            Accessible.name: text
                        }
                    }
                }
            }
            Rectangle {
                width: parent.width
                height: 1
                color: panel.colors["border-subtle"]
                Accessible.ignored: true
            }
            Row {
                objectName: "messageTimePanelTimeRow"
                topPadding: 12
                bottomPadding: 16
                spacing: 10
                Text {
                    objectName: "messageTimePanelTimeLabel"
                    text: qsTranslate("Main", "Time")
                    textFormat: Text.PlainText
                    font.pixelSize: 13
                    color: panel.colors["text-muted"]
                    Accessible.role: Accessible.StaticText
                    Accessible.name: qsTranslate("Main", "Time")
                }
                Text {
                    objectName: "messageTimePanelTime"
                    text: panel.clockText(panel.instantText)
                    textFormat: Text.PlainText
                    font.pixelSize: 14
                    font.weight: Font.Medium
                    color: panel.colors["text-primary"]
                    // Review finding 4638145299: expose the stored time value.
                    Accessible.role: Accessible.StaticText
                    Accessible.name: panel.clockText(panel.instantText)
                }
            }
        }
    }
}
