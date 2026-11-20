pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Column {
    id: root
    required property var colors
    required property var controller
    required property bool infoScreen
    readonly property var crypto: controller.encryption
    readonly property var incoming: crypto.incomingKeyRequests || []
    readonly property var granted: crypto.grantedKeyRequests || []
    readonly property var rejected: crypto.rejectedKeyRequests || []
    readonly property bool busy: Boolean(crypto.busy || crypto.grantAllBusy)
    readonly property bool hasRows: incoming.length + granted.length + rejected.length > 0
    property string expandedScope: controller.selectedStream + "/" + infoScreen
    property var expanded: ({})
    onExpandedScopeChanged: expanded = ({})
    spacing: 12
    height: implicitHeight

    component Caption: Text {
        x: 16
        width: parent.width - 32
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        color: root.colors["text-muted"]
        font.pixelSize: 12
    }

    Text {
        objectName: "drawerKeyInfoTitle"
        visible: root.infoScreen
        x: 16
        text: qsTr("Security key requests (%1)").arg(root.incoming.length)
        textFormat: Text.PlainText
        font.pixelSize: 16
        font.weight: Font.Medium
        color: root.colors["text-primary"]
    }
    Caption {
        objectName: "drawerKeyRequestHistoryHint"
        visible: root.hasRows
        text: qsTr("Showing loaded history.")
    }
    Caption {
        objectName: "drawerKeyRequestsEmpty"
        visible: !root.hasRows && !root.infoScreen
        text: qsTr("No key requests")
        font.pixelSize: 14
    }
    Repeater {
        model: ["incoming", "granted", "rejected"]
        delegate: Column {
            id: group
            required property string modelData
            readonly property bool incoming: modelData === "incoming"
            readonly property var entries: incoming ? root.incoming : modelData === "granted" ? root.granted : root.rejected
            readonly property bool open: root.infoScreen || Boolean(root.expanded[modelData])
            visible: entries.length > 0 && (!root.infoScreen || incoming)
            width: root.width
            spacing: 8

            Button {
                id: header
                objectName: "drawerKeyRequests-" + group.modelData
                visible: !root.infoScreen
                width: group.width
                height: 44
                leftPadding: 16
                rightPadding: 16
                text: group.incoming ? qsTr("Incoming requests (%1)").arg(group.entries.length)
                    : group.modelData === "granted" ? qsTr("Granted keys (%1)").arg(group.entries.length)
                    : qsTr("Rejected requests (%1)").arg(group.entries.length)
                Accessible.name: text
                checkable: true
                checked: group.open
                Accessible.checkable: true
                Accessible.checked: group.open
                contentItem: RowLayout {
                    Text {
                        Layout.fillWidth: true
                        text: header.text
                        font.pixelSize: 14
                        color: root.colors["text-primary"]
                        elide: Text.ElideRight
                    }
                    NativeIconButton {
                        colors: root.colors
                        asset: group.open ? "chevron-up.svg" : "chevron-down.svg"
                        glyphSize: 20
                        implicitWidth: 20
                        implicitHeight: 20
                        enabled: false
                        Accessible.ignored: true
                    }
                }
                background: Rectangle { color: header.hovered ? root.colors["sidebar-item-hover"] : "transparent" }
                onClicked: {
                    const values = Object.assign({}, root.expanded)
                    values[group.modelData] = !group.open
                    root.expanded = values
                }
            }
            Button {
                id: grantAll
                objectName: group.incoming ? "drawerKeyGrantAll" : ""
                visible: group.incoming && group.open && group.entries.some(row => row.offerGrant)
                enabled: !root.busy && group.entries.some(row => row.canGrant)
                x: group.width - width - 16
                text: qsTr("Grant all")
                Accessible.name: text
                contentItem: Text {
                    text: grantAll.text
                    font.pixelSize: 14
                    font.underline: true
                    color: root.colors.accent
                }
                background: Rectangle { color: "transparent" }
                onClicked: root.controller.grantAllChatKeys()
            }
            Repeater {
                model: group.visible && group.open ? group.entries : []
                delegate: Column {
                    id: request
                    required property var modelData
                    readonly property var row: modelData
                    readonly property string requestId: String(row.id)
                    readonly property bool recoverable: root.crypto.keyResponse && root.crypto.keyResponse.requestId === requestId
                        && Boolean(root.crypto.keyResponse.canCheck)
                    width: group.width
                    RowLayout {
                        x: 16
                        width: parent.width - 32
                        height: 56
                        spacing: 12
                        Rectangle {
                            implicitWidth: 32
                            implicitHeight: 32
                            radius: 16
                            color: root.colors["card-bg-active"]
                            NativeAvatarImage {
                                id: photo
                                anchors.fill: parent
                                reference: String(request.row.avatar || "")
                                loadActive: root.visible && group.open
                            }
                            Text {
                                visible: !photo.replacesFallback
                                anchors.centerIn: parent
                                text: String(request.row.name || "?").slice(0, 1)
                                textFormat: Text.PlainText
                                color: root.colors["text-primary"]
                                font.pixelSize: 14
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                NativeIconButton {
                                    colors: root.colors
                                    asset: "business_center.svg"
                                    implicitWidth: 16
                                    implicitHeight: 16
                                    glyphSize: 16
                                    enabled: false
                                    Accessible.ignored: true
                                }
                                Text {
                                    objectName: "drawerKeyRequestName-" + request.requestId
                                    Layout.fillWidth: true
                                    text: String(request.row.name || "")
                                    textFormat: Text.PlainText
                                    font.pixelSize: 14
                                    color: root.colors["text-primary"]
                                    elide: Text.ElideRight
                                }
                            }
                            Text {
                                objectName: "drawerKeyRequestDevice-" + request.requestId
                                Layout.fillWidth: true
                                text: String(request.row.deviceName || "")
                                textFormat: Text.PlainText
                                font.pixelSize: 12
                                color: root.colors["text-secondary"]
                                elide: Text.ElideRight
                            }
                        }
                        NativeIconButton {
                            objectName: "drawerKeyGrant-" + request.requestId
                            visible: group.incoming && Boolean(request.row.offerGrant)
                            enabled: !root.busy && Boolean(request.row.canGrant)
                            implicitWidth: 24
                            implicitHeight: 24
                            colors: root.colors
                            iconColor: root.colors.accent
                            glyphSize: 24
                            asset: request.row.pending ? "" : "check.svg"
                            help: qsTr("Grant access to %1").arg(request.row.name)
                            Accessible.description: request.row.pending ? qsTr("Sending response") : ""
                            BusyIndicator {
                                objectName: "drawerKeyGrantPending-" + request.requestId
                                anchors.fill: parent
                                running: Boolean(request.row.pending)
                                visible: running
                            }
                            onClicked: root.controller.grantChatKey(request.requestId, true)
                        }
                        NativeIconButton {
                            objectName: "drawerKeyReject-" + request.requestId
                            visible: group.incoming && Boolean(request.row.offerReject)
                            enabled: !root.busy && Boolean(request.row.canReject)
                            implicitWidth: 24
                            implicitHeight: 24
                            colors: root.colors
                            iconColor: root.colors.accent
                            glyphSize: 24
                            asset: "close.svg"
                            help: qsTr("Reject request from %1").arg(request.row.name)
                            onClicked: root.controller.rejectChatKey(request.requestId, true)
                        }
                    }
                    Caption {
                        objectName: "drawerKeyRequestError-" + request.requestId
                        visible: group.incoming && text.length > 0
                        text: String(request.row.error || "")
                        color: root.colors["notice-base"]
                        Accessible.role: Accessible.AlertMessage
                    }
                    Row {
                        x: 16
                        visible: group.incoming && request.recoverable
                        Button {
                            text: qsTr("Check")
                            onClicked: root.controller.checkChatKeyResponse(request.requestId)
                        }
                        Button {
                            text: qsTr("Retry")
                            visible: Boolean(root.crypto.keyResponse.canRetry)
                            onClicked: root.controller.retryChatKeyResponse(request.requestId)
                        }
                    }
                }
            }
        }
    }
    Caption {
        visible: Number(root.crypto.grantAllFailures || 0) > 0
        text: qsTr("Could not send the key. Requests to retry: %1.").arg(root.crypto.grantAllFailures)
        color: root.colors["notice-base"]
        Accessible.role: Accessible.AlertMessage
    }
}
