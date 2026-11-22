pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs as NativeDialogs
import QtQuick.Effects

// Issue #637 (docs/NATIVE_DESKTOP_CHAT_BACKGROUNDS.md section 4): the settings
// "background" options panel and the apply-scope dialog cloned from the
// reference features/chat-background/chat-background.ui.tsx at workspace_ui
// ca73acf. The hidden <input type=file> becomes a FileDialog; prepareImageFromFile
// maps the file name onto the declared MIME the way the browser derives
// file.type (prepareChatBackground stays the single pipeline oracle).
// Reference container: "space-y-3 p-2" -> spacing 12, margins 8 (an Item
// wrapper because positioners have no padding property).
Item {
    id: panel
    objectName: "chatBackgroundOptions"
    required property var colors
    required property var chatBackground
    required property string chatKey
    implicitHeight: layout.implicitHeight + 16

    // Live re-read seam: every ChatBackground mutation emits changed(); bumping
    // revision re-evaluates the resolved-value bindings below (invokables have
    // no property-change signal of their own).
    property int revision: 0
    property string error: ""
    property bool working: false
    property string pendingKind: ""   // "preset" | "image" | "default" | ""
    property string pendingText: ""
    readonly property string resolvedPreset: chatBackground && revision >= 0
        ? chatBackground.selectedPreset(chatKey) : ""
    readonly property string resolvedImage: chatBackground && revision >= 0
        ? chatBackground.customImage(chatKey) : ""
    Connections {
        target: panel.chatBackground
        function onChanged() { panel.revision++ }
    }

    function urlFor(kind, text) {
        if (kind === "preset" && text !== "")
            return "qrc:/workspace/chat-background/" + text + ".svg"
        if (kind === "image")
            return text
        return ""
    }
    // Reference chatBackground.errors.* (chat-background.ui.tsx role=alert).
    function errorText(code) {
        if (code === "type") return qsTr("Choose a JPEG, PNG or WebP image.")
        if (code === "size") return qsTr("The image is too large or empty. Choose a file up to 8 MB and 24 megapixels.")
        if (code === "storage") return qsTr("Could not save the background. Your previous background was kept.")
        return qsTr("Could not open the image. Try another file.")
    }
    function choose(kind, text) {
        panel.error = ""
        panel.chatBackground.clearError()
        panel.working = false
        panel.pendingKind = kind
        panel.pendingText = text
    }
    function save(scope) {
        if (panel.pendingKind === "" || !panel.chatBackground)
            return
        let ok = false
        if (panel.pendingKind === "preset")
            ok = panel.chatBackground.applyPreset(panel.chatKey, panel.pendingText, scope)
        else if (panel.pendingKind === "image")
            ok = panel.chatBackground.applyImage(panel.chatKey, panel.pendingText, scope)
        else
            ok = panel.chatBackground.applyDefault(panel.chatKey, scope)
        // Reference save(): success closes the dialog (pending drops); a failed
        // apply KEEPS the dialog open with the storage line - the storage core
        // never mutated, so retrying or cancelling stays possible.
        if (ok) {
            panel.pendingKind = ""
            panel.pendingText = ""
            panel.error = ""
        } else {
            panel.error = "storage"
        }
    }

    ColumnLayout {
        id: layout
        anchors.fill: parent
        anchors.margins: 8
        spacing: 12


    Label {
        objectName: "chatBackgroundFileHint"
        Layout.fillWidth: true
        text: qsTr("JPEG, PNG, WebP - up to 8 MB. Backgrounds stay on this device.")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        font.pixelSize: 12
        lineHeightMode: Text.FixedHeight; lineHeight: 20
        color: panel.colors["text-secondary"]
    }

    GridLayout {
        objectName: "chatBackgroundGrid"
        Layout.fillWidth: true
        columns: 2
        columnSpacing: 8
        rowSpacing: 8

        // The three bundled presets (reference CHAT_BACKGROUNDS order).
        Repeater {
            model: panel.chatBackground ? panel.chatBackground.presets : []
            delegate: Button {
                id: presetCard
                required property string modelData
                objectName: "chatBackgroundPreset-" + modelData
                Layout.fillWidth: true
                checkable: true
                checked: panel.resolvedPreset === modelData
                enabled: panel.chatBackground && panel.chatBackground.available
                Accessible.name: ({
                    "mountains": qsTr("Mountains"),
                    "sea": qsTr("Sea"),
                    "forest": qsTr("Forest")
                })[modelData]
                onClicked: panel.choose("preset", modelData)
                padding: 0
                contentItem: Item {
                    implicitWidth: 160
                    implicitHeight: Math.round(width * 5 / 8) + 26
                    Image {
                        objectName: "chatBackgroundPresetImage-" + presetCard.modelData
                        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                        anchors.margins: 1
                        height: Math.round((width) * 5 / 8)
                        source: "qrc:/workspace/chat-background/" + presetCard.modelData + ".svg"
                        fillMode: Image.PreserveAspectCrop
                        clip: true
                    }
                    Label {
                        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                        leftPadding: 8; rightPadding: 8; topPadding: 6; bottomPadding: 6
                        text: ({
                            "mountains": qsTr("Mountains"),
                            "sea": qsTr("Sea"),
                            "forest": qsTr("Forest")
                        })[presetCard.modelData]
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        font.pixelSize: 12
                        color: panel.colors["text-primary"]
                    }
                    Rectangle {
                        objectName: "chatBackgroundPresetBadge-" + presetCard.modelData
                        visible: presetCard.checked
                        width: 18; height: 18; radius: 9
                        anchors.top: parent.top; anchors.right: parent.right
                        anchors.topMargin: 6; anchors.rightMargin: 6
                        color: panel.colors["accent"]
                        Image {
                            anchors.centerIn: parent
                            width: 14; height: 14
                            source: "qrc:/workspace/icons/check.svg"
                            layer.enabled: true
                            layer.effect: MultiEffect { colorization: 1; colorizationColor: panel.colors["on-accent"] }
                        }
                    }
                }
                background: Rectangle {
                    radius: 8
                    color: panel.colors["bg"]
                    border.width: presetCard.checked ? 2 : 1
                    border.color: presetCard.checked ? panel.colors["accent"] : panel.colors["border-subtle"]
                    opacity: presetCard.enabled ? 1 : 0.5
                }
            }
        }

        Button {
            id: addTile
            objectName: "chatBackgroundAddTile"
            Layout.fillWidth: true
            enabled: panel.chatBackground && panel.chatBackground.available && !panel.working
            Accessible.name: panel.working
                ? qsTr("Processing image...") : qsTr("Add your own image")
            onClicked: backgroundFileDialog.open()
            padding: 0
            contentItem: Item {
                implicitWidth: 160
                implicitHeight: Math.round(width * 5 / 8) + 26
                Image {
                    anchors.centerIn: parent
                    anchors.verticalCenterOffset: -13
                    width: 28; height: 28
                    source: "qrc:/workspace/icons/plus.svg"
                    layer.enabled: true
                    layer.effect: MultiEffect { colorization: 1; colorizationColor: panel.colors["text-primary"] }
                }
                Label {
                    objectName: "chatBackgroundAddCaption"
                    anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                    leftPadding: 8; rightPadding: 8; topPadding: 6; bottomPadding: 6
                    text: panel.working ? qsTr("Processing image...") : qsTr("Add your own")
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    font.pixelSize: 12
                    color: panel.colors["text-primary"]
                }
            }
            background: Rectangle {
                radius: 8
                color: addTile.hovered && addTile.enabled ? panel.colors["bg-elevated"] : panel.colors["bg"]
                border.width: 1
                border.color: addTile.hovered && addTile.enabled
                    ? panel.colors["accent"] : panel.colors["border-subtle"]
                opacity: addTile.enabled ? 1 : 0.5
            }
        }
    }

    RowLayout {
        objectName: "chatBackgroundCustomRow"
        visible: panel.resolvedImage.length > 0
        Layout.fillWidth: true
        spacing: 12
        Rectangle {
            Layout.preferredWidth: 64; Layout.preferredHeight: 40
            radius: 4
            clip: true
            Image {
                objectName: "chatBackgroundCustomImage"
                anchors.fill: parent
                source: panel.resolvedImage
                fillMode: Image.PreserveAspectCrop
                layer.enabled: true
                layer.effect: MultiEffect { maskEnabled: true; maskSource: customMask }
            }
            // Rounded-corner mask for the 64x40 thumbnail (reference rounded).
            Item {
                id: customMask
                anchors.fill: parent
                visible: false
                Rectangle { anchors.fill: parent; radius: 4; color: "white" }
            }
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("Your image")
            textFormat: Text.PlainText
            elide: Text.ElideRight
            font.pixelSize: 12
            color: panel.colors["text-primary"]
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        Button {
            objectName: "chatBackgroundReset"
            enabled: panel.chatBackground && panel.chatBackground.available
            text: qsTr("Default background")
            font.pixelSize: 13
            onClicked: panel.choose("default", "")
            contentItem: Text {
                text: parent.text; textFormat: Text.PlainText
                color: parent.enabled ? panel.colors["text-primary"] : panel.colors["text-muted"]
                horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: 8
                color: parent.hovered && parent.enabled ? panel.colors["card-bg-active"] : "transparent"
                opacity: parent.enabled ? 1 : 0.5
            }
        }
    }

    Label {
        objectName: "chatBackgroundError"
        visible: text.length > 0 && panel.pendingKind === ""
        Layout.fillWidth: true
        text: panel.error.length > 0 ? panel.errorText(panel.error) : ""
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        font.pixelSize: 13
        color: panel.colors["danger"]
        Accessible.role: Accessible.AlertMessage
    }

    NativeDialogs.FileDialog {
        id: backgroundFileDialog
        objectName: "chatBackgroundFileDialog"
        title: qsTr("Choose a background image file")
        fileMode: NativeDialogs.FileDialog.OpenFile
        nameFilters: ["Images (*.jpeg *.jpg *.png *.webp)"]
        onAccepted: {
            if (!panel.chatBackground)
                return
            panel.error = ""
            panel.chatBackground.clearError()
            panel.working = true
            const url = selectedFile.toString()
            // Paint the processing caption before the synchronous pipeline run.
            Qt.callLater(function() {
                const result = panel.chatBackground.prepareImageFromFile(url)
                panel.working = false
                if (result.ok)
                    panel.choose("image", result.dataUrl)
                else
                    panel.error = result.error
            })
        }
    }

    Popup {
        id: applyDialog
        objectName: "chatBackgroundApplyDialog"
        parent: Overlay.overlay
        anchors.centerIn: Overlay.overlay
        modal: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        width: Math.min(448, (parent ? parent.width : 448) - 32) // reference max-w-md
        padding: 16
        visible: panel.pendingKind !== ""
        // NOTE: no Accessible.name here - Popup is not an Item, the attached
        // property is rejected. The title Label names the dialog for AT.
        // External dismiss (Escape / click outside) drops the pending value and
        // its error, exactly like the reference onOpenChange branch.
        onClosed: {
            panel.pendingKind = ""
            panel.pendingText = ""
            panel.error = ""
        }
        background: Rectangle {
            radius: 12
            color: panel.colors["bg-elevated"]
            border.color: panel.colors["border-subtle"]
        }
        contentItem: Item {
            // Test seam lives on the content Item (Popup itself is not a
            // QQuickItem, so objectName lookups cannot see it).
            objectName: "chatBackgroundApplyDialog"
            implicitHeight: dialogBody.implicitHeight
            implicitWidth: dialogBody.implicitWidth
            ColumnLayout {
                id: dialogBody
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: 10
            Label {
                Layout.fillWidth: true
                text: qsTr("Apply background")
                textFormat: Text.PlainText
                font.pixelSize: 15
                font.weight: Font.Medium
                color: panel.colors["text-primary"]
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("For all chats or only the chat currently open?")
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                font.pixelSize: 13
                color: panel.colors["text-secondary"]
            }
            Image {
                objectName: "chatBackgroundApplyPreview"
                visible: source.toString().length > 0
                Layout.fillWidth: true
                Layout.maximumHeight: 192 // reference max-h-48
                source: panel.urlFor(panel.pendingKind, panel.pendingText)
                fillMode: Image.PreserveAspectCrop
                clip: true
                Accessible.name: qsTr("Selected background preview")
            }
            Label {
                objectName: "chatBackgroundApplyPreviewText"
                visible: !applyDialog.previewIsImage
                Layout.fillWidth: true
                text: qsTr("The default background will be restored.")
                textFormat: Text.PlainText
                font.pixelSize: 13
                color: panel.colors["text-secondary"]
            }
            Label {
                objectName: "chatBackgroundApplyNoChat"
                visible: panel.chatKey.length === 0
                Layout.fillWidth: true
                text: qsTr("Open a chat to apply a background only to that chat.")
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                font.pixelSize: 13
                color: panel.colors["text-secondary"]
            }
            Label {
                objectName: "chatBackgroundApplyDialogError"
                visible: text.length > 0
                Layout.fillWidth: true
                text: panel.error.length > 0 ? panel.errorText(panel.error) : ""
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                font.pixelSize: 13
                color: panel.colors["danger"]
                Accessible.role: Accessible.AlertMessage
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Button {
                    objectName: "chatBackgroundApplyCancel"
                    Layout.alignment: Qt.AlignLeft
                    text: qsTr("Cancel")
                    font.pixelSize: 13
                    onClicked: { panel.pendingKind = ""; panel.pendingText = ""; panel.error = "" }
                    contentItem: Text {
                        text: parent.text; textFormat: Text.PlainText
                        color: panel.colors["text-primary"]
                        horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: 8
                        color: parent.hovered ? panel.colors["card-bg-active"] : "transparent"
                    }
                }
                Item { Layout.fillWidth: true }
                Button {
                    objectName: "chatBackgroundApplyChat"
                    text: qsTr("Only this chat")
                    font.pixelSize: 13
                    enabled: panel.chatKey.length > 0
                    onClicked: panel.save("chat")
                    contentItem: Text {
                        text: parent.text; textFormat: Text.PlainText
                        color: parent.enabled ? panel.colors["text-primary"] : panel.colors["text-muted"]
                        horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: 8
                        color: "transparent"
                        border.width: 1
                        border.color: panel.colors["border-subtle"]
                        opacity: parent.enabled ? 1 : 0.5
                    }
                }
                Button {
                    objectName: "chatBackgroundApplyAll"
                    text: qsTr("For all chats")
                    font.pixelSize: 13
                    onClicked: panel.save("all")
                    contentItem: Text {
                        text: parent.text; textFormat: Text.PlainText
                        color: panel.colors["on-accent"]
                        horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: 8
                        color: panel.colors["accent"]
                        opacity: parent.down ? 0.8 : 1
                    }
                }
            }
            }
        }
        readonly property bool previewIsImage: panel.pendingKind === "preset" || panel.pendingKind === "image"
    }
}
}
