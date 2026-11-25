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
    // routes them to the controller summary-config seam. The trailing flags are
    // the per-field edit mask of issue #739: only an edited field may ride the
    // request, so a peer admin's newer write to an untouched field survives.
    signal topicConfigRequested(string topic, bool enabled, string prompt, bool promptIsDefault, string effort,
        bool editEnabled, bool editPrompt, bool editEffort)
    // Which section dispatched the in-flight op; only ITS OWN success may reset
    // that draft - a shared "saved" reset discarded unsaved edits in the other
    // section (review finding 5). Save buttons gate on !busy, so one op is in flight.
    property string pendingOp: ""

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
        // The gear that opens this dialog always belongs to a topic's summary
        // row, so the topic section is the promised landing tab; fall back to
        // the first permitted tab only when this user may not see "topic".
        const landing = dlg.tabs.indexOf("topic") >= 0 ? "topic"
            : (dlg.tabs.length ? dlg.tabs[0] : "")
        topicTab.reset()
        gatesTab.reset()
        endpointsTab.reset()
        // #738: a reopen keeps the same landing tab, so the change signal
        // below cannot be the only load trigger - an error state would strand
        // the section until restart. Load explicitly when no change will
        // fire; when the tab does change, onActiveTabChanged owns the single
        // request for it (no duplicate GET on the open path).
        if (landing == dlg.activeTab)
            dlg.loadSection(landing)
        dlg.activeTab = landing
    }
    onActiveTabChanged: dlg.loadSection(activeTab)
    function loadSection(tab) {
        if (tab === "gates") controller.loadTopicSummaryGates()
        else if (tab === "endpoints") controller.loadTopicSummaryEndpoints()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        // ---- Header (h 88 px, px-32 pt-24) ----
        Item { Layout.preferredHeight: 88; Layout.fillWidth: true
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
        Item { Layout.preferredHeight: 52; Layout.fillWidth: true
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
            currentIndex: dlg.activeTab === "gates" ? 1 : (dlg.activeTab === "endpoints" ? 2 : 0)
            // FIXED mapping (review finding 1): the three children below are static;
            // indexing by the permission-FILTERED tabs list shifted every section
            // when an earlier tab was not permitted (a gates-only admin saw the
            // disabled topic section).

            // ===== Tab 1: this topic =====
            Flickable {
                id: topicTab
                clip: true; contentWidth: width; boundsBehavior: Flickable.StopAtBounds
                property bool enabledDraft: false
                property string promptDraft: ""
                property bool promptIsDefault: true
                property string effortDraft: ""      // "" = provider default
                // Issue #739: the per-field baseline (the base leg of the three-way
                // rebase of docs/topic-summary-settings.md). Drafts are seeded on
                // open; every incoming topic-row update rebases them so an untouched
                // field follows the server instead of looking dirty, and the save
                // request carries ONLY the fields the current user actually edited
                // (a peer admin's newer change to another field survives).
                property bool enabledBase: false
                property string promptBase: ""
                property bool promptIsDefaultBase: true
                property string effortBase: ""
                property bool saving: false
                property bool failed: false
                function reset() {
                    const t = controller.channelInfoTopics.find(row => row.id === dlg.topicId)
                    enabledDraft = !t || t.summaryEnabled !== false
                    promptDraft = t && t.summarySystemPrompt ? String(t.summarySystemPrompt) : ""
                    promptIsDefault = !t || !t.summarySystemPrompt
                    effortDraft = t && t.summaryReasoningEffort ? String(t.summaryReasoningEffort) : ""
                    enabledBase = enabledDraft; promptBase = promptDraft
                    promptIsDefaultBase = promptIsDefault; effortBase = effortDraft
                    saving = false; failed = false
                }
                function rebaseOntoRow() {
                    const t = topicFind()
                    const rowEnabled = !t || t.summaryEnabled !== false
                    const rowPrompt = t && t.summarySystemPrompt ? String(t.summarySystemPrompt) : ""
                    const rowDefault = !t || !t.summarySystemPrompt
                    const rowEffort = t && t.summaryReasoningEffort ? String(t.summaryReasoningEffort) : ""
                    if (enabledDraft === enabledBase) enabledDraft = rowEnabled
                    if (promptDraft === promptBase) promptDraft = rowPrompt
                    if (effortDraft === effortBase) effortDraft = rowEffort
                    promptIsDefault = promptDraft.length === 0 || rowDefault
                    enabledBase = rowEnabled; promptBase = rowPrompt
                    promptIsDefaultBase = rowDefault; effortBase = rowEffort
                }
                readonly property bool topicEditable: Boolean(dlg.controller.channelInfoStream.canEditTopicSummary) && !saving
                readonly property bool dirty: enabledDraft !== enabledBase
                    || promptDraft !== promptBase
                    || (promptDraft.length === 0) !== promptIsDefaultBase
                    || effortDraft !== effortBase
                function topicFind() { return controller.channelInfoTopics.find(row => row.id === dlg.topicId) || {} }
                Connections {
                    target: controller
                    // Remote writes (realtime topic.updated, catalog refresh) land
                    // through this NOTIFY: rebase the local draft, never fetch.
                    function onChannelInfoTopicsChanged() { if (dlg.visible) topicTab.rebaseOntoRow() }
                }
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
                            onActivated: index => topicTab.effortDraft = values[index]
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
                        checkedFlag: gatesTab.globalDraft; enabledFlag: gatesTab.gatesAllowed && !controller.topicSummarySettingsBusy && controller.topicSummaryGatesLoaded
                        onFlipped: checked => { gatesTab.userEdited = true; gatesTab.globalDraft = checked }
                    }
                    SwitchRow {
                        objectNameRow: "channelTopicSummaryGatesProject"; label: qsTr("Enable summaries in this workspace")
                        checkedFlag: gatesTab.projectDraft; enabledFlag: gatesTab.gatesAllowed && !controller.topicSummarySettingsBusy && controller.topicSummaryGatesLoaded
                        onFlipped: checked => { gatesTab.userEdited = true; gatesTab.projectDraft = checked }
                    }
                    ErrorNotice { name: "channelTopicSummaryGatesError"; producer: "gates" }
                    Item { Layout.preferredHeight: 16 }
                }
            }

            // ===== Tab 3: LLM endpoints =====
            Flickable {
                id: endpointsTab
                objectName: "channelTopicSummaryEndpointsScroll"
                clip: true; contentWidth: width; boundsBehavior: Flickable.StopAtBounds
                property string editingUuid: ""      // "" = not editing; "new" = create form
                property var form: ({})
                property var editBaseline: ({})      // DTO snapshot taken at startEdit
                property bool confirmDelete: false
                // The text fields flush their value through a Qt.callLater defer. If the
                // dialog is torn down while a defer is still queued (window fixtures die
                // mid-event-loop), a scope-dependent closure resolves `text`/`setField`
                // against the dead Flickable and QML logs a warning, which fails tests.
                // The guard below is a plain JS object captured by value: after
                // destruction only that survives, so the deferred closure never touches
                // the QML scope chain.
                property var liveGuard: ({ alive: true })
                Component.onDestruction: liveGuard.alive = false
                // The revision captured at dispatch time: a "saved" reply may only
                // reset THIS draft, never a newer one typed while the request flew.
                // formRevision ticks on every draft mutation (QML var identity across
                // property reads is not stable, so a counter is the safe comparison).
                property int formRevision: 0
                // Revision at which the CURRENT draft session started (set by
                // reset/startCreate/startEdit). #795 round 2 (finding
                // 4687482010): merely opening an unchanged edit or a delete
                // confirmation must not read as unsaved - only mutations after
                // this mark are real draft changes.
                property int draftStartRevision: 0
                property int dispatchRevision: -1
                property string dispatchUuid: ""
                // Dispatch identity (review finding 1): a late "saved" may only touch
                // the draft its OWN request dispatched. The revision counter alone
                // cannot tell "edited while pending" from "dialog closed and reopened,
                // a different draft started" - the session counter ticks only at
                // draft-replacement seams (reset/startCreate/startEdit), so a stale
                // echo recognises its draft by session, never by revision alone.
                property int draftSession: 0
                property int dispatchSession: -1
                function reset() { editingUuid = ""; form = ({}); editBaseline = ({}); confirmDelete = false; formRevision += 1
                    draftStartRevision = formRevision
                    draftSession += 1; dispatchSession = -1 }
                // The values that the in-flight request submits, and the endpoint
                // uuids visible when a CREATE was dispatched: a late "saved" that
                // finds newer edits must rebase the draft onto THESE (review
                // findings 1/2), never onto the pre-save baseline or a fresh create.
                property var dispatchValues: ({})
                property var dispatchSeenUuids: []
                function rebaseAfterSave() {
                    if (editingUuid === "new") {
                        // The create succeeded: the preserved draft must become an
                        // UPDATE for the created row, never a second create.
                        let created = ""
                        for (const row of (controller.topicSummaryEndpoints || [])) {
                            if (!dispatchSeenUuids.includes(String(row.uuid))) { created = String(row.uuid); break }
                        }
                        if (created.length) editingUuid = created
                    }
                    editBaseline = JSON.parse(JSON.stringify(dispatchValues))
                    // The successful request consumed the credential it submitted
                    // (review finding 2): a preserved draft that keeps the submitted
                    // key resends it on the next save, because a nonblank key always
                    // rides the update payload. Drop it from the baseline and -
                    // unless the user replaced it while the request was in flight,
                    // which makes the newer key a pending edit that must survive -
                    // from the form too.
                    const submittedKey = String(dispatchValues["api" + "_key"] || "")
                    if (submittedKey.length) {
                        editBaseline["api" + "_key"] = ""
                        if (String(form["api" + "_key"] || "") === submittedKey)
                            setField("api" + "_key", "")
                    }
                    dispatchRevision = -1
                    dispatchUuid = ""
                    dispatchSession = -1
                }
                readonly property bool endpointsAllowed: dlg.perm("endpoints") === "allowed"
                readonly property bool editing: editingUuid.length > 0
                // Opening an edit/delete confirmation is NOT a draft change:
                // dirty counts only setField mutations after the session's start
                // mark (finding 4687482010). #837: the counter alone also ticks
                // when a mutation is REVERTED, so an edit draft that matches its
                // baseline again must not read as unsaved - the footer follows
                // the same content divergence the Save gate uses. The create
                // form keeps the revision semantics: it has no saved baseline to
                // diverge from and its Save never disables on revert.
                readonly property bool dirty: editing && formRevision !== draftStartRevision
                    && (editingUuid === "new" || hasChanges)
                // Mutable numeric fields with the reference bounds
                // (topic-summary-endpoints.lib.ts validateInteger/validateNumber ranges).
                readonly property var numberRules: ({
                    priority: {min: 0, max: 1000000, integer: true},
                    max_output_tokens: {min: 1, max: 32768, integer: true},
                    temperature: {min: 0, max: 2, integer: false},
                    top_p: {min: 0, max: 1, integer: false},
                    presence_penalty: {min: -2, max: 2, integer: false},
                    frequency_penalty: {min: -2, max: 2, integer: false}
                })
                // Reference isValidBaseUrl: http(s) only, host required, no userinfo,
                // no query, no fragment, <=2048 after trimming trailing slashes.
                function isEndpointUrl(value) {
                    const t = String(value).trim().replace(/\/+$/, "")
                    if (t.length === 0 || t.length > 2048) return false
                    const parts = /^(https?):\/\/([^/]*)([\s\S]*)$/i.exec(t)
                    if (!parts) return false
                    if (parts[2].length === 0 || parts[2].indexOf("@") !== -1) return false
                    return parts[3].indexOf("?") === -1 && parts[3].indexOf("#") === -1
                }
                // Live per-field validation (reference endpointValidationErrorText):
                // every error is shown beside its field and Save stays gated while any
                // field is invalid (review finding 7). The controller re-validates on
                // dispatch as the authoritative second gate.
                readonly property var formErrors: validateDraft(editingUuid === "new")
                function validateDraft(isCreate) {
                    const msgs = ({})
                    const src = form && Object.keys(form).length ? form : ({})
                    const reqText = qsTr("This field is required.")
                    const text = function (name) { return String(src[name] !== undefined ? src[name] : "").trim() }
                    if (isCreate || src.name !== undefined) {
                        if (text("name").length === 0) msgs.name = reqText
                        else if (text("name").length > 255) msgs.name = qsTr("The value is too long.")
                    }
                    if (isCreate || src.model !== undefined) {
                        if (text("model").length === 0) msgs.model = reqText
                        else if (text("model").length > 255) msgs.model = qsTr("The value is too long.")
                    }
                    if (isCreate || src.base_url !== undefined) {
                        if (text("base_url").length === 0) msgs.base_url = reqText
                        else if (!isEndpointUrl(String(src.base_url))) msgs.base_url = qsTr("Check the entered values.")
                    }
                    if (isCreate) {
                        const key = String(src.api_key || "")
                        if (key.length === 0) msgs.api_key = reqText
                        else if (key.length > 8192) msgs.api_key = qsTr("The value is too long.")
                    } else if (String(src.api_key || "").length > 8192) {
                        msgs.api_key = qsTr("The value is too long.")
                    }
                    for (const field in numberRules) {
                        if (src[field] === undefined) continue
                        if (!isCreate && String(src[field]) === String(editBaseline[field])) continue
                        const rule = numberRules[field]
                        let value = src[field]
                        if (typeof value !== "number") {
                            const raw = String(value).trim()
                            if (raw.length === 0 || !isFinite(Number(raw))) {
                                msgs[field] = rule.integer ? qsTr("Enter a whole number.") : qsTr("Enter a valid value.")
                                continue
                            }
                            value = Number(raw)
                        }
                        if (rule.integer && value !== Math.floor(value))
                            msgs[field] = qsTr("Enter a whole number.")
                        else if (value < rule.min || value > rule.max)
                            msgs[field] = qsTr("The value is outside the allowed range.")
                    }
                    return msgs
                }
                readonly property bool hasChanges: {
                    if (editingUuid === "new") return form && Object.keys(form).length > 0
                    const src = form || ({})
                    // exordy findings 4692802566 + 4693183527: while THIS
                    // endpoint's own save is in flight the pending-diff oracle is
                    // the DISPATCHED values - what the held request is about to
                    // persist - not the pre-save baseline. A revert typed during
                    // flight diverges from the stale value the server will store,
                    // so it must stay marked unsaved. The guard is tied to the
                    // endpoint's own dispatch (pendingOp scope + session + the
                    // retired identity): a FAILED save retires the identity in
                    // the state-changed handler below, so a later unrelated busy
                    // op never reopens the comparison onto the failed payload,
                    // and after close/reopen the baseline comparison resumes.
                    const inFlight = controller.topicSummarySettingsBusy
                        && dlg.pendingOp === "endpoint"
                        && dispatchRevision !== -1 && dispatchSession === draftSession
                    const cmp = inFlight ? dispatchValues : editBaseline
                    for (const key in src) {
                        if (key === "api_key") { if (String(src.api_key || "").length > 0) return true; continue }
                        if (String(src[key]) !== String(cmp[key])) return true
                    }
                    return false
                }
                readonly property bool canDispatch: editing && hasChanges
                    && Object.keys(formErrors).length === 0 && !confirmDelete
                // An update carries ONLY fields changed from the edit baseline - reference
                // topicSummaryEndpointUpdateBody(): "absent means unchanged", so an
                // untouched base_url can never overwrite a concurrent administrator's
                // change (review finding 9); a blank api_key means "keep the credential".
                // Edited numeric text is coerced to a number - the controller validation
                // accepts numeric QVariants only (review finding 3).
                function buildDispatchPayload(isCreate) {
                    const out = ({})
                    const src = form || ({})
                    for (const key in src) {
                        if (key === "api_key") {
                            const value = String(src.api_key || "")
                            if (isCreate || value.length > 0) out[key] = value
                            continue
                        }
                        if (!isCreate && String(src[key]) === String(editBaseline[key])) continue
                        if (numberRules[key] === undefined) { out[key] = src[key]; continue }
                        const value = src[key]
                        out[key] = (typeof value === "number") ? value : Number(String(value).trim())
                    }
                    return out
                }
                // The draft is kept until its OWN request succeeds (review finding 4):
                // on failure the values stay for retry; endpointsTab.reset() runs from
                // the pendingOp-scoped "saved" handler below.
                function commit() {
                    if (!canDispatch) return
                    const isCreate = editingUuid === "new"
                    const payload = buildDispatchPayload(isCreate)
                    dispatchRevision = formRevision
                    dispatchUuid = editingUuid
                    dispatchSession = draftSession
                    dispatchValues = Object.assign({}, editBaseline, payload)
                    if (isCreate) {
                        const seen = []
                        for (const row of (controller.topicSummaryEndpoints || [])) seen.push(String(row.uuid))
                        dispatchSeenUuids = seen
                    }
                    dlg.pendingOp = "endpoint"
                    if (isCreate) controller.createTopicSummaryEndpoint(payload)
                    else controller.updateTopicSummaryEndpoint(editingUuid, payload)
                }
                function field(name, fallback) {
                    const f = form && Object.keys(form).length ? form : ({})
                    return f[name] !== undefined ? f[name] : fallback
                }
                function setField(name, value) {
                    // Identity guard: every text field writes back through here,
                    // and re-assigning the whole map re-notifies every binding.
                    // Without this guard the form re-reduces onto itself and Qt
                    // reports a binding loop the moment the dialog loads.
                    if (form && String(form[name]) === String(value)) return
                    const next = Object.assign({}, form)
                    next[name] = value
                    form = next
                    formRevision += 1
                }
                function deferSetField(name, value) {
                    // Capture everything the deferred write needs as plain JS values so
                    // the closure survives the form being destroyed before it fires.
                    const guard = liveGuard
                    const tab = endpointsTab
                    Qt.callLater(function () { if (guard.alive) tab.setField(name, value) })
                }
                function startCreate() {
                    editingUuid = "new"
                    editBaseline = ({})
                    form = {name: "", base_url: "", model: "", api_key: "", priority: 100, temperature: 0.2,
                            max_output_tokens: 512, top_p: 1, presence_penalty: 0, frequency_penalty: 0,
                            enabled: true, supports_vision: false, supports_reasoning: false}
                    formRevision += 1
                    draftStartRevision = formRevision
                    draftSession += 1
                }
                function startEdit(row) {
                    editingUuid = String(row.uuid)
                    const seeded = {name: String(row.name || ""), base_url: String(row.base_url || ""),
                            model: String(row.model || ""), api_key: "", priority: Number(row.priority ?? 100),
                            temperature: Number(row.temperature ?? 0.2), max_output_tokens: Number(row.max_output_tokens ?? 512),
                            top_p: Number(row.top_p ?? 1), presence_penalty: Number(row.presence_penalty ?? 0),
                            frequency_penalty: Number(row.frequency_penalty ?? 0),
                            enabled: Boolean(row.enabled), supports_vision: Boolean(row.supports_vision),
                            supports_reasoning: Boolean(row.supports_reasoning)}
                    form = seeded
                    editBaseline = JSON.parse(JSON.stringify(seeded))
                    formRevision += 1
                    draftStartRevision = formRevision
                    draftSession += 1
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
                                    implicitWidth: 10; implicitHeight: 10; radius: 5
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
                        Layout.fillWidth: true; radius: 8; implicitHeight: confirmRow.implicitHeight + 24
                        color: "transparent"; border.color: dlg.colors.danger
                        RowLayout { id: confirmRow; anchors.fill: parent; anchors.margins: 12; spacing: 10
                            Text { Layout.fillWidth: true; wrapMode: Text.Wrap
                                text: qsTr("Delete this endpoint? Existing summaries are not affected.")
                                color: dlg.colors["text-primary"]; font.pixelSize: 13 }
                            Button { text: qsTr("Cancel"); onClicked: { endpointsTab.confirmDelete = false; endpointsTab.editingUuid = "" } }
                            Button { objectName: "channelTopicSummaryDeleteConfirmYes"; text: qsTr("Delete")
                                onClicked: {
                                    const uuid = endpointsTab.editingUuid.startsWith("confirm:") ? endpointsTab.editingUuid.slice(8) : ""
                                    endpointsTab.confirmDelete = false; endpointsTab.editingUuid = ""
                                    if (uuid) { endpointsTab.dispatchRevision = endpointsTab.formRevision
                                        endpointsTab.dispatchUuid = endpointsTab.editingUuid
                                        dlg.pendingOp = "endpoint"; dlg.controller.deleteTopicSummaryEndpoint(uuid) }
                                } }
                        }
                    }
                    // Create/edit form (reference form fields order). Every field sits
                    // beside its live validation message (reference error= rendering).
                    GridLayout {
                        visible: endpointsTab.editing && !endpointsTab.confirmDelete
                        objectName: "channelTopicSummaryEndpointForm"
                        columns: 2; columnSpacing: 12; rowSpacing: 8; Layout.fillWidth: true
                        ColumnLayout {
                            Layout.fillWidth: true; spacing: 2
                            TextField { objectName: "channelTopicSummaryField-name"; placeholderText: qsTr("Name *")
                                Layout.fillWidth: true
                                text: String(endpointsTab.field("name", "")); onTextChanged: endpointsTab.deferSetField("name", text) }
                            FieldError { errorField: "name" }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true; spacing: 2
                            TextField { objectName: "channelTopicSummaryField-model"; placeholderText: qsTr("Model *")
                                Layout.fillWidth: true
                                text: String(endpointsTab.field("model", "")); onTextChanged: endpointsTab.deferSetField("model", text) }
                            FieldError { errorField: "model" }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true; Layout.columnSpan: 2; spacing: 2
                            TextField { objectName: "channelTopicSummaryField-base_url"; placeholderText: qsTr("Base URL (https://...) *")
                                Layout.fillWidth: true
                                text: String(endpointsTab.field("base_url", "")); onTextChanged: endpointsTab.deferSetField("base_url", text.trim()) }
                            FieldError { errorField: "base_url" }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true; Layout.columnSpan: 2; spacing: 2
                            // The write-only key is rotatable from the edit form too
                            // (reference apiKeyEditHint): a blank field means "keep the
                            // current credential", so it is only sent when typed.
                            TextField { objectName: "channelTopicSummaryField-api_key";
                                placeholderText: endpointsTab.editingUuid === "new"
                                    ? qsTr("API key *") : qsTr("New API key")
                                echoMode: TextInput.Password; Layout.fillWidth: true
                                text: String(endpointsTab.field("api_key", "")); onTextChanged: endpointsTab.deferSetField("api_key", text) }
                            FieldError { errorField: "api_key" }
                            Text { objectName: "channelTopicSummaryApiKeyHint"
                                text: endpointsTab.editingUuid === "new"
                                    ? qsTr("Required. The key is encrypted and never shown again.")
                                    : qsTr("Leave blank to keep the current write-only credential.")
                                font.pixelSize: 12; color: dlg.colors["text-muted"] }
                        }
                        // #838: every numeric setting is pre-filled, so the name
                        // must be a persistent label (reference NumberField's
                        // FormField label, text-xs text-muted), never a
                        // placeholder that vanishes under the value. The allowed
                        // range, previously only in that placeholder, stays
                        // associated with its field as hint text. Accessible.name
                        // gives each input a distinct screen-reader name - the
                        // two penalty fields both rendering "0" was the report.
                        ColumnLayout {
                            Layout.fillWidth: true; spacing: 2
                            Text { objectName: "channelTopicSummaryLabel-priority"
                                text: qsTr("Priority")
                                font.pixelSize: 12; color: dlg.colors["text-muted"] }
                            TextField { objectName: "channelTopicSummaryField-priority"
                                Accessible.name: qsTr("Priority")
                                Layout.fillWidth: true
                                text: String(endpointsTab.field("priority", 100)); onTextChanged: endpointsTab.deferSetField("priority", text) }
                            FieldError { errorField: "priority" }
                            Text { objectName: "channelTopicSummaryHint-priority"
                                text: qsTr("Range: 0-1000000")
                                font.pixelSize: 12; color: dlg.colors["text-muted"] }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true; spacing: 2
                            Text { objectName: "channelTopicSummaryLabel-temperature"
                                text: qsTr("Temperature")
                                font.pixelSize: 12; color: dlg.colors["text-muted"] }
                            TextField { objectName: "channelTopicSummaryField-temperature"
                                Accessible.name: qsTr("Temperature")
                                Layout.fillWidth: true
                                text: String(endpointsTab.field("temperature", 0.2)); onTextChanged: endpointsTab.deferSetField("temperature", text) }
                            FieldError { errorField: "temperature" }
                            Text { objectName: "channelTopicSummaryHint-temperature"
                                text: qsTr("Range: 0-2")
                                font.pixelSize: 12; color: dlg.colors["text-muted"] }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true; spacing: 2
                            Text { objectName: "channelTopicSummaryLabel-max_output_tokens"
                                text: qsTr("Output token limit")
                                font.pixelSize: 12; color: dlg.colors["text-muted"] }
                            TextField { objectName: "channelTopicSummaryField-max_output_tokens"
                                Accessible.name: qsTr("Output token limit")
                                Layout.fillWidth: true
                                text: String(endpointsTab.field("max_output_tokens", 512)); onTextChanged: endpointsTab.deferSetField("max_output_tokens", text) }
                            FieldError { errorField: "max_output_tokens" }
                            Text { objectName: "channelTopicSummaryHint-max_output_tokens"
                                text: qsTr("Range: 1-32768")
                                font.pixelSize: 12; color: dlg.colors["text-muted"] }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true; spacing: 2
                            Text { objectName: "channelTopicSummaryLabel-top_p"
                                text: qsTr("Top P")
                                font.pixelSize: 12; color: dlg.colors["text-muted"] }
                            TextField { objectName: "channelTopicSummaryField-top_p"
                                Accessible.name: qsTr("Top P")
                                Layout.fillWidth: true
                                text: String(endpointsTab.field("top_p", 1)); onTextChanged: endpointsTab.deferSetField("top_p", text) }
                            FieldError { errorField: "top_p" }
                            Text { objectName: "channelTopicSummaryHint-top_p"
                                text: qsTr("Range: 0-1")
                                font.pixelSize: 12; color: dlg.colors["text-muted"] }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true; spacing: 2
                            Text { objectName: "channelTopicSummaryLabel-presence_penalty"
                                text: qsTr("Presence penalty")
                                font.pixelSize: 12; color: dlg.colors["text-muted"] }
                            TextField { objectName: "channelTopicSummaryField-presence_penalty"
                                Accessible.name: qsTr("Presence penalty")
                                Layout.fillWidth: true
                                text: String(endpointsTab.field("presence_penalty", 0)); onTextChanged: endpointsTab.deferSetField("presence_penalty", text) }
                            FieldError { errorField: "presence_penalty" }
                            Text { objectName: "channelTopicSummaryHint-presence_penalty"
                                text: qsTr("Range: -2..2")
                                font.pixelSize: 12; color: dlg.colors["text-muted"] }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true; spacing: 2
                            Text { objectName: "channelTopicSummaryLabel-frequency_penalty"
                                text: qsTr("Frequency penalty")
                                font.pixelSize: 12; color: dlg.colors["text-muted"] }
                            TextField { objectName: "channelTopicSummaryField-frequency_penalty"
                                Accessible.name: qsTr("Frequency penalty")
                                Layout.fillWidth: true
                                text: String(endpointsTab.field("frequency_penalty", 0)); onTextChanged: endpointsTab.deferSetField("frequency_penalty", text) }
                            FieldError { errorField: "frequency_penalty" }
                            Text { objectName: "channelTopicSummaryHint-frequency_penalty"
                                text: qsTr("Range: -2..2")
                                font.pixelSize: 12; color: dlg.colors["text-muted"] }
                        }
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
                    ErrorNotice { name: "channelTopicSummaryEndpointsError"; producer: "endpoints" }
                    Item { Layout.preferredHeight: 16 }
                }
            }
        }

        // ---- Footer (h 80 px): status dot + Cancel/Save ----
        Item { Layout.preferredHeight: 80; Layout.fillWidth: true
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 32; anchors.rightMargin: 32
                Rectangle { implicitWidth: 10; implicitHeight: 10; radius: 5
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
                            // A failed gates GET must keep the section locked: unverified
                            // gate values are never dispatched (review finding 8).
                            ? (gatesTab.dirty && gatesTab.gatesAllowed && !controller.topicSummarySettingsBusy
                               && controller.topicSummaryGatesLoaded)
                            : (endpointsTab.canDispatch && endpointsTab.endpointsAllowed
                               && !controller.topicSummarySettingsBusy))
                    onClicked: {
                        if (dlg.activeTab === "topic") {
                            // Issue #739: patch from the per-field edit mask (draft vs
                            // baseline), never from "differs from the live row" - the
                            // live row may already carry a peer admin's newer write.
                            const editEnabled = topicTab.enabledDraft !== topicTab.enabledBase
                            const editPrompt = topicTab.promptDraft !== topicTab.promptBase
                                || (topicTab.promptDraft.length === 0) !== topicTab.promptIsDefaultBase
                            const editEffort = topicTab.effortDraft !== topicTab.effortBase
                            topicTab.saving = true; topicTab.failed = false
                            if (editEnabled && !editPrompt && !editEffort) controller.applyChannelTopicAction(dlg.topicId, "summary", String(topicTab.enabledDraft))
                            else dlg.topicConfigRequested(dlg.topicId, topicTab.enabledDraft, topicTab.promptDraft,
                                topicTab.promptDraft.length === 0, topicTab.effortDraft,
                                editEnabled, editPrompt, editEffort)
                        } else if (dlg.activeTab === "gates") {
                            dlg.pendingOp = "gates"
                            controller.saveTopicSummaryGates(gatesTab.globalDraft, gatesTab.projectDraft)
                        } else {
                            endpointsTab.commit()
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
            // The Basic style does not fold the indicator into implicitWidth, so an
            // explicit 36x20 (reference track size) keeps the control clickable.
            Layout.preferredWidth: 36; Layout.preferredHeight: 20
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

    // Live per-field validation message beside an endpoint form field (reference
    // endpointValidationErrorText() rendering: a 12px danger line under the input).
    component FieldError: Text {
        required property string errorField
        objectName: "channelTopicSummaryFieldError-" + errorField
        visible: text.length > 0
        text: String(endpointsTab.formErrors[errorField] || "")
        font.pixelSize: 12; color: dlg.colors.danger
    }

    // Error notice band shared by the gates/endpoints tabs (reference: border danger/30, bg danger/5).
    // #795: the band renders only when this tab's own op family produced the
    // failure. The controller records every producing family when it sets an
    // error, so a late reply that lands while the other tab is visible cannot
    // paint the wrong band, and one interruption that retired in-flight ops of
    // BOTH families keeps both bands attributed (finding 4687482019).
    component ErrorNotice: Rectangle {
        id: errBand
        property string name: ""
        required property string producer
        objectName: name
        Layout.fillWidth: true
        visible: dlg.controller.topicSummarySettingsStatus === "error"
            && dlg.controller.topicSummarySettingsError !== "forbidden"
            && dlg.controller.topicSummarySettingsErrorOps.includes(producer)
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
            if (s === "saved") {
                // Only the section whose OWN request completed may reset: a shared
                // reset discarded unsaved edits in the other section (review finding 5),
                // and the endpoint draft no longer resets at dispatch time (finding 4).
                // The endpoint reset is further scoped to the revision that was
                // actually submitted: an edit made while the request was in flight
                // must survive the late success (review finding: in-flight edits).
                if (dlg.pendingOp === "gates") gatesTab.reset()
                else if (dlg.pendingOp === "endpoint"
                         && endpointsTab.dispatchSession === endpointsTab.draftSession) {
                    // A late success may only touch the draft its own request
                    // dispatched (finding 1): after close/reopen or another
                    // endpoint's startEdit the sessions differ, and the stale echo
                    // must leave the current draft alone entirely.
                    if (endpointsTab.formRevision === endpointsTab.dispatchRevision
                        && endpointsTab.editingUuid === endpointsTab.dispatchUuid) endpointsTab.reset()
                    else if (endpointsTab.formRevision !== endpointsTab.dispatchRevision)
                        endpointsTab.rebaseAfterSave()
                }
                dlg.pendingOp = ""
            }
            else if (s === "error") {
                // exordy finding 4693183527: the failed request retires its
                // dispatch identity. The draft survives for retry and equals
                // the failed payload exactly - keeping the identity alive let a
                // later, unrelated busy op reopen the in-flight comparison and
                // read the pending draft as saved.
                if (dlg.pendingOp === "endpoint"
                    && endpointsTab.dispatchSession === endpointsTab.draftSession) {
                    endpointsTab.dispatchRevision = -1
                    endpointsTab.dispatchUuid = ""
                    endpointsTab.dispatchSession = -1
                }
                if (controller.topicSummarySettingsError === "forbidden") dlg.tabs = allowedTabs()
            }
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

    // Unsaved marker: #795 inverts the old controller-wins priority. The
    // controller's "saved"/"error" are op-end statuses that linger until the
    // next op starts, so a dirty draft must outrank them: the first edit after
    // a settled save (or after a failed attempt) shows the yellow unsaved
    // marker immediately instead of the stale "Saved"/silent-error render.
    property string footerStatus: {
        const anyDirty = topicTab.dirty || gatesTab.dirty || endpointsTab.dirty
        if (anyDirty) return "unsaved"
        return controller.topicSummarySettingsStatus === "saved" ? "saved" : ""
    }
    // Maps the reference status.forbidden/network/invalid/contract/server kinds to visible text.
    function errorStatusText() {
        const kind = controller.topicSummarySettingsError
        if (kind === "forbidden") return qsTr("You do not have permission to change these settings.")
        if (kind === "network") return qsTr("Network error. Your changes were kept, try again.")
        if (kind === "interrupted") return qsTr("The settings operation was interrupted by a session refresh. Try again.")
        if (kind === "invalid") return qsTr("The server rejected the values.")
        if (kind === "contract") return qsTr("The server returned an unexpected response.")
        if (kind === "server") return qsTr("The server failed. Please try again.")
        return qsTr("Something went wrong. Please try again.")
    }

    // The gates GET is dispatched on the tab switch, i.e. after onOpened seeded the
    // drafts. Re-seed when its reply lands - but only while the gates tab is on screen
    // and the user has not touched the switches (a late GET must never clobber edits).
    Connections {
        target: dlg.controller
        function onTopicSummaryStateChanged() {
            if (dlg.activeTab === "gates" && !gatesTab.userEdited) gatesTab.reset()
        }
    }
}
