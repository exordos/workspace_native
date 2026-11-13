pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects
import QtQuick.Dialogs as NativeDialogs

// qmllint disable unqualified

// User profile drawer content (issue #134, docs/NATIVE_DESKTOP_USER_PROFILE.md).
// Reference: workspace_ui right-panel-user-profile.ui.tsx,
// right-panel-user-profile-actions.ui.tsx, right-panel-user-profile-details.ui.tsx
// and right-panel-user-profile-edit-avatar-dialog.ui.tsx at
// 61dacef78dab2f5d0be76f283bebf49fe29b84af. Mounted on the NativeRightDrawer
// body slot for the chat-info profile override (other users) and the account
// personal-info screen (own profile). The shell owns title + back (#129), so
// this page renders no nested header. Geometry and colors are measured from
// the pinned reference.
Item {
    id: page
    objectName: "profilePage"
    required property var colors
    required property var controller
    // The drawer passes the requested user; own profile uses the session user.
    property string userUuid: ""
    property string locale: "en"
    anchors.fill: parent
    clip: true

    // ---- Projection state (visibility-gated; the page never fetches hidden) ----
    property var snapshot: ({})
    property var details: []
    property bool isSelf: false
    property bool editing: false
    property string draftFirst: ""
    property string draftLast: ""
    property string draftPhone: ""
    property string draftDescription: ""
    property string pendingAvatarPath: ""
    property bool pendingRemoveAvatar: false
    property string avatarErrorLocal: ""
    property string copiedKey: ""
    readonly property bool saving: controller.ownProfileSaving || controller.ownAvatarBusy
    readonly property string shareLink: userUuid.length > 0 ? controller.buildProfileShareLink(userUuid) : ""

    function sync() {
        const shown = visible && userUuid.length > 0
        page.snapshot = shown ? controller.profileSnapshot(userUuid) : ({})
        page.details = shown ? controller.profileDetails(userUuid, page.locale) : []
        page.isSelf = shown && userUuid === controller.currentUserId
    }
    function resetTransient() {
        page.editing = false
        page.pendingAvatarPath = ""
        page.pendingRemoveAvatar = false
        page.avatarErrorLocal = ""
        page.copiedKey = ""
        avatarDialog.close()
    }
    function onVisibleActive() {
        page.sync()
        if (page.isSelf && !controller.ownProfileLoaded && !controller.ownProfileBusy)
            controller.loadOwnProfile()
    }
    onVisibleChanged: visible ? onVisibleActive() : resetTransient()
    onUserUuidChanged: { resetTransient(); sync() }
    onLocaleChanged: sync()
    Component.onCompleted: if (visible) onVisibleActive()
    Connections {
        target: page.controller
        function onStateChanged() { if (page.visible) page.sync() }
        function onOwnProfileSettledChanged() { page.afterSettle() }
        function onOwnAvatarSettledChanged() { page.afterSettle() }
        function onOwnAvatarErrorChanged() {
            if (page.visible && controller.ownAvatarError.length > 0) {
                // Reference: a failed mutation reopens the dialog with the error.
                page.avatarErrorLocal = controller.ownAvatarError
                avatarDialog.open()
            }
        }
    }
    function afterSettle() {
        if (!page.editing) return
        if (controller.ownProfileSaving || controller.ownAvatarBusy) return
        if (page.avatarErrorLocal.length > 0 || controller.ownAvatarError.length > 0) return
        if (controller.ownProfileError.length > 0) return
        page.editing = false
        page.pendingAvatarPath = ""
        page.pendingRemoveAvatar = false
    }
    function startEdit() {
        // exordy #199 review 4553884671: never snapshot drafts from a baseline
        // that has not loaded yet — those blanks become real saved changes.
        if (!controller.ownProfileLoaded) {
            if (!controller.ownProfileBusy) controller.loadOwnProfile()
            return
        }
        const baseline = controller.ownProfile
        page.draftFirst = String(baseline.firstName || "")
        page.draftLast = String(baseline.lastName || "")
        page.draftPhone = String(baseline.phone || "")
        page.draftDescription = String(baseline.description || "")
        page.pendingAvatarPath = ""
        page.pendingRemoveAvatar = false
        page.editing = true
    }
    function cancelEdit() {
        page.editing = false
        page.pendingAvatarPath = ""
        page.pendingRemoveAvatar = false
        avatarDialog.close()
    }
    function draftValid() {
        if (page.draftFirst.length > 128 || page.draftFirst.indexOf("\n") >= 0) return false
        if (page.draftLast.length > 128 || page.draftLast.indexOf("\n") >= 0) return false
        const phone = page.draftPhone.replace(/^\s+|\s+$/g, "")
        if (phone.length > 15) return false
        return true
    }
    function displayPreview() {
        const name = (page.draftFirst + " " + page.draftLast).replace(/^\s+|\s+$/g, "")
        return name.length > 0 ? name : String(controller.ownProfile.username || "")
    }
    function saveEdit() {
        const draft = {firstName: page.draftFirst, lastName: page.draftLast,
            phone: page.draftPhone, description: page.draftDescription}
        const action = page.pendingRemoveAvatar ? "remove"
            : (page.pendingAvatarPath.length > 0 ? "upload" : "")
        controller.saveOwnProfile(draft, action, page.pendingAvatarPath)
    }
    function copyText(text, key) {
        if (text.length === 0) return
        controller.copyToClipboard(text)
        page.copiedKey = key
        copyRevertTimer.restart()
    }
    Timer {
        id: copyRevertTimer
        interval: 2000
        onTriggered: page.copiedKey = ""
    }

    // Presence maps (resolvePresenceStatusTextClass + resolveUserPresenceVisual).
    function presenceText(status) {
        if (status === "active") return qsTr("online")
        if (status === "idle") return qsTr("away")
        if (status === "offline") return qsTr("offline")
        if (status.length === 0) return ""
        return qsTr("do not disturb")
    }
    function presenceTextColor(status) {
        if (status === "active") return page.colors["call-green"]
        if (status === "idle") return page.colors["indicator-orange"]
        if (status.length === 0 || status === "offline") return page.colors["text-muted"]
        return page.colors["call-red"]
    }
    function dotColor(presence) {
        if (presence === "active") return page.colors["indicator-green"]
        if (presence === "idle") return page.colors["indicator-orange"]
        return page.colors["text-muted"]
    }
    function rowLabel(id) {
        switch (id) {
        case "userId": return qsTr("User ID")
        case "email": return qsTr("Email")
        case "phone": return qsTr("Phone")
        case "jobTitle": return qsTr("Team > Job title")
        case "manager": return qsTr("Manager")
        case "role": return qsTr("Role")
        case "accountType": return qsTr("Account type")
        case "accountStatus": return qsTr("Account status")
        case "timezone": return qsTr("Timezone")
        case "localTime": return qsTr("Local time")
        case "joined": return qsTr("Joined")
        case "birthday": return qsTr("Birthday")
        case "username": return qsTr("Username")
        }
        return id
    }
    function rowIcon(id) {
        switch (id) {
        case "userId": return "alternate_email.svg"
        case "email": return "mail.svg"
        case "phone": return "phone.svg"
        case "jobTitle": return "business_center.svg"
        case "manager": return "handshake.svg"
        case "role": return "account_circle.svg"
        case "accountType": return "group.svg"
        case "accountStatus": return "info.svg"
        case "timezone": return "globe_location_pin.svg"
        case "localTime": return "schedule.svg"
        case "joined": return "calendar_month.svg"
        case "birthday": return "celebration.svg"
        case "username": return "alternate_email.svg"
        }
        return "info.svg"
    }
    // Edit-mode row order (reference ProfileEditor + detail list): firstName,
    // lastName, username (read), standard details with the phone row replaced
    // by an editable field, then the multiline About me row.
    function detailModel() {
        const rows = []
        const editable = page.editing && page.isSelf
        if (editable) {
            rows.push({kind: "edit", id: "firstName", field: "firstName",
                icon: "account_circle.svg", label: qsTr("First name")})
            rows.push({kind: "edit", id: "lastName", field: "lastName",
                icon: "account_circle.svg", label: qsTr("Last name")})
            rows.push({kind: "row", id: "username", icon: "alternate_email.svg",
                label: qsTr("Username"), value: String(controller.ownProfile.username || ""),
                unavailable: false, copy: false})
        }
        for (const detail of page.details) {
            if (editable && detail.id === "phone") {
                rows.push({kind: "edit", id: "phone", field: "phone",
                    icon: "phone.svg", label: qsTr("Phone")})
                continue
            }
            rows.push({kind: "row", id: detail.id, icon: page.rowIcon(detail.id),
                label: page.rowLabel(detail.id), value: String(detail.value || ""),
                unavailable: detail.unavailable === true, copy: !detail.unavailable})
        }
        if (editable)
            rows.push({kind: "description", id: "description", icon: "info.svg",
                label: qsTr("About me")})
        return rows
    }

    // Tinted static glyph (codebase MultiEffect pattern, NativeInboxPage.qml).
    component ProfileGlyph: Item {
        required property string asset
        required property int glyph
        required property color tint
        implicitWidth: glyph
        implicitHeight: glyph
        Image {
            id: glyphImage
            anchors.fill: parent
            source: "qrc:/workspace/icons/" + parent.asset
            sourceSize.width: parent.glyph
            sourceSize.height: parent.glyph
            asynchronous: false
            layer.enabled: true
            layer.effect: MultiEffect {
                colorizationColor: glyphImage.parent.tint
                colorization: 1.0
            }
        }
    }

    component ProfilePresenceDot: Item {
        id: dot
        required property string presence
        width: 12; height: 12
        visible: presence === "active" || presence === "idle" || presence === "offline"
        Rectangle {
            anchors.fill: parent
            radius: 6
            color: page.colors["bg-elevated"]
        }
        Rectangle {
            objectName: "profilePresenceDot"
            x: 2; y: 2; width: 8; height: 8; radius: 4
            color: page.dotColor(dot.presence)
        }
    }

    component ProfileChip: Button {
        id: chip
        required property var modelData
        objectName: "profileAction-" + modelData.id
        Accessible.name: modelData.label
        enabled: !modelData.disabled
        opacity: enabled ? 1.0 : 0.5
        hoverEnabled: true
        padding: 0
        implicitHeight: 72
        contentItem: Column {
            spacing: 0
            anchors.centerIn: parent
            Item {
                width: 40; height: 40
                ProfileGlyph {
                    objectName: "profileActionGlyph-" + chip.modelData.id
                    anchors.centerIn: parent
                    asset: page.copiedKey === "share" && chip.modelData.id === "share"
                        ? "check.svg" : chip.modelData.icon
                    glyph: page.copiedKey === "share" && chip.modelData.id === "share" ? 28 : chip.modelData.glyph
                    tint: page.colors["icon-active"]
                }
            }
            Text {
                objectName: "profileActionLabel-" + chip.modelData.id
                text: page.copiedKey === "share" && chip.modelData.id === "share"
                    ? qsTr("Copied") : chip.modelData.label
                textFormat: Text.PlainText
                font.pixelSize: 12
                lineHeightMode: Text.FixedHeight; lineHeight: 16
                color: page.colors["text-secondary"]
                elide: chip.modelData.hug ? Text.ElideNone : Text.ElideRight
                width: chip.modelData.hug ? implicitWidth : Math.min(implicitWidth, chip.width - 16)
            }
        }
        background: Rectangle {
            radius: 8
            color: chip.hovered && chip.enabled ? page.colors["bg"] : page.colors["card-bg"]
        }
        onClicked: {
            switch (chip.modelData.id) {
            case "edit": page.startEdit(); break
            case "share": page.copyText(page.shareLink, "share"); break
            case "message": page.controller.openDirectChatFromProfile(page.userUuid); break
            case "call": page.controller.callFromProfile(page.userUuid); break
            default: break // favorites (no destination until #70)
            }
        }
    }

    component ProfileDialogRow: Button {
        id: dialogRow
        required property string label
        required property string asset
        property color tone: page.colors["text-primary"]
        property color glyphTint: page.colors["text-muted"]
        height: 32
        hoverEnabled: true
        padding: 0
        contentItem: Row {
            anchors.left: parent.left
            anchors.leftMargin: 0
            spacing: 12
            Item {
                width: 32; height: 32
                anchors.verticalCenter: parent.verticalCenter
                ProfileGlyph {
                    anchors.centerIn: parent
                    asset: dialogRow.asset
                    glyph: 24
                    tint: dialogRow.enabled ? dialogRow.glyphTint : dialogRow.tone
                }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: dialogRow.label
                textFormat: Text.PlainText
                font.pixelSize: 14
                lineHeightMode: Text.FixedHeight; lineHeight: 20
                color: dialogRow.tone
            }
        }
        background: Rectangle {
            radius: 8
            color: dialogRow.hovered && dialogRow.enabled ? page.colors["card-bg"] : "transparent"
        }
    }

    // ---- Content scroll: py-2 blocks space-y-5 ----
        Flickable {
        id: profileScroll
        objectName: "profileScroll"
        anchors.fill: parent
        contentWidth: width
        contentHeight: blocks.height + 16
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        Column {
            id: blocks
            x: 0
            y: 8
            width: profileScroll.width
            spacing: 20

            // ---- View/edit header: avatar xl 64 + name column ----
            Item {
                id: headerBlock
                objectName: "profileHeader"
                x: 8
                width: parent.width - 16
                height: 64
                Item {
                    id: avatarBlock
                    objectName: "profileAvatarBlock"
                    width: 64; height: 64
                    Rectangle {
                        // profileHeaderAvatar: "profileAvatar" is already the
                        // 36 px account-menu avatar in Main.qml; window tests
                        // resolve objectNames globally, so the page's xl
                        // avatar keeps a unique name.
                        objectName: "profileHeaderAvatar"
                        anchors.fill: parent
                        radius: 32
                        clip: true
                        color: page.colors["bg-elevated"]
                        NativeAvatarImage {
                            id: headerAvatar
                            anchors.fill: parent
                            reference: page.pendingAvatarPath.length === 0 && !page.pendingRemoveAvatar
                                ? String(page.snapshot.avatar || "") : ""
                            loadActive: page.visible
                        }
                        Image {
                            objectName: "profileAvatarPreview"
                            anchors.fill: parent
                            anchors.margins: 1
                            visible: page.pendingAvatarPath.length > 0
                            source: page.pendingAvatarPath.length > 0
                                ? "file://" + page.pendingAvatarPath : ""
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: false
                        }
                        Text {
                            objectName: "profileAvatarInitial"
                            anchors.centerIn: parent
                            // exordy #199 review 4553884690: the fallback initial
                            // renders only when neither the avatar image nor the
                            // edit preview is displayed - never over a photo.
                            visible: text.length > 0
                                && page.pendingAvatarPath.length === 0
                                && (page.pendingRemoveAvatar
                                    || (String(page.snapshot.avatar || "").length === 0
                                        && !headerAvatar.replacesFallback))
                            text: {
                                const title = String(page.snapshot.title || "")
                                if (page.editing && page.isSelf)
                                    return page.displayPreview().slice(0, 1).toUpperCase()
                                return page.pendingAvatarPath.length === 0
                                    ? title.slice(0, 1).toUpperCase() : ""
                            }
                            textFormat: Text.PlainText
                            font.pixelSize: 20
                            font.weight: Font.DemiBold
                            color: page.colors["text-secondary"]
                        }
                    }
                    ProfilePresenceDot {
                        visible: !(page.editing && page.isSelf)
                            && String(page.snapshot.presence || "").length > 0
                            && String(page.snapshot.status || "").length > 0
                        presence: String(page.snapshot.presence || "")
                        x: 64 * 0.8535 - 6
                        y: 64 * 0.8535 - 6
                    }
                    // Edit mode: stylus badge opens the avatar dialog.
                    Button {
                        objectName: "profileAvatarBadge"
                        visible: page.editing && page.isSelf
                        width: 20; height: 20
                        x: 64 - 20
                        y: 64 - 20
                        enabled: !page.saving
                        padding: 0
                        Accessible.name: qsTr("Edit avatar")
                        background: Rectangle {
                            radius: 10
                            color: page.colors["icon-base"]
                            opacity: parent.enabled ? 1.0 : 0.6
                        }
                        contentItem: Item {
                            ProfileGlyph {
                                anchors.centerIn: parent
                                asset: "stylus.svg"
                                glyph: 12
                                tint: "white"
                            }
                        }
                        onClicked: {
                            page.avatarErrorLocal = ""
                            avatarDialog.open()
                        }
                    }
                }
                // View column: name + custom status + presence label; edit
                // column: Display name label + preview box.
                Item {
                    id: nameColumn
                    objectName: "profileNameColumn"
                    anchors.left: avatarBlock.right
                    anchors.leftMargin: 16
                    anchors.right: parent.right
                    height: 64
                    Text {
                        objectName: "profileTitle"
                        visible: !(page.editing && page.isSelf)
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        text: String(page.snapshot.title || "")
                        textFormat: Text.PlainText
                        font.pixelSize: 20
                        font.weight: Font.Medium
                        lineHeightMode: Text.FixedHeight; lineHeight: 24
                        elide: Text.ElideRight
                        color: page.colors["text-primary"]
                    }
                    Text {
                        objectName: "profileStatusText"
                        visible: !(page.editing && page.isSelf) && text.length > 0
                        anchors.top: parent.top
                        anchors.topMargin: 24
                        anchors.left: parent.left
                        anchors.right: parent.right
                        text: page.editing && page.isSelf ? "" : String(page.snapshot.statusText || "")
                        textFormat: Text.PlainText
                        font.pixelSize: 12
                        lineHeightMode: Text.FixedHeight; lineHeight: 16
                        elide: Text.ElideRight
                        color: page.colors["text-secondary"]
                    }
                    Text {
                        objectName: "profilePresenceLine"
                        visible: !(page.editing && page.isSelf) && text.length > 0
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.right: parent.right
                        text: page.presenceText(String(page.snapshot.status || ""))
                        textFormat: Text.PlainText
                        font.pixelSize: 12
                        lineHeightMode: Text.FixedHeight; lineHeight: 16
                        elide: Text.ElideRight
                        color: page.presenceTextColor(String(page.snapshot.status || ""))
                    }
                    Item {
                        objectName: "profileDisplayNameBlock"
                        visible: page.editing && page.isSelf
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.right: parent.right
                        Text {
                            id: displayNameLabel
                            objectName: "profileDisplayNameLabel"
                            anchors.top: parent.top
                            text: qsTr("Display name")
                            textFormat: Text.PlainText
                            font.pixelSize: 12
                            lineHeightMode: Text.FixedHeight; lineHeight: 16
                            color: page.colors["text-secondary"]
                        }
                        Rectangle {
                            objectName: "profileDisplayNamePreview"
                            anchors.top: displayNameLabel.bottom
                            anchors.topMargin: 4
                            anchors.left: parent.left
                            anchors.right: parent.right
                            height: 32
                            radius: 8
                            color: page.colors["card-bg"]
                            Text {
                                anchors.left: parent.left
                                anchors.leftMargin: 8
                                anchors.right: parent.right
                                anchors.rightMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                text: page.displayPreview()
                                textFormat: Text.PlainText
                                font.pixelSize: 12
                                lineHeightMode: Text.FixedHeight; lineHeight: 16
                                elide: Text.ElideRight
                                color: page.colors["text-primary"]
                            }
                        }
                    }
                }
            }

            // ---- Own-profile status lines ----
            Text {
                objectName: "profileLoadingLine"
                visible: text.length > 0
                x: 8
                width: parent.width - 16
                text: page.isSelf && controller.ownProfileBusy ? qsTr("Loading profile…") : ""
                textFormat: Text.PlainText
                font.pixelSize: 14
                color: page.colors["text-secondary"]
            }
            Text {
                objectName: "profileRefreshPendingLine"
                visible: text.length > 0
                x: 8
                width: parent.width - 16
                text: page.isSelf && controller.ownProfileRefreshPending
                    ? qsTr("Profile saved. Workspace could not refresh it yet.") : ""
                textFormat: Text.PlainText
                font.pixelSize: 14
                color: page.colors["text-secondary"]
            }
            Item {
                id: profileErrorBlock
                objectName: "profileErrorBlock"
                visible: profileErrorBlock.visibleError
                x: 8
                width: parent.width - 16
                height: errorText.height + (retryButton.visible ? retryButton.height + 4 : 0)
                readonly property bool visibleError: page.isSelf
                    && page.visible && controller.ownProfileError.length > 0
                Text {
                    id: errorText
                    objectName: "profileErrorLine"
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    text: profileErrorBlock.visibleError ? controller.ownProfileError : ""
                    textFormat: Text.PlainText
                    font.pixelSize: 14
                    color: page.colors["danger"]
                    wrapMode: Text.WordWrap
                }
                Button {
                    id: retryButton
                    objectName: "profileRetryButton"
                    visible: profileErrorBlock.visibleError && !page.controller.ownProfileLoaded
                        && !page.controller.ownProfileBusy
                    anchors.top: errorText.bottom
                    anchors.topMargin: 4
                    anchors.left: parent.left
                    padding: 0
                    text: qsTr("Retry")
                    font.pixelSize: 14
                    display: AbstractButton.TextOnly
                    contentItem: Text {
                        text: retryButton.text
                        textFormat: Text.PlainText
                        font.pixelSize: 14
                        color: page.colors.accent
                    }
                    background: Item {}
                    onClicked: page.controller.loadOwnProfile()
                }
            }
            Text {
                objectName: "profileInvalidLine"
                visible: text.length > 0
                x: 8
                width: parent.width - 16
                text: page.editing && page.isSelf && !page.draftValid()
                    && controller.ownProfileError.length === 0
                    ? qsTr("Check the lengths of the editable fields.") : ""
                textFormat: Text.PlainText
                font.pixelSize: 14
                color: page.colors["text-primary"]
                wrapMode: Text.WordWrap
            }

            // ---- Action chips (hidden while self-editing) ----
            RowLayout {
                objectName: "profileActions"
                visible: !(page.editing && page.isSelf)
                    && String(page.snapshot.title || "").length > 0
                x: 8
                width: parent.width - 16
                spacing: 8
                Repeater {
                    model: page.chipModel()
                    // With ComponentBehavior: Bound the view context injection
                    // can only satisfy required properties declared on the
                    // inline delegate root (the detail rows rely on that);
                    // a separate component root such as ProfileChip must be
                    // fed explicitly, or delegate creation fails at runtime.
                    delegate: Item {
                        id: chipHost
                        required property var modelData
                        Layout.fillWidth: !chipHost.modelData.hug
                        Layout.preferredWidth: profileChip.implicitWidth
                        Layout.preferredHeight: profileChip.implicitHeight
                        ProfileChip {
                            id: profileChip
                            anchors.fill: parent
                            modelData: chipHost.modelData
                        }
                    }
                }
            }

            // Hairline between the avatar block and the details while editing.
            Rectangle {
                objectName: "profileEditHairline"
                visible: page.editing && page.isSelf
                x: 8
                width: parent.width - 16
                height: 1
                color: page.colors["border-subtle"]
            }

            // ---- Detail rows ----
            Column {
                objectName: "profileDetailsColumn"
                x: 8
                width: parent.width - 16
                spacing: 12
                Repeater {
                    model: page.detailModel()
                    delegate: Item {
                        id: detailRow
                        required property var modelData
                        objectName: "profileDetail-" + modelData.id
                        width: parent.width
                        height: modelData.kind === "description" ? 64 : 36
                        Item {
                            id: detailIconBox
                            width: 32; height: 32
                            anchors.bottom: parent.bottom
                            ProfileGlyph {
                                anchors.centerIn: parent
                                asset: detailRow.modelData.icon
                                glyph: 24
                                tint: page.colors["icon-base"]
                            }
                        }
                        Column {
                            objectName: "profileDetailTexts-" + detailRow.modelData.id
                            anchors.left: detailIconBox.right
                            anchors.leftMargin: 12
                            anchors.right: parent.right
                            anchors.rightMargin: 32
                            anchors.bottom: parent.bottom
                            spacing: 4
                            visible: detailRow.modelData.kind === "row"
                            Text {
                                objectName: "profileDetailLabel-" + detailRow.modelData.id
                                width: parent.width
                                text: detailRow.modelData.label
                                textFormat: Text.PlainText
                                font.pixelSize: 12
                                lineHeightMode: Text.FixedHeight; lineHeight: 16
                                elide: Text.ElideRight
                                color: page.colors["text-secondary"]
                            }
                            Text {
                                objectName: "profileDetailValue-" + detailRow.modelData.id
                                width: parent.width
                                text: String(detailRow.modelData.value || "")
                                textFormat: Text.PlainText
                                font.pixelSize: 14
                                font.italic: detailRow.modelData.unavailable === true
                                lineHeightMode: Text.FixedHeight; lineHeight: 16
                                elide: Text.ElideRight
                                color: detailRow.modelData.unavailable === true
                                    ? page.colors["text-muted"] : page.colors["text-primary"]
                            }
                        }
                        Button {
                            id: copyButton
                            objectName: "profileDetailCopy-" + detailRow.modelData.id
                            visible: detailRow.modelData.kind === "row"
                                && detailRow.modelData.copy === true
                            width: 24; height: 24
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            hoverEnabled: true
                            padding: 0
                            Accessible.name: detailRow.modelData.label
                            contentItem: Item {
                                ProfileGlyph {
                                    anchors.centerIn: parent
                                    asset: page.copiedKey === detailRow.modelData.id ? "check.svg" : "copy.svg"
                                    glyph: 24
                                    tint: copyButton.hovered
                                        ? page.colors["text-primary"] : page.colors["text-muted"]
                                }
                            }
                            background: Rectangle {
                                radius: 4
                                color: copyButton.hovered ? page.colors["bg-elevated"] : "transparent"
                            }
                            onClicked: page.copyText(String(detailRow.modelData.value || ""),
                                detailRow.modelData.id)
                        }
                        // Editable field row (first/last/phone).
                        Item {
                            objectName: "profileEditFieldBlock-" + detailRow.modelData.id
                            visible: detailRow.modelData.kind === "edit"
                            anchors.left: detailIconBox.right
                            anchors.leftMargin: 12
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 32
                            TextField {
                                objectName: "profileField-" + detailRow.modelData.field
                                anchors.fill: parent
                                text: detailRow.modelData.field === "firstName" ? page.draftFirst
                                    : detailRow.modelData.field === "lastName" ? page.draftLast : page.draftPhone
                                onTextChanged: {
                                    if (detailRow.modelData.field === "firstName") page.draftFirst = text
                                    else if (detailRow.modelData.field === "lastName") page.draftLast = text
                                    else page.draftPhone = text
                                }
                                enabled: !page.saving
                                font.pixelSize: 14
                                color: page.colors["text-primary"]
                                selectByMouse: true
                                placeholderText: detailRow.modelData.label
                                padding: 0
                                verticalAlignment: TextInput.AlignVCenter
                                background: Rectangle {
                                    radius: 8
                                    color: page.colors["card-bg"]
                                    border.width: 1
                                    border.color: parent.activeFocus
                                        ? page.colors.accent : page.colors["border-subtle"]
                                    opacity: parent.enabled ? 1.0 : 0.6
                                }
                                leftPadding: 12
                                rightPadding: 12
                            }
                        }
                        // Multiline About me field.
                        Item {
                            objectName: "profileDescriptionBlock"
                            visible: detailRow.modelData.kind === "description"
                            anchors.left: detailIconBox.right
                            anchors.leftMargin: 12
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 64
                            // exordy #199 review 4553884709: About me is a
                            // multiline field - a TextArea, not a single-line
                            // TextField, so entered newlines survive the draft.
                            TextArea {
                                objectName: "profileDescriptionField"
                                anchors.fill: parent
                                text: page.draftDescription
                                onTextChanged: page.draftDescription = text
                                enabled: !page.saving
                                font.pixelSize: 14
                                color: page.colors["text-primary"]
                                selectByMouse: true
                                placeholderText: detailRow.modelData.label
                                wrapMode: TextArea.WordWrap
                                bottomPadding: 8
                                topPadding: 8
                                leftPadding: 12
                                rightPadding: 12
                                background: Rectangle {
                                    radius: 8
                                    color: page.colors["card-bg"]
                                    border.width: 1
                                    border.color: parent.activeFocus
                                        ? page.colors.accent : page.colors["border-subtle"]
                                    opacity: parent.enabled ? 1.0 : 0.6
                                }
                            }
                        }
                    }
                }
            }

            // ---- Edit footer ----
            RowLayout {
                objectName: "profileEditFooter"
                visible: page.editing && page.isSelf
                x: 8
                width: parent.width - 16
                spacing: 8
                Button {
                    objectName: "profileCancelButton"
                    Layout.preferredWidth: 110
                    Layout.preferredHeight: 40
                    enabled: !page.saving
                    text: qsTr("Cancel")
                    contentItem: Text {
                        text: parent.text
                        textFormat: Text.PlainText
                        font.pixelSize: 14
                        font.weight: Font.Medium
                        color: page.colors.accent
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: 8
                        color: page.colors["card-bg-active"]
                        opacity: parent.enabled ? 1.0 : 0.6
                    }
                    onClicked: page.cancelEdit()
                }
                Button {
                    objectName: "profileSaveButton"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 40
                    enabled: !page.saving && page.draftValid()
                    text: qsTr("Save changes")
                    contentItem: Text {
                        text: parent.text
                        textFormat: Text.PlainText
                        font.pixelSize: 14
                        font.weight: Font.Medium
                        color: page.colors["on-accent"]
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: 8
                        color: page.colors.accent
                        opacity: parent.enabled ? 1.0 : 0.6
                    }
                    onClicked: page.saveEdit()
                }
            }
        }
    }

    function chipModel() {
        if (page.editing && page.isSelf) return []
        const shareCopied = page.copiedKey === "share"
        if (page.isSelf) {
            return [
                {id: "favorites", label: qsTr("Favorites"), icon: "home.svg", glyph: 26, hug: false, disabled: false},
                // Editing needs the loaded baseline (review 4553884671).
                {id: "edit", label: qsTr("Edit"), icon: "border_color.svg", glyph: 30, hug: true,
                    disabled: !controller.ownProfileLoaded},
                {id: "share", label: shareCopied ? qsTr("Copied") : qsTr("Share"),
                    icon: shareCopied ? "check.svg" : "links.svg", glyph: shareCopied ? 28 : 40,
                    hug: false, disabled: page.shareLink.length === 0},
            ]
        }
        const messagePending = controller.profileDirectPending || controller.directChatBusy
        return [
            {id: "message", label: qsTr("Message"), icon: "chat_bubble.svg", glyph: 28, hug: false,
                disabled: messagePending},
            // Issue #164: the Call chip is live (right-panel-user-profile-actions
            // .ui.tsx:113-120, disabled={callPending}); native additionally
            // refuses E2EE DMs without a local key (recorded deviation).
            // callTargetCryptoUnavailable() is a plain method call that QML
            // cannot observe on its own, so the notifying callCryptoSessionReady
            // mirror must stay inside this binding for the chip to re-evaluate on
            // every stream-catalog, viewed-chat or local-send-key transition.
            // The probe may NOT be short-circuited behind that mirror: an E2EE DM
            // with no local key reports unavailable exactly while the mirror is
            // false (round-2 finding 4563207117). The comma reads the mirror on
            // every evaluation (capturing the NOTIFY dependency) while the bound
            // value stays "call pending or target crypto unavailable".
            {id: "call", label: qsTr("Call"), icon: "phone.svg", glyph: 30, hug: false,
                disabled: controller.profileCallPending
                    || (controller.callCryptoSessionReady,
                        controller.callTargetCryptoUnavailable(page.userUuid))},
            {id: "share", label: shareCopied ? qsTr("Copied") : qsTr("Share"),
                icon: shareCopied ? "check.svg" : "links.svg", glyph: shareCopied ? 28 : 40,
                hug: false, disabled: page.shareLink.length === 0},
        ]
    }

    // ---- Edit avatar dialog (reference -edit-avatar-dialog.ui.tsx) ----
    Popup {
        id: avatarDialog
        objectName: "profileAvatarDialog"
        parent: page
        width: 323
        padding: 0
        modal: false
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        x: Math.max(-12, (page.width - width) / 2)
        y: Math.max(8, (page.height - implicitHeight) / 2)
        background: Rectangle {
            radius: 12
            color: page.colors["bg-elevated"]
            border.width: 1
            border.color: page.colors["border-subtle"]
            Accessible.name: qsTr("Change avatar")
        }
        contentItem: Column {
            spacing: 20
            leftPadding: 12
            rightPadding: 12
            topPadding: 20
            bottomPadding: 20
            // Reference action row block: gap-2 (8 px) between the two rows.
            Column {
                width: parent.width - 24
                spacing: 8
                ProfileDialogRow {
                    objectName: "profileAvatarChoose"
                    width: parent.width
                    label: qsTr("Choose from gallery")
                    asset: "images.svg"
                    onClicked: {
                        page.avatarErrorLocal = ""
                        profileFileDialog.open()
                    }
                }
                ProfileDialogRow {
                    objectName: "profileAvatarRemove"
                    width: parent.width
                    label: qsTr("Delete current photo")
                    asset: "delete.svg"
                    tone: page.colors["call-red"]
                    glyphTint: page.colors["call-red"]
                    // Disabled when there is no avatar and nothing pending (reference
                    // lines 320-323).
                    enabled: String(page.snapshot.avatar || "").length > 0
                        || page.pendingAvatarPath.length > 0
                        || page.pendingRemoveAvatar
                    onClicked: {
                        page.avatarErrorLocal = ""
                        if (page.pendingAvatarPath.length > 0) page.pendingAvatarPath = ""
                        else page.pendingRemoveAvatar = true
                    }
                }
            }
            Text {
                objectName: "profileAvatarError"
                visible: text.length > 0
                width: parent.width - 24
                text: page.avatarErrorLocal
                textFormat: Text.PlainText
                font.pixelSize: 12
                wrapMode: Text.WordWrap
                color: page.colors["danger"]
            }
            Button {
                objectName: "profileAvatarCancel"
                width: parent.width - 24
                height: 40
                text: qsTr("Cancel")
                contentItem: Text {
                    text: parent.text
                    textFormat: Text.PlainText
                    font.pixelSize: 14
                    font.weight: Font.Medium
                    color: page.colors.accent
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: 8
                    color: page.colors["card-bg-active"]
                }
                onClicked: avatarDialog.close()
            }
        }
    }

    NativeDialogs.FileDialog {
        id: profileFileDialog
        objectName: "profileFileDialog"
        title: qsTr("Choose from gallery")
        nameFilters: ["Images (*.png *.jpg *.jpeg *.gif *.webp)"]
        onAccepted: {
            const path = selectedFile.toString()
            const local = path.startsWith("file://") ? path.substring(7) : path
            page.pendingRemoveAvatar = false
            page.pendingAvatarPath = decodeURIComponent(local)
        }
    }
}
