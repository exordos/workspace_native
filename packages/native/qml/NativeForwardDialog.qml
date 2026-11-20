pragma ComponentBehavior: Bound

// Issue #7 slice 2 (docs/NATIVE_DESKTOP_MULTI_FORWARD.md): the forward modal
// over the controller batch API. Geometry reproduces the measured reference
// table at workspace_ui 75bd21f37
// packages/web/src/features/workspace-forward-message/workspace-forward-message.ui.tsx:
// dialog max-w-md (448 px) centered, header px-4 py-3 with a bottom rule and
// an 18 px close icon, body p-4 gap-3 with muted 14 px labels, a danger-tinted
// error alert (bg-danger/10 rounded-lg px-3 py-2 text-sm) and a right-aligned
// footer (gap-2). The direct-target tab is deferred (the controller takes an
// explicit stream+topic pair), so only the channel picker is reproduced.
// Submission calls NativeController.forwardMessages once; the unconfirmed
// state is resolved by the GET-only checkForwardStatus() - never a re-send.
import QtQuick
import QtQuick.Controls

Popup {
    id: root
    objectName: "forwardDialog"
    parent: Overlay.overlay
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    width: parent ? Math.min(parent.width - 48, 448) : 448
    padding: 0
    modal: true
    dim: true
    focus: true
    // Close is blocked while a batch is submitting (reference
    // workspace-forward-message.model.ts:46-54 blocks close during submit).
    readonly property bool requestInFlight: controller && controller.forwardPending && !controller.forwardAwaitingCheck
    closePolicy: requestInFlight
        ? Popup.NoAutoClose : Popup.CloseOnEscape | Popup.CloseOnPressOutside
    Overlay.modal: Rectangle { color: "#80000000" }
    background: Rectangle {
        objectName: "forwardDialogSurface"
        radius: 10
        color: root.colors["bg-elevated"]
        border.color: root.colors["border-subtle"]
    }

    required property var colors
    required property var controller
    // Selected source message uuids in selection order (the reference keeps
    // store.messageUuids and clears the chat selection only on success).
    property var sourceIds: []

    signal forwardSucceeded(var submittedIds)

    property string streamUuid: ""
    property string topicUuid: ""
    property var topicRows: []
    property bool submitAttempted: false
    property bool wasBusy: false
    // Review round 1 (cid 4586012848): success must settle exactly the batch
    // it was submitted with. The snapshot is taken at submit time so a Check
    // that lands after the modal went away can recognise its own selection
    // and leave a newer one untouched.
    property var submittedIds: []

    // Picker rows over the catalog: the reference lists every channel and
    // resolves the picked channel's topics for the topic select. Encrypted
    // conversations are never offered: the reference's only submit route
    // sendMessengerMessage throws on stream.encryption === true before any
    // POST (workspace_ui 75bd21f37 messenger-message-actions.lib.ts:309-311),
    // and this native route posts plaintext markdown with no crypto path, so
    // the picker drops encrypted destinations outright (the controller
    // refuses them as a second line).
    property var streamRows: (root.controller ? root.controller.streams : []).filter(function (stream) {
        return !stream.encrypted
    }).map(function (stream) {
        return { id: stream.id, name: stream.isDirect && stream.displayName ? stream.displayName : stream.name }
    })
    readonly property var streamOptions: [{ id: "", name: qsTr("Select channel") }].concat(root.streamRows)
    readonly property var topicOptions: [{ id: "", name: qsTr("Select a topic to send messages to the channel") }].concat(root.topicRows)

    function resetForm() {
        root.streamUuid = ""
        root.topicUuid = ""
        root.topicRows = []
        root.submitAttempted = false
        streamCombo.currentIndex = -1
        topicCombo.currentIndex = -1
    }
    onOpened: resetForm()
    // An unconfirmed batch opened against keeps its Check escape; a fresh
    // submit replaces the previous error text through the controller state.
    onClosed: root.submitAttempted = false

    Connections {
        target: root.controller
        // Success closes the modal and clears the chat selection (reference
        // onSuccess -> setSelectedMessageUuids(new Set())); failures leave the
        // dialog open with the alert visible (reference keeps the modal open).
        function onForwardChanged() {
            const busy = root.controller.forwardPending
            if (root.wasBusy && !busy && root.controller.forwardError.length === 0) {
                root.forwardSucceeded(root.submittedIds)
                root.close()
            }
            root.wasBusy = busy
        }
        function onForwardTopicsChanged() {
            root.topicRows = root.controller.forwardTopicOptions(root.streamUuid)
        }
    }

    contentItem: Item {
        implicitWidth: root.availableWidth
        implicitHeight: formColumn.implicitHeight

        Column {
            id: formColumn
            width: parent.width

            // Header: px-4 py-3, border-b border-border-subtle, title
            // text-sm font-semibold, close icon 18 px (lines 547-563).
            Item {
                width: parent.width
                height: headerRow.implicitHeight + 24
                Row {
                    id: headerRow
                    anchors.fill: parent
                    anchors.leftMargin: 16
                    anchors.rightMargin: 16
                    anchors.topMargin: 12
                    anchors.bottomMargin: 12
                    spacing: 12
                    Text {
                        objectName: "forwardDialogTitle"
                        id: forwardTitle
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - forwardCloseButton.width - parent.spacing
                        text: qsTr("Forward to channel")
                        textFormat: Text.PlainText
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                        color: root.colors["text-primary"]
                    }
                    NativeIconButton {
                        id: forwardCloseButton
                        objectName: "forwardCloseButton"
                        anchors.verticalCenter: parent.verticalCenter
                        width: 18
                        height: 18
                        glyphSize: 18
                        colors: root.colors
                        asset: "close.svg"
                        help: qsTr("Close")
                        enabled: !root.requestInFlight
                        background: null
                        onClicked: root.close()
                    }
                }
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 1
                    color: root.colors["border-subtle"]
                }
            }

            // Body: p-4 gap-3 (lines 182-215): "Channel" label + channel
            // select, "Topic name" label + topic select (disabled until a
            // stream is picked), then the error alert.
            Column {
                width: parent.width
                spacing: 12
                padding: 16
                Text {
                    text: qsTr("Channel")
                    textFormat: Text.PlainText
                    font.pixelSize: 14
                    color: root.colors["text-muted"]
                }
                ComboBox {
                    id: streamCombo
                    objectName: "forwardStreamCombo"
                    width: parent.width - 32
                    enabled: !root.controller.forwardPending
                    model: root.streamOptions
                    textRole: "name"
                    valueRole: "id"
                    currentIndex: -1
                    displayText: currentIndex >= 0 && currentText.length > 0
                        ? currentText : qsTr("Select channel")
                    onActivated: {
                        root.streamUuid = currentValue
                        root.topicUuid = ""
                        root.topicRows = []
                        topicCombo.currentIndex = -1
                        // Review round 1 (cid 4586012826): each stream
                        // activation is a fresh read of the topic list, so
                        // topics created mid-session appear when the picker
                        // reopens (GET-only, never a replay).
                        root.controller.fetchForwardTopics(currentValue, true)
                    }
                    delegate: ItemDelegate {
                        required property var modelData
                        required property int index
                        objectName: "forwardStreamOption-" + modelData.id
                        width: streamCombo.width
                        text: modelData.name
                        highlighted: streamCombo.highlightedIndex === index
                    }
                }
                Text {
                    text: qsTr("Topic name")
                    textFormat: Text.PlainText
                    font.pixelSize: 14
                    color: root.colors["text-muted"]
                }
                ComboBox {
                    id: topicCombo
                    objectName: "forwardTopicCombo"
                    width: parent.width - 32
                    enabled: root.streamUuid.length > 0 && !root.controller.forwardPending
                    model: root.topicOptions
                    textRole: "name"
                    valueRole: "id"
                    currentIndex: -1
                    displayText: currentIndex >= 0 && currentText.length > 0
                        ? currentText : qsTr("Select a topic to send messages to the channel")
                    onActivated: root.topicUuid = currentValue
                    delegate: ItemDelegate {
                        required property var modelData
                        required property int index
                        objectName: "forwardTopicOption-" + modelData.id
                        width: topicCombo.width
                        text: modelData.name
                        highlighted: topicCombo.highlightedIndex === index
                    }
                }
                // role=alert bg-danger/10 rounded-lg px-3 py-2 text-sm
                // text-danger (lines 289-293, 566).
                Rectangle {
                    objectName: "forwardErrorAlert"
                    width: parent.width - 32
                    // The alert carries both hard failures and the unconfirmed
                    // (awaiting-check) notice; it clears itself when the error
                    // does, so an in-flight batch that still owes an answer
                    // keeps its notice visible next to Check.
                    visible: (root.submitAttempted || root.controller.forwardAwaitingCheck)
                        && root.controller.forwardError.length > 0
                    height: visible ? forwardErrorText.implicitHeight + 16 : 0
                    radius: 8
                    color: Qt.alpha(root.colors["danger"], 0.1)
                    Text {
                        id: forwardErrorText
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        text: root.controller.forwardError
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        font.pixelSize: 14
                        color: root.colors["danger"]
                        Accessible.role: Accessible.AlertMessage
                    }
                }
                // Unconfirmed batches confirm through the GET read-back Check
                // alone (docs/NATIVE_DESKTOP_MULTI_FORWARD.md): the modal
                // offers Check, never a second POST.
                NativeButton {
                    objectName: "forwardCheckButton"
                    visible: root.controller.forwardAwaitingCheck
                    colors: root.colors
                    text: qsTr("Check")
                    onClicked: root.controller.checkForwardStatus()
                }
            }

            // Footer: mt-auto justify-end gap-2; cancel px-3 py-1.5,
            // primary px-3 py-1.5 (lines 294-320).
            Item {
                width: parent.width
                height: footerRow.implicitHeight + 16
                Row {
                    id: footerRow
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.rightMargin: 16
                    anchors.topMargin: 4
                    anchors.bottomMargin: 16
                    spacing: 8
                    NativeButton {
                        objectName: "forwardCancelButton"
                        colors: root.colors
                        text: qsTr("Cancel")
                        enabled: !root.requestInFlight
                        onClicked: root.close()
                    }
                    NativeButton {
                        objectName: "forwardConfirmButton"
                        colors: root.colors
                        primary: true
                        text: root.controller.forwardPending ? qsTr("Please wait…") : qsTr("Forward")
                        // reference forwardDisabled = nothing picked yet (or
                        // submitting); an unconfirmed batch must not be re-POSTed.
                        enabled: root.sourceIds.length > 0 && root.streamUuid.length > 0
                            && root.topicUuid.length > 0 && !root.controller.forwardPending
                            && !root.controller.forwardAwaitingCheck
                        onClicked: {
                            root.submitAttempted = true
                            // The success handler recognises this snapshot
                            // later, even after the modal goes away.
                            root.submittedIds = root.sourceIds.slice()
                            root.controller.forwardMessages(root.sourceIds, root.streamUuid, root.topicUuid)
                        }
                    }
                }
            }
        }
    }
}
