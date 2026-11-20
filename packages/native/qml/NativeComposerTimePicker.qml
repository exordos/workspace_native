import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: panel
    required property var colors
    required property string localeName
    property date openedAt: new Date()
    property date selectedDay: new Date()
    property date month: new Date(selectedDay.getFullYear(), selectedDay.getMonth(), 1)
    readonly property date instant: {
        const local = new Date(selectedDay.getFullYear(), selectedDay.getMonth(), selectedDay.getDate(),
                               hours.value, minutes.value, seconds.value, 0);
        // A repeated wall-clock hour has two instants. Preserve the offset of
        // the selected day when both candidates have the requested local time.
        const delta = (selectedDay.getTimezoneOffset() - local.getTimezoneOffset()) * 60000;
        const candidate = new Date(local.getTime() + delta);
        return candidate.getFullYear() === local.getFullYear() && candidate.getMonth() === local.getMonth()
            && candidate.getDate() === local.getDate() && candidate.getHours() === hours.value
            && candidate.getMinutes() === minutes.value && candidate.getSeconds() === seconds.value
            ? candidate : local;
    }
    readonly property var cells: {
        const first = new Date(month.getFullYear(), month.getMonth(), 1);
        const count = new Date(month.getFullYear(), month.getMonth() + 1, 0).getDate();
        const offset = (first.getDay() - Qt.locale(localeName).firstDayOfWeek + 7) % 7;
        return Array.from({length: offset + count}, (_, i) => i < offset ? 0 : i - offset + 1);
    }
    signal inserted(string markdown)
    function reset() {
        const now = new Date();
        openedAt = now;
        const next = new Date(now.getTime() + 60000);
        selectedDay = next; month = new Date(next.getFullYear(), next.getMonth(), 1);
        hours.value = next.getHours(); minutes.value = next.getMinutes(); seconds.value = next.getSeconds();
    }
    width: Math.min(371, parent.width - 16)
    height: Math.min(396, parent.height - 16)
    padding: 12; focus: true; modal: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle { radius: 8; color: panel.colors["bg-elevated"]; border.color: panel.colors["border-subtle"] }
    contentItem: ScrollView {
        clip: true; contentWidth: availableWidth; ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        Accessible.role: Accessible.Dialog; Accessible.name: qsTr("Insert date and time")
        ColumnLayout {
            width: parent.width; spacing: 6
            RowLayout {
                Layout.fillWidth: true
                ToolButton { text: "‹"; Accessible.name: qsTr("Previous month"); onClicked: panel.month = new Date(panel.month.getFullYear(), panel.month.getMonth() - 1, 1) }
                Label { Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter; color: panel.colors["text-primary"]; text: Qt.locale(panel.localeName).toString(panel.month, "MMMM yyyy") }
                ToolButton { text: "›"; Accessible.name: qsTr("Next month"); onClicked: panel.month = new Date(panel.month.getFullYear(), panel.month.getMonth() + 1, 1) }
            }
            GridLayout {
                Layout.fillWidth: true; columns: 7; rowSpacing: 0; columnSpacing: 1
                Repeater {
                    model: 7
                    delegate: Label {
                        required property int index
                        Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter
                        color: panel.colors["text-muted"]
                        text: Qt.locale(panel.localeName).dayName((Qt.locale(panel.localeName).firstDayOfWeek + index) % 7, Locale.ShortFormat)
                    }
                }
                Repeater {
                    model: panel.cells
                    delegate: ToolButton {
                        required property int modelData
                        Layout.fillWidth: true; implicitHeight: 30
                        text: modelData ? String(modelData) : ""
                        enabled: modelData > 0
                        Accessible.ignored: !enabled
                        checked: panel.selectedDay.getFullYear() === panel.month.getFullYear()
                            && panel.selectedDay.getMonth() === panel.month.getMonth() && panel.selectedDay.getDate() === modelData
                        onClicked: panel.selectedDay = new Date(panel.month.getFullYear(), panel.month.getMonth(), modelData)
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                SpinBox { id: hours; objectName: "composerTimeHours"; Accessible.name: qsTr("Hours"); Layout.fillWidth: true; from: 0; to: 23; editable: true }
                Label { text: ":"; color: panel.colors["text-primary"] }
                SpinBox { id: minutes; objectName: "composerTimeMinutes"; Accessible.name: qsTr("Minutes"); Layout.fillWidth: true; from: 0; to: 59; editable: true }
                Label { text: ":"; color: panel.colors["text-primary"] }
                SpinBox { id: seconds; objectName: "composerTimeSeconds"; Accessible.name: qsTr("Seconds"); Layout.fillWidth: true; from: 0; to: 59; editable: true }
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                Button { text: qsTr("Cancel"); onClicked: panel.close() }
                Button {
                    objectName: "composerTimeInsert"; text: qsTr("Insert")
                    enabled: panel.instant.getTime() > panel.openedAt.getTime()
                    onClicked: panel.inserted("urn:time:" + panel.instant.toISOString().replace(/\.000Z$/, "Z"))
                }
            }
        }
    }
}
