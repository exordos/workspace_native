pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Issue #635: three-tab AI topic-summary settings dialog.
// Design oracle: workspace_ui e0194ff packages/web/src/features/topic-summary-settings/
// (shell + tabs topic-summary-settings-dialog.ui.tsx, sections *-section.ui.tsx,
// SwitchRow in topic-summary-settings-shared.ui.tsx); geometry per docs/topic-summary-settings.md.
Dialog {
    id: dlg
    objectName: "channelTopicSummarySettingsDialog"
    required property var controller
    required property var colors
    required property string topicId
    property string topicName: ""
    property string streamName: ""
    // Emitted when the topic tab carries prompt/reasoning edits; the owning panel
    // routes them to the controller summary-config seam.
    signal topicConfigRequested(string topic, bool enabled, string prompt, bool promptIsDefault, string effort)

    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 0
    x: parent ? Math.max(8, (parent.width - width) / 2) : 0
    y: parent ? Math.max(8, (parent.height - height) / 2) : 0

    // Tabs permitted for editing (reference: only allowed tabs render).
    function perm(tab) { return String(controller.topicSummaryTabPermission(tab)) }
    function allowedTabs() {
        const out = []
        if (perm("topic") === "allowed") out.push("topic")
        if (perm("gates") === "allowed") out.push("gates")
        if (perm("endpoints") === "allowed") out.push("endpoints")
        return out
    }
    property var tabs: []
    property string activeTab: tabs.length ? tabs[0] : "topic"

    // Header 88 px, tab strip 52 px, footer 80 px (reference h-[min(Xrem,92vh)] per tab).
    readonly property int tabHeightPx: activeTab === "gates" ? 600 : (activeTab === "endpoints" ? 760 : 680)
    width: parent ? Math.min(parent.width - 32, 1120) : 1120
    height: parent ? Math.min(tabHeightPx, Math.round(parent.height * 0.92)) : tabHeightPx
    background: Rectangle { radius: 12; color: dlg.colors["bg-elevated"]; border.color: dlg.colors["border-subtle"] }

    onOpened: {
        dlg.tabs = allowedTabs()
        dlg.activeTab = dlg.tabs.length ? dlg.tabs[0] : ""
        topicTab.reset()
        gatesTab.reset()
        endpointsTab.reset()
    }
    onActiveTabChanged: {
        if (activeTab === "gates") controller.loadTopicSummaryGates()
        else if (activeTab === "endpoints") controller.loadTopicSummaryEndpoints()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        // ---- Header (h 88 px, px-32 pt-24) ----
        ItemLayout { Layout.preferredHeight: 88; Layout.fillWidth: true
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 32; anchors.rightMargin: 16; anchors.topMargin: 24
                ColumnLayout {
                    spacing: 2
                    Text { objectName: "channelTopicSummarySettingsTitle"; text: qsTr("AI summary settings")
                        font.pixelSize: 20; font.weight: Font.DemiBold; color: dlg.colors["text-primary"] }
                    Text {
                        objectName: "channelTopicSummarySettingsSubtitle"
                        text: dlg.topicName ? (dlg.streamName ? "# " + dlg.topicName + " . " + dlg.streamName : "# " + dlg.topicName) : ""
                        visible: text.length > 0
                        font.pixelSize: 13; color: dlg.colors["text-muted"]
                    }
                }
                Item { Layout.fillWidth: true }
                ToolButton {
                    objectName: "channelTopicSummarySettingsClose"
                    Layout.preferredWidth: 32; Layout.preferredHeight: 32
                    icon.source: "qrc:/workspace/icons/close.svg"; icon.width: 16; icon.height: 16; icon.color: dlg.colors["text-muted"]
                    Accessible.name: qsTr("Close")
                    onClicked: dlg.close()
                }
            }
        }
        // ---- Tab strip (h 52 px, active 2 px accent underline) ----
        ItemLayout { Layout.preferredHeight: 52; Layout.fillWidth: true
            Row {
                id: tabRow
                anchors.left: parent.left; anchors.leftMargin: 20; height: parent.height
                spacing: 4
                Repeater {
                    model: dlg.tabs
                    delegate: TabButton {
                        id: tabBtn
                        required property string modelData
                        objectName: "channelTopicSummaryTab-" + modelData
                        text: modelData === "topic" ? qsTr("This topic")
                              : (modelData === "gates" ? qsTr("Common settings") : qsTr("LLM endpoints"))
                        height: 52; leftPadding: 16; rightPadding: 16
                        background: Item {
                            Rectangle { anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
                                height: 2; color: tabBtn.checked ? dlg.colors.accent : "transparent" }
                        }
                        contentItem: Text { text: tabBtn.text; font.pixelSize: 14
                            color: tabBtn.checked ? dlg.colors.accent : dlg.colors["text-muted"]
                            verticalAlignment: Text.AlignVCenter }
                        onClicked: dlg.activeTab = modelData
                        Keys.onPressed: (event) => {
                            const order = dlg.tabs
                            const at = order.indexOf(dlg.activeTab)
                            if (event.key === Qt.Key_Right) dlg.activeTab = order[(at + 1) % order.length]
                            else if (event.key === Qt.Key_Left) dlg.activeTab = order[(at + order.length - 1) % order.length]
                            else if (event.key === Qt.Key_Home) dlg.activeTab = order[0]
                            else if (event.key === Qt.Key_End) dlg.activeTab = order[order.length - 1]
                            else return
                            event.accepted = true
                        }
                    }
                }
            }
            Rectangle { anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
                height: 1; color: dlg.colors["border-subtle"] }
        }

        // ---- Body ----
        StackLayout {
            Layout.fillWidth: true; Layout.fillHeight: true
            currentIndex: Math.max(0, dlg.tabs.indexOf(dlg.activeTab))

            // ===== Tab 1: this topic =====
            Flickable {
                id: topicTab
                clip: true; contentWidth: width; boundsBehavior: Flickable.StopAtBounds
                property bool enabledDraft: false
                property string promptDraft: ""
                property bool promptIsDefault: true
                property string effortDraft: ""      // "" = provider default
                property bool saving: false
                property bool failed: false
                function reset() {
                    const t = controller.channelInfoTopics.find(row => row.id === dlg.topicId)
                    enabledDraft = !t || t.summaryEnabled !== false
                    promptDraft = t && t.summarySystemPrompt ? String(t.summarySystemPrompt) : ""
                    promptIsDefault = !t || !t.summarySystemPrompt
                    effortDraft = t && t.summaryReasoningEffort ? String(t.summaryReasoningEffort) : ""
                    saving = false; failed = false
                }
                readonly property bool topicEditable: Boolean(dlg.controller.channelInfoStream.canEditTopicSummary) && !saving
                readonly property bool dirty: enabledDraft !== (!topicFind().summaryEnabled)
                    || promptDraft !== (String(topicFind().summarySystemPrompt || ""))
                    || (promptDraft.length === 0) !== promptIsDefault
                    || effortDraft !== String(topicFind().summaryReasoningEffort || "")
                function topicFind() { return controller.channelInfoTopics.find(row => row.id === dlg.topicId) || {} }
                contentHeight: topicCol.implicitHeight
                ScrollBar.vertical: ScrollBar {}
                ColumnLayout {
                    id: topicCol; width: topicTab.width - 64; x: 32; spacing: 16
                    Item { Layout.preferredHeight: 20 }
                    Text { objectName: "channelTopicSummaryDeniedTopic"
                        visible: dlg.perm("topic") !== "allowed"
                        text: qsTr("You do not have permission to change these settings."); color: dlg.colors.danger; font.pixelSize: 13 }
                    SwitchRow {
                        objectNameRow: "channelTopicSummaryEnabled"; label: qsTr("Automatic summary updates")
                        checkedFlag: topicTab.enabledDraft; enabledFlag: topicTab.topicEditable
                        onFlipped: checked => topicTab.enabledDraft = checked
                    }
                    Text { text: qsTr("System prompt"); font.pixelSize: 13; color: dlg.colors["text-primary"] }
                    Rectangle {
                        Layout.fillWidth: true; Layout.preferredHeight: Math.max(144, promptArea.implicitHeight + 16)
                        radius: 8; color: dlg.colors["text-field-bg"]; border.color: dlg.colors["border-subtle"]
                        TextArea {
                            id: promptArea; objectName: "channelTopicSummaryPrompt"
                            anchors.fill: parent; anchors.margins: 8; wrapMode: TextArea.Wrap
                            placeholderText: qsTr("Leave empty to use the default prompt.")
                            font.pixelSize: 14; color: dlg.colors["text-primary"]
                            enabled: topicTab.topicEditable
                            text: topicTab.promptDraft
                            onTextChanged: if (text !== topicTab.promptDraft) topicTab.promptDraft = text
                        }
                    }
                    RowLayout {
                        Button {
                            objectName: "channelTopicSummaryUseDefaultPrompt"
                            text: qsTr("Use default prompt"); Layout.minimumWidth: 112
                            enabled: topicTab.topicEditable
                            onClicked: { topicTab.promptDraft = ""; topicTab.promptIsDefault = true }
                        }
                        Item { Layout.fillWidth: true }
                    }
                    Text { visible: topicTab.promptDraft.trim().length === 0 && topicTab.promptDraft.length > 0
                        text: qsTr("The prompt must not be empty."); color: dlg.colors.danger; font.pixelSize: 12 }
                    Text { visible: topicTab.promptDraft.length > 16384
                        text: qsTr("The prompt is too long (maximum %1 characters).").arg(16384); color: dlg.colors.danger; font.pixelSize: 12 }
                    RowLayout {
                        spacing: 8
                        Text { text: qsTr("Reasoning effort"); font.pixelSize: 13; color: dlg.colors["text-primary"] }
                        ComboBox {
                            id: effortBox; objectName: "channelTopicSummaryReasoning"; Layout.preferredWidth: 180
                            enabled: topicTab.topicEditable
                            model: [qsTr("Provider default"), "off", "minimal", "low", "medium", "high"]
                            property var values: ["", "off", "minimal", "low", "medium", "high"]
                            currentIndex: Math.max(0, values.indexOf(topicTab.effortDraft))
                            onActivated: topicTab.effortDraft = values[index]
                        }
                        Item { Layout.fillWidth: true }
                    }
                    Text { visible: topicTab.failed; objectName: "channelTopicSummaryTopicError"
                        text: qsTr("Could not update the topic. Please try again."); color: dlg.colors.danger; font.pixelSize: 13 }
                    Item { Layout.preferredHeight: 16 }
                }
            }

            // ===== Tab 2: common gates =====
            Flickable {
                id: gatesTab
                clip: true; contentWidth: width; boundsBehavior: Flickable.StopAtBounds
                property bool globalDraft: false
                property bool projectDraft: false
                // Set by user flips; clears on every seed so a late GET only
                // overwrites drafts nobody touched yet.
                property bool userEdited: false
                function reset() {
                    const g = controller.topicSummaryGates
                    globalDraft = Boolean(g.globalEnabled); projectDraft = Boolean(g.projectEnabled)
                    userEdited = false
                }
                readonly property bool gatesAllowed: dlg.perm("gates") === "allowed"
                readonly property bool dirty: globalDraft !== Boolean(controller.topicSummaryGates.globalEnabled)
                    || projectDraft !== Boolean(controller.topicSummaryGates.projectEnabled)
                contentHeight: gatesCol.implicitHeight
                ScrollBar.vertical: ScrollBar {}
                ColumnLayout {
                    id: gatesCol; width: gatesTab.width - 64; x: 32; spacing: 16
                    Item { Layout.preferredHeight: 20 }
                    Rectangle {
                        objectName: "channelTopicSummaryGatesBanner"
                        Layout.fillWidth: true; radius: 8; Layout.preferredHeight: bannerText.implicitHeight + 20
                        color: "transparent"
                        border.color: (gatesTab.globalDraft && gatesTab.projectDraft) ? dlg.colors["call-green"] : dlg.colors["border-subtle"]
                        Text { id: bannerText; anchors.fill: parent; anchors.margins: 10; wrapMode: Text.Wrap
                            text: (gatesTab.globalDraft && gatesTab.projectDraft) ? qsTr("Generation is active") : qsTr("Generation is paused")
                            color: (gatesTab.globalDraft && gatesTab.projectDraft) ? dlg.colors["call-green"] : dlg.colors["text-muted"]
                            font.pixelSize: 13 }
                    }
                    SwitchRow {
                        objectNameRow: "channelTopicSummaryGatesGlobal"; label: qsTr("Global summary generation")
                        checkedFlag: gatesTab.globalDraft; enabledFlag: gatesTab.gatesAllowed && !controller.topicSummarySettingsBusy
                        onFlipped: checked => { gatesTab.userEdited = true; gatesTab.globalDraft = checked }
                    }
                    SwitchRow {
                        objectNameRow: "channelTopicSummaryGatesProject"; label: qsTr("Enable summaries in this workspace")
                        checkedFlag: gatesTab.projectDraft; enabledFlag: gatesTab.gatesAllowed && !controller.topicSummarySettingsBusy
                        onFlipped: checked => { gatesTab.userEdited = true; gatesTab.projectDraft = checked }
                    }
                    ErrorNotice { name: "channelTopicSummaryGatesError" }
                    Item { Layout.preferredHeight: 16 }
                }
            }

            // ===== Tab 3: LLM endpoints =====
            Flickable {
                id: endpointsTab
                clip: true; contentWidth: width; boundsBehavior: Flickable.StopAtBounds
                property string editingUuid: ""      // "" = not editing; "new" = create form
                property var form: ({})
                property bool confirmDelete: false
                function reset() { editingUuid = ""; form = ({}); confirmDelete = false }
                readonly property bool endpointsAllowed: dlg.perm("endpoints") === "allowed"
                readonly property bool editing: editingUuid.length > 0
                readonly property bool dirty: editing
                function field(name, fallback) {
                    const f = form && Object.keys(form).length ? form : ({})
                    return f[name] !== undefined ? f[name] : fallback
                }
                function setField(name, value) {
                    const next = Object.assign({}, form)
                    next[name] = value
                    form = next
                }
                function startCreate() {
                    editingUuid = "new"
                    form = {name: "", base_url: "", model: "", api_key: "", priority: 100, temperature: 0.2,
                            max_output_tokens: 512, top_p: 1, presence_penalty: 0, frequency_penalty: 0,
                            enabled: true, supports_vision: false, supports_reasoning: false}
                }
                function startEdit(row) {
                    editingUuid = String(row.uuid)
                    form = {name: String(row.name || ""), base_url: String(row.base_url || ""),
                            model: String(row.model || ""), api_key: "", priority: Number(row.priority ?? 100),
                            temperature: Number(row.temperature ?? 0.2), max_output_tokens: Number(row.max_output_tokens ?? 512),
                            top_p: Number(row.top_p ?? 1), presence_penalty: Number(row.presence_penalty ?? 0),
                            frequency_penalty: Number(row.frequency_penalty ?? 0),
                            enabled: Boolean(row.enabled), supports_vision: Boolean(row.supports_vision),
                            supports_reasoning: Boolean(row.supports_reasoning)}
                }
                contentHeight: endpointsCol.implicitHeight
                ScrollBar.vertical: ScrollBar {}
                ColumnLayout {
                    id: endpointsCol; width: endpointsTab.width - 64; x: 32; spacing: 12
                    Item { Layout.preferredHeight: 16 }
                    Button {
                        objectName: "channelTopicSummaryEndpointAdd"
                        text: qsTr("Add endpoint"); Layout.alignment: Qt.AlignLeft; Layout.minimumWidth: 144
                        visible: endpointsTab.endpointsAllowed && !endpointsTab.editing
                        onClicked: endpointsTab.startCreate()
                    }
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 6
                        Repeater {
                            model: dlg.controller.topicSummaryEndpoints
                            delegate: RowLayout {
                                id: epRow
                                required property var modelData
                                required property int index
                                objectName: "channelTopicSummaryEndpoint-" + index
                                Layout.fillWidth: true; spacing: 8
                                Text { objectName: "channelTopicSummaryEndpointName"
                                    text: String(modelData.name || ""); font.pixelSize: 14; color: dlg.colors["text-primary"]
                                    Layout.maximumWidth: implicitWidth; Layout.fillWidth: true; elide: Text.ElideRight }
                                Text { text: String(modelData.model || ""); font.pixelSize: 12; color: dlg.colors["text-muted"] }
                                Rectangle {   // credential + enabled markers
                                    width: 10; height: 10; radius: 5
                                    color: modelData.enabled ? dlg.colors["call-green"] : dlg.colors["border-subtle"]
                                }
                                Button { objectName: "channelTopicSummaryEndpointEdit"
                                    text: qsTr("Edit"); enabled: endpointsTab.endpointsAllowed && !endpointsTab.editing
                                    onClicked: endpointsTab.startEdit(epRow.modelData) }
                                Button { objectName: "channelTopicSummaryEndpointDelete"
                                    text: qsTr("Delete"); enabled: endpointsTab.endpointsAllowed && !endpointsTab.editing
                                    onClicked: { endpointsTab.confirmDelete = true; endpointsTab.editingUuid = "confirm:" + modelData.uuid } }
                            }
                        }
                    }
                    Rectangle {
                        visible: endpointsTab.confirmDelete
                        objectName: "channelTopicSummaryDeleteConfirm"
                        Layout.fillWidth: true; radius: 8; padding: 12
                        color: "transparent"; border.color: dlg.colors.danger
                        RowLayout { spacing: 10
                            Text { Layout.fillWidth: true; wrapMode: Text.Wrap
                                text: qsTr("Delete this endpoint? Existing summaries are not affected.")
                                color: dlg.colors["text-primary"]; font.pixelSize: 13 }
                            Button { text: qsTr("Cancel"); onClicked: { endpointsTab.confirmDelete = false; endpointsTab.editingUuid = "" } }
                            Button { objectName: "channelTopicSummaryDeleteConfirmYes"; text: qsTr("Delete")
                                onClicked: {
                                    const uuid = endpointsTab.editingUuid.startsWith("confirm:") ? endpointsTab.editingUuid.slice(8) : ""
                                    endpointsTab.confirmDelete = false; endpointsTab.editingUuid = ""
                                    if (uuid) dlg.controller.deleteTopicSummaryEndpoint(uuid)
                                } }
                        }
                    }
                    // Create/edit form (reference form fields order).
                    GridLayout {
                        visible: endpointsTab.editing && !endpointsTab.confirmDelete
                        objectName: "channelTopicSummaryEndpointForm"
                        columns: 2; columnSpacing: 12; rowSpacing: 8; Layout.fillWidth: true
                        TextField { objectName: "channelTopicSummaryField-name"; placeholderText: qsTr("Name *")
                            text: String(endpointsTab.field("name", "")); onTextChanged: endpointsTab.setField("name", text) }
                        TextField { objectName: "channelTopicSummaryField-model"; placeholderText: qsTr("Model *")
                            text: String(endpointsTab.field("model", "")); onTextChanged: endpointsTab.setField("model", text) }
                        TextField { objectName: "channelTopicSummaryField-base_url"; placeholderText: qsTr("Base URL (https://...) *")
                            Layout.columnSpan: 2; Layout.fillWidth: true
                            text: String(endpointsTab.field("base_url", "")); onTextChanged: endpointsTab.setField("base_url", text.trim()) }
                        TextField { objectName: "channelTopicSummaryField-api_key"; placeholderText: qsTr("API key *")
                            echoMode: TextInput.Password; visible: endpointsTab.editingUuid === "new"
                            text: String(endpointsTab.field("api_key", "")); onTextChanged: endpointsTab.setField("api_key", text) }
                        TextField { objectName: "channelTopicSummaryField-priority"; placeholderText: qsTr("Priority (0-1000000)")
                            text: String(endpointsTab.field("priority", 100)); onTextChanged: endpointsTab.setField("priority", text) }
                        TextField { objectName: "channelTopicSummaryField-temperature"; placeholderText: qsTr("Temperature (0-2)")
                            text: String(endpointsTab.field("temperature", 0.2)); onTextChanged: endpointsTab.setField("temperature", text) }
                        TextField { objectName: "channelTopicSummaryField-max_output_tokens"; placeholderText: qsTr("Max output tokens (1-32768)")
                            text: String(endpointsTab.field("max_output_tokens", 512)); onTextChanged: endpointsTab.setField("max_output_tokens", text) }
                        TextField { objectName: "channelTopicSummaryField-top_p"; placeholderText: qsTr("Top-p (0-1)")
                            text: String(endpointsTab.field("top_p", 1)); onTextChanged: endpointsTab.setField("top_p", text) }
                        TextField { objectName: "channelTopicSummaryField-presence_penalty"; placeholderText: qsTr("Presence penalty (-2..2)")
                            text: String(endpointsTab.field("presence_penalty", 0)); onTextChanged: endpointsTab.setField("presence_penalty", text) }
                        TextField { objectName: "channelTopicSummaryField-frequency_penalty"; placeholderText: qsTr("Frequency penalty (-2..2)")
                            text: String(endpointsTab.field("frequency_penalty", 0)); onTextChanged: endpointsTab.setField("frequency_penalty", text) }
                        SwitchRow { objectNameRow: "channelTopicSummaryField-enabled"; label: qsTr("Enabled")
                            Layout.columnSpan: 2
                            checkedFlag: Boolean(endpointsTab.field("enabled", true)); enabledFlag: true
                            onFlipped: checked => endpointsTab.setField("enabled", checked) }
                        SwitchRow { objectNameRow: "channelTopicSummaryField-supports_vision"; label: qsTr("Supports vision")
                            Layout.columnSpan: 2
                            checkedFlag: Boolean(endpointsTab.field("supports_vision", false)); enabledFlag: true
                            onFlipped: checked => endpointsTab.setField("supports_vision", checked) }
                        SwitchRow { objectNameRow: "channelTopicSummaryField-supports_reasoning"; label: qsTr("Supports reasoning")
                            Layout.columnSpan: 2
                            checkedFlag: Boolean(endpointsTab.field("supports_reasoning", false)); enabledFlag: true
                            onFlipped: checked => endpointsTab.setField("supports_reasoning", checked) }
                    }
                    ErrorNotice { name: "channelTopicSummaryEndpointsError" }
                    Item { Layout.preferredHeight: 16 }
                }
            }
        }

        // ---- Footer (h 80 px): status dot + Cancel/Save ----
        ItemLayout { Layout.preferredHeight: 80; Layout.fillWidth: true
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 32; anchors.rightMargin: 32
                Rectangle { width: 10; height: 10; radius: 5
                    visible: statusText.text.length > 0
                    color: dlg.footerStatus === "unsaved" ? dlg.colors["indicator-yellow"] : dlg.colors["call-green"] }
                Text {
                    id: statusText
                    text: dlg.footerStatus === "unsaved" ? qsTr("Unsaved changes")
                        : (dlg.footerStatus === "saved" ? qsTr("Saved") : "")
                    font.pixelSize: 13; color: dlg.colors["text-muted"]
                }
                Item { Layout.fillWidth: true }
                Button { objectName: "channelTopicSummaryCancel"; text: qsTr("Cancel"); Layout.minimumWidth: 112
                    onClicked: dlg.close() }
                Button {
                    objectName: "channelTopicSummarySave"; text: qsTr("Save"); Layout.minimumWidth: 144
                    visible: dlg.activeTab === "topic" ? Boolean(controller.channelInfoStream.canEditTopicSummary) : true
                    enabled: dlg.activeTab === "topic"
                        ? (topicTab.dirty && topicTab.topicEditable)
                        : (dlg.activeTab === "gates"
                            ? (gatesTab.dirty && gatesTab.gatesAllowed && !controller.topicSummarySettingsBusy)
                            : (endpointsTab.editing && endpointsTab.endpointsAllowed && !controller.topicSummarySettingsBusy))
                    onClicked: {
                        if (dlg.activeTab === "topic") {
                            const t = topicTab.topicFind()
                            const enabledOnly = topicTab.promptDraft === String(t.summarySystemPrompt || "")
                                && topicTab.effortDraft === String(t.summaryReasoningEffort || "")
                            topicTab.saving = true; topicTab.failed = false
                            if (enabledOnly) controller.applyChannelTopicAction(dlg.topicId, "summary", String(topicTab.enabledDraft))
                            else dlg.topicConfigRequested(dlg.topicId, topicTab.enabledDraft, topicTab.promptDraft,
                                topicTab.promptDraft.length === 0, topicTab.effortDraft)
                        } else if (dlg.activeTab === "gates") {
                            controller.saveTopicSummaryGates(gatesTab.globalDraft, gatesTab.projectDraft)
                        } else {
                            const uuid = endpointsTab.editingUuid
                            const errors = controller.topicSummaryEndpointValidationErrors(endpointsTab.form, uuid === "new")
                            if (errors.length === 0) {
                                if (uuid === "new") controller.createTopicSummaryEndpoint(endpointsTab.form)
                                else controller.updateTopicSummaryEndpoint(uuid, endpointsTab.form)
                                endpointsTab.reset()
                            }
                        }
                    }
                }
            }
        }
    }

    // Reference SwitchRow (topic-summary-settings-shared.ui.tsx): track 36x20,
    // knob 16x16 at 2 px inset, translate 16 px when on.
    component SwitchRow: RowLayout {
        id: swRow
        property string objectNameRow: ""
        property string label: ""
        property bool checkedFlag: false
        property bool enabledFlag: true
        signal flipped(bool on)
        spacing: 10
        Switch {
            id: sw
            objectName: swRow.objectNameRow
            checked: swRow.checkedFlag
            enabled: swRow.enabledFlag
            padding: 0; spacing: 0
            contentItem: Item { width: 0; height: 20 }
            Accessible.name: swRow.label
            Accessible.checkable: true
            Accessible.checked: sw.checked
            onToggled: if (checked !== swRow.checkedFlag) swRow.flipped(checked)
            indicator: Rectangle {
                implicitWidth: 36; implicitHeight: 20; radius: 10
                color: sw.checked ? swRow.swAccent : swRow.swBorder
                opacity: sw.enabled ? 1.0 : 0.6
                Behavior on color { ColorAnimation { duration: 120 } }
                Rectangle {
                    width: 16; height: 16; radius: 8
                    x: sw.checked ? 18 : 2; y: 2
                    color: "white"
                    Behavior on x { NumberAnimation { duration: 120 } }
                }
            }
        }
        Text { text: swRow.label; font.pixelSize: 14; color: dlg.colors["text-primary"] }
        readonly property color swAccent: dlg.colors.accent
        readonly property color swBorder: dlg.colors["border-subtle"]
    }

    // Error notice band shared by the gates/endpoints tabs (reference: border danger/30, bg danger/5).
    component ErrorNotice: Rectangle {
        id: errBand
        property string name: ""
        objectName: name
        Layout.fillWidth: true
        visible: dlg.controller.topicSummarySettingsStatus === "error"
            && dlg.controller.topicSummarySettingsError !== "forbidden"
        radius: 8
        Layout.preferredHeight: visible ? errText.implicitHeight + 20 : 0
        color: Qt.alpha(dlg.colors.danger, 0.05)
        border.color: Qt.alpha(dlg.colors.danger, 0.3)
        Text {
            id: errText
            anchors.fill: parent; anchors.margins: 10
            wrapMode: Text.Wrap; font.pixelSize: 13; color: dlg.colors.danger
            text: dlg.controller.topicSummarySettingsError.length ? dlg.errorStatusText() : ""
        }
    }

    Connections {
        target: dlg.controller
        function onTopicSummaryStateChanged() {
            const s = controller.topicSummarySettingsStatus
            if (s === "saved") { gatesTab.reset(); endpointsTab.reset() }
            else if (s === "error" && controller.topicSummarySettingsError === "forbidden") { dlg.tabs = allowedTabs() }
            // A seeded draft predates the on-demand GET: when the gates DTO lands
            // (map populated, no op in flight, no error) and nobody flipped a
            // switch yet, adopt the server values instead of showing the stale
            // pre-load seed forever.
            else if (!controller.topicSummarySettingsBusy && s === ""
                     && !gatesTab.userEdited && Object.keys(controller.topicSummaryGates).length)
                gatesTab.reset()
        }
        function onChannelTopicActionFinished(id, action, ok) {
            if (!dlg.visible || !topicTab.saving || id !== dlg.topicId || action !== "summary") return
            topicTab.saving = false
            topicTab.failed = !ok
            if (ok) dlg.close()
        }
    }

    // Unsaved marker: while a tab carries a dirty draft the footer shows the yellow dot;
    // controller statuses ("saved"/"error") win over the local marker.
    property string footerStatus: {
        const ctrlStatus = controller.topicSummarySettingsStatus
        if (ctrlStatus === "saved" || ctrlStatus === "error") return ctrlStatus
        const anyDirty = topicTab.dirty || gatesTab.dirty || endpointsTab.dirty
        return anyDirty ? "unsaved" : ""
    }
    // Maps the reference status.forbidden/network/invalid/contract/server kinds to visible text.
    function errorStatusText() {
        const kind = controller.topicSummarySettingsError
        if (kind === "forbidden") return qsTr("You do not have permission to change these settings.")
        if (kind === "network") return qsTr("Network error. Your changes were kept, try again.")
        if (kind === "invalid") return qsTr("The server rejected the values.")
        if (kind === "contract") return qsTr("The server returned an unexpected response.")
        if (kind === "server") return qsTr("The server failed. Please try again.")
        return qsTr("Something went wrong. Please try again.")
    }
}
