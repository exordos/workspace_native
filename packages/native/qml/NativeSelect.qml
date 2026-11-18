import QtQuick
import QtQuick.Controls
import QtQuick.Effects

pragma ComponentBehavior: Bound

// Shared labelled dropdown (issue #60). Style port of the reference
// Select control at 61dacef78dab2f5d0be76f283bebf49fe29b84af:
// shared/ui/listbox/listbox.lib.ts:3-9 (trigger h-10 px-3 gap-2 rounded-lg
// border-border-subtle bg-text-field-bg text-sm text-text-primary; listbox
// rounded-xl p-1 border-subtle bg-bg-elevated) and shared/ui/select/select.ui.tsx
// (muted placeholder text, 16 px chevron-down in text-muted, options
// min-h-9 px-2.5 py-2 with bg-card-bg-active highlight).
// Rows are QVariantMaps carrying `id` and `displayLabel`.
ComboBox {
    id: control
    required property var colors
    // Fallback trigger copy while no row is selected (auth.selectProjectPlaceholder).
    property string placeholderText: ""
    // Option delegates get objectName: objectNamePrefix + <row id>.
    property string objectNamePrefix: ""
    property int radius: 8

    textRole: "displayLabel"
    valueRole: "id"
    displayText: currentIndex >= 0 && currentText.length > 0 ? currentText : placeholderText
    implicitHeight: 40                                   // h-10
    leftPadding: 12                                     // px-3
    rightPadding: 36                                    // text + gap-2 + 16 px chevron
    topPadding: 0
    bottomPadding: 0
    Accessible.name: placeholderText

    contentItem: Text {
        text: control.displayText
        textFormat: Text.PlainText
        color: control.currentIndex >= 0
            ? control.colors["text-primary"] : control.colors["text-muted"]
        font.pixelSize: 14                              // text-sm
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: Item {
        x: control.width - width - 12                   // pr-3
        y: (control.height - height) / 2
        width: 16                                       // chevron-down size 16
        height: 16
        Image {
            anchors.fill: parent
            source: "qrc:/workspace/icons/chevron-down.svg"
            sourceSize.width: 16
            sourceSize.height: 16
            asynchronous: false
            rotation: control.popup.visible ? 180 : 0
            Behavior on rotation { NumberAnimation { duration: 100 } }
            layer.enabled: true
            layer.effect: MultiEffect {
                colorizationColor: control.colors["text-muted"]
                colorization: 1.0
            }
            Accessible.ignored: true
        }
    }

    background: Rectangle {
        radius: control.radius                          // rounded-lg
        color: control.colors["text-field-bg"]
        border.width: 1
        border.color: control.pressed || control.popup.visible
            ? control.colors.accent : control.colors["border-subtle"]
        opacity: control.enabled ? 1.0 : 0.5            // disabled:opacity-50
    }

    popup: Popup {
        id: selectPopup
        y: control.height + 4
        width: control.width
        implicitHeight: Math.min(300, contentItem.implicitHeight + padding * 2)
        padding: 4                                      // listbox p-1
        margins: 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            radius: 12                                  // rounded-xl
            color: control.colors["bg-elevated"]
            border.width: 1
            border.color: control.colors["border-subtle"]
        }
        contentItem: ListView {
            objectName: "nativeSelectListView"
            clip: true
            implicitHeight: Math.min(288, contentHeight)
            // The view fills the popup's available area (the popup bounds
            // itself to `implicitHeight`, capped at 300 px), so options past
            // the cap stay inside the scroll range. Binding the height to
            // `contentHeight` instead leaves `interactive` false whenever the
            // binding re-fires after the popup's own layout (late delegate
            // incubation or a model change while open) and the overflowing
            // options become unreachable.
            height: selectPopup.availableHeight
            model: selectPopup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            boundsBehavior: Flickable.StopAtBounds
            interactive: contentHeight > height
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        }
    }

    delegate: ItemDelegate {
        required property var modelData
        required property int index
        objectName: control.objectNamePrefix + modelData.id
        width: ListView.view ? ListView.view.width : control.width
        // listbox option: min-h-9 px-2.5 py-2 text-sm (listbox.ui.tsx:136).
        implicitHeight: Math.max(36, contentItem.implicitHeight + 16)
        padding: 0
        leftPadding: 10
        rightPadding: 10
        hoverEnabled: true
        highlighted: control.highlightedIndex === index
        // Keep ItemDelegate's own text in sync: it is the accessible/inspection
        // surface (`property("text")`), the visible row renders below.
        text: modelData.displayLabel
        contentItem: Text {
            text: modelData.displayLabel
            textFormat: Text.PlainText
            color: control.colors["text-primary"]
            font.pixelSize: 14
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: 8
            color: highlighted || hovered ? control.colors["card-bg-active"] : "transparent"
        }
    }
}
