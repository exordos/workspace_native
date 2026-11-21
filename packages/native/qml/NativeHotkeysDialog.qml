pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog
    required property var registry
    required property var colors
    property bool helpOnly: false
    property string editor: ""
    property string candidate: ""
    property string errorCode: ""
    readonly property var categories: ["navigation", "interface", "composer", "fixed"]
    anchors.centerIn: parent
    width: Math.min(768, parent ? parent.width - 32 : 768)
    height: Math.min(helpOnly ? 524 : 828, parent ? parent.height - 40 : 828)
    modal: true
    focus: true
    padding: 20
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    title: qsTr("Keyboard shortcuts")
    background: Rectangle { color: dialog.colors["bg-elevated"]; radius: 14; border.width: 1; border.color: dialog.colors["border-subtle"] }
    function label(id) {
        switch (id) {
        case "navigation": return qsTr("Navigation")
        case "interface": return qsTr("Interface")
        case "composer": return qsTr("Composer")
        case "invalid": return qsTr("Invalid shortcut")
        case "reserved": return qsTr("This shortcut is reserved")
        case "conflict": return qsTr("This shortcut is already in use")
        case "storageError": return qsTr("Could not save settings")
        case "fixed": return qsTr("Fixed shortcuts")
        case "navigation.back": return qsTr("Back")
        case "navigation.forward": return qsTr("Forward")
        case "navigation.messenger": return qsTr("Messenger")
        case "navigation.calendar": return qsTr("Calendar")
        case "navigation.mail": return qsTr("Mail")
        case "navigation.calls": return qsTr("Calls")
        case "navigation.activity": return qsTr("Activity")
        case "navigation.chatSearch": return qsTr("Search chats")
        case "navigation.folderList": return qsTr("Folder list")
        case "interface.theme": return qsTr("Change theme")
        case "interface.help": return qsTr("Help")
        case "interface.sidebar": return qsTr("Toggle sidebar")
        case "interface.infoPanel": return qsTr("Chat information")
        case "interface.quoteMessage": return qsTr("Quote message")
        case "composer.editLastMessage": return qsTr("Edit last message")
        case "composer.bold": return qsTr("Bold")
        case "composer.italic": return qsTr("Italic")
        case "composer.strike": return qsTr("Strikethrough")
        case "composer.code": return qsTr("Code")
        case "fixed.cancel": return qsTr("Close or cancel")
        case "fixed.focus": return qsTr("Move focus (Shift+Tab moves backward)")
        case "fixed.send": return qsTr("Send or insert a line, depending on send settings")
        case "fixed.editing": return qsTr("Standard editing shortcuts, such as copy")
        case "fixed.focusZones": return qsTr("Move between focus areas")
        default: return ""
        }
    }
    function display(binding) {
        if (!binding) return qsTr("Disabled");
        let text = binding.replace(/^Mod\+/, Qt.platform.os === "osx" ? "⌘+" : "Ctrl+").replace(/^Control\+/, "Ctrl+");
        for (const [key, label] of Object.entries({ArrowUp:"↑", ArrowDown:"↓", ArrowLeft:"←", ArrowRight:"→",
            Escape:"Esc", Tab:"⇥", Enter:"↵"}))
            text = text.replace(key, label);
        return text;
    }
    function rows(section) {
        const src = section === "fixed" ? registry.fixed : registry.actions;
        const query = search.text.trim().toLocaleLowerCase();
        return src.filter(row => (section === "fixed" || row.category === section)
                          && (!helpOnly || row.binding.length)
                          && (!query || label(row.id).toLocaleLowerCase().includes(query)
                              || (row.binding && display(row.binding).toLocaleLowerCase().includes(query))));
    }
    function finishEdit() {
        registry.recording = false;
        const previous = editor;
        editor = "";
        candidate = "";
        errorCode = "";
        if (previous) Qt.callLater(() => {
            for (let i = 0; i < sectionList.count; ++i) {
                const section = sectionList.itemAt(i);
                for (let j = 0; section && j < section.rowsList.count; ++j) {
                    const row = section.rowsList.itemAt(j);
                    if (row && row.modelData.id === previous) { row.forceActiveFocus(); return; }
                }
            }
        });
    }
    function edit(id) {
        editor = id;
        candidate = registry.binding(id);
        errorCode = "";
        registry.recording = true;
        recordField.forceActiveFocus();
    }
    function save(result) {
        if (result) errorCode = result;
        else finishEdit();
    }
    onOpened: if (!helpOnly) search.forceActiveFocus()
    onClosed: { finishEdit(); search.clear(); }
    Connections {
        target: dialog.registry
        function onRecorded(binding) {
            if (!dialog.visible || !dialog.editor) return;
            dialog.candidate = binding;
            dialog.errorCode = dialog.registry.validate(dialog.editor, binding);
        }
        function onRecordingCancelled() { if (dialog.visible && dialog.editor) dialog.finishEdit(); }
    }
    header: RowLayout {
        spacing: 12
        Label { Layout.leftMargin: 20; Layout.topMargin: 16; Layout.fillWidth: true; text: qsTr("Keyboard shortcuts"); font.pixelSize: 20; font.bold: true; color: dialog.colors["text-primary"] }
        ToolButton { Layout.rightMargin: 12; Layout.topMargin: 12; text: "×"; Accessible.name: qsTr("Close"); onClicked: dialog.close() }
    }
    contentItem: ColumnLayout {
        spacing: 10
        Label {
            visible: !dialog.helpOnly
            text: qsTr("Settings are stored on this device. Composer shortcuts apply while editing a message.")
            color: dialog.colors["text-muted"]; wrapMode: Text.Wrap; Layout.fillWidth: true
        }
        RowLayout {
            visible: !dialog.helpOnly
            CheckBox {
                objectName: "shortcutsEnabled"
                text: qsTr("Enable keyboard shortcuts")
                checked: dialog.registry.enabled
                onClicked: dialog.errorCode = dialog.registry.setEnabled(checked)
            }
            Item { Layout.fillWidth: true }
            NativeButton { objectName: "shortcutsResetAll"; colors: dialog.colors; text: qsTr("Reset all"); onClicked: dialog.save(dialog.registry.resetAll()) }
        }
        NativeField {
            id: search
            objectName: "shortcutsSearch"
            visible: !dialog.helpOnly
            Layout.fillWidth: true
            colors: dialog.colors
            placeholderText: qsTr("Search shortcuts")
            Accessible.name: placeholderText
        }
        ScrollView {
            id: scroll
            Layout.fillHeight: true; Layout.fillWidth: true
            clip: true
            contentWidth: availableWidth
            Column {
                width: scroll.availableWidth; spacing: 12
                Repeater {
                    id: sectionList
                    model: dialog.categories
                    delegate: Column {
                        id: section
                        required property string modelData
                        property alias rowsList: rowsList
                        readonly property var filtered: dialog.rows(modelData)
                        visible: filtered.length > 0
                        width: scroll.availableWidth; spacing: 6
                        Label { text: dialog.label(section.modelData); color: dialog.colors["text-primary"]; font.bold: true; font.pixelSize: 15 }
                        GridLayout {
                            width: section.width
                            columns: width >= 580 ? 2 : 1; columnSpacing: 14; rowSpacing: 4
                            Repeater {
                                id: rowsList
                                model: section.filtered
                                delegate: ItemDelegate {
                                    id: row
                                    required property var modelData
                                    objectName: "shortcut-" + modelData.id
                                    Layout.fillWidth: true
                                    Layout.preferredWidth: section.width >= 580 ? (section.width - 16) / 2 : section.width
                                    implicitHeight: hint.visible ? 50 : 34
                                    activeFocusOnTab: !dialog.helpOnly && section.modelData !== "fixed"
                                    Accessible.name: dialog.label(modelData.id) + " " + dialog.display(modelData.binding)
                                    onClicked: if (!dialog.helpOnly && section.modelData !== "fixed") dialog.edit(modelData.id)
                                    background: Rectangle {
                                        radius: 6
                                        color: row.hovered || row.visualFocus ? dialog.colors["card-bg-active"] : dialog.colors["card-bg"]
                                    }
                                    contentItem: RowLayout {
                                        spacing: 8
                                        ColumnLayout {
                                            Layout.fillWidth: true; spacing: 0
                                            Label { Layout.fillWidth: true; elide: Text.ElideRight; text: dialog.label(row.modelData.id); color: dialog.colors["text-primary"] }
                                            Label {
                                                id: hint
                                                Layout.fillWidth: true
                                                visible: !dialog.helpOnly && text.length > 0
                                                text: row.modelData.id === "interface.quoteMessage" ? qsTr("Select text, focus a message with Tab, or point to it.")
                                                    : row.modelData.id === "composer.editLastMessage" ? qsTr("When the composer is empty and not editing a message") : ""
                                                elide: Text.ElideRight; font.pixelSize: 11; color: dialog.colors["text-muted"]
                                            }
                                        }
                                        Label { text: dialog.display(row.modelData.binding); color: dialog.colors["text-muted"]; font.pixelSize: 12 }
                                    }
                                }
                            }
                        }
                    }
                }
                Label {
                    visible: dialog.categories.every(key => dialog.rows(key).length === 0)
                    objectName: "shortcutsEmpty"; text: qsTr("No shortcuts found"); color: dialog.colors["text-muted"]
                }
            }
        }
        ColumnLayout {
            visible: dialog.editor.length > 0; Layout.fillWidth: true
            Label { text: dialog.label(dialog.editor); font.bold: true; color: dialog.colors["text-primary"] }
            Label { text: qsTr("Press a key combination. Escape cancels."); color: dialog.colors["text-muted"] }
            NativeField {
                id: recordField; objectName: "shortcutsRecord"
                Layout.fillWidth: true; colors: dialog.colors; readOnly: true
                text: dialog.candidate ? dialog.display(dialog.candidate) : ""
                placeholderText: qsTr("Press a shortcut"); Accessible.name: placeholderText
                onActiveFocusChanged: if (dialog.editor) dialog.registry.recording = activeFocus
            }
            RowLayout {
                NativeButton { objectName: "shortcutSave"; colors: dialog.colors; primary: true; text: qsTr("Save")
                    enabled: !!dialog.candidate && !dialog.registry.validate(dialog.editor,dialog.candidate)
                    onClicked: dialog.save(dialog.registry.assign(dialog.editor,dialog.candidate)) }
                NativeButton { objectName: "shortcutDisable"; colors: dialog.colors; text: qsTr("Disable"); onClicked: dialog.save(dialog.registry.assign(dialog.editor,"")) }
                NativeButton { objectName: "shortcutReset"; colors: dialog.colors; text: qsTr("Reset"); onClicked: dialog.save(dialog.registry.reset(dialog.editor)) }
                NativeButton { colors: dialog.colors; text: qsTr("Cancel"); onClicked: dialog.finishEdit() }
            }
        }
        Label { objectName: "shortcutsError"; visible: dialog.errorCode.length > 0; text: dialog.label(dialog.errorCode); Accessible.role: Accessible.AlertMessage; color: dialog.colors["notice-base"] }
    }
}
