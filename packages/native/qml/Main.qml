pragma ComponentBehavior: Bound

import QtQuick
import QtQml
import QtQuick.Controls
import QtQuick.Controls.impl as ControlsImpl
import QtQuick.Layouts
import QtQuick.Effects
import QtQuick.Dialogs as NativeDialogs

ApplicationWindow {
    id: root
    objectName: "workspaceNativeWindow"
    width: 1440
    height: 900
    minimumWidth: 700
    minimumHeight: 600
    visible: true
    title: root.controller.title.length ? root.controller.title : qsTr("Workspace")
    color: colors.bg
    font.pixelSize: 14
    palette.window: colors["bg-elevated"]
    palette.windowText: colors["text-primary"]
    palette.base: colors["text-field-bg"]
    palette.text: colors["text-primary"]
    palette.button: colors["card-bg"]
    palette.buttonText: colors["text-primary"]
    palette.highlight: colors.accent
    palette.highlightedText: colors["on-accent"]

    // The controller is injected by the C++ application context.
    // qmllint disable unqualified
    readonly property var controller: nativeController
    readonly property var markdown: nativeMarkdown
    readonly property var preferences: nativePreferences
    readonly property var updater: nativeUpdater
    readonly property var messageImages: nativeMessageImages
    // Per-window reveal level; stream expansion itself is persisted by owner.
    property var sidebarTopicRevealLevels: ({})
    // Issue #107: the platform reduced-motion preference (XDG Settings portal
    // org.freedesktop.appearance/reduced-motion, surfaced by NativePreferences),
    // the native stand-in for the reference prefers-reduced-motion media query
    // that freezes the skeleton pulse (docs/NATIVE_DESKTOP_ANCHOR_TRANSITION.md).
    property bool prefersReducedMotion: root.preferences.prefersReducedMotion
    Connections {
        target: root.controller
        function onStateChanged() {
            if (!root.controller.authenticated && Object.keys(root.sidebarTopicRevealLevels).length)
                root.sidebarTopicRevealLevels = ({});
        }
    }
    function setSidebarTopicReveal(id, level) {
        const next = Object.assign({}, sidebarTopicRevealLevels);
        next[id] = level;
        sidebarTopicRevealLevels = next;
    }
    property bool updatesVisible: false
    property bool aboutFromProfile: false
    function openAbout(fromProfile) {
        if (!updatesVisible)
            aboutFromProfile = fromProfile;
        accountPopup.close();
        updatesVisible = true;
        aboutDrawer.forceActiveFocus();
    }
    onUpdatesVisibleChanged: {
        if (updatesVisible) {
            accountPopup.close();
            aboutDrawer.forceActiveFocus();
        } else {
            aboutFromProfile = false;
        }
    }
    // qmllint enable unqualified
    NativeTheme {
        id: theme
        paletteId: root.preferences.paletteId
        dark: root.preferences.dark
    }
    readonly property var colors: theme.colors
    NativeAboutDrawer {
        id: aboutDrawer
        anchors.right: parent.right
        anchors.top: root.controller.authenticated ? topBar.bottom : parent.top
        anchors.topMargin: root.controller.authenticated ? 4 : 8
        anchors.bottom: parent.bottom
        visible: root.updatesVisible && !!root.updater
        updater: root.updater
        colors: root.colors
        canGoBack: root.aboutFromProfile
        onCloseRequested: root.updatesVisible = false
        onLicensesRequested: {
            root.updatesVisible = false;
            root.controller.showSection("licenses");
        }
        onBackRequested: {
            root.updatesVisible = false;
            accountPopup.open();
        }
    }
    // Right drawer shell (issue #129): docks beside the chat like aboutDrawer.
    // The channel info content (issue #132) mounts on the body slot as the
    // chat-info root screen content.
    NativeRightDrawer {
        id: rightDrawer
        anchors.right: parent.right
        anchors.top: root.controller.authenticated ? topBar.bottom : parent.top
        anchors.topMargin: root.controller.authenticated ? 4 : 8
        anchors.bottom: parent.bottom
        colors: root.colors
        // Favorites forces the right drawer closed when it mounts
        // (chat-page.ui.tsx:40-42 setRightDrawerOpen(false) plus
        // useCloseFavoritesRightDrawer; issue #70).
        readonly property bool favoritesActive: root.controller.favoritesVisible
        onFavoritesActiveChanged: if (favoritesActive) rightDrawer.close()
        NativeChannelInfo {
            id: channelInfoLayer
            objectName: "channelInfoLayer"
            colors: root.colors
            controller: root.controller
            visible: rightDrawer.topLayerKind === "chat-info"
                && rightDrawer.currentMode === "info"
                && rightDrawer.currentChatScreen === ""
                && rightDrawer.currentProfileOverride === ""
                // Issue #243: a protected chat shows the encrypted presentation
                // instead (reference right-panel-workspace-info.ui.tsx:829-845).
                && !root.encryptedConversation
        }
        // Encrypted chat info (issue #243, docs/NATIVE_DESKTOP_E2EE_CHAT_INFO.md):
        // the reference screen switch, mounted as the chat-info layer content.
        // The shell keeps the chrome, so this layer only renders the active
        // screen body and routes navigation back through openChatScreen.
        NativeEncryptedChatInfo {
            id: encryptedChatInfoLayer
            objectName: "encryptedChatInfoLayer"
            colors: root.colors
            controller: root.controller
            localeName: root.preferences.locale
            chatScreen: rightDrawer.currentChatScreen === "" ? "info" : rightDrawer.currentChatScreen
            keyUuid: rightDrawer.currentChatKeyUuid
            visible: rightDrawer.topLayerKind === "chat-info"
                && rightDrawer.currentMode === "info"
                && rightDrawer.currentProfileOverride === ""
                && root.encryptedConversation
            onScreenChange: function (screen) {
                const scope = root.rightDrawerChatScope
                rightDrawer.openChatScreen({ownerKey: scope.ownerKey,
                    runtimeGeneration: scope.runtimeGeneration,
                    streamUuid: scope.streamUuid, screen: screen})
            }
            // Saved-key details push the local-key screen onto the chat stack
            // (right-drawer.model.ts openChatScreen contract, #244).
            onLocalKeyOpen: function (keyUuid) {
                const scope = root.rightDrawerChatScope
                rightDrawer.openChatScreen({ownerKey: scope.ownerKey,
                    runtimeGeneration: scope.runtimeGeneration,
                    streamUuid: scope.streamUuid, screen: "local-key", keyUuid: keyUuid})
            }
            // Member management stays available in the protected drawer: the
            // add affordance opens the one shared add-members dialog (the
            // ordinary layer owns it; the dialog parents to the overlay, so
            // the hidden ordinary layer does not suppress it).
            onAddMembersRequested: channelInfoLayer.openAddMembers()
        }
        // User profile content (issue #134, docs/NATIVE_DESKTOP_USER_PROFILE.md):
        // the chat-info profile override for other users and the account
        // personal-info screen for the verified session user. All loads are
        // visibility-gated inside the page (#197 lesson: no hidden fetches).
        NativeUserProfile {
            id: profileLayer
            objectName: "profileLayer"
            colors: root.colors
            controller: root.controller
            locale: root.preferences.locale
            readonly property bool selfScreen: rightDrawer.topLayerKind === "account"
                && rightDrawer.currentAccountScreen === "personal-info"
            readonly property string overrideUser: rightDrawer.currentProfileOverride.startsWith("workspace-user-uuid:")
                ? rightDrawer.currentProfileOverride.substring("workspace-user-uuid:".length) : ""
            userUuid: selfScreen ? root.controller.currentUserId : overrideUser
            visible: rightDrawer.drawerOpen && (selfScreen
                || (rightDrawer.topLayerKind === "chat-info" && rightDrawer.currentMode === "info"
                    && userUuid.length > 0
                    && rightDrawer.currentProfileOverride === "workspace-user-uuid:" + userUuid))
        }
        title: profileLayer.visible
            ? (profileLayer.isSelf ? qsTr("Personal info") : qsTr("Information"))
            : ""
    }
    // While the add-members dialog is open its Escape closes only the dialog;
    // the drawer layer shortcut must stay silent until the dialog settles.
    Binding {
        target: rightDrawer
        property: "enabled"
        value: !channelInfoLayer.addDialogOpen
        when: root.controller.authenticated
    }
    // The reference runtime generation is a login-generation counter: a new
    // authenticated session drops hidden chat screens of the previous runtime.
    // `authenticated` notifies through stateChanged, so its QML changed
    // handler re-fires on every controller state change: the generation may
    // only move on an actual login transition, otherwise every live update
    // would bump the scope generation and pop the open chat screens.
    property int rightDrawerRuntimeGeneration: 0
    property bool rightDrawerAuthenticated: false
    readonly property var rightDrawerChatScope: ({
        ownerKey: root.controller.ownerKey,
        runtimeGeneration: root.rightDrawerRuntimeGeneration,
        streamUuid: root.controller.selectedStream
    })
    function resetRightDrawerChatScreens() {
        // The live scope is only meaningful for an authenticated session. The
        // unauthenticated projection reports an empty scope, and treating that
        // as a scope change let a controller signal emitted inside a drawer
        // change cascade (the reveal-driven key refresh firing stateChanged)
        // wipe the open chat screens mid-cascade and trip binding-loop
        // detection on topScreen (Bug #300). Logout already drops everything
        // via clearAll, so an unauthenticated reset can only ever act on
        // unknown information.
        if (!root.controller.authenticated)
            return;
        rightDrawer.resetChatScreens(root.rightDrawerChatScope);
    }
    onRightDrawerRuntimeGenerationChanged: root.resetRightDrawerChatScreens()
    Connections {
        target: root.controller
        function onProfileLinkStarted() { rightDrawer.clearAll() }
        function onProfileLinkOpened(userUuid) { rightDrawer.openWorkspaceUserProfile(userUuid) }
        function onStateChanged() {
            const authenticated = root.controller.authenticated
            if (authenticated !== root.rightDrawerAuthenticated) {
                root.rightDrawerAuthenticated = authenticated
                if (authenticated)
                    root.rightDrawerRuntimeGeneration += 1
                else
                    rightDrawer.clearAll()
            }
            root.resetRightDrawerChatScreens();
        }
    }
    readonly property real preferredSidebarWidth: root.preferences.sidebarPreferredWidth
    readonly property real sidebarMaximum: Math.max(240, Math.min(720, Math.round(width * 0.4)))
    readonly property real sidebarMinimum: Math.min(sidebarMaximum, Math.max(240, Math.round(width * 0.12)))
    readonly property real sidebarWidth: Math.max(sidebarMinimum, Math.min(sidebarMaximum, preferredSidebarWidth))
    function selectedStreamIsEncrypted(source) {
        const selected = source.selectedStream;
        if (!selected)
            return true;
        const streams = source.streams;
        for (let i = 0; i < streams.length; ++i) {
            if (streams[i].id === selected)
                return Boolean(streams[i].encrypted);
        }
        const archived = source.archivedChats;
        for (let i = 0; i < archived.length; ++i) {
            if (archived[i].id === selected)
                return Boolean(archived[i].encrypted);
        }
        return true;
    }
    readonly property bool selectedStreamEncrypted: selectedStreamIsEncrypted(root.controller)
    readonly property var chatEncryption: root.controller.encryption
    readonly property bool encryptedConversation: root.controller.authenticated && root.controller.hasConversation && root.selectedStreamEncrypted
    readonly property string encryptedStreamName: root.controller.selectedStream.length ? root.selectedName(root.controller.streams, root.controller.selectedStream) || root.selectedName(root.controller.archivedChats, root.controller.selectedStream) : ""
    // Issue #90 (docs/NATIVE_DESKTOP_E2EE_HEADER_AVATAR.md): the reference
    // paints the encrypted header lock avatar with
    // resolveAvatarBackgroundColor(stream.color) (chat-header-channel.ui.tsx:56,
    // avatar-color.lib.ts:2-7: integer 0..0xffffff -> #rrggbb with 0 -> #000000,
    // null/invalid -> the Avatar bg-bg theme fallback, avatar.tsx:61). The
    // streams() projection already carries that resolution as avatarColor
    // (native-controller.cpp:12996-12997, DTO admission domain.cpp:492-493);
    // archived rows expose the raw integer color (native-controller.cpp:12907),
    // hexed here with the same padding. Pure binding over existing projections
    // (same pattern as the sidebar avatar, Main.qml:4986-4988): no new fetch,
    // no new signal. Empty string means "no stream color" - callers must test
    // the length, never falsiness, so 0 keeps its #000000 paint.
    readonly property string encryptedHeaderAvatarColor: {
        const selected = root.controller.selectedStream
        if (!selected.length) return ""
        const streams = root.controller.streams
        for (let i = 0; i < streams.length; ++i)
            if (streams[i].id === selected) return String(streams[i].avatarColor || "")
        const archived = root.controller.archivedChats
        for (let i = 0; i < archived.length; ++i) {
            if (archived[i].id !== selected) continue
            const color = archived[i].color
            return Number.isInteger(color) && color >= 0 && color <= 0xffffff
                ? "#" + Number(color).toString(16).padStart(6, "0") : ""
        }
        return ""
    }
    // Issue #89 (docs/NATIVE_DESKTOP_CHAT_HEADER_COUNTERS.md): the caption of
    // chat-header-channel.ui.tsx - t("channel.participants", {count}) + ", " +
    // t("channel.online", {count}). The plural sources match the reviewed
    // catalog entries of the channel info subline (NativeChannelInfo.qml).
    function chatHeaderCountersText() {
        const members = root.controller.chatHeaderMemberCount
        const online = root.controller.chatHeaderOnlineCount
        const membersLabel = members === 1 ? qsTr("%n member", "", members)
                                           : qsTr("%n members", "", members)
        return membersLabel + ", " + qsTr("%n online", "", online)
    }
    readonly property var encryptedReadSummary: {
        const result = { ready: 0, missing: 0, error: 0, preparing: 0, currentReady: false };
        if (!root.encryptedConversation)
            return result;
        const messages = root.controller.messages;
        for (let i = 0; i < messages.length; ++i) {
            const message = messages[i];
            if (message.kind === "e2ee" && ["ready", "missing", "error", "preparing"].indexOf(message.encryptionState) >= 0) {
                ++result[message.encryptionState];
                // A completed request may concern historical A while the
                // catalog names current B. Only exact readable B proves B.
                if (message.encryptionState === "ready" && Boolean(message.decrypted) && String(root.chatEncryption.currentKeyUuid || "").length > 0 && message.encryptionKeyUuid === root.chatEncryption.currentKeyUuid)
                    result.currentReady = true;
            }
        }
        return result;
    }
    function encryptedPreparationState(message) {
        return String(message.encryptionState || "locked");
    }
    function encryptionControlLabel(kind) {
        switch (kind) {
        case "key_request": return qsTr("Key requested");
        case "key_grant": return qsTr("Encrypted key grant received");
        case "key_announced": return qsTr("New key announced");
        case "key_reject": return qsTr("Key request rejected");
        default: return "";
        }
    }
    // One public metadata snapshot/index per encryption binding evaluation.
    // Delegates only look up their exact request UUID; no getter or SQL loop.
    readonly property var incomingKeyRequestsById: {
        const result = {};
        if (!root.controller.authenticated || !root.controller.hasConversation)
            return result;
        const requests = root.chatEncryption.keyRequests || [];
        const count = requests.length;
        for (let i = 0; i < count; ++i) {
            const request = requests[i];
            if (request && String(request.id || "").length)
                result[String(request.id)] = request;
        }
        return result;
    }
    function ownsKeyRequest(request) {
        return Boolean(request && String(request.authorId || "").length
            && String(request.authorId) === String(root.controller.account.uuid || ""));
    }
    function incomingKeyRequestTitle(request) {
        if (!request || !String(request.id || "").length)
            return "";
        const device = String(request.deviceName || "").trim() || qsTr("Unknown device");
        return (root.ownsKeyRequest(request)
            ? qsTr("You requested a security key for device “%1”")
            : qsTr("requested a security key for device “%1”")).arg(device);
    }
    function incomingKeyRequestDescription(request) {
        if (!request || !String(request.id || "").length || request.status === "granted" || request.status === "rejected")
            return "";
        if (Boolean(request.ownCurrentDevice))
            return Boolean(request.canManageKeys)
                ? qsTr("Approve the request on another device with the key or wait for access to be granted")
                : qsTr("Wait for access to be granted to read the messages");
        if (Boolean(request.canManageKeys))
            return Boolean(request.hasRequestedKey) ? "" : qsTr("This device does not have the requested key");
        return root.ownsKeyRequest(request) ? qsTr("Wait for access to be granted to read the messages") : "";
    }
    function incomingKeyRequestStatus(request) {
        return request && request.status === "granted" ? qsTr("Key sent")
            : request && request.status === "rejected" ? qsTr("Request rejected.") : "";
    }
    function keyServiceResponseLabel(template, values) {
        return template.replace(/%([1-3])/g, (_, index) => String(values[Number(index) - 1]));
    }
    function keyServiceResponseTitle(message, response) {
        const grant = message.kind === "key_grant";
        const sender = String(response.senderName || "").trim() || "#" + String(message.authorId || "").slice(0, 8);
        if (!Boolean(response.recipientKnown) || !String(response.recipientId || "").length)
            return root.keyServiceResponseLabel(grant ? qsTr("%1 sent an encrypted security key") : qsTr("%1 rejected the key request"), [sender]);
        const viewer = String(root.controller.account.uuid || "");
        const ownSender = viewer.length > 0 && String(message.authorId) === viewer;
        const ownRecipient = viewer.length > 0 && String(response.recipientId) === viewer;
        const recipient = String(response.recipientName || "").trim() || "#" + String(response.recipientId).slice(0, 8);
        const device = String(response.deviceName || "").trim() || qsTr("Unknown device");
        if (ownSender && ownRecipient)
            return root.keyServiceResponseLabel(grant ? qsTr("You sent a security key for device “%1”")
                          : qsTr("You rejected the security key request for device “%1”"), [device]);
        if (ownSender)
            return root.keyServiceResponseLabel(grant ? qsTr("You sent a security key to %1 for device “%2”")
                          : qsTr("You rejected the security key request from %1 for device “%2”"), [recipient, device]);
        if (ownRecipient)
            return root.keyServiceResponseLabel(grant ? qsTr("%1 sent you a security key for device “%2”")
                          : qsTr("%1 rejected your security key request for device “%2”"), [sender, device]);
        return root.keyServiceResponseLabel(grant ? qsTr("%1 sent a security key to %2 for device “%3”")
                      : qsTr("%1 rejected the security key request from %2 for device “%3”"), [sender, recipient, device]);
    }
    function presenceLabel(presence) {
        if (presence === "active") return qsTr("online");
        if (presence === "idle") return qsTr("away");
        if (presence === "offline") return qsTr("offline");
        return "";
    }
    // Issue #177: dm.partnerBlocked - en source string, ru catalog entry.
    function accountDeactivatedLabel() { return qsTr("Account deactivated"); }
    // Issue #88 (reference chat-header.lib.ts:4-27, see
    // docs/NATIVE_DESKTOP_DM_HEADER_STATUS.md): the resolveDmStatusText
    // priority chain on the directPeer projection. A presence value outside
    // active/idle, including the empty "data unavailable" value, falls to the
    // last-seen branch exactly like the contract: an unknown user shows the
    // offline caption, never online.
    function directStatusCaption() {
        const peer = root.controller.directPeer
        if (peer.accountDeactivated === true)
            return root.accountDeactivatedLabel()
        // Reference branch 2 (chat.typing). No handled realtime kind carries
        // typing today, so directPeer.typing stays unset until the wire
        // provides it; the branch matches the contract shape meanwhile.
        if (peer.typing === true)
            return qsTr("Typing")
        const custom = String(peer.customStatus || "")
        if (custom.length > 0)
            return custom
        const online = qsTr("online")
        if (peer.presence === "active")
            return online
        if (peer.presence === "idle")
            return qsTr("away")
        const lastSeen = String(peer.lastSeen || "")
        if (lastSeen.length === 0)
            return qsTr("offline")
        if (lastSeen === online)
            return online
        return qsTr("last seen %1").arg(lastSeen)
    }
    component PresenceBadge: Item {
        id: badge
        property string presence: ""
        width: 12; height: 12
        x: parent ? parent.width * 0.8535 - 6 : 0
        y: parent ? parent.height * 0.8535 - 6 : 0
        z: 1
        visible: presence === "active" || presence === "idle" || presence === "offline"
        Accessible.role: Accessible.StatusBar
        Accessible.name: root.presenceLabel(presence)
        Accessible.ignored: !visible
        Rectangle {
            anchors.fill: parent
            radius: 6
            color: root.colors.bg
            Accessible.ignored: true
        }
        Rectangle {
            objectName: "presence-fill"
            x: 2; y: 2; width: 8; height: 8; radius: 4
            color: root.colors[badge.presence === "active" ? "indicator-green"
                    : badge.presence === "idle" ? "indicator-orange" : "text-muted"]
            Accessible.ignored: true
        }
    }

    component ChatPickerRowBody: Row {
        id: picker
        required property var user
        required property string prefix
        readonly property string secondary: user.statusText || user.email || ""
        spacing: 8
        PresenceBadge {
            objectName: picker.prefix + "-presence-" + picker.user.id
            presence: picker.user.presence || "offline"
            anchors.verticalCenter: parent.verticalCenter
        }
        Column {
            width: Math.max(0, picker.width - 20)
            anchors.verticalCenter: parent.verticalCenter
            Text {
                objectName: picker.prefix + "-name-" + picker.user.id
                width: parent.width
                height: 20
                text: picker.user.displayName
                textFormat: Text.PlainText
                color: root.colors["text-primary"]
                font.pixelSize: 14
                font.weight: Font.Medium
                elide: Text.ElideRight
            }
            Text {
                objectName: picker.prefix + "-email-" + picker.user.id
                width: parent.width
                height: 15.6875
                visible: picker.secondary.length > 0
                text: picker.secondary
                textFormat: Text.PlainText
                color: root.colors["text-secondary"]
                font.pixelSize: 11
                elide: Text.ElideRight
            }
        }
    }
    component InlineKeyRequestAction: NativeButton {
        id: serviceAction
        colors: root.colors
        implicitWidth: contentItem.implicitWidth
        implicitHeight: 20
        height: 20
        padding: 0; horizontalPadding: 0; verticalPadding: 0
        font.pixelSize: 14; font.weight: Font.Normal; font.underline: true
        contentItem: Text {
            text: serviceAction.text; textFormat: Text.PlainText
            font: serviceAction.font; color: root.colors["text-primary"]
            lineHeightMode: Text.FixedHeight; lineHeight: 20
            opacity: serviceAction.enabled ? 1 : 0.5
        }
        background: Rectangle {
            color: "transparent"; radius: 2
            Rectangle {
                x: -4; y: -4; width: parent.width + 8; height: parent.height + 8
                color: "transparent"; radius: 6
                border.width: 2; border.color: root.colors.accent
                visible: serviceAction.visualFocus
            }
        }
    }
    // Issue #221: the ghost-underline Retry inside the encrypted message error row
    // (reference encrypted-message-placeholder.ui.tsx:32-47: ghost neutral sm Button with
    // !h-auto !p-0 underline, disabled opacity 50). Same ghost-link convention as
    // InlineKeyRequestAction, riding the row's 20 px text-sm line in text-muted.
    component InlineDecryptRetryAction: NativeButton {
        id: decryptRetryAction
        colors: root.colors
        implicitWidth: contentItem.implicitWidth
        implicitHeight: 20
        height: 20
        padding: 0; horizontalPadding: 0; verticalPadding: 0
        font.pixelSize: 14; font.weight: Font.Normal; font.underline: true
        contentItem: Text {
            text: decryptRetryAction.text; textFormat: Text.PlainText
            font: decryptRetryAction.font; color: root.colors["text-muted"]
            lineHeightMode: Text.FixedHeight; lineHeight: 20
            opacity: decryptRetryAction.enabled ? 1 : 0.5
        }
        background: Rectangle {
            color: "transparent"; radius: 2
            Rectangle {
                x: -4; y: -4; width: parent.width + 8; height: parent.height + 8
                color: "transparent"; radius: 6
                border.width: 2; border.color: root.colors.accent
                visible: decryptRetryAction.visualFocus
            }
        }
    }
    function requestMessageKey(id) {
        const message = root.currentMessage(id);
        if (!message || message.kind !== "e2ee" || Boolean(message.decrypted) || !Boolean(message.canRequestKey) || !root.chatEncryption.canRequest || !String(message.encryptionKeyUuid || "").length)
            return;
        messageMenu.close();
        root.controller.requestChatKey(String(message.encryptionKeyUuid));
    }
    readonly property bool composerAvailable: root.controller.authenticated && !root.controller.draftsVisible && (root.selectedStreamEncrypted ? root.controller.canCompose : root.controller.selectedTopic.length > 0 && !root.controller.topicClosed)
    // Poll bubble interactivity gate (issue #216): the reference and the
    // #214 controller share supportsMessengerPolls; canCreatePoll is its
    // single native expression, re-evaluated when the stream changes.
    readonly property bool pollInteractiveStream: root.controller.selectedStream.length > 0
        && root.controller.canCreatePoll()
    property string composerEmojiContext: ""
    property string composerEmojiStream: ""
    property string composerEmojiTopic: ""
    property string composerEmojiKind: ""
    readonly property bool composerEmojiAllowed: root.composerAvailable
        && root.composerEmojiContext.length > 0 && root.composerEmojiContext === root.controller.ownStatusContextKey
        && root.composerEmojiStream === root.controller.selectedStream && root.composerEmojiTopic === root.controller.selectedTopic
        && root.composerEmojiKind === root.controller.conversationKind
    function settleComposerEmojiUi() {
        if (composerEmojiPicker.visible && !root.composerEmojiAllowed)
            composerEmojiPicker.close();
    }

    function positionComposerEmojiPicker() {
        const point = composerEmojiButton.mapToItem(root.contentItem, 0, 0);
        const width = composerEmojiPicker.width, height = composerEmojiPicker.height;
        composerEmojiPicker.x = Math.max(8, Math.min(root.width - width - 8, point.x + composerEmojiButton.width / 2 - width / 2));
        const above = point.y - height - 8;
        composerEmojiPicker.y = above >= 8 ? above : Math.max(8, Math.min(root.height - height - 8, point.y + composerEmojiButton.height + 8));
    }
    // ---- Issue #123 composer @mention popup (docs/NATIVE_DESKTOP_COMPOSER_MENTION.md) ----
    // ComposerMentionTrigger.REMINDER: "@query" only counts as a mention token at
    // a line start or after whitespace/punctuation, never inside "name@example".
    function composerMentionTokenBefore(caretText) {
        return /(?:^|[\s([{,.:;!?])([@#])(\S*)$/.exec(caretText);
    }
    // WorkspaceAvatar fallback: first character of the display name, then of
    // the username, then "?" (message-composer-mention-dropdown.ui.tsx:78).
    function composerMentionInitials(row) {
        if (!row)
            return "?";
        const name = String(row.displayName || "").trim();
        if (name.length)
            return name.slice(0, 1);
        const username = String(row.username || "").trim();
        return username.length ? username.slice(0, 1) : "?";
    }
    function detectComposerMentions() {
        const match = root.composerMentionTokenBefore(composerText.text.slice(0, composerText.selectionStart));
        if (!match || !composerText.activeFocus || !composerText.enabled
            || !root.controller.composerMentionsEnabled()
            || ownStatusDialog.visible || deleteDialog.visible || createTopicDialog.visible
            || createChatDialog.visible || referenceNotice.visible || attachmentPicker.visible
            || attachmentSaveDialog.visible) {
            composerMentionPopup.close();
            return;
        }
        composerMentionPopup.query = match[2];
        composerMentionPopup.tokenStart = composerText.selectionStart - match[2].length - 1;
        // Positional snapshot: each keystroke replaces the whole array so no
        // delegate can dangle over a refreshed C++ candidate list.
        composerMentionPopup.references = match[1] === "#";
        composerMentionPopup.rows = composerMentionPopup.references
            ? root.controller.composerReferences(match[2]) : root.controller.mentionSuggestions(match[2]);
        composerMentionPopup.activeIndex = 0;
        root.positionComposerMentionPopup();
        composerMentionPopup.open();
    }
    function positionComposerMentionPopup() {
        const point = composerText.mapToItem(root.contentItem, 0, 0);
        const width = composerMentionPopup.width, height = composerMentionPopup.height;
        composerMentionPopup.x = Math.max(8, Math.min(root.width - width - 8, point.x));
        const above = point.y - height - 6;
        composerMentionPopup.y = above >= 8 ? above
            : Math.max(8, Math.min(root.height - height - 8, point.y + composerText.height + 6));
    }
    function toggleComposerEmojiPicker() {
        if (composerEmojiPicker.visible) {
            composerEmojiPicker.close();
            return;
        }
        if (!root.composerAvailable || !composerText.enabled || !root.controller.ownStatusContextKey.length)
            return;
        root.composerEmojiContext = root.controller.ownStatusContextKey;
        root.composerEmojiStream = root.controller.selectedStream;
        root.composerEmojiTopic = root.controller.selectedTopic;
        root.composerEmojiKind = root.controller.conversationKind;
        composerEmojiChoices.reset();
        root.positionComposerEmojiPicker();
        composerEmojiPicker.open();
    }
    function pickComposerEmoji(name) {
        if (!root.composerEmojiAllowed || !composerText.enabled)
            return;
        const entry = composerEmojiChoices.byName[name];
        const unicode = entry && entry.pickerEligible ? String(entry.pickerUnicode || "").trim() : "";
        if (!unicode.length)
            return;
        root.controller.draft = String(root.controller.draft) + unicode;
        if (!root.composerEmojiAllowed)
            return;
        composerText.forceActiveFocus(Qt.OtherFocusReason);
        composerText.cursorPosition = composerText.length;
    }
    readonly property var pendingOutgoing: root.controller.outgoingSend || ({})
    readonly property var encryptedPendingSend: root.encryptedConversation && String(root.pendingOutgoing.id || "").length > 0 ? root.pendingOutgoing : root.chatEncryption.pendingSend || ({})
    readonly property bool encryptedComposerHasKey: root.encryptedConversation && String(root.chatEncryption.sendingKeyUuid || "").length > 0
    readonly property bool encryptedSendingKeyOutdated: root.encryptedComposerHasKey && String(root.chatEncryption.sendingKeyUuid) !== String(root.chatEncryption.currentKeyUuid || "")
    readonly property string encryptionKeyNoticeId: root.controller.ownerKey + "/" + root.controller.selectedStream + "/" + String(root.chatEncryption.currentKeyUuid || "")
    property string dismissedEncryptionKeyNotice: ""
    readonly property bool encryptedKeyNoticeVisible: root.encryptedSendingKeyOutdated && root.dismissedEncryptionKeyNotice !== root.encryptionKeyNoticeId
    readonly property bool aggregateConversation: root.controller.conversationKind === "stream"
    readonly property var aggregateTopics: root.controller.topics.filter(topic => String(topic.name || "").trim().length > 0)
    property string sidebarQuery: ""
    readonly property string normalizedSidebarQuery: sidebarQuery.trim().toLocaleLowerCase()
    onNormalizedSidebarQueryChanged: root.controller.setSidebarSearchQuery(normalizedSidebarQuery)
    // Issue #100: the reference sidebar catalog state machine projection
    // (docs/NATIVE_DESKTOP_SIDEBAR_STATES.md).
    readonly property var sidebarCatalogState: root.controller.sidebarCatalogState
    property date calendarDate: new Date()
    // Issue #7 slice 2 (docs/NATIVE_DESKTOP_MULTI_FORWARD.md): message
    // multi-select for forwarding. The reference keeps the selection as a set
    // of uuids on the chat page (chat-page-workspace.ui.tsx:606); selection
    // mode starts with the first toggle (selectionMode = size > 0, :1257) and
    // switching conversations empties it (:1707). The bar carries the count
    // and the Forward action (chat-page-selection-bar.ui.tsx).
    property var messageSelectionIds: []
    // Issue #41: the starred card forward flow opens the same forward modal
    // with [uuid] without touching the chat selection (the reference forward
    // store is independent, workspace-forward-message.model.ts:43-52).
    property var starredForwardIds: []
    readonly property int messageSelectionCount: root.messageSelectionIds.length
    // The composer shell (and with it the selection bar) is not present on
    // closed topics, aggregates or locked encrypted chats, so selection is
    // only offered where the bar can render.
    readonly property bool messageSelectionSurface: !root.aggregateConversation
        && (root.encryptedConversation ? root.encryptedComposerHasKey : !root.controller.topicClosed)
    readonly property string messageSelectionConversation:
        root.controller.selectedStream + "/" + root.controller.selectedTopic
    onMessageSelectionConversationChanged: root.clearMessageSelection()
    function isMessageSelected(id) {
        return root.messageSelectionIds.indexOf(String(id)) >= 0
    }
    function toggleMessageSelection(id) {
        const key = String(id)
        if (key.length === 0)
            return
        const next = root.messageSelectionIds.slice()
        const at = next.indexOf(key)
        if (at >= 0)
            next.splice(at, 1)
        else
            next.push(key)
        root.messageSelectionIds = next
    }
    function clearMessageSelection() {
        if (root.messageSelectionCount > 0)
            root.messageSelectionIds = []
    }
    property string menuMessageId: ""
    // Issue #110: the href of the body hyperlink under a right-click, when the
    // target is an anchor (workspace-message-body-interactions.hook.ts:266-280).
    property string menuContextLink: ""
    // Snapshot of the body selection at the moment the bubble menu opened
    // (workspace-message-body-interactions.hook.ts:364,383 at 230e02b8): the
    // menu Reply quotes exactly what was selected when the menu appeared,
    // even if the live selection is lost before the click lands.
    property string menuSelectedText: ""
    // Issue #218 review r2: the reference keeps a single live document
    // selection, so a body selection becoming active clears the persistent
    // selections left in the other bubbles (workspace-message-body-
    // interactions.hook.ts:97-116). Bubbles register their TextEdit here
    // whenever a selection becomes active; entries for destroyed delegates
    // are pruned lazily when touched.
    property var bodySelectionBodies: ({})
    function noteBodySelection(messageId, body) {
        if (!body || String(body.selectedText || "").length === 0)
            return;
        const key = String(messageId);
        const bodies = root.bodySelectionBodies;
        const stale = [];
        for (const other in bodies) {
            if (other === key)
                continue;
            const held = bodies[other];
            try {
                if (held && held.selectionStart !== held.selectionEnd)
                    held.select(held.selectionEnd, held.selectionEnd);
            } catch (error) {
                stale.push(other);
            }
        }
        for (const gone of stale)
            delete bodies[gone];
        bodies[key] = body;
    }
    function forgetBodySelection(messageId) {
        delete root.bodySelectionBodies[String(messageId)];
    }
    property string menuMessageContent: ""
    property bool menuMessageOwn: false
    property Item menuMessageAnchor: null
    property string menuOwner: ""
    property string menuStream: ""
    property string menuTopic: ""
    readonly property bool menuMessageOrdinaryReadable: {
        const message = currentMessage(menuMessageId);
        return Boolean(message && message.kind === "markdown" && !selectedStreamEncrypted);
    }
    readonly property bool menuMessageReadable: {
        const message = currentMessage(menuMessageId);
        return root.menuMessageOrdinaryReadable || Boolean(message && message.kind === "e2ee" && message.decrypted);
    }
    readonly property bool menuMessageKeyMissing: {
        const message = currentMessage(menuMessageId);
        return Boolean(message && message.kind === "e2ee" && !message.decrypted && message.canRequestKey && String(message.encryptionKeyUuid || "").length);
    }
    readonly property bool messageMutationAllowed: {
        const message = currentMessage(menuMessageId);
        return root.controller.authenticated && !root.controller.cacheStale && !root.controller.busy && menuOwner === root.controller.ownerKey && menuStream === root.controller.selectedStream && menuTopic === root.controller.selectedTopic && Boolean(message && message.canReact);
    }
    property string pickerOwner: ""
    property string pickerEditSession: ""
    property string pickerStream: ""
    property string pickerTopic: ""
    property string streamMenuTarget: ""
    property string streamMenuOwner: ""
    property string streamMenuContext: ""
    property string streamMenuRoute: ""
    property Item streamMenuAnchor: null
    property bool topicFormKeyboard: false
    property string topicFormTarget: ""
    property string topicFormOwner: ""
    property string topicFormContext: ""
    property string topicFormRoute: ""
    property string topicFormAttempt: ""
    property string topicFormLifetime: ""
    readonly property bool topicFormIntent: topicFormTarget.length > 0 && root.controller.topicCreationStream === topicFormTarget && topicFormAttempt === root.controller.topicCreationAttemptId
    readonly property bool topicFormBusy: topicFormIntent && root.controller.topicCreationBusy
    readonly property bool topicFormUncertain: topicFormIntent && root.controller.topicCreationUncertain
    property string archiveQuery: ""
    property string archiveOwner: ""
    property string archiveRuntime: ""
    property var archiveRequests: ({})
    property string archiveErrorTarget: ""
    readonly property var archiveVisibleChats: {
        const query = archiveQuery.trim().toLowerCase();
        return root.controller.archivedChats.filter(chat => String(chat.name).toLowerCase().indexOf(query) >= 0).sort((a, b) => String(a.name).localeCompare(String(b.name)));
    }
    function resetArchiveView() {
        archiveQuery = "";
        archiveRequests = ({});
        archiveErrorTarget = "";
        archiveOwner = root.controller.ownerKey;
        archiveRuntime = root.controller.channelCreationContextKey;
        archiveChatList.contentY = 0;
        archiveBody.contentY = 0;
    }
    function validateArchiveUi() {
        if (!createChatDialog.visible || chatTabs.currentIndex !== 3) return;
        if (!root.controller.authenticated || archiveOwner !== root.controller.ownerKey) {
            createChatDialog.close();
            return;
        }
        // Source clears modal-local requests on runtime changes. Core retains
        // unknown server outcomes independently, for safe explicit recovery.
        if (archiveRuntime !== root.controller.channelCreationContextKey) {
            archiveRuntime = root.controller.channelCreationContextKey;
            archiveRequests = ({});
            archiveErrorTarget = "";
        }
    }
    function syncArchiveRequests() {
        if (!createChatDialog.visible) return;
        const pending = Object.assign({}, archiveRequests);
        const rows = root.controller.archivedChats;
        for (const id of Object.keys(pending)) {
            const row = rows.find(chat => chat.id === id);
            if (!row || !row.pending) {
                delete pending[id];
                if (row && row.error.length) archiveErrorTarget = id;
            }
        }
        archiveRequests = pending;
    }
    function restoreArchivedChat(id) {
        if (!createChatDialog.visible || !root.controller.canUnarchiveChat(id)) return;
        const pending = Object.assign({}, archiveRequests);
        pending[id] = true;
        archiveRequests = pending;
        archiveErrorTarget = "";
        root.controller.unarchiveChat(id);
        syncArchiveRequests();
    }
    function openArchivedChat(id) {
        if (!createChatDialog.visible || !root.controller.archivedChats.some(chat => chat.id === id)) return;
        root.controller.openArchivedChat(id);
        if (root.controller.selectedStream === id && root.controller.conversationKind === "stream") createChatDialog.close();
    }
    function focusArchiveRow(index, restore) {
        if (index < 0) { archiveSearch.forceActiveFocus(Qt.BacktabFocusReason); return; }
        if (index >= archiveChatList.count) { channelCancel.forceActiveFocus(Qt.TabFocusReason); return; }
        archiveChatList.positionViewAtIndex(index, ListView.Contain);
        archiveChatList.forceLayout();
        const row = archiveChatList.itemAtIndex(index);
        if (row) (restore && row.restoreControl.enabled ? row.restoreControl : row.openControl).forceActiveFocus(restore ? Qt.BacktabFocusReason : Qt.TabFocusReason);
    }
    property string chatUserQuery: ""
    property string lastChatFlow: "dm"
    property string directShellContext: ""
    property string directViewAttempt: ""
    property string directViewPeer: ""
    readonly property bool chatMutationBusy: root.controller.channelCreationBusy || root.controller.directChatBusy || root.controller.topicCreationBusy
    readonly property bool directUnresolved: root.controller.directChatBusy || root.controller.directChatUncertain
    readonly property bool channelUnresolved: root.controller.channelCreationBusy || root.controller.channelCreationUncertain || root.channelPartialMembers
    readonly property bool directViewIntent: directViewPeer.length > 0 && directViewPeer === root.controller.directChatForm.peerUuid && (!directViewAttempt.length || directViewAttempt === root.controller.directChatForm.attemptId)
    readonly property var directVisibleUsers: {
        // Closed/inactive pickers must not project and sort the entire user
        // directory on every startup state notification (issue #157).
        if (!createChatDialog.visible || chatTabs.currentIndex !== 0) return [];
        const query = chatUserQuery.trim().toLowerCase();
        return root.controller.chatPickerChoices.filter(user => [user.displayName, user.username, user.email].join("\n").toLowerCase().indexOf(query) >= 0).sort((a, b) => String(a.displayName).localeCompare(String(b.displayName)));
    }
    function setChatQuery(query) {
        chatUserQuery = query;
        if (channelMemberSearch.text !== query) channelMemberSearch.text = query;
        if (directChatSearch.text !== query) directChatSearch.text = query;
    }
    function streamDisplayName(stream) {
        return stream && stream.isDirect ? String(stream.displayName || stream.name || "") : String(stream ? stream.name : "");
    }
    function resetDirectView() {
        directShellContext = "";
        directViewAttempt = "";
        directViewPeer = "";
    }
    function startDirectPeer(peer) {
        if (chatMutationBusy || !root.controller.canStartDirectChatWith(peer)) return;
        directViewAttempt = "";
        directViewPeer = peer;
        root.controller.startDirectChat(peer);
        const form = root.controller.directChatForm;
        if (form.peerUuid === peer) directViewAttempt = String(form.attemptId || "");
    }
    function chooseChatTab(index) {
        if (index < 0 || index > 3) return;
        if (chatTabs.currentIndex === 3 && index !== 3) {
            channelShellContext = root.controller.channelCreationContextKey;
            directShellContext = root.controller.directChatContextKey;
            globalTopicLifetime = root.controller.topicCreationContextKey;
        }
        chatTabs.currentIndex = index;
        if (index < 3) lastChatFlow = ["dm", "channel", "topic"][index];
        if (index === 2) restoreGlobalTopicView();
    }
    property string channelShellContext: ""
    property string channelViewAttempt: ""
    property bool channelKeyboard: false
    property bool channelSubmitted: false
    property string channelRequestedAction: "create"
    property var channelSelectedMembers: []
    property bool channelEncryptionChecked: false
    // The reference switch clears the exclusive channel options when it is
    // turned on; while it is on those options stay disabled (create-chat-
    // dialog.ui.tsx + create-chat-dialog.hook.ts in workspace_ui 230e02b8).
    function setChannelEncryption(checked) {
        channelEncryptionChecked = checked;
        if (checked) {
            channelInviteOnly.checked = false;
            channelAnnounce.checked = false;
        }
    }
    readonly property string channelPhase: root.controller.channelCreationPhase
    readonly property bool channelPartialMembers: channelPhase === "members-required" || channelPhase === "members-failed"
    readonly property bool channelFormFrozen: root.chatMutationBusy || channelPhase !== "idle" && channelPhase !== "failed" && channelPhase !== "ready"
    readonly property string channelPrimaryAction: root.controller.channelCreationBusy ? channelRequestedAction : channelPhase === "uncertain" ? "check" : channelPhase === "members-required" ? "add" : channelPhase === "members-failed" ? "retry" : ["idle", "failed", "ready"].indexOf(channelPhase) >= 0 ? "create" : "none"
    readonly property var channelVisibleMembers: {
        if (!createChatDialog.visible || chatTabs.currentIndex !== 1) return [];
        const query = chatUserQuery.trim().toLowerCase();
        return root.controller.chatPickerChoices.filter(user => [user.displayName, user.username, user.email].join("\n").toLowerCase().indexOf(query) >= 0).sort((a, b) => {
            const selected = Number(root.channelSelectedMembers.indexOf(b.id) >= 0) - Number(root.channelSelectedMembers.indexOf(a.id) >= 0);
            return selected || String(a.displayName).localeCompare(String(b.displayName));
        });
    }
    function resetChannelView() {
        channelShellContext = "";
        channelViewAttempt = "";
        channelSubmitted = false;
        channelRequestedAction = "create";
        channelSelectedMembers = [];
        channelNameInput.text = "";
        channelDescriptionInput.text = "";
        setChatQuery("");
        channelInviteOnly.checked = false;
        channelAnnounce.checked = false;
        channelEncryptionChecked = false;
    }
    function revealChannelControl(control) {
        if (!createChatDialog.visible || !control)
            return;
        const top = control.mapToItem(channelBody.contentItem, 0, 0).y;
        const bottom = top + control.height;
        let offset = channelBody.contentY;
        if (top < offset)
            offset = top;
        else if (bottom > offset + channelBody.height)
            offset = bottom - channelBody.height;
        channelBody.contentY = Math.max(0, Math.min(Math.max(0, channelBody.contentHeight - channelBody.height), offset));
    }
    function openChannelForm(keyboard) {
        if (!root.controller.authenticated)
            return;
        resetChannelView();
        resetArchiveView();
        channelKeyboard = Boolean(keyboard);
        channelShellContext = root.controller.channelCreationContextKey;
        directShellContext = root.controller.directChatContextKey;
        globalTopicLifetime = root.controller.topicCreationContextKey;
        const restoreDirect = directUnresolved;
        const restoreChannel = channelUnresolved;
        chooseChatTab(lastChatFlow === "topic" && root.topicUnresolved ? 2 : restoreChannel && (!restoreDirect || lastChatFlow === "channel") ? 1 : 0);
        const directForm = root.controller.directChatForm;
        if (restoreDirect) {
            directViewAttempt = String(directForm.attemptId || "");
            directViewPeer = String(directForm.peerUuid || "");
        }
        channelBody.contentY = 0;
        channelMemberList.contentY = 0;
        // A dismissed unresolved intent belongs to core. Its old contextKey
        // does not make it a new request; explicit Check rebinds recovery.
        const form = root.controller.channelCreationForm;
        if (form.attemptId && ["idle", "failed", "ready"].indexOf(channelPhase) < 0) {
            channelViewAttempt = form.attemptId;
            channelNameInput.text = form.name;
            channelDescriptionInput.text = form.description;
            channelInviteOnly.checked = Boolean(form.inviteOnly);
            channelAnnounce.checked = Boolean(form.announce);
            channelEncryptionChecked = Boolean(form.encrypted);
            channelSelectedMembers = Array.from(form.memberUuids || []);
            channelSubmitted = true;
        }
        createChatDialog.open();
    }
    function validateChannelUi() {
        if (!createChatDialog.visible)
            return;
        if (chatTabs.currentIndex === 3) { validateArchiveUi(); return; }
        if (!root.controller.authenticated || channelShellContext !== root.controller.channelCreationContextKey || directShellContext !== root.controller.directChatContextKey) {
            createChatDialog.close();
            resetChannelView();
            resetDirectView();
            return;
        }
        const form = root.controller.channelCreationForm;
        if (!channelViewAttempt.length && form.attemptId && channelFormFrozen)
            channelViewAttempt = form.attemptId;
    }
    function toggleChannelMember(id) {
        if (channelFormFrozen || !root.controller.channelMembersVerified)
            return;
        const members = channelSelectedMembers.slice();
        const index = members.indexOf(id);
        if (index < 0)
            members.push(id);
        else
            members.splice(index, 1);
        channelSelectedMembers = members;
    }
    function runChannelAction(action) {
        if (chatMutationBusy)
            return;
        channelSubmitted = true;
        channelRequestedAction = action;
        if (action === "check" && root.controller.canCheckChannelCreation)
            root.controller.checkChannelCreation();
        else if ((action === "add" || action === "retry") && root.controller.canRetryChannelMembers)
            root.controller.retryChannelMembers();
        else if (action === "create" && ["idle", "failed", "ready"].indexOf(channelPhase) >= 0 && root.controller.canCreateChannel && root.controller.channelMembersVerified && channelNameInput.text.trim().length) {
            channelViewAttempt = "";
            root.controller.createChannel(channelNameInput.text, channelDescriptionInput.text, channelInviteOnly.checked, channelAnnounce.checked, channelSelectedMembers, root.channelEncryptionChecked);
        }
    }
    readonly property bool streamMenuAvailable: {
        const stream = currentStream(streamMenuTarget);
        return root.controller.authenticated && stream && (!root.controller.topicCreationBusy && root.controller.canCreateTopicIn(streamMenuTarget) || root.controller.topicCreationStream === streamMenuTarget && (root.controller.topicCreationBusy || root.controller.topicCreationUncertain));
    }
    function currentStream(id) {
        for (const stream of root.controller.streams)
            if (stream.id === id)
                return stream;
        return null;
    }
    function currentRoute() {
        return root.controller.selectedStream + "/" + root.controller.selectedTopic;
    }
    function openStreamMenu(id, anchor, point) {
        const stream = currentStream(id);
        if (!stream || !root.controller.authenticated)
            return;
        streamMenuTarget = id;
        streamMenuOwner = root.controller.ownerKey;
        streamMenuContext = String(stream.contextKey || "");
        streamMenuRoute = currentRoute();
        streamMenuAnchor = anchor;
        streamMenu.popup(point);
    }
    property string chatActionTarget: ""
    property string chatActionOwner: ""
    property string chatActionContext: ""
    property string chatActionLifetime: ""
    property string chatActionMode: ""
    property bool chatActionSubmitted: false
    readonly property var chatActionState: {
        const rows = root.controller.streams;
        return root.controller.chatActionState(chatActionTarget);
    }
    readonly property var streamMenuState: {
        const rows = root.controller.streams;
        return root.controller.chatActionState(streamMenuTarget);
    }
    // Issue #97 (docs/NATIVE_DESKTOP_SIDEBAR_MUTE_INDICATORS.md): the stream menu
    // level switch model. Order and glyphs follow STREAM_NOTIFICATION_LEVEL_OPTIONS;
    // direct-private streams exclude the mentions_only default level
    // (DIRECT_PRIVATE_STREAM_NOTIFICATION_OPTIONS), matching the reference filter.
    readonly property var streamMenuNotificationOptions: {
        const rows = root.controller.streams;
        const stream = root.currentStream(streamMenuTarget);
        const options = [
            {mode: "mentions_only", objectName: "streamMenuNotifDefault",
             asset: "at.svg", label: qsTr("Mentions only")},
            {mode: "muted", objectName: "streamMenuNotifMuted",
             asset: "bell_off.svg", label: qsTr("Muted")},
            {mode: "all_messages", objectName: "streamMenuNotifSubscribed",
             asset: "bell.svg", label: qsTr("All messages")}
        ];
        if (stream && Boolean(stream.isDirect)) return options.slice(1);
        return options;
    }
    // Issue #97 review (cid 4629685053): the level switch must be inactive while
    // its target stream has a notification write in flight, because the
    // controller silently rejects a second same-stream request. streamNotificationBusy
    // is an invokable, so the channelInfoNotificationBusy read (NOTIFY stateChanged,
    // which the controller emits whenever a request slot is taken or released)
    // pins this binding to every busy transition, including ones that land while
    // the menu is already open.
    readonly property bool streamMenuNotificationBusy: {
        const stateTick = root.controller.channelInfoNotificationBusy;
        return root.streamMenuTarget !== "" && root.controller.streamNotificationBusy(root.streamMenuTarget);
    }
    function openChatAction(mode) {
        const stream = currentStream(streamMenuTarget);
        if (!stream || !(mode === "archive" ? streamMenuState.canArchive : streamMenuState.available)) return;
        chatActionTarget = stream.id;
        chatActionOwner = root.controller.ownerKey;
        chatActionContext = String(stream.contextKey || "");
        chatActionLifetime = root.controller.topicCreationContextKey;
        chatActionMode = mode;
        chatActionSubmitted = false;
        chatRenameInput.text = stream.name;
        streamMenu.close();
        if (mode === "read") submitChatAction();
        else chatActionDialog.open();
    }
    function submitChatAction() {
        if (!chatActionTarget.length || chatActionState.pending || !(chatActionMode === "archive" ? chatActionState.canArchive : chatActionState.available)) return;
        const stream = currentStream(chatActionTarget);
        if (!stream) return;
        if (chatActionMode === "rename" && (!chatRenameInput.text.trim().length || chatRenameInput.text.trim() === stream.name)) return;
        chatActionSubmitted = true;
        if (chatActionMode === "archive") root.controller.archiveChat(chatActionTarget);
        else if (chatActionMode === "rename") root.controller.renameChat(chatActionTarget, chatRenameInput.text);
        else root.controller.markChatRead(chatActionTarget);
    }
    function validateChatAction() {
        const stream = currentStream(chatActionTarget);
        if (!root.controller.authenticated || chatActionOwner !== root.controller.ownerKey
            || chatActionLifetime !== root.controller.topicCreationContextKey || !stream
            || String(stream.contextKey || "") !== chatActionContext) {
            chatActionDialog.close(); chatActionTarget = ""; return;
        }
        const state = root.controller.chatActionState(chatActionTarget);
        if (chatActionSubmitted && !state.pending) {
            if (!state.error.length) { chatActionDialog.close(); chatActionTarget = ""; }
            else if (chatActionMode === "read") chatActionDialog.open();
        }
    }
    function clearTopicForm() {
        createTopicDialog.close();
        topicFormTarget = "";
        topicFormOwner = "";
        topicFormContext = "";
        topicFormRoute = "";
        topicFormAttempt = "";
        topicFormLifetime = "";
        topicNameInput.text = "";
    }
    function validateStreamUi() {
        validateChatAction();
        const menuTarget = currentStream(streamMenuTarget);
        if (!root.controller.authenticated || streamMenuOwner !== root.controller.ownerKey || streamMenuRoute !== currentRoute() || !menuTarget || String(menuTarget.contextKey || "") !== streamMenuContext) {
            streamMenu.close();
            streamMenuTarget = "";
            streamMenuAnchor = null;
        }
        const formTarget = currentStream(topicFormTarget);
        if (!root.controller.authenticated || topicFormOwner !== root.controller.ownerKey || topicFormRoute !== currentRoute() || topicFormLifetime !== root.controller.topicCreationContextKey || !formTarget || String(formTarget.contextKey || "") !== topicFormContext)
            clearTopicForm();
    }
    function openTopicForm(keyboard) {
        topicFormKeyboard = Boolean(keyboard);
        const stream = currentStream(streamMenuTarget);
        if (!stream || !streamMenuAvailable)
            return;
        topicFormTarget = stream.id;
        topicFormOwner = streamMenuOwner;
        topicFormContext = streamMenuContext;
        topicFormRoute = streamMenuRoute;
        topicFormLifetime = root.controller.topicCreationContextKey;
        // A dismissed request can already exist on the server. Reopen its
        // confirmation state, never create a fresh intent for the same channel.
        const resume = root.controller.topicCreationStream === stream.id && (root.controller.topicCreationBusy || root.controller.topicCreationUncertain);
        topicNameInput.text = resume ? root.controller.topicCreationName : "";
        topicFormAttempt = resume ? root.controller.topicCreationAttemptId : "";
        streamMenu.close();
        createTopicDialog.open();
    }
    function submitTopicForm() {
        if (topicFormBusy || !topicFormTarget.length)
            return;
        if (topicFormUncertain && root.controller.canCheckTopicCreation)
            root.controller.checkTopicCreation();
        else if (root.controller.canCreateTopicIn(topicFormTarget) && topicNameInput.text.trim().length) {
            root.controller.createTopic(topicFormTarget, topicNameInput.text);
            if (topicFormTarget === root.controller.topicCreationStream) topicFormAttempt = root.controller.topicCreationAttemptId;
        }
    }
    property string globalTopicTarget: ""
    property string globalTopicAcl: ""
    property string globalTopicAttempt: ""
    property string globalTopicLifetime: ""
    readonly property bool topicUnresolved: root.controller.topicCreationBusy || root.controller.topicCreationUncertain
    readonly property bool globalTopicIntent: globalTopicTarget.length > 0 && globalTopicTarget === root.controller.topicCreationStream && globalTopicAttempt === root.controller.topicCreationAttemptId
    readonly property bool globalTopicUncertain: globalTopicIntent && root.controller.topicCreationUncertain
    readonly property bool globalTopicFrozen: root.chatMutationBusy || globalTopicUncertain
    readonly property var globalTopicStreams: root.controller.streams.slice().sort((a, b) => String(a.name).localeCompare(String(b.name)))
    function selectGlobalTopic(id) {
        globalTopicTarget = id;
        const stream = currentStream(id);
        globalTopicAcl = stream ? String(stream.contextKey || "") : "";
    }
    function clearGlobalTopicView() {
        globalTopicOptions.close();
        globalTopicTarget = "";
        globalTopicAcl = "";
        globalTopicAttempt = "";
        globalTopicLifetime = "";
        globalTopicName.text = "";
    }
    function restoreGlobalTopicView() {
        globalTopicLifetime = root.controller.topicCreationContextKey;
        if (root.topicUnresolved) {
            selectGlobalTopic(root.controller.topicCreationStream);
            globalTopicAttempt = root.controller.topicCreationAttemptId;
            globalTopicName.text = root.controller.topicCreationName;
        } else {
            if (!currentStream(globalTopicTarget)) selectGlobalTopic(globalTopicStreams.length ? globalTopicStreams[0].id : "");
        }
    }
    function validateGlobalTopicUi() {
        if (!createChatDialog.visible || chatTabs.currentIndex === 3) return;
        const stream = currentStream(globalTopicTarget);
        if (!root.controller.authenticated || globalTopicLifetime !== root.controller.topicCreationContextKey || globalTopicTarget.length && (!stream || globalTopicAcl !== String(stream.contextKey || ""))) {
            createChatDialog.close();
            clearGlobalTopicView();
        }
    }
    function submitGlobalTopic() {
        if (root.chatMutationBusy || !globalTopicTarget.length) return;
        if (globalTopicUncertain && root.controller.canCheckTopicCreation) root.controller.checkTopicCreation();
        else if (root.controller.canCreateTopicIn(globalTopicTarget) && globalTopicName.text.trim().length) {
            root.controller.createTopic(globalTopicTarget, globalTopicName.text);
            if (globalTopicTarget === root.controller.topicCreationStream) globalTopicAttempt = root.controller.topicCreationAttemptId;
        }
    }
    property string reopenOwner: ""
    property string reopenStream: ""
    property string reopenTopic: ""
    readonly property bool reopenAllowed: root.controller.canReopenTopic && reopenOwner === root.controller.ownerKey && reopenStream === root.controller.selectedStream && reopenTopic === root.controller.selectedTopic
    property string downloadOwner: ""
    property string downloadId: ""
    property string reactionOwner: ""
    property string reactionStream: ""
    property string reactionTopic: ""
    property string reactionMessage: ""
    property var quickUsage: root.preferences.quickReactionUsage
    readonly property var quickReactionChoices: makeQuickReactions(quickUsage)

    function makeQuickReactions(usage) {
        const base = ["heart", "thumbs_up", "joy", "open_mouth", "cry", "clap"];
        const byName = {};
        for (let i = 0; i < root.controller.reactionChoices.length; ++i) {
            const entry = root.controller.reactionChoices[i];
            byName[entry.emojiName] = entry;
        }
        const popular = Object.keys(usage).filter(name => base.indexOf(name) < 0 && byName[name] && Number(usage[name]) > 0).sort((a, b) => Number(usage[b]) - Number(usage[a]) || String(byName[a].pickerUnicode).localeCompare(String(byName[b].pickerUnicode))).slice(0, 5);
        const names = base.concat(popular);
        const order = {};
        for (let i = 0; i < names.length; ++i)
            order[names[i]] = i;
        names.sort((a, b) => Number(usage[b] || 0) - Number(usage[a] || 0) || order[a] - order[b]);
        return names.map(name => byName[name]).filter(entry => entry);
    }

    function chooseQuickReaction(id, emoji) {
        const message = currentMessage(id);
        if (!message || !message.canReact || message.reactionBusy)
            return;
        messageMenu.close();
        root.controller.toggleReaction(id, emoji);
    }
    readonly property var reactionTarget: currentMessage(reactionMessage)
    readonly property bool reactionPickerAllowed: root.controller.authenticated && reactionOwner === root.controller.ownerKey && reactionStream === root.controller.selectedStream && reactionTopic === root.controller.selectedTopic && reactionTarget !== null && Boolean(reactionTarget.canReact) && !Boolean(reactionTarget.reactionBusy)

    function currentMessage(id) {
        if (!id)
            return null;
        const messages = root.controller.messages;
        for (let i = 0; i < messages.length; ++i) {
            if (messages[i].id === id)
                return messages[i];
        }
        return null;
    }

    function openReactionPicker(id, anchor) {
        const message = currentMessage(id);
        if (!message || !message.canReact || message.reactionBusy)
            return;
        reactionOwner = root.controller.ownerKey;
        reactionStream = root.controller.selectedStream;
        reactionTopic = root.controller.selectedTopic;
        reactionMessage = id;
        fullReactionPicker.reset();
        const point = anchor ? anchor.mapToItem(root.contentItem, anchor.width, 0) : Qt.point(8, root.height - reactionPicker.height - 8);
        let left = point.x + 8;
        if (anchor && left + reactionPicker.width > root.width - 8)
            left = point.x - anchor.width - reactionPicker.width - 8;
        reactionPicker.x = Math.max(8, Math.min(root.width - reactionPicker.width - 8, left));
        reactionPicker.y = Math.max(8, Math.min(root.height - reactionPicker.height - 8, point.y));
        reactionPicker.open();
        Qt.callLater(() => fullReactionPicker.focusSearch());
    }

    function pickReaction(emoji) {
        if (!reactionPickerAllowed)
            return;
        const id = reactionMessage;
        root.preferences.recordSuggestedReaction(emoji, reactionOwner);
        reactionPicker.close();
        root.controller.toggleReaction(id, emoji);
    }

    function checkReaction(id) {
        const message = currentMessage(id);
        if (!message || !message.canReact || !message.reactionUncertain)
            return;
        for (let i = 0; i < message.reactions.length; ++i) {
            if (message.reactions[i].pending && message.reactions[i].uncertain) {
                root.controller.toggleReaction(id, message.reactions[i].emojiName);
                return;
            }
        }
    }

    function trailingMessageParagraph(html) {
        // Consume only NativeMarkdown's sanitized QTextDocument fragment.
        // Like the desktop DOM anchor, the last block itself must be a root
        // paragraph; a paragraph nested in a list/quote is not an inline tail.
        const text = String(html).trim();
        if (!text.endsWith("</p>"))
            return -1;
        const start = text.lastIndexOf("<p ");
        if (start < 0)
            return -1;
        const open = text.slice(start, text.indexOf(">", start) + 1);
        if (/-qt-block-indent:\s*[1-9]/.test(open) || /margin-left:\s*[1-9]/.test(open))
            return -1;
        const tags = /<(\/?)([a-z][a-z0-9]*)\b[^>]*>/gi;
        const stack = [];
        let match;
        const before = text.slice(0, start);
        while ((match = tags.exec(before)) !== null) {
            const tag = match[2].toLowerCase();
            if (match[1]) {
                if (stack.length && stack[stack.length - 1] === tag)
                    stack.pop();
            } else if (["br", "hr", "img", "meta", "link", "input"].indexOf(tag) < 0 && !match[0].endsWith("/>")) {
                stack.push(tag);
            }
        }
        return stack.length ? -1 : start;
    }

    function reserveMessageTime(html, time) {
        // No user Markdown is interpolated here. Preserve the renderer's
        // escaped fragment, and escape even the controller-derived clock.
        const clock = String(time).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;").replace(/'/g, "&#39;");
        const text = String(html).trim();
        const end = text.lastIndexOf("</p>");
        // One 14px monospace space reserves the source's 8px gap without
        // altering the message font. The clock is drawn at the trailing corner.
        const reserve = '<span style="font-family:monospace;font-size:14px;color:transparent;">&#160;</span><span style="font-size:12px;color:transparent;">' + clock + '</span>';
        return text.slice(0, end) + reserve + text.slice(end);
    }

    function selectedName(items, identifier) {
        for (let i = 0; i < items.length; ++i) {
            if (items[i].id === identifier)
                return items[i].name;
        }
        return "";
    }

    function countText(count) {
        return count > 1000 ? Math.floor(count / 1000) + "k+" : String(count);
    }
    function messageDay(value) {
        const date = new Date(String(value || ""));
        return isNaN(date.getTime()) ? "" : Qt.formatDateTime(date, "yyyy-MM-dd");
    }
    // Issue #142: wheel distance parity with the reference client
    // (docs/NATIVE_DESKTOP_WHEEL_SCROLL_FEEL.md). The reference never remaps
    // the vertical wheel - one notch ends exactly the notch distance away
    // (100 px in the measured reference contract, section 1). This replaces
    // the flick-based Qt default for angle deltas (cadence-dependent
    // acceleration, ~72 px per idle notch measured in the message list and
    // ~22 px in the sidebar list) with the deterministic, velocity-free
    // mapping, while the system's scroll-lines preference keeps scaling it.
    // High-resolution pixel-delta input (touchpads) keeps Qt's own path: the
    // window delivers synthesized pixel wheels straight to the Flickable
    // under the pointer (touchpad grab semantics), where they drag 1:1 with
    // the reported pixels after DPR normalization - the precise reference
    // touchpad behavior, left untouched by the mapping.
    readonly property real wheelPixelsPerNotch:
        (Qt.styleHints && Qt.styleHints.wheelScrollLines > 0
            ? Qt.styleHints.wheelScrollLines : 3) * 100 / 3
    function scrollFlickableWithWheel(flick, wheel) {
        // Returns whether the wheel was consumed by this list's own movement;
        // anything this mapping does not move (including synthesized
        // pixel-delta wheels, which belong to the Flickable's own precise
        // touchpad path) keeps travelling to the Flickable, like the
        // reference ScrollArea's non-passive hand-off at an edge.
        if (wheel.angleDelta.y === 0
                || wheel.pixelDelta.y !== 0 && wheel.phase !== Qt.NoScrollPhase)
            return false
        const dy = -wheel.angleDelta.y / 120 * root.wheelPixelsPerNotch
        // An outward wheel that cannot move the list (already pinned at the
        // requested edge) must stay unconsumed so an outer scroller under the
        // pointer still receives it, like the reference ScrollArea's
        // non-passive hand-off. Compare against the list's own reachable
        // contentY range, with a half-pixel tolerance so a settled list that
        // rests on a fractional edge (contentHeight rounding, DPR alignment)
        // is not falsely treated as movable.
        const minY = flick.originY - flick.topMargin
        const maxY = flick.originY
            + Math.max(0, flick.contentHeight - flick.height) + flick.bottomMargin
        const settled = Math.min(Math.max(flick.contentY + dy, minY), maxY)
        if (Math.abs(settled - flick.contentY) < 0.5)
            return false
        if (flick.moving)
            flick.flick(0, 0)
        // Out-of-range targets settle through Flickable.returnToBounds()
        // instead of a hand-rolled clamp: programmatic contentY writes do not
        // refresh atYBeginning/atYEnd or the drag extents on their own, so
        // the same fixup the flick path relied on must stay in charge.
        flick.contentY = settled
        flick.returnToBounds()
        return true
    }
    function dayLabel(day) {
        if (day === messageDay(calendarDate))
            return qsTr("Today");
        const yesterday = new Date(calendarDate.getTime());
        yesterday.setDate(yesterday.getDate() - 1);
        if (day === messageDay(yesterday))
            return qsTr("Yesterday");
        const date = new Date(day + "T12:00:00");
        return isNaN(date.getTime()) ? day : Qt.locale(root.preferences.locale).toString(date, root.preferences.locale === "ru" ? "d MMMM" : "MMMM d");
    }
    function topicLabel(name) {
        const label = String(name || "").trim();
        return label.length && !label.startsWith("#") ? "#" + label : label;
    }
    Timer {
        interval: 60000
        repeat: true
        running: root.controller.authenticated
        onTriggered: root.calendarDate = new Date()
    }

    // #108 (docs/NATIVE_DESKTOP_MESSAGE_TIMESTAMP_VIEWER.md): the stored instant
    // shown by the read-only timestamp panel and the trigger that opened it
    // (null when opened through an urn:time reference instead of a chip).
    property var messageTimeAnchor: null
    property string messageTimeInstant: ""
    readonly property var messageTimeDate: messageTimeInstant.length ? new Date(messageTimeInstant) : null
    readonly property bool messageTimeValid: messageTimeDate != null && !isNaN(messageTimeDate.getTime())
    readonly property int messageTimeSelectedDay: root.messageTimeValid ? messageTimeDate.getDate() : 0
    readonly property string messageTimeMonthCaption: root.messageTimeValid
        ? Qt.locale(root.preferences.locale).toString(messageTimeDate, "MMMM yyyy") : ""
    // Leading 0 cells are blank grid slots before the first day.
    readonly property var messageTimeCalendar: {
        if (!root.messageTimeValid)
            return []
        const firstWeekday = Qt.locale(root.preferences.locale).firstDayOfWeek
        const monthStart = new Date(messageTimeDate.getFullYear(), messageTimeDate.getMonth(), 1)
        const daysInMonth = new Date(messageTimeDate.getFullYear(), messageTimeDate.getMonth() + 1, 0).getDate()
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
    function messageTimeClock(instant) {
        const date = new Date(String(instant || ""))
        if (isNaN(date.getTime()))
            return ""
        const pad = part => String(part).padStart(2, "0")
        const parts = [pad(date.getHours()), pad(date.getMinutes())]
        if (date.getSeconds() !== 0 || date.getMilliseconds() !== 0)
            parts.push(pad(date.getSeconds()))
        return parts.join(":")
    }

    function openMessageTimeView(instant, anchor) {
        const value = String(instant || "")
        if (!value.length || isNaN(new Date(value).getTime()))
            return
        root.messageTimeInstant = value
        root.messageTimeAnchor = anchor || null
        messageTimePanel.open()
    }

    // The history uses translated items. Reading each position makes this
    // binding follow row layout and the Flickable's moving content item.
    function messageTimeOffset(item, ancestor) {
        let x = 0, y = 0
        for (let node = item; node && node !== ancestor; node = node.parent) {
            x += node.x
            y += node.y
        }
        return Qt.point(x, y)
    }
    readonly property point messageTimeChipInList: messageTimeAnchor
        ? messageTimeOffset(messageTimeAnchor, messageList) : Qt.point(0, 0)
    readonly property bool messageTimeChipOffscreen: !!messageTimeAnchor
        && (messageTimeChipInList.y >= messageList.height || messageTimeChipInList.y + messageTimeAnchor.height <= 0)
    onMessageTimeChipOffscreenChanged: {
        if (messageTimeChipOffscreen)
            Qt.callLater(function() {
                if (messageTimePanel.visible && root.messageTimeChipOffscreen) {
                    root.messageTimeAnchor = null
                    messageTimePanel.close()
                }
            })
    }

    // Mirrors computeFloatingPickerPosition (floating-panel-position.lib.ts:1-46):
    // 8 px viewport margin, 8 px anchor gap, centered, prefer above, clamp.
    // The panel binds x/y to this so every open recomputes from the trigger.
    readonly property point messageTimePanelTopLeft: {
        // Touching the instant keeps the position reactive to each open.
        if (!messageTimeInstant.length)
            return Qt.point(8, Math.max(8, root.height - messageTimePanel.implicitHeight - 8))
        const margin = 8
        const gap = 8
        const panelWidth = messageTimePanel.width
        const panelHeight = messageTimePanel.height > 0 ? messageTimePanel.height : messageTimePanel.implicitHeight
        const anchor = root.messageTimeAnchor
        if (anchor == null)
            return Qt.point(margin, Math.max(margin, root.height - panelHeight - margin))
        const listOrigin = messageTimeOffset(messageList, messageTimePanel.parent)
        const origin = Qt.point(listOrigin.x + messageTimeChipInList.x, listOrigin.y + messageTimeChipInList.y)
        const maxLeft = Math.max(margin, root.width - panelWidth - margin)
        const x = Math.min(Math.max(margin, origin.x + anchor.width / 2 - panelWidth / 2), maxLeft)
        const topAbove = origin.y - panelHeight - gap
        if (topAbove >= margin)
            return Qt.point(x, topAbove)
        return Qt.point(x, Math.min(Math.max(margin, origin.y + anchor.height + gap),
            Math.max(margin, root.height - panelHeight - margin)))
    }

    function openMessageActions(message, anchor, selectedText, bodyLink) {
        if (root.encryptionControlLabel(String(message.kind)).length)
            return;
        root.menuOwner = root.controller.ownerKey;
        root.menuStream = root.controller.selectedStream;
        root.menuTopic = root.controller.selectedTopic;
        root.menuMessageId = message.id;
        // Issue #110: the bubble menu gains the "Copy link" item only when the
        // right-click landed on an http/https anchor (reference
        // workspace-message-bubble-menu.ui.tsx:349-360 with the isValidUrl
        // gate in shared/lib/validation.ts:8,20-27).
        root.menuContextLink = String(bodyLink || "");
        // TextEdit encodes paragraph breaks as U+2029 and hard breaks as
        // U+2028; the fragment travels to the controller carrying the
        // selected text's own newlines (issue #218 review r2).
        root.menuSelectedText = String(selectedText || "").replace(/[\u2028\u2029]/g, "\n");
        root.menuMessageContent = message.content;
        root.menuMessageOwn = Boolean(message.own);
        root.menuMessageAnchor = anchor || null;
        messageMenu.popup();
    }

    // Issue #109 (docs/NATIVE_DESKTOP_MENTION_CHIPS.md): the reference chip is
    // inline accent text (messenger-workspace-message-body.ui.tsx:143-153:
    // font-medium, text-accent, no background/decoration), so the exported
    // mention anchor span is restyled with the palette accent token here and
    // follows the light/dark theme automatically.
    function mentionChipHtml(html) {
        const accent = String(root.colors["accent"] || "#7087ff");
        return String(html).replace(/(<a href="urn:user:[0-9a-fA-F-]{36}"><span style=")[^"]*(")/g,
            function (whole, head, tail) {
                return head + " color:" + accent + "; font-weight:500; text-decoration:none;" + tail;
            });
    }

// Issue #109 review F3 (cid 4639284120), round 5: the reference chip is a
// real <button> (workspace-message-render.lib.ts:342-352), so a browser
// user reaches it with Tab and activates it with Enter/Space
// (workspace-message-body-interactions.hook.ts:490-496). The chip here is
// an overlay item, so it joins the window tab chain explicitly
// (activeFocusOnTab true) and exposes an accessible name plus Enter/Space
// activation; no pointer is needed to reach or use it.
component NativeMentionChip: Item {
    required property var modelData
    objectName: "message-mention-" + String(modelData.urn).replace("urn:user:", "")
    x: modelData.x
    y: modelData.y
    width: modelData.w
    height: modelData.h
    visible: modelData.w > 0 && modelData.h > 0
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: qsTr("Open profile for %1")
        .arg(String(modelData.label || modelData.urn).replace("urn:user:", ""))
    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            event.accepted = true;
            root.openReference(modelData.urn);
        }
    }
    Keys.onReleased: (event) => {
        if (event.key === Qt.Key_Space) {
            event.accepted = true;
            root.openReference(modelData.urn);
        }
    }
    Rectangle {
        anchors.fill: parent
        anchors.margins: -1
        visible: parent.activeFocus
        color: "transparent"
        radius: 2
        border.width: 2
        border.color: root.colors["accent-soft"]
    }
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.PointingHandCursor
        onPressed: parent.forceActiveFocus()
        onClicked: root.openReference(modelData.urn)
    }
}

    // The reference keeps the "Copy link" item only for http/https hrefs
    // (isValidUrl, shared/lib/validation.ts:8,20-27); a mailto anchor right-click
    // therefore shows the plain message menu (issue #110).
    function isSafeBodyLink(link) {
        const parsed = root.markdown.reference(link);
        if (String(parsed.kind || "") !== "external")
            return false;
        const url = String(parsed.url || "");
        return url.startsWith("http://") || url.startsWith("https://");
    }

    function openReference(link, imageFailed) {
        const reference = root.markdown.reference(link);
        if (reference.kind === "external") {
            root.markdown.openExternal(link);
        } else if ((reference.kind === "quote" || reference.kind === "message") && root.controller.authenticated) {
            // Quote frames and inline message anchors jump to the origin (source
            // workspace-message-quote.ui.tsx:353-397 onOpenMessage contract).
            root.controller.openMessageInChat(reference.id);
        } else if (reference.kind === "stream" && root.controller.authenticated) {
            root.controller.openStreamReference(reference.id);
        } else if (reference.kind === "topic" && root.controller.authenticated) {
            root.controller.openTopic(reference.id, reference.stream || "");
        } else if (reference.kind === "user" && root.controller.authenticated) {
            // Workspace mention links open the user profile drawer (#134).
            rightDrawer.openWorkspaceUserProfile(reference.id);
        } else if (["file", "image", "video"].indexOf(reference.kind) >= 0 && root.controller.authenticated) {
            // Inline images and videos open the full-size viewer whenever the
            // clicked file belongs to the conversation gallery
            // (docs/NATIVE_DESKTOP_MEDIA_VIEWER.md, video slots added by
            // docs/NATIVE_DESKTOP_MEDIA_VIEWER_VIDEO.md); every other file
            // kind keeps the established save-dialog route. A preview whose
            // load failed keeps its Save fallback instead (the viewer cannot
            // show bytes the client never received).
            if ((reference.kind === "image" || reference.kind === "video") && !imageFailed
                && root.controller.mediaViewer.openFromMessages(root.controller.messages, reference.id))
                return;
            root.downloadOwner = root.controller.ownerKey;
            root.downloadId = reference.id;
            attachmentSaveDialog.open();
        } else if (reference.kind === "time" && String(reference.utc || "").length) {
            // #108: an urn:time reference opens the read-only timestamp panel
            // with the normalized instant (workspace-reference.cpp:328-333)
            // instead of falling through to the under-development notice.
            root.openMessageTimeView(reference.utc, null)
        } else {
            referenceNotice.open();
        }
    }

    Connections {
        target: root.preferences
        function onReactionHistoryChanged() {
            root.quickUsage = root.preferences.quickReactionUsage;
        }
    }

    Connections {
        target: root.controller
        function onConversationMarkedRead(streamUuid, topicUuid) {
            messageList.forgetPositions(streamUuid, topicUuid);
        }
        function onStateChanged() {
            root.settleComposerEmojiUi();
            root.settleOwnStatusUi();
            root.validateStreamUi();
            root.validateChannelUi();
            root.validateGlobalTopicUi();
            if (!root.controller.authenticated || root.reopenOwner !== root.controller.ownerKey || root.reopenStream !== root.controller.selectedStream || root.reopenTopic !== root.controller.selectedTopic)
                reopenTopicMenu.close();
            if (root.menuOwner !== root.controller.ownerKey || root.menuStream !== root.controller.selectedStream || root.menuTopic !== root.controller.selectedTopic) {
                messageMenu.close();
                deleteDialog.close();
            }
            if (!root.reactionPickerAllowed)
                reactionPicker.close();
            if (!root.controller.authenticated || root.pickerOwner !== root.controller.ownerKey || root.pickerStream !== root.controller.selectedStream || root.pickerTopic !== root.controller.selectedTopic || !root.composerAvailable)
                attachmentPicker.close();
            if (!root.controller.authenticated || root.downloadOwner !== root.controller.ownerKey) {
                attachmentSaveDialog.close();
                root.downloadId = "";
            }
            // Credential fields live in the credentials wizard step and are
            // destroyed (not just cleared) once a session or projects appear.
            messageList.updateServiceContext();
            messageList.continueOpening();
            messageList.scheduleVisibleRead();
        }
        function onEditSessionChanged() {
            // A picker belongs to the editor that opened it, including a reopen
            // of the same message. A late acceptance must not attach to a draft.
            if (root.pickerEditSession !== root.controller.editSessionKey) {
                root.pickerOwner = "";
                attachmentPicker.close();
            }
        }
        function onArchivedChatsChanged() {
            root.validateArchiveUi();
            root.syncArchiveRequests();
        }
        function onStreamsChanged() {
            root.validateStreamUi();
            root.validateGlobalTopicUi();
        }
        function onTopicCreated(stream, topic) {
            if (root.topicFormTarget === stream && root.topicFormAttempt === topic)
                root.clearTopicForm();
            if (createChatDialog.visible && root.globalTopicTarget === stream && root.globalTopicAttempt === topic)
                createChatDialog.close();
        }
        function onChannelCreated(stream, defaultTopic) {
            if (createChatDialog.visible && root.channelViewAttempt === stream)
                createChatDialog.close();
        }
        function onDirectChatOpened(stream, peer, defaultTopic) {
            const form = root.controller.directChatForm;
            if (createChatDialog.visible && root.directViewPeer === peer && root.directViewAttempt === form.attemptId)
                createChatDialog.close();
        }
        function onMessagesChanged() {
            messageList.replaceMessages(root.controller.messages);
            if (!root.reactionPickerAllowed)
                reactionPicker.close();
        }
        function onMessageSent(id, stream, topic) {
            if (composerEmojiPicker.visible && root.composerEmojiAllowed && root.composerEmojiStream === stream
                    && (root.composerEmojiKind === "stream" || root.composerEmojiTopic === topic))
                composerEmojiPicker.close();
            if (root.controller.selectedStream === stream && root.controller.selectedTopic === topic)
                messageList.followSentMessage(id);
        }
    }

    component LoginErrorBanner: Rectangle {
        visible: root.controller.error.length > 0
        Layout.fillWidth: true
        implicitHeight: bannerText.implicitHeight + 16
        radius: 8
        readonly property color tone: root.colors["notice-base"]
        border.color: Qt.rgba(tone.r, tone.g, tone.b, 0.2)
        color: Qt.rgba(tone.r, tone.g, tone.b, 0.1)
        Text {
            id: bannerText
            x: 12
            y: 8
            width: parent.width - 24
            text: root.controller.error
            textFormat: Text.PlainText
            color: parent.tone
            font.pixelSize: 14
            wrapMode: Text.Wrap
            Accessible.role: Accessible.AlertMessage
        }
    }

    component LoginFieldLabel: Text {
        color: root.colors["text-primary"]
        font.pixelSize: 14
        font.weight: Font.Medium
        textFormat: Text.PlainText
        Layout.fillWidth: true
    }

    Item {
        id: loginCard
        property bool showSavedAccounts: false
        readonly property bool savedAccountsOnly: showSavedAccounts && root.controller.savedAccounts.length > 0
            && root.controller.loginStep === "organization"
        visible: !root.controller.authenticated
        onVisibleChanged: if (!visible) showSavedAccounts = false
        width: Math.min(root.width - 32, 448)
        height: Math.min(root.height - 32, loginContent.implicitHeight)
        anchors.centerIn: parent

        ScrollView {
            id: loginScroll
            anchors.fill: parent
            contentWidth: availableWidth
            clip: true
            ColumnLayout {
                id: loginContent
                width: loginScroll.availableWidth
                spacing: 24
                ToolButton {
                    id: wizardBack
                    objectName: root.controller.loginStep === "organization" ? "savedAccountsBackButton"
                        : root.controller.loginStep === "project" ? "projectBackButton" : "loginBackButton"
                    visible: !loginCard.savedAccountsOnly && (root.controller.loginStep !== "organization"
                        || root.controller.savedAccounts.length > 0)
                    enabled: !root.controller.busy
                    Layout.alignment: Qt.AlignLeft
                    padding: 0
                    spacing: 8
                    font.pixelSize: 14
                    text: root.controller.loginStep === "organization" ? qsTr("Back") : qsTr("Organization")
                    contentItem: RowLayout {
                        spacing: 8
                        Image {
                            source: "qrc:/workspace/icons/chevron-right.svg"
                            Layout.preferredWidth: 16
                            Layout.preferredHeight: 16
                            rotation: 180
                            Accessible.ignored: true
                        }
                        Text {
                            text: wizardBack.text
                            color: root.colors["text-muted"]
                            font.pixelSize: 14
                        }
                    }
                    onClicked: {
                        const returnToAccounts = root.controller.loginStep === "organization";
                        root.controller.backToOrganization();
                        loginCard.showSavedAccounts = returnToAccounts;
                    }
                }
                ColumnLayout {
                    visible: !loginCard.savedAccountsOnly
                    Layout.fillWidth: true
                    spacing: 4
                    Text {
                        objectName: "loginTitle"
                        text: root.controller.savedAccounts.length ? qsTr("Add server") : qsTr("Connect to server")
                        textFormat: Text.PlainText
                        color: root.colors["text-primary"]
                        font.pixelSize: 20
                        font.weight: Font.DemiBold
                        horizontalAlignment: Text.AlignHCenter
                        Layout.fillWidth: true
                    }
                    Text {
                        objectName: "loginDescription"
                        text: root.controller.loginStep === "organization"
                                ? qsTr("Enter your organization address first.")
                                : root.controller.loginStep === "otp"
                                    ? qsTr("Enter the six-digit code from your authenticator app.")
                                    : root.controller.loginStep === "project"
                                        ? qsTr("Choose the project you want to open.")
                                        : qsTr("Choose a sign-in method or enter your email/login and password.")
                        textFormat: Text.PlainText
                        color: root.colors["text-muted"]
                        font.pixelSize: 14
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                }
                ColumnLayout {
                    visible: root.controller.projects.length === 0 && root.controller.savedAccounts.length > 0
                    Layout.fillWidth: true
                    spacing: 10
                    Text {
                        text: qsTr("Saved accounts on this device")
                        textFormat: Text.PlainText
                        color: root.colors["text-secondary"]
                        font.pixelSize: 13
                        Layout.fillWidth: true
                    }
                    Repeater {
                        model: root.controller.projects.length === 0 ? root.controller.savedAccounts : []
                        delegate: Rectangle {
                            id: savedCard
                            required property var modelData
                            objectName: "remembered-" + modelData.id
                            Layout.fillWidth: true
                            implicitHeight: savedContent.implicitHeight + 24
                            radius: 10
                            color: root.colors["card-bg"]
                            border.color: root.colors["border-subtle"]
                            ColumnLayout {
                                id: savedContent
                                x: 12
                                y: 12
                                width: parent.width - 24
                                spacing: 6
                                Text {
                                    objectName: "remembered-label-" + savedCard.modelData.id
                                    text: savedCard.modelData.projectName || savedCard.modelData.project
                                    textFormat: Text.PlainText
                                    color: root.colors["text-primary"]
                                    font.pixelSize: 14
                                    font.weight: Font.Medium
                                    wrapMode: Text.Wrap
                                    Layout.fillWidth: true
                                }
                                Text {
                                    text: savedCard.modelData.login || savedCard.modelData.user
                                    textFormat: Text.PlainText
                                    color: root.colors["text-secondary"]
                                    font.pixelSize: 13
                                    wrapMode: Text.Wrap
                                    Layout.fillWidth: true
                                }
                                Text {
                                    text: savedCard.modelData.server
                                    textFormat: Text.PlainText
                                    color: root.colors["text-muted"]
                                    font.pixelSize: 12
                                    wrapMode: Text.WrapAnywhere
                                    Layout.fillWidth: true
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.topMargin: 4
                                    spacing: 8
                                    NativeButton {
                                        objectName: "resume-" + savedCard.modelData.id
                                        colors: root.colors
                                        Layout.fillWidth: true
                                        primary: true
                                        text: qsTr("Continue")
                                        enabled: !root.controller.busy
                                        Accessible.name: qsTr("Continue as %1 in %2").arg(savedCard.modelData.login || savedCard.modelData.user).arg(savedCard.modelData.projectName || savedCard.modelData.project)
                                        onClicked: root.controller.resumeSavedAccount(savedCard.modelData.id)
                                    }
                                    NativeButton {
                                        objectName: "forget-" + savedCard.modelData.id
                                        colors: root.colors
                                        text: qsTr("Remove")
                                        enabled: !root.controller.busy
                                        Accessible.name: qsTr("Remove saved account %1").arg(savedCard.modelData.login || savedCard.modelData.user)
                                        ToolTip.visible: hovered
                                        ToolTip.text: qsTr("Forget this account on this device")
                                        onClicked: root.controller.forgetSavedAccount(savedCard.modelData.id)
                                    }
                                }
                            }
                        }
                    }
                }
                NativeButton {
                    objectName: "savedAccountsAddServer"
                    visible: loginCard.savedAccountsOnly
                    colors: root.colors
                    Layout.fillWidth: true
                    text: qsTr("Add server")
                    onClicked: loginCard.showSavedAccounts = false
                }
                Loader {
                    objectName: "organizationFormLoader"
                    visible: active
                    active: !loginCard.savedAccountsOnly && root.controller.projects.length === 0
                        && root.controller.loginStep === "organization"
                    Layout.fillWidth: true
                    sourceComponent: ColumnLayout {
                        id: organizationForm
                        spacing: 16
                        readonly property var suggestionEntries: root.controller.organizationHistory()
                        readonly property var matchingEntries: suggestionEntries.filter(
                            value => value.toLocaleLowerCase().includes(serverInput.text.toLocaleLowerCase()))
                        property bool showHistory: false
                        property int activeSuggestion: -1

                        function chooseOrigin(value) {
                            root.controller.organizationInputChanged(value);
                            serverInput.forceActiveFocus();
                            showHistory = false;
                            activeSuggestion = -1;
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            LoginFieldLabel { text: qsTr("Server address") }
                            NativeField {
                                id: serverInput
                                objectName: "serverInput"
                                colors: root.colors
                                Layout.fillWidth: true
                                implicitHeight: 40
                                rightPadding: 40
                                enabled: !root.controller.busy
                                text: root.controller.organizationRealm
                                onTextChanged: {
                                    if (text !== root.controller.organizationRealm)
                                        root.controller.organizationInputChanged(text)
                                }
                                onTextEdited: {
                                    organizationForm.activeSuggestion = -1;
                                    organizationForm.showHistory = true;
                                }
                                placeholderText: qsTr("https://chat.example.com")
                                inputMethodHints: Qt.ImhUrlCharactersOnly
                                Accessible.name: qsTr("Server address")
                                Keys.onPressed: event => {
                                    if (inputMethodComposing) return;
                                    const count = organizationForm.matchingEntries.length;
                                    if ((event.key === Qt.Key_Down || event.key === Qt.Key_Up) && count) {
                                        const current = organizationForm.showHistory ? organizationForm.activeSuggestion : -1;
                                        organizationForm.activeSuggestion = current < 0
                                            ? (event.key === Qt.Key_Down ? 0 : count - 1)
                                            : Math.max(0, Math.min(count - 1, current + (event.key === Qt.Key_Down ? 1 : -1)));
                                        organizationForm.showHistory = true;
                                    } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                            && organizationHistoryPopup.visible && organizationForm.activeSuggestion >= 0) {
                                        organizationForm.chooseOrigin(organizationForm.matchingEntries[organizationForm.activeSuggestion]);
                                    } else if (event.key === Qt.Key_Escape && organizationHistoryPopup.visible) {
                                        organizationForm.showHistory = false;
                                    } else {
                                        return;
                                    }
                                    event.accepted = true;
                                }
                                onAccepted: root.controller.continueFromOrganization(text.trim())
                                onActiveFocusChanged: {
                                    organizationForm.showHistory = activeFocus;
                                    organizationForm.activeSuggestion = -1;
                                    if (!activeFocus && root.controller.isOrganizationUrlValid(text.trim()))
                                        root.controller.discoverOrganization(text.trim());
                                }
                                NativeIconButton {
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.rightMargin: 6
                                    width: 24
                                    height: 24
                                    colors: root.colors
                                    asset: "chevron-down.svg"
                                    glyphSize: 16
                                    help: qsTr("Show options")
                                    focusPolicy: Qt.NoFocus
                                    onClicked: {
                                        const wasOpen = organizationHistoryPopup.visible;
                                        serverInput.forceActiveFocus();
                                        organizationForm.showHistory = !wasOpen;
                                    }
                                }
                            }
                        }
                        Popup {
                            id: organizationHistoryPopup
                            objectName: "organizationHistoryPopup"
                            parent: serverInput
                            padding: 4
                            width: serverInput.width
                            x: 0
                            y: serverInput.height + 4
                            closePolicy: Popup.NoAutoClose
                            visible: serverInput.activeFocus && organizationForm.showHistory
                                && organizationForm.matchingEntries.length > 0
                            contentItem: ColumnLayout {
                                spacing: 2
                                Repeater {
                                    model: organizationForm.matchingEntries
                                    delegate: ItemDelegate {
                                        required property var modelData
                                        required property int index
                                        objectName: "organizationHistoryOption-" + index
                                        Layout.fillWidth: true
                                        text: modelData
                                        highlighted: index === organizationForm.activeSuggestion
                                        focusPolicy: Qt.NoFocus
                                        onClicked: organizationForm.chooseOrigin(modelData)
                                    }
                                }
                            }
                        }
                        Loader {
                            objectName: "defaultOrganizationLoader"
                            visible: active
                            active: root.controller.defaultOrganizationUrl.length > 0
                            Layout.fillWidth: true
                            sourceComponent: Rectangle {
                                implicitHeight: shortcutContents.implicitHeight + 24
                                color: root.colors["bg-elevated"]
                                radius: 8
                                border.color: root.colors["border-subtle"]
                                ColumnLayout {
                                    id: shortcutContents
                                    x: 16
                                    y: 12
                                    width: parent.width - 32
                                    spacing: 8
                                    Text {
                                        text: qsTr("Or connect to our public service.")
                                        color: root.colors["text-muted"]
                                        font.pixelSize: 14
                                        textFormat: Text.PlainText
                                        wrapMode: Text.Wrap
                                        Layout.fillWidth: true
                                    }
                                    NativeButton {
                                        id: defaultOrganizationButton
                                        objectName: "defaultOrganizationBlock"
                                        colors: root.colors
                                        Layout.fillWidth: true
                                        text: root.controller.defaultOrganizationName
                                        enabled: !root.controller.busy
                                        Accessible.name: text
                                        contentItem: RowLayout {
                                            spacing: 8
                                            Item { Layout.fillWidth: true }
                                            Image {
                                                objectName: "defaultOrganizationLogo"
                                                source: "qrc:/workspace/brand/organization-fallback.svg"
                                                Layout.preferredWidth: 18
                                                Layout.preferredHeight: 18
                                                fillMode: Image.PreserveAspectFit
                                                Accessible.ignored: true
                                            }
                                            Text {
                                                text: defaultOrganizationButton.text
                                                textFormat: Text.PlainText
                                                color: root.colors["text-primary"]
                                                font.pixelSize: 14
                                                font.weight: Font.Medium
                                                elide: Text.ElideRight
                                                Layout.maximumWidth: Math.max(0, defaultOrganizationButton.availableWidth - 26)
                                            }
                                            Item { Layout.fillWidth: true }
                                        }
                                        onClicked: root.controller.selectDefaultOrganization()
                                    }
                                }
                            }
                        }
                        LoginErrorBanner { objectName: "organizationErrorBanner" }
                        NativeButton {
                            objectName: "organizationNextButton"
                            colors: root.colors
                            primary: true
                            Layout.fillWidth: true
                            text: root.controller.organizationLoading ? qsTr("Loading organization settings…") : qsTr("Next")
                            // Pressing Continue while a blur-triggered check is
                            // in flight merges with it instead of waiting.
                            enabled: !root.controller.busy
                                && root.controller.isOrganizationUrlValid(serverInput.text.trim())
                            onClicked: root.controller.continueFromOrganization(serverInput.text.trim())
                        }
                    }
                }
                Loader {
                    objectName: "realmPreviewLoader"
                    visible: active && !loginCard.savedAccountsOnly
                    active: Boolean((root.controller.organizationSettings.realmName || "").trim()
                        || root.controller.organizationSettings.realmIconUrl)
                    Layout.fillWidth: true
                    sourceComponent: Rectangle {
                        objectName: "realmPreviewCard"
                        radius: 8
                        color: root.colors["bg-elevated"]
                        border.color: root.colors["border-subtle"]
                        implicitHeight: realmPreviewContent.implicitHeight + 24
                        ColumnLayout {
                            id: realmPreviewContent
                            x: 16
                            y: 12
                            width: parent.width - 32
                            spacing: 8
                            Item {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.topMargin: 4
                                Layout.bottomMargin: 4
                                width: 48
                                height: 48
                                Image {
                                    objectName: "realmPreviewIcon"
                                    anchors.fill: parent
                                    fillMode: Image.PreserveAspectFit
                                    readonly property string fallbackSource: "qrc:/workspace/brand/organization-fallback.svg"
                                    readonly property string baseSource: root.controller.organizationSettings.realmIconUrl || fallbackSource
                                    source: baseSource
                                    onBaseSourceChanged: source = baseSource
                                    onStatusChanged: if (status === Image.Error && source !== fallbackSource)
                                        source = fallbackSource
                                    Accessible.ignored: true
                                }
                            }
                            Text {
                                objectName: "realmPreviewName"
                                Layout.alignment: Qt.AlignHCenter
                                text: root.controller.organizationSettings.realmName || ""
                                visible: text.trim().length > 0
                                textFormat: Text.PlainText
                                color: root.colors["text-primary"]
                                font.pixelSize: 14
                                font.weight: Font.Medium
                                wrapMode: Text.Wrap
                                horizontalAlignment: Text.AlignHCenter
                                Layout.fillWidth: true
                            }
                        }
                    }
                }
                Loader {
                    objectName: "credentialsFormLoader"
                    visible: active && root.controller.loginStep === "credentials"
                    active: root.controller.projects.length === 0
                        && (root.controller.loginStep === "credentials" || root.controller.loginStep === "otp")
                    Layout.fillWidth: true
                    sourceComponent: ColumnLayout {
                        spacing: 12
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            LoginFieldLabel { text: qsTr("Email or login") }
                            NativeField {
                                id: userInput
                                objectName: "userInput"
                                colors: root.colors
                                Layout.fillWidth: true
                                enabled: !root.controller.busy
                                placeholderText: qsTr("email@example.com or login")
                                Component.onCompleted: text = root.controller.loginUsername
                                Accessible.name: qsTr("Email or login")
                                onAccepted: if (signInButton.enabled) signInButton.clicked()
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            LoginFieldLabel { text: qsTr("Password") }
                            NativeField {
                                id: passwordInput
                                objectName: "passwordInput"
                                colors: root.colors
                                Layout.fillWidth: true
                                enabled: !root.controller.busy
                                placeholderText: "••••••••"
                                echoMode: TextInput.Password
                                inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                                Accessible.name: qsTr("Password")
                                onAccepted: if (signInButton.enabled) signInButton.clicked()
                            }
                        }
                        LoginErrorBanner {}
                        NativeButton {
                            id: signInButton
                            objectName: "signInButton"
                            colors: root.colors
                            primary: true
                            Layout.fillWidth: true
                            text: root.controller.busy ? qsTr("Logging in…") : qsTr("Next")
                            enabled: !root.controller.busy
                            onClicked: {
                                if (enabled) {
                                    root.controller.login(root.controller.organizationRealm,
                                        userInput.text.trim(), passwordInput.text, "");
                                }
                            }
                        }
                        // Issue #61 (reference login-page.ui.tsx:524-536): the
                        // block renders only when organization discovery kept an
                        // https registration URL (normalized at the Session parse
                        // site); a press hands the URL to the system default
                        // browser, mirroring the reference target=_blank anchor.
                        Loader {
                            Layout.fillWidth: true
                            active: (root.controller.organizationSettings.registrationUrl || "").length > 0
                            sourceComponent: ColumnLayout {
                                objectName: "registrationBlock"
                                spacing: 8
                                Text {
                                    objectName: "registrationHintText"
                                    Layout.fillWidth: true
                                    text: qsTr("No account?")
                                    textFormat: Text.PlainText
                                    color: root.colors["text-muted"]
                                    font.pixelSize: 14
                                    horizontalAlignment: Text.AlignHCenter
                                }
                                NativeButton {
                                    id: registerButton
                                    objectName: "registerButton"
                                    colors: root.colors
                                    Layout.fillWidth: true
                                    implicitHeight: 36
                                    horizontalPadding: 16
                                    font.pixelSize: 14
                                    font.weight: Font.Medium
                                    readonly property string url:
                                        root.controller.organizationSettings.registrationUrl || ""
                                    text: qsTr("Register")
                                    contentItem: Text {
                                        text: registerButton.text
                                        textFormat: Text.PlainText
                                        color: root.colors["text-primary"]
                                        font: registerButton.font
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                    background: Rectangle {
                                        readonly property color tone: root.colors["bg-elevated"]
                                        radius: 8
                                        color: registerButton.hovered
                                            ? Qt.rgba(tone.r, tone.g, tone.b, 0.6) : tone
                                        border.width: registerButton.visualFocus ? 2 : 1
                                        border.color: registerButton.visualFocus
                                            ? root.colors.accent : root.colors["border-subtle"]
                                    }
                                    Accessible.name: registerButton.text
                                    onClicked: Qt.openUrlExternally(registerButton.url)
                                }
                            }
                        }
                    }
                }
                Loader {
                    objectName: "otpFormLoader"
                    visible: active
                    active: root.controller.projects.length === 0 && root.controller.loginStep === "otp"
                    Layout.fillWidth: true
                    sourceComponent: ColumnLayout {
                        spacing: 16
                        ColumnLayout {
                            spacing: 6
                            Layout.fillWidth: true
                            Text {
                                text: qsTr("Authentication code")
                                color: root.colors["text-primary"]
                                font.pixelSize: 14
                                font.weight: Font.Medium
                                Layout.fillWidth: true
                            }
                            NativeField {
                                id: otpInput
                                objectName: "otpInput"
                                colors: root.colors
                                Layout.fillWidth: true
                                implicitHeight: 50
                                padding: 12
                                enabled: !root.controller.busy
                                placeholderText: "000000"
                                background: Rectangle {
                                    radius: 8
                                    color: root.colors["bg-elevated"]
                                    border.width: otpInput.activeFocus ? 2 : 1
                                    border.color: otpInput.activeFocus ? root.colors.accent : root.colors["border-subtle"]
                                }
                                horizontalAlignment: TextInput.AlignHCenter
                                font.family: "monospace"
                                font.pixelSize: 18
                                font.letterSpacing: 6.3
                                inputMethodHints: Qt.ImhDigitsOnly | Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                                Accessible.name: qsTr("Authentication code")
                                onTextChanged: {
                                    const digits = text.replace(/[^0-9]/g, "").slice(0, 6)
                                    if (text !== digits) text = digits
                                }
                                onAccepted: otpConfirmButton.clicked()
                                Connections {
                                    target: root.controller
                                    function onOtpCodeRejected() { otpInput.text = "" }
                                }
                                Component.onCompleted: forceActiveFocus()
                            }
                        }
                        Rectangle {
                            objectName: "otpErrorBanner"
                            visible: root.controller.error.length > 0
                            Layout.fillWidth: true
                            implicitHeight: otpErrorText.implicitHeight + 16
                            radius: 8
                            readonly property color tone: root.colors["notice-base"]
                            color: Qt.rgba(tone.r, tone.g, tone.b, 0.1)
                            border.color: Qt.rgba(tone.r, tone.g, tone.b, 0.2)
                            Text {
                                id: otpErrorText
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 12
                                anchors.topMargin: 8
                                anchors.bottomMargin: 8
                                text: root.controller.error
                                textFormat: Text.PlainText
                                color: parent.tone
                                wrapMode: Text.Wrap
                                font.pixelSize: 14
                                Accessible.role: Accessible.AlertMessage
                            }
                        }
                        NativeButton {
                            id: otpConfirmButton
                            objectName: "otpConfirmButton"
                            colors: root.colors
                            primary: true
                            Layout.fillWidth: true
                            implicitHeight: 36
                            text: root.controller.busy ? qsTr("Logging in…") : qsTr("Confirm code")
                            background: Rectangle {
                                radius: 8
                                color: root.colors.accent
                                opacity: otpConfirmButton.enabled ? (otpConfirmButton.down ? 0.8 : 1) : 0.5
                                border.width: otpConfirmButton.visualFocus ? 2 : 0
                                border.color: root.colors.accent
                            }
                            enabled: !root.controller.busy && otpInput.text.length === 6
                            onClicked: {
                                if (!enabled) return
                                root.controller.submitOtp(otpInput.text)
                            }
                        }
                        NativeButton {
                            id: otpBackButton
                            objectName: "otpBackButton"
                            colors: root.colors
                            Layout.fillWidth: true
                            implicitHeight: 36
                            text: qsTr("Back")
                            enabled: !root.controller.busy
                            contentItem: Text {
                                text: otpBackButton.text
                                color: root.colors["text-muted"]
                                font: otpBackButton.font
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                opacity: otpBackButton.enabled ? 1 : 0.5
                            }
                            background: Rectangle {
                                radius: 8
                                color: otpBackButton.hovered ? root.colors["bg-elevated"] : "transparent"
                            }
                            onClicked: root.controller.backToCredentials()
                        }
                    }
                }
                // Issue #60 project step (login-page-project-form.ui.tsx):
                // labelled dropdown with the organization suffix and the
                // Login submit; the blocking noProjectsAvailable notice when
                // the account sees no projects.
                Loader {
                    objectName: "projectStepLoader"
                    visible: active
                    active: root.controller.loginStep === "project"
                        && root.controller.projects.length > 0
                    Layout.fillWidth: true
                    sourceComponent: ColumnLayout {
                        spacing: 16
                        ColumnLayout {
                            spacing: 6
                            Layout.fillWidth: true
                            Text {
                                objectName: "projectStepLabel"
                                text: qsTr("Project")
                                textFormat: Text.PlainText
                                color: root.colors["text-primary"]
                                font.pixelSize: 14
                                font.weight: Font.Medium
                                Layout.fillWidth: true
                            }
                            NativeSelect {
                                id: projectSelect
                                objectName: "projectSelect"
                                colors: root.colors
                                Layout.fillWidth: true
                                model: root.controller.projects
                                objectNamePrefix: "projectOption-"
                                placeholderText: qsTr("Select a project")
                                enabled: !root.controller.busy
                                Component.onCompleted: {
                                    // Preselect once per step mount
                                    // (login-page.ui.tsx:316); a later
                                    // stateChanged must not undo the user's
                                    // own pick.
                                    const pre = root.controller.projectPreselection
                                    currentIndex = root.controller.projects
                                        .findIndex(function (project) { return project.id === pre })
                                }
                            }
                        }
                        Rectangle {
                            objectName: "projectErrorBanner"
                            visible: root.controller.error.length > 0
                            Layout.fillWidth: true
                            implicitHeight: projectErrorText.implicitHeight + 16
                            radius: 8
                            readonly property color tone: root.colors["notice-base"]
                            color: Qt.rgba(tone.r, tone.g, tone.b, 0.1)
                            border.color: Qt.rgba(tone.r, tone.g, tone.b, 0.2)
                            Text {
                                id: projectErrorText
                                objectName: "projectErrorText"
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 12
                                anchors.topMargin: 8
                                anchors.bottomMargin: 8
                                text: root.controller.error
                                textFormat: Text.PlainText
                                color: parent.tone
                                wrapMode: Text.Wrap
                                font.pixelSize: 14
                                Accessible.role: Accessible.AlertMessage
                            }
                        }
                        NativeButton {
                            id: projectLoginButton
                            objectName: "projectLoginButton"
                            colors: root.colors
                            primary: true
                            Layout.fillWidth: true
                            implicitHeight: 36
                            text: root.controller.busy ? qsTr("Logging in…") : qsTr("Login")
                            enabled: !root.controller.busy && projectSelect.currentIndex >= 0
                            background: Rectangle {
                                radius: 8
                                color: root.colors.accent
                                opacity: projectLoginButton.enabled ? (projectLoginButton.down ? 0.8 : 1) : 0.5
                                border.width: projectLoginButton.visualFocus ? 2 : 0
                                border.color: root.colors.accent
                            }
                            onClicked: {
                                if (!enabled) return
                                root.controller.openProject(projectSelect.currentValue)
                            }
                        }
                    }
                }
                Loader {
                    objectName: "noProjectsLoader"
                    visible: active
                    active: root.controller.loginStep === "project"
                        && root.controller.projects.length === 0
                    Layout.fillWidth: true
                    sourceComponent: ColumnLayout {
                        spacing: 16
                        Rectangle {
                            objectName: "noProjectsBanner"
                            Layout.fillWidth: true
                            implicitHeight: noProjectsText.implicitHeight + 16
                            radius: 8
                            readonly property color tone: root.colors["notice-base"]
                            color: Qt.rgba(tone.r, tone.g, tone.b, 0.1)
                            border.color: Qt.rgba(tone.r, tone.g, tone.b, 0.2)
                            Text {
                                id: noProjectsText
                                objectName: "noProjectsText"
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 12
                                anchors.topMargin: 8
                                anchors.bottomMargin: 8
                                text: qsTr("Your account does not have access to any projects in this organization.")
                                textFormat: Text.PlainText
                                color: parent.tone
                                wrapMode: Text.Wrap
                                font.pixelSize: 14
                                Accessible.role: Accessible.AlertMessage
                            }
                        }
                        Rectangle {
                            objectName: "noProjectsErrorBanner"
                            visible: root.controller.error.length > 0
                            Layout.fillWidth: true
                            implicitHeight: noProjectsErrorText.implicitHeight + 16
                            radius: 8
                            readonly property color tone: root.colors["notice-base"]
                            color: Qt.rgba(tone.r, tone.g, tone.b, 0.1)
                            border.color: Qt.rgba(tone.r, tone.g, tone.b, 0.2)
                            Text {
                                id: noProjectsErrorText
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 12
                                anchors.topMargin: 8
                                anchors.bottomMargin: 8
                                text: root.controller.error
                                textFormat: Text.PlainText
                                color: parent.tone
                                wrapMode: Text.Wrap
                                font.pixelSize: 14
                                Accessible.role: Accessible.AlertMessage
                            }
                        }
                    }
                }
                NativeButton {
                    objectName: "projectCredentialsBackButton"
                    visible: root.controller.loginStep === "project"
                    colors: root.colors
                    Layout.fillWidth: true
                    text: qsTr("Back")
                    enabled: !root.controller.busy
                    onClicked: root.controller.backToCredentials()
                }
                LoginErrorBanner {
                    visible: loginCard.savedAccountsOnly && root.controller.error.length > 0
                }
            }
        }
    }

    Rectangle {
        id: profileLinkFailure
        objectName: "profileLinkFailure"
        visible: root.controller.authenticated && root.controller.profileLinkFailed
        anchors.top: parent.top
        width: parent.width
        height: visible ? Math.max(profileLinkFailureText.implicitHeight, profileLinkFailureClose.height) + 25 : 0
        readonly property color tone: root.colors["notice-base"]
        color: Qt.rgba(tone.r, tone.g, tone.b, 0.2)
        Accessible.role: Accessible.AlertMessage
        Accessible.name: profileLinkFailureText.text
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width; height: 1
            color: Qt.rgba(profileLinkFailure.tone.r, profileLinkFailure.tone.g, profileLinkFailure.tone.b, 0.6)
        }
        Text {
            id: profileLinkFailureText
            objectName: "profileLinkFailureText"
            anchors.left: parent.left; anchors.leftMargin: 16
            anchors.right: profileLinkFailureClose.left; anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Could not open this profile. It may be unavailable or you may not have access.")
            textFormat: Text.PlainText
            font.pixelSize: 14
            lineHeight: 20
            lineHeightMode: Text.FixedHeight
            color: profileLinkFailure.tone
            wrapMode: Text.Wrap
        }
        NativeButton {
            id: profileLinkFailureClose
            objectName: "profileLinkFailureClose"
            anchors.right: parent.right; anchors.rightMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            colors: root.colors
            text: qsTr("Close")
            height: 32
            implicitWidth: contentItem.implicitWidth + 24
            horizontalPadding: 12
            verticalPadding: 0
            font.pixelSize: 12
            font.weight: Font.Medium
            background: Rectangle {
                radius: 6
                color: profileLinkFailureClose.down ? root.colors["card-bg"]
                    : profileLinkFailureClose.hovered ? root.colors["bg-elevated"] : root.colors["card-bg-active"]
                border.width: profileLinkFailureClose.visualFocus ? 2 : 1
                border.color: profileLinkFailureClose.visualFocus ? root.colors.accent
                    : profileLinkFailureClose.hovered ? root.colors["border-subtle"] : "transparent"
            }
            onClicked: root.controller.dismissProfileLinkFailure()
        }
    }

    Rectangle {
        id: topBar
        Keys.onEscapePressed: event => root.handleChatEscape(event)
        objectName: "topBar"
        visible: root.controller.authenticated
        anchors.top: profileLinkFailure.bottom
        width: parent.width
        readonly property real titleStripHeight: Qt.platform.os === "osx" ? 28 : 0
        readonly property real toolbarHeight: Math.max(48, profileControl.height) + 16
        height: titleStripHeight + toolbarHeight + 1
        color: root.colors["bg-elevated"]
        bottomLeftRadius: 6
        bottomRightRadius: 6
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: root.colors["border-subtle"]
        }

        Row {
            x: 28
            anchors.verticalCenter: sectionButtons.verticalCenter
            SessionSwitcher {
                objectName: "sessionSwitcher"
                colors: root.colors
                controller: root.controller
            }
        }
        Row {
            id: sectionButtons
            anchors.horizontalCenter: parent.horizontalCenter
            y: topBar.titleStripHeight + (topBar.toolbarHeight - height) / 2
            spacing: 12
            NativeIconButton {
                objectName: "sectionChatButton"
                colors: root.colors
                asset: "chat_bubble.svg"
                help: qsTr("Chats and channels")
                selected: root.controller.section === "chat"
                activeBorder: true
                width: 48
                height: 48
                glyphSize: 32
                cornerRadius: 12
                iconColor: selected ? root.colors["icon-active"] : root.colors["text-muted"]
                onClicked: { root.controller.showSection("chat"); root.controller.closeDrafts(); root.sidebarQuery = ""; }
            }
            NativeIconButton {
                objectName: "sectionCalendarButton"
                colors: root.colors
                asset: "calendar.svg"
                help: qsTr("Calendar")
                selected: root.controller.section === "calendar"
                activeBorder: true
                width: 48
                height: 48
                glyphSize: 32
                cornerRadius: 12
                iconColor: selected ? root.colors["icon-active"] : root.colors["text-muted"]
                onClicked: root.controller.showSection("calendar")
            }
            NativeIconButton {
                objectName: "sectionMailButton"
                colors: root.colors
                asset: "mail.svg"
                help: qsTr("Mail")
                width: 48
                height: 48
                glyphSize: 32
                cornerRadius: 12
                selected: root.controller.section === "mail"
                activeBorder: true
                iconColor: selected ? root.colors["icon-active"] : root.colors["text-muted"]
                onClicked: root.controller.showSection("mail")
            }
        }
        Row {
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: sectionButtons.verticalCenter
            anchors.alignWhenCentered: false
            spacing: 12
            NativeDownloadCenter {
                controller: root.controller
                colors: root.colors
                width: implicitWidth; height: implicitHeight
                anchors.verticalCenter: parent.verticalCenter
            }
            NativeIconButton {
                id: updateIndicator
                objectName: "nativeUpdateIndicator"
                anchors.verticalCenter: parent.verticalCenter
                anchors.alignWhenCentered: false
                visible: aboutDrawer.indicatorVisible
                colors: root.colors
                width: 36; height: 36; glyphSize: 18
                asset: "download.svg"
                help: aboutDrawer.indicatorLabel
                iconColor: hovered ? root.colors["text-primary"] : root.colors.accent
                onClicked: root.openAbout(false)
                background: Item {
                    Rectangle {
                        anchors.fill: parent; anchors.margins: -2
                        visible: updateIndicator.visualFocus
                        radius: 8; color: "transparent"
                        border.width: 2; border.color: root.colors["accent-soft"]
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: 6
                        color: updateIndicator.hovered ? root.colors["card-bg-active"] : root.colors["accent-soft"]
                        border.width: 1
                        border.color: updateIndicator.hovered ? root.colors.accent : root.colors["border-subtle"]
                    }
                }
                Rectangle {
                    anchors.right: parent.right; anchors.rightMargin: 3
                    anchors.top: parent.top; anchors.topMargin: 3
                    width: 8; height: 8; radius: 4
                    color: root.colors["bg-elevated"]
                    Rectangle {
                        objectName: "nativeUpdateIndicatorDot"
                        anchors.centerIn: parent
                        width: 6; height: 6; radius: 3
                        color: root.colors.accent
                    }
                    Accessible.ignored: true
                }
                SequentialAnimation on opacity {
                    running: updateIndicator.visible && root.visible && aboutDrawer.phase === "downloading"
                    loops: Animation.Infinite
                    NumberAnimation { from: 1; to: 0.5; duration: 1000 }
                    NumberAnimation { from: 0.5; to: 1; duration: 1000 }
                    onStopped: updateIndicator.opacity = 1
                }
            }
            Button {
                id: profileControl
                objectName: "accountMenuButton"
                readonly property string displayName: root.controller.viewerDisplayName || root.controller.account.displayName || qsTr("Profile")
                readonly property string email: root.controller.account.displayEmail || root.controller.account.email || ""
                readonly property string presence: root.controller.authenticated ? String(root.controller.account.presence || "") : ""
                readonly property string statusLabel: root.controller.authenticated
                    ? String(root.controller.account.statusLabel || "") || root.presenceLabel(presence) : ""
                implicitWidth: Math.min(320, 36 + 8 + Math.max(profileNameText.implicitWidth,
                    profileEmailText.visible ? profileEmailText.implicitWidth : 0,
                    profileStatusText.visible ? Math.min(profileStatusText.implicitWidth, profileStatusMetrics.advanceWidth("0") * 32) : 0) + 8 + 16 + 12)
                width: Math.min(implicitWidth, Math.max(96, root.width * 0.3))
                height: Math.max(48, profileTextColumn.implicitHeight + topPadding + bottomPadding)
                padding: 6
                horizontalPadding: 6
                verticalPadding: 6
                hoverEnabled: true
                Accessible.name: qsTr("Profile")
                Accessible.description: displayName + (statusLabel.length ? ", " + statusLabel : "") + (email.length ? ", " + email : "")
                background: Rectangle {
                    radius: 6
                    color: profileControl.hovered || profileControl.down ? root.colors["card-bg-active"] : "transparent"
                    border.width: profileControl.visualFocus ? 1 : 0
                    border.color: root.colors.accent
                }
                // Plain positioners retain the Source fractional line boxes;
                // Layouts rounded this stack and its available text width.
                contentItem: Row {
                    // Keep Source subpixel centers for fractional line boxes.
                    anchors.alignWhenCentered: false
                    spacing: 8
                    Rectangle {
                        objectName: "profileAvatar"
                        width: 36; height: 36
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.alignWhenCentered: false
                        color: root.colors.bg
                        border.color: root.colors["border-subtle"]
                        radius: 18
                        NativeAvatarImage {
                            id: profilePhoto
                            anchors.fill: parent
                            reference: String(root.controller.account.avatar || "")
                            loadActive: root.visible && root.controller.authenticated
                        }
                        PresenceBadge {
                            objectName: "presence-viewer"
                            presence: profileControl.presence
                        }
                        Text {
                            visible: !profilePhoto.replacesFallback
                            objectName: "profileInitial"
                            anchors.centerIn: parent
                            text: profileControl.displayName.slice(0, 1).toUpperCase()
                            textFormat: Text.PlainText
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                            color: root.colors["text-primary"]
                            Accessible.ignored: true
                        }
                        Accessible.ignored: true
                    }
                    Column {
                        id: profileTextColumn
                        width: Math.max(0, profileControl.availableWidth - 36 - 16 - 2 * 8)
                        height: implicitHeight
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.alignWhenCentered: false
                        spacing: 0
                        Text {
                            id: profileNameText
                            objectName: "profileDisplayName"
                            text: profileControl.displayName
                            textFormat: Text.PlainText
                            width: Math.min(implicitWidth, profileTextColumn.width)
                            height: 20
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            color: root.colors["text-primary"]
                            font.pixelSize: 14
                            font.weight: Font.Medium
                            lineHeightMode: Text.FixedHeight
                            lineHeight: 20
                        }
                        FontMetrics {
                            id: profileStatusMetrics
                            font: profileStatusText.font
                        }
                        Text {
                            id: profileStatusText
                            objectName: "profilePresenceStatus"
                            visible: profileControl.statusLabel.length > 0
                            text: profileControl.statusLabel
                            textFormat: Text.PlainText
                            width: Math.min(implicitWidth, profileTextColumn.width, profileStatusMetrics.advanceWidth("0") * 32)
                            height: 13.75
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            color: root.colors["text-secondary"]
                            font.pixelSize: 11
                            lineHeightMode: Text.FixedHeight
                            lineHeight: 13.75
                            HoverHandler { id: profileStatusHover }
                            ToolTip.visible: profileStatusHover.hovered && profileControl.statusLabel.length > 32
                            ToolTip.text: profileControl.statusLabel
                        }
                        Text {
                            id: profileEmailText
                            objectName: "profileEmail"
                            visible: profileControl.email.length > 0
                            text: profileControl.email
                            textFormat: Text.PlainText
                            width: Math.min(implicitWidth, profileTextColumn.width)
                            height: 13.75
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            color: root.colors["text-secondary"]
                            font.pixelSize: 11
                            lineHeightMode: Text.FixedHeight
                            lineHeight: 13.75
                        }
                    }
                    NativeIconButton {
                        colors: root.colors
                        asset: "chevron-down.svg"
                        enabled: false
                        width: 16; height: 16
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.alignWhenCentered: false
                        glyphSize: 16
                        icon.color: root.colors["text-muted"]
                        rotation: accountPopup.visible ? 180 : 0
                        Accessible.ignored: true
                    }
                }
                onClicked: accountPopup.open()
            }
        }
    }

    function openOwnStatus() {
        if (!root.controller.authenticated)
            return;
        const contextKey = String(root.controller.ownStatusContextKey || "");
        const facts = root.controller.ownStatus;
        if (!contextKey.length || facts.pending)
            return;
        ownStatusDialog.contextKey = contextKey;
        ownStatusText.text = String(facts.text || "");
        ownStatusDialog.emojiDraft = String(facts.emoji || "");
        ownStatusAway.checked = Boolean(facts.away);
        ownStatusDialog.submitted = false;
        ownStatusDialog.emojiPickerOpen = false;
        accountPopup.close();
        ownStatusDialog.open();
    }

    function settleOwnStatusUi() {
        const contextKey = String(root.controller.ownStatusContextKey || "");
        if (!root.controller.authenticated || !contextKey.length
                || (ownStatusDialog.contextKey.length && ownStatusDialog.contextKey !== contextKey)) {
            ownStatusDialog.contextKey = "";
            ownStatusDialog.submitted = false;
            ownStatusDialog.emojiPickerOpen = false;
            ownStatusText.text = "";
            ownStatusDialog.emojiDraft = "";
            ownStatusAway.checked = false;
            ownStatusDialog.close();
            ownStatusErrorToast.close();
            return;
        }
        if (!ownStatusDialog.visible)
            return;
        const facts = root.controller.ownStatus;
        if (ownStatusDialog.submitted && !facts.pending) {
            ownStatusDialog.submitted = false;
            if (String(facts.error || "").length)
                ownStatusErrorToast.open();
            else
                ownStatusDialog.close();
        }
    }

    component StatusSpinner: Canvas {
        width: 14; height: 14
        Accessible.ignored: true
        onPaint: {
            const c = getContext("2d"); c.reset(); c.lineWidth = 2;
            c.strokeStyle = root.colors["on-accent"]; c.globalAlpha = 0.25;
            c.beginPath(); c.arc(7, 7, 6, 0, Math.PI * 2); c.stroke();
            c.globalAlpha = 1; c.beginPath(); c.arc(7, 7, 6, 0, Math.PI / 2); c.stroke();
        }
        RotationAnimator on rotation {
            from: 0; to: 360; duration: 1000; loops: Animation.Infinite
            running: ownStatusDialog.visible && ownStatusDialog.pending
        }
    }

    component StatusActionButton: Button {
        id: statusButton
        property bool primary: false
        property bool outlined: false
        property bool selected: false
        property real disabledTextOpacity: 0.5
        implicitHeight: 36
        implicitWidth: contentItem.implicitWidth + leftPadding + rightPadding
        horizontalPadding: 12
        verticalPadding: 8
        hoverEnabled: true
        font.pixelSize: 14
        font.weight: primary ? Font.Medium : Font.Normal
        contentItem: Text {
            text: statusButton.text
            textFormat: Text.PlainText
            font.pixelSize: statusButton.font.pixelSize
            lineHeightMode: Text.FixedHeight
            lineHeight: statusButton.font.pixelSize === 12 ? 16 : 20
            font.weight: statusButton.font.weight
            color: statusButton.primary ? root.colors["on-accent"] : statusButton.outlined ? root.colors["text-primary"] : root.colors["text-muted"]
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            opacity: statusButton.enabled ? 1 : statusButton.disabledTextOpacity
        }
        background: Rectangle {
            radius: 6
            color: statusButton.primary ? root.colors.accent
                : statusButton.outlined && statusButton.selected ? Qt.rgba(root.colors.accent.r, root.colors.accent.g, root.colors.accent.b, 0.15)
                : statusButton.outlined ? (statusButton.hovered ? root.colors["bg-elevated"] : root.colors.bg)
                : statusButton.hovered ? root.colors.bg : "transparent"
            opacity: statusButton.enabled ? 1 : 0.5
            border.width: statusButton.outlined ? 1 : statusButton.visualFocus ? 2 : 0
            border.color: statusButton.selected || statusButton.visualFocus ? root.colors.accent : root.colors["border-subtle"]
        }
    }

    Dialog {
        id: ownStatusDialog
        objectName: "ownStatusDialog"
        title: qsTr("Status")
        header: null
        footer: null
        property string contextKey: ""
        property string emojiDraft: ""
        property bool emojiPickerOpen: false
        property bool submitted: false
        readonly property bool pending: Boolean(root.controller.ownStatus.pending)
        readonly property color accentColor: root.colors.accent
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(448, root.width - 32)
        padding: 1 // The CSS border encloses, rather than overlays, the content box.
        modal: true
        dim: true
        focus: true
        closePolicy: pending ? Popup.NoAutoClose : Popup.CloseOnEscape | Popup.CloseOnPressOutside
        Overlay.modal: Rectangle { color: "#80000000" }
        background: Rectangle {
            radius: 10
            color: root.colors["bg-elevated"]
            border.color: root.colors["border-subtle"]
        }
        onOpened: {
            const first = statusPresets.itemAt(0);
            if (first) first.forceActiveFocus(Qt.PopupFocusReason);
        }
        onClosed: {
            submitted = false;
            emojiPickerOpen = false;
            if (root.controller.authenticated) profileControl.forceActiveFocus(Qt.PopupFocusReason);
        }
        contentItem: ColumnLayout {
            spacing: 0
            ColumnLayout {
                spacing: 4
                Layout.fillWidth: true
                Layout.margins: 20
                Layout.topMargin: 16
                Layout.bottomMargin: 16
                Text {
                    text: qsTr("Status"); textFormat: Text.PlainText
                    font.pixelSize: 16; font.weight: Font.DemiBold
                    lineHeightMode: Text.FixedHeight; lineHeight: 24
                    color: root.colors["text-primary"]
                    Layout.fillWidth: true
                }
                Text {
                    text: qsTr("Add an emoji and text to show your current availability.")
                    textFormat: Text.PlainText; wrapMode: Text.Wrap
                    color: root.colors["text-muted"]; font.pixelSize: 14
                    lineHeightMode: Text.FixedHeight; lineHeight: 20
                    Layout.fillWidth: true
                }
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: root.colors["border-subtle"] }
            ColumnLayout {
                spacing: 16
                Layout.fillWidth: true
                Layout.margins: 20
                Layout.topMargin: 16
                Layout.bottomMargin: 16
                Flow {
                    Layout.fillWidth: true
                    spacing: 8
                    Repeater {
                        id: statusPresets
                        model: ["💬", "🏠", "🌴", "🍽️", "⛑️", "🗓️"]
                        delegate: Button {
                            id: preset
                            required property string modelData
                            required property int index
                            objectName: "ownStatusPreset-" + index
                            width: 36; height: 36; padding: 0
                            Accessible.name: qsTr("Status") + " " + modelData
                            background: Rectangle {
                                radius: 6
                                color: ownStatusDialog.emojiDraft === preset.modelData
                                    ? Qt.rgba(ownStatusDialog.accentColor.r, ownStatusDialog.accentColor.g, ownStatusDialog.accentColor.b, 0.15)
                                    : root.colors.bg
                                border.color: preset.visualFocus || ownStatusDialog.emojiDraft === preset.modelData
                                    ? root.colors.accent : root.colors["border-subtle"]
                            }
                            contentItem: Text { text: preset.modelData; font.pixelSize: 16; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            onClicked: { ownStatusDialog.emojiDraft = modelData; ownStatusDialog.emojiPickerOpen = false; }
                        }
                    }
                    StatusActionButton {
                        objectName: "ownStatusChooseEmoji"
                        text: qsTr("Choose emoji")
                        font.pixelSize: 12; font.weight: Font.Medium
                        outlined: true
                        selected: ownStatusDialog.emojiPickerOpen
                        // 10px CSS padding plus the 1px border on each side.
                        horizontalPadding: 11
                        verticalPadding: 0
                        onClicked: ownStatusDialog.emojiPickerOpen = !ownStatusDialog.emojiPickerOpen
                    }
                }
                Rectangle {
                    id: statusPickerFrame
                    Layout.fillWidth: true
                    Layout.preferredHeight: 320 + 2 * border.width
                    visible: ownStatusDialog.visible && ownStatusDialog.emojiPickerOpen
                    radius: 8; color: "transparent"
                    border.width: 1; border.color: root.colors["border-subtle"]
                    clip: true
                    Loader {
                        anchors.fill: parent; anchors.margins: statusPickerFrame.border.width
                        active: statusPickerFrame.visible
                        onLoaded: item.focusSearch()
                        sourceComponent: Component {
                            NativeReactionPicker {
                                objectName: "ownStatusEmojiPicker"
                                choices: root.controller.reactionChoices
                                suggested: []
                                dark: theme.dark
                                skinTonesEnabled: false
                                previewEnabled: true
                                active: true
                                onChosen: emojiName => {
                                    const rows = root.controller.reactionChoices;
                                    for (let i = 0; i < rows.length; ++i) {
                                        if (rows[i].emojiName === emojiName) {
                                            const value = String(rows[i].pickerUnicode || "").trim();
                                            if (value.length) ownStatusDialog.emojiDraft = value.slice(0, 64);
                                            break;
                                        }
                                    }
                                    ownStatusDialog.emojiPickerOpen = false;
                                }
                            }
                        }
                    }
                }
                ColumnLayout {
                    spacing: 6
                    Layout.fillWidth: true
                    Text {
                        text: qsTr("Status"); textFormat: Text.PlainText
                        color: root.colors["text-muted"]; font.pixelSize: 14
                        lineHeightMode: Text.FixedHeight; lineHeight: 20
                        Layout.fillWidth: true
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: Math.max(statusDraftEmoji.implicitHeight, ownStatusText.implicitHeight) + 2 * (8 + border.width)
                        radius: 6; color: root.colors.bg; border.color: root.colors["border-subtle"]
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 13; anchors.rightMargin: 13
                            anchors.topMargin: 9; anchors.bottomMargin: 9
                            spacing: 8
                            Text {
                                id: statusDraftEmoji
                                objectName: "ownStatusEmojiDraft"
                                text: ownStatusDialog.emojiDraft || "🙂"
                                textFormat: Text.PlainText; font.pixelSize: 16
                                lineHeightMode: Text.FixedHeight; lineHeight: 24
                                horizontalAlignment: Text.AlignHCenter
                                Layout.minimumWidth: 20
                            }
                            TextField {
                                id: ownStatusText
                                objectName: "ownStatusText"
                                Layout.fillWidth: true
                                Layout.preferredHeight: font.pixelSize * 10 / 7
                                padding: 0
                                leftPadding: 0; rightPadding: 0; topPadding: 0; bottomPadding: 0
                                maximumLength: 256
                                selectByMouse: true
                                color: root.colors["text-primary"]
                                placeholderTextColor: root.colors["text-muted"]
                                placeholderText: qsTr("Set your status")
                                // Pinned global input rule wins over text-sm.
                                font.pixelSize: 16
                                background: Item {}
                                Accessible.name: qsTr("Status")
                            }
                        }
                    }
                }
                CheckBox {
                    id: ownStatusAway
                    objectName: "ownStatusAway"
                    text: qsTr("Mark me as away")
                    Layout.fillWidth: true
                    implicitHeight: 20
                    padding: 0
                    indicator: Rectangle {
                        width: 16; height: 16; radius: 3
                        anchors.verticalCenter: parent.verticalCenter
                        color: ownStatusAway.checked ? root.colors.accent : root.colors.bg
                        border.color: ownStatusAway.checked ? root.colors.accent : root.colors["border-subtle"]
                        Text { anchors.centerIn: parent; text: "✓"; font.pixelSize: 12; color: root.colors["on-accent"]; visible: ownStatusAway.checked }
                    }
                    contentItem: Text {
                        text: ownStatusAway.text; textFormat: Text.PlainText
                        leftPadding: 24; verticalAlignment: Text.AlignVCenter
                        color: root.colors["text-primary"]; font.pixelSize: 14
                        lineHeightMode: Text.FixedHeight; lineHeight: 20
                    }
                }
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: root.colors["border-subtle"] }
            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 20; Layout.topMargin: 12; Layout.bottomMargin: 12
                spacing: 8
                StatusActionButton {
                    objectName: "ownStatusClear"
                    disabledTextOpacity: 1
                    text: qsTr("Clear status")
                    enabled: !ownStatusDialog.pending
                    onClicked: { ownStatusText.text = ""; ownStatusDialog.emojiDraft = ""; ownStatusAway.checked = false; ownStatusDialog.emojiPickerOpen = false; }
                }
                Item { Layout.fillWidth: true }
                StatusActionButton {
                    objectName: "ownStatusCancel"
                    horizontalPadding: 16
                    text: qsTr("Cancel")
                    enabled: !ownStatusDialog.pending
                    onClicked: ownStatusDialog.close()
                }
                StatusActionButton {
                    id: statusSaveButton
                    objectName: "ownStatusSave"
                    horizontalPadding: 16
                    text: qsTr("Save")
                    contentItem: RowLayout {
                        spacing: 8
                        opacity: statusSaveButton.enabled ? 1 : 0.5
                        StatusSpinner { visible: ownStatusDialog.pending; Layout.preferredWidth: 14; Layout.preferredHeight: 14 }
                        Text {
                            text: statusSaveButton.text; textFormat: Text.PlainText
                            color: root.colors["on-accent"]; font.pixelSize: 14; font.weight: Font.Medium
                            lineHeightMode: Text.FixedHeight; lineHeight: 20
                        }
                    }
                    primary: true
                    enabled: !ownStatusDialog.pending
                    onClicked: {
                        if (!root.controller.authenticated || ownStatusDialog.pending) return;
                        ownStatusDialog.submitted = true;
                        root.controller.saveOwnStatus(ownStatusText.text, ownStatusDialog.emojiDraft, ownStatusAway.checked);
                    }
                }
            }
        }
    }

    Popup {
        id: ownStatusErrorToast
        objectName: "ownStatusErrorToast"
        readonly property color noticeColor: root.colors["notice-base"]
        parent: Overlay.overlay
        x: root.width - width - 16
        y: 16
        z: 10000
        width: Math.min(384, root.width - 32)
        leftPadding: 16; rightPadding: 16; topPadding: 12; bottomPadding: 12
        closePolicy: Popup.NoAutoClose
        modal: false
        focus: false
        background: Rectangle {
            radius: 8
            color: Qt.rgba(ownStatusErrorToast.noticeColor.r, ownStatusErrorToast.noticeColor.g, ownStatusErrorToast.noticeColor.b, 0.1)
            border.color: Qt.rgba(ownStatusErrorToast.noticeColor.r, ownStatusErrorToast.noticeColor.g, ownStatusErrorToast.noticeColor.b, 0.4)
        }
        contentItem: RowLayout {
            spacing: 8
            Text {
                objectName: "ownStatusErrorText"
                text: qsTr("Failed to update status")
                textFormat: Text.PlainText; wrapMode: Text.Wrap
                color: root.colors["notice-base"]; font.pixelSize: 14
                Layout.fillWidth: true
                Accessible.role: Accessible.AlertMessage
                Accessible.name: text
            }
            NativeIconButton {
                objectName: "ownStatusDismissError"
                colors: root.colors; asset: "close.svg"
                implicitWidth: 20; implicitHeight: 20
                help: qsTr("Dismiss")
                glyphSize: 16
                iconColor: ownStatusErrorToast.noticeColor
                onClicked: ownStatusErrorToast.close()
            }
        }
        Timer { interval: 5000; running: ownStatusErrorToast.visible; onTriggered: ownStatusErrorToast.close() }
    }

    Popup {
        id: accountPopup
        x: root.width - width - 16
        y: topBar.height - 4
        width: 260
        padding: 12
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: root.colors["bg-elevated"]
            border.color: root.colors["border-subtle"]
            radius: 10
        }
        contentItem: ColumnLayout {
            spacing: 8
            NativeButton {
                objectName: "ownStatusAction"
                colors: root.colors
                Layout.fillWidth: true
                text: qsTr("Status")
                enabled: root.controller.authenticated && String(root.controller.ownStatusContextKey || "").length > 0
                onClicked: root.openOwnStatus()
            }
            NativeButton {
                objectName: "personalInfoAction"
                colors: root.colors
                Layout.fillWidth: true
                text: qsTr("Personal info")
                enabled: root.controller.authenticated
                onClicked: {
                    accountPopup.close()
                    rightDrawer.openPersonalInfo()
                }
            }
            NativeButton {
                objectName: "aboutAction"
                colors: root.colors
                Layout.fillWidth: true
                text: qsTr("App version")
                onClicked: root.openAbout(true)
            }
            // Issue #50: the reference user menu lists Diagnostics directly
            // below the App version row and navigates to /settings/logs
            // (right-panel-user-menu.ui.tsx:361-379, settings.connectionDiagnostics).
            NativeButton {
                objectName: "diagnosticsAction"
                colors: root.colors
                Layout.fillWidth: true
                text: qsTr("Diagnostics")
                enabled: root.controller.authenticated
                onClicked: {
                    accountPopup.close()
                    root.controller.showSection("diagnostics")
                }
            }
            NativeButton {
                colors: root.colors
                Layout.fillWidth: true
                text: theme.dark ? qsTr("Use light appearance") : qsTr("Use dark appearance")
                objectName: "appearanceToggle"
                onClicked: root.preferences.toggleMode()
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                Repeater {
                    model: [
                        {
                            id: "light",
                            name: qsTr("Light")
                        },
                        {
                            id: "dark",
                            name: qsTr("Dark")
                        },
                        {
                            id: "system",
                            name: qsTr("System")
                        }
                    ]
                    delegate: NativeButton {
                        required property var modelData
                        objectName: "appearance-mode-" + modelData.id
                        colors: root.colors
                        Layout.fillWidth: true
                        implicitWidth: 0
                        text: modelData.name
                        primary: root.preferences.mode === modelData.id
                        onClicked: root.preferences.mode = modelData.id
                    }
                }
            }
            Repeater {
                model: [
                    {
                        id: "orange-warm",
                        name: qsTr("Orange Warm")
                    },
                    {
                        id: "blue-cold",
                        name: qsTr("Blue Cold")
                    },
                    {
                        id: "blue-mist",
                        name: qsTr("Blue Mist")
                    },
                    {
                        id: "emerald-chat",
                        name: qsTr("Emerald Chat")
                    }
                ]
                delegate: NativeButton {
                    required property var modelData
                    objectName: "palette-" + modelData.id
                    colors: root.colors
                    Layout.fillWidth: true
                    text: modelData.name
                    primary: theme.paletteId === modelData.id
                    onClicked: root.preferences.paletteId = modelData.id
                }
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: root.colors["border-subtle"]
            }
            NativeButton {
                id: languageSetting
                objectName: "languageSetting"
                property bool expanded: false
                colors: root.colors
                Layout.fillWidth: true
                text: qsTr("Language") + " · " + (root.preferences.locale === "ru" ? "Русский" : "English")
                onClicked: expanded = !expanded
            }
            RowLayout {
                visible: languageSetting.expanded
                Layout.fillWidth: true
                NativeButton {
                    objectName: "language-en"
                    colors: root.colors
                    Layout.fillWidth: true
                    text: qsTr("English")
                    primary: root.preferences.locale === "en"
                    onClicked: root.preferences.locale = "en"
                }
                NativeButton {
                    objectName: "language-ru"
                    colors: root.colors
                    Layout.fillWidth: true
                    text: qsTr("Русский")
                    primary: root.preferences.locale === "ru"
                    onClicked: root.preferences.locale = "ru"
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Text {
                    text: qsTr("Disk cache (GB)")
                    textFormat: Text.PlainText
                    color: root.colors["text-secondary"]
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    font.pixelSize: 12
                }
                SpinBox {
                    id: cacheLimit
                    objectName: "cacheLimitGb"
                    from: 1
                    to: 1000
                    value: root.preferences.cacheLimitGb
                    editable: true
                    implicitWidth: 104
                    implicitHeight: 32
                    Accessible.name: qsTr("Disk cache limit in GB")
                    onValueModified: root.preferences.cacheLimitGb = value
                }
            }
            Text {
                objectName: "cacheSettingsBusy"
                visible: root.controller.authenticated && root.preferences.cacheBusy
                text: qsTr("Updating disk cache…")
                textFormat: Text.PlainText
                color: root.colors["text-secondary"]
                wrapMode: Text.Wrap
                font.pixelSize: 12
                Layout.fillWidth: true
                Accessible.role: Accessible.AlertMessage
            }
            Text {
                objectName: "cacheSettingsLimits"
                visible: root.controller.authenticated && (root.preferences.cacheBusy || root.preferences.cacheAppliedLimitBytes > 0)
                text: root.preferences.cacheAppliedLimitBytes > 0
                    ? qsTr("Requested: %1 GB · Applied: %2 GB").arg(root.preferences.cacheLimitGb).arg(root.preferences.cacheAppliedLimitBytes / 1000000000)
                    : qsTr("Requested: %1 GB").arg(root.preferences.cacheLimitGb)
                textFormat: Text.PlainText
                color: root.colors["text-muted"]
                wrapMode: Text.Wrap
                font.pixelSize: 12
                Layout.fillWidth: true
            }
            Text {
                objectName: "cacheSettingsUsage"
                visible: root.controller.authenticated && root.preferences.cacheAppliedLimitBytes > 0
                text: qsTr("Used: %1").arg(Qt.locale(root.preferences.locale).formattedDataSize(root.preferences.cacheUsageBytes, 1, Locale.DataSizeSIFormat))
                textFormat: Text.PlainText
                color: root.colors["text-muted"]
                wrapMode: Text.Wrap
                font.pixelSize: 12
                Layout.fillWidth: true
            }
            Text {
                objectName: "cacheSettingsUnavailable"
                visible: root.controller.authenticated && !root.preferences.cacheBusy && !root.preferences.cacheAvailable
                text: qsTr("Disk cache is unavailable.")
                textFormat: Text.PlainText
                color: root.colors["text-muted"]
                wrapMode: Text.Wrap
                font.pixelSize: 12
                Layout.fillWidth: true
            }
            Text {
                objectName: "cacheSettingsError"
                visible: root.preferences.cacheError.length > 0
                text: root.preferences.cacheError
                textFormat: Text.PlainText
                color: root.colors.danger
                wrapMode: Text.Wrap
                font.pixelSize: 12
                Layout.fillWidth: true
                Accessible.role: Accessible.AlertMessage
            }
            Text {
                visible: root.preferences.error.length > 0
                text: root.preferences.error
                textFormat: Text.PlainText
                color: root.colors.danger
                wrapMode: Text.Wrap
                font.pixelSize: 12
                Layout.fillWidth: true
                Accessible.role: Accessible.AlertMessage
            }
            NativeButton {
                objectName: "refreshButton"
                colors: root.colors
                Layout.fillWidth: true
                text: qsTr("Refresh")
                enabled: !root.controller.busy
                onClicked: {
                    root.controller.refresh();
                    accountPopup.close();
                }
            }
            NativeButton {
                colors: root.colors
                Layout.fillWidth: true
                text: qsTr("Sign out")
                objectName: "signOutButton"
                onClicked: {
                    accountPopup.close();
                    root.controller.closeDrafts();
                    root.controller.logout();
                }
            }
        }
    }

    // Issue #83: one rail cell for both the pinned All slot and the scrollable
    // group. Geometry measured from workspace_ui 61dacef7
    // folder-rail-folder-items.ui.tsx::VerticalFolderItem: 72x56 cell, 48x64
    // card scaled 1.1 while selected, icon 32 at top-10, label at top-42
    // width 52 text-xs leading-4; visual rules from folder-rail-visual.lib.ts
    // (hover surface only, selection carries on icon/label color + scale).
    Component {
        id: folderCellComponent
        Item {
            id: folderCell
            required property var modelData
            readonly property var folder: folderCell.modelData ?? ({})
            readonly property string systemType: String(folder.systemType || "created")
            readonly property bool isCustom: systemType === "created"
            readonly property bool isSelected: String(root.controller.selectedFolder) === String(folder.id)
            readonly property bool interactive: folderCellPointer.containsMouse || isSelected
            readonly property color folderColor: Qt.rgba(
                (((folder.color || 0) >> 16) & 0xff) / 255,
                (((folder.color || 0) >> 8) & 0xff) / 255,
                ((folder.color || 0) & 0xff) / 255, 1.0)
            readonly property string iconAsset: systemType === "all" ? "folders.svg"
                : systemType === "personal" ? "profile.svg"
                : systemType === "channels" ? "channels.svg"
                : isSelected ? "folder_open.svg" : "folder.svg"
            width: 72
            height: 72
            Accessible.role: Accessible.Button
            Accessible.name: String(folder.title || "")
            MouseArea {
                id: folderCellPointer
                objectName: "folder-" + String(folderCell.folder.key)
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: (mouse) => {
                    // Issue #84: right click opens the rename/delete menu;
                    // selecting (left click) keeps closing drafts first.
                    if (mouse.button === Qt.RightButton) {
                        root.openFolderMenu(String(folderCell.folder.key),
                            folderCellPointer.mapToItem(root.contentItem, mouse.x, mouse.y));
                        return;
                    }
                    root.controller.closeDrafts(); root.controller.selectFolder(String(folderCell.folder.key));
                }
            }
            Item {
                id: folderCard
                objectName: "folderCard-" + String(folderCell.folder.key)
                width: 48
                height: 64
                anchors.centerIn: parent
                scale: folderCell.isSelected ? 1.1 : 1.0
                Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutQuad } }
                NativeIconButton {
                    colors: root.colors
                    objectName: "folderIcon-" + String(folderCell.folder.key)
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: 10
                    width: 32
                    height: 32
                    cornerRadius: 8
                    asset: folderCell.iconAsset
                    help: String(folderCell.folder.title || "")
                    glyphSize: 32
                    // The reference icon button selects like the whole cell,
                    // drafts-closing included (review cid 4577604523).
                    onClicked: { root.controller.closeDrafts(); root.controller.selectFolder(String(folderCell.folder.key)) }
                    iconColor: folderCell.isCustom ? folderCell.folderColor
                        : folderCell.interactive ? root.colors["text-primary"] : root.colors["text-muted"]
                    background: Rectangle {
                        // Hover surface only; selection stays outline-free
                        // (folder-rail-visual.lib.ts resolveSystemFolderSurfaceClassName).
                        visible: folderCellPointer.containsMouse && !folderCell.isSelected
                        radius: 8
                        color: folderCell.isCustom
                            ? Qt.rgba(folderCell.folderColor.r, folderCell.folderColor.g, folderCell.folderColor.b, 0.10)
                            : root.colors["sidebar-item-hover"]
                        border.width: 1
                        border.color: folderCell.isCustom
                            ? Qt.rgba(folderCell.folderColor.r, folderCell.folderColor.g, folderCell.folderColor.b, 0.22)
                            : root.colors["border-subtle"]
                    }
                }
                Rectangle {
                    visible: Number(folderCell.folder.unreadCount || 0) > 0
                    x: parent.width - width - 4
                    y: 0
                    z: 2
                    width: Math.max(16, folderBadgeText.implicitWidth + 6)
                    height: 16
                    radius: 8
                    color: root.colors["badge-bg"]
                    Text {
                        id: folderBadgeText
                        objectName: "folderBadge-" + String(folderCell.folder.key)
                        anchors.centerIn: parent
                        text: root.countText(Number(folderCell.folder.unreadCount || 0))
                        textFormat: Text.PlainText
                        color: root.colors["badge-text"]
                        font.pixelSize: 10
                    }
                }
                Text {
                    x: -2
                    y: 42
                    width: 52
                    height: 16
                    text: String(folderCell.folder.title || "")
                    textFormat: Text.PlainText
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight
                    font.pixelSize: 12
                    lineHeightMode: Text.FixedHeight
                    lineHeight: 16
                    color: folderCell.isCustom && folderCell.interactive ? folderCell.folderColor
                        : folderCell.interactive ? root.colors["text-primary"] : root.colors["text-muted"]
                }
            }
        }
    }

    // Issue #85: horizontal rail chip, measured from
    // folder-rail-folder-items.ui.tsx:226-240: h-8 rounded-lg px-2.5 text-xs
    // gap-1.5, icon 18, label max-w-[112px] truncate, badge at
    // -right-1 -top-2.5. The chip is purely visual: the strip-level MouseArea
    // owns clicks and drag exactly like the React rail owns gestures through
    // its capture-phase handlers, so a drag can start on a chip while a
    // non-drag press still selects it.
    component FolderRailChip: Item {
        id: folderChip
        required property var modelData
        readonly property var folder: folderChip.modelData ?? ({})
        readonly property string folderKey: String(folder.key ?? "")
        readonly property string systemType: String(folder.systemType || "created")
        readonly property bool isCustom: systemType === "created"
        readonly property bool isSelected: String(root.controller.selectedFolder) === String(folder.id)
        readonly property bool hovered: folderChip.folderKey.length > 0
            && folderRailHorizontal.hoverKey === folderChip.folderKey
        readonly property bool interactive: hovered || isSelected
        readonly property color folderColor: Qt.rgba(
            (((folder.color || 0) >> 16) & 0xff) / 255,
            (((folder.color || 0) >> 8) & 0xff) / 255,
            ((folder.color || 0) & 0xff) / 255, 1.0)
        readonly property string iconAsset: systemType === "all" ? "folders.svg"
            : systemType === "personal" ? "profile.svg"
            : systemType === "channels" ? "channels.svg"
            : isSelected ? "folder_open.svg" : "folder.svg"
        // Issue #85 review (cid 4623255478): the strip MouseArea is not the
        // only activation path. The chip is a keyboard-operable control:
        // Enter/Space and the accessible Press action select through the
        // same path a plain click drives (see folderRailStripPointer).
        function select() {
            root.controller.closeDrafts()
            root.controller.selectFolder(folderChip.folderKey)
        }
        activeFocusOnTab: true
        Keys.onReturnPressed: (event) => { folderChip.select(); event.accepted = true }
        Keys.onSpacePressed: (event) => { folderChip.select(); event.accepted = true }
        objectName: "folder-" + folderKey
        width: chipContent.implicitWidth + 20
        height: 32
        Accessible.role: Accessible.Button
        Accessible.name: String(folder.title || "")
        Accessible.onPressAction: folderChip.select()
        Rectangle {
            anchors.fill: parent
            radius: 8
            visible: folderChip.hovered && !folderChip.isSelected
            // Hover surface only; selection carries on icon/label color
            // (getFolderItemVisualState has no selected fill in either mode).
            color: folderChip.isCustom
                ? Qt.rgba(folderChip.folderColor.r, folderChip.folderColor.g, folderChip.folderColor.b, 0.10)
                : root.colors["sidebar-item-hover"]
            border.width: 1
            border.color: folderChip.isCustom
                ? Qt.rgba(folderChip.folderColor.r, folderChip.folderColor.g, folderChip.folderColor.b, 0.22)
                : root.colors["border-subtle"]
        }
        Row {
            id: chipContent
            x: 10
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6
            Image {
                width: 18
                height: 18
                source: "qrc:/workspace/icons/" + folderChip.iconAsset
                sourceSize.width: 18
                sourceSize.height: 18
                layer.enabled: true
                layer.effect: MultiEffect {
                    // This Qt kit's Effects plugin exposes colorization
                    // without the colorizationEnabled gate.
                    colorization: 1.0
                    colorizationColor: folderChip.isCustom ? folderChip.folderColor
                        : folderChip.interactive ? root.colors["text-primary"] : root.colors["text-muted"]
                }
            }
            Text {
                objectName: "folderChipLabel-" + folderChip.folderKey
                text: String(folderChip.folder.title || "")
                textFormat: Text.PlainText
                elide: Text.ElideRight
                width: Math.min(112, implicitWidth)
                font.pixelSize: 12
                // Row leaves the cross axis to the children; center the
                // 16 px line box against the 18 px icon.
                y: Math.round((18 - height) / 2)
                color: folderChip.isCustom && folderChip.interactive ? folderChip.folderColor
                    : folderChip.interactive ? root.colors["text-primary"] : root.colors["text-muted"]
            }
        }
        Rectangle {
            visible: Number(folderChip.folder.unreadCount || 0) > 0
            x: parent.width - width + 4
            y: -6
            z: 2
            width: Math.max(16, folderChipBadgeText.implicitWidth + 6)
            height: 16
            radius: 8
            color: root.colors["badge-bg"]
            Text {
                id: folderChipBadgeText
                objectName: "folderBadge-" + folderChip.folderKey
                anchors.centerIn: parent
                text: root.countText(Number(folderChip.folder.unreadCount || 0))
                textFormat: Text.PlainText
                color: root.colors["badge-text"]
                font.pixelSize: 10
            }
        }
    }

    Connections {
        target: root.controller
        // Reference handleSelectWorkspaceFolder: picking a different folder
        // clears the sidebar search query (sidebar-shell.ui.tsx:242-244).
        function onFolderSelected() { root.sidebarQuery = "" }
        // Issue #84: the form/delete dialogs follow the reference
        // "promise resolves true closes the modal" contract (folder-form-
        // modal.ui.tsx): submitted && !pending settles success (close) vs
        // failure (toast, modal stays open) exactly once per submit.
        function onFolderAddRequested() { root.openFolderCreate() }
        function onFolderMutationChanged() {
            const facts = root.controller.folderMutation;
            if (folderFormDialog.submitted && !facts.pending) {
                folderFormDialog.submitted = false;
                if (String(facts.error || "").length) {
                    root.folderErrorNotice = String(facts.error);
                    folderErrorToast.open();
                } else {
                    folderFormDialog.close();
                }
            } else if (folderDeleteDialog.submitted && !facts.pending) {
                folderDeleteDialog.submitted = false;
                if (String(facts.error || "").length) {
                    root.folderErrorNotice = String(facts.error);
                    folderErrorToast.open();
                } else {
                    folderDeleteDialog.close();
                }
            }
        }
    }

    // Static swatch chrome for the folder form's color row. The Repeater
    // variant left its delegates parented to an orphan popup content copy
    // (Qt 6.10 Popup lazy instantiation), so the eight presets are explicit
    // instances of one inline component instead.
    component FolderSwatch: Button {
        id: swatchButton
        required property real colorValue
        property bool selected: false
        width: 36; height: 36; padding: 0
        Accessible.name: qsTr("Color") + " " + String(colorValue)
        background: Rectangle {
            radius: 18
            color: Qt.rgba(
                ((swatchButton.colorValue >>> 16) & 0xff) / 255,
                ((swatchButton.colorValue >>> 8) & 0xff) / 255,
                (swatchButton.colorValue & 0xff) / 255, 1.0)
            border.width: 1
            border.color: swatchButton.selected ? root.colors["text-primary"] : root.colors["border-subtle"]
        }
        contentItem: Item {
            // ring-2 ring-accent-soft on the selected swatch.
            Rectangle {
                anchors.centerIn: parent
                width: 28; height: 28; radius: 14
                border.width: 2; border.color: root.colors["accent-soft"]
                color: "transparent"
                visible: swatchButton.selected
            }
        }
    }

    // Issue #84 (docs/NATIVE_DESKTOP_FOLDERS.md): rail CRUD surfaces. The
    // form modal internals are the port of workspace_ui 61dacef7
    // folder-form-modal.ui.tsx: name field focused on open (timeout 0), 8
    // round 36 px swatches with 10 px gap and the measured preset list,
    // preview chip at 12 % alpha of the selected color, submit gated on
    // non-empty name and, in edit mode, on an actual change.
    readonly property var folderColorPresets: [0xff2563eb, 0xff4f46e5, 0xff0d9488, 0xff059669,
        0xfff59e0b, 0xffdc2626, 0xff6b7280, 0xff334155]
    // Issue #85 (docs/NATIVE_DESKTOP_FOLDERS_RAIL_LAYOUT.md): the persisted
    // rail display options. The layout flag drives which rail view renders;
    // the visibility flag filters the system slots from a display-only
    // projection (selection and sidebar scoping stay on controller state, so
    // the All-chats fallback rule from #83 keeps holding).
    readonly property bool folderRailHorizontalMode: root.preferences.folderRailLayout === "horizontal"
    readonly property var folderRailRows: {
        const rows = []
        for (const row of root.controller.folders) {
            const systemType = String(row.systemType || "created")
            if (!root.preferences.showSystemFolders
                && (systemType === "personal" || systemType === "channels"))
                continue
            rows.push(row)
        }
        return rows
    }
    property string folderMenuTargetKey: ""
    property string folderFormMode: "create"
    property string folderFormTargetId: ""
    property string folderFormInitialName: ""
    property var folderFormInitialColor: 0xff2563eb
    property string folderErrorNotice: ""
    function folderMenuRow() {
        for (const row of root.controller.folders)
            if (String(row.key) === root.folderMenuTargetKey || String(row.id) === root.folderMenuTargetKey)
                return row;
        return null;
    }
    readonly property bool folderMenuCanEdit: {
        const row = root.folderMenuRow();
        return !!row && String(row.systemType) === "created" && !row.synthesized;
    }
    function openFolderCreate() {
        if (!root.controller.authenticated) return;
        root.folderFormMode = "create";
        root.folderFormTargetId = "";
        root.folderFormInitialName = "";
        root.folderFormInitialColor = root.folderColorPresets[0];
        folderFormDialog.open();
    }
    function openFolderRename() {
        const row = root.folderMenuRow();
        if (!row || !root.folderMenuCanEdit) return;
        root.folderFormMode = "edit";
        root.folderFormTargetId = String(row.id);
        root.folderFormInitialName = String(row.title || "");
        // Review 4616033386: the projection distinguishes an unset color (null)
        // from a stored black (numeric 0). The reference folder-form-modal
        // falls back to preset 0 only for an unset color
        // (initialBackgroundColor ?? FOLDER_COLOR_PRESETS[0]) and keeps a
        // stored value verbatim, so renaming a black folder resends black
        // instead of silently repainting it to the default blue.
        const raw = row.color;
        const hasColor = raw !== undefined && raw !== null;
        const rgb = hasColor ? Number(raw) : -1;
        const presetMatch = hasColor
            ? root.folderColorPresets.find(p => (p & 0xffffff) === rgb) : undefined;
        let matched = root.folderColorPresets[0];
        if (presetMatch !== undefined)
            matched = presetMatch;
        else if (hasColor)
            // A color outside the presets is carried through as stored: black
            // stays the numeric 0 itself; other hues regain the full ARGB
            // alpha the presets are defined with, so a rename never repaints.
            matched = rgb === 0 ? 0 : 4278190080 + rgb;
        root.folderFormInitialColor = matched;
        folderFormDialog.open();
    }
    function openFolderDelete() {
        const row = root.folderMenuRow();
        if (!row || !root.folderMenuCanEdit) return;
        folderDeleteDialog.targetId = String(row.id);
        folderDeleteDialog.targetTitle = String(row.title || "");
        folderDeleteDialog.open();
    }
    function openFolderMenu(key, point) {
        if (!root.controller.authenticated) return;
        root.folderMenuTargetKey = key;
        folderContextMenu.popup(point);
    }

    Popup {
        id: folderFormDialog
        objectName: "folderFormDialog"
        property bool submitted: false
        property real colorValue: 0xff2563eb
        readonly property bool pending: Boolean(root.controller.folderMutation.pending)
        readonly property bool hasName: folderNameField.text.trim().length > 0
        readonly property bool noChanges: root.folderFormMode === "edit"
            && folderNameField.text.trim() === root.folderFormInitialName.trim()
            && colorValue === root.folderFormInitialColor
        readonly property color selectedColor: Qt.rgba(
            ((colorValue >>> 16) & 0xff) / 255, ((colorValue >>> 8) & 0xff) / 255,
            (colorValue & 0xff) / 255, 1.0)
        function submit() {
            if (pending || !hasName || noChanges) return;
            submitted = true;
            if (root.folderFormMode === "create")
                root.controller.createFolder(folderNameField.text.trim(), colorValue);
            else
                root.controller.renameFolder(root.folderFormTargetId, folderNameField.text.trim(), colorValue);
        }
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(448, root.width - 32)
        padding: 1
        modal: true
        dim: true
        focus: true
        closePolicy: pending ? Popup.NoAutoClose : Popup.CloseOnEscape | Popup.CloseOnPressOutside
        Overlay.modal: Rectangle { color: "#80000000" }
        background: Rectangle {
            radius: 10
            color: root.colors["bg-elevated"]
            border.color: root.colors["border-subtle"]
        }
        onOpened: {
            submitted = false;
            if (root.folderFormMode === "create") {
                folderNameField.text = "";
                colorValue = root.folderColorPresets[0];
            } else {
                folderNameField.text = root.folderFormInitialName;
                colorValue = root.folderFormInitialColor;
            }
            // Reference autofocus on open with a timeout-0 (folder-form-modal).
            Qt.callLater(function() { folderNameField.forceActiveFocus(Qt.PopupFocusReason); });
        }
        onClosed: submitted = false
        contentItem: ColumnLayout {
            spacing: 16
            Text {
                text: root.folderFormMode === "create" ? qsTr("Create folder") : qsTr("Rename folder")
                textFormat: Text.PlainText
                font.pixelSize: 16; font.weight: Font.DemiBold
                color: root.colors["text-primary"]
                Layout.fillWidth: true
            }
            NativeField {
                id: folderNameField
                objectName: "folderNameField"
                colors: root.colors
                Layout.fillWidth: true
                Layout.preferredHeight: 36
                placeholderText: qsTr("Folder name")
                Accessible.name: qsTr("Folder name")
                maximumLength: 200
                enabled: !folderFormDialog.pending
                selectByMouse: true
                onAccepted: folderFormDialog.submit()
            }
            ColumnLayout {
                spacing: 8
                Layout.fillWidth: true
                Text {
                    text: qsTr("Color"); textFormat: Text.PlainText
                    font.pixelSize: 12; font.weight: Font.Medium
                    color: root.colors["text-muted"]
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 10
                    FolderSwatch { objectName: "folderSwatch-0"
                        colorValue: root.folderColorPresets[0]
                        selected: folderFormDialog.colorValue === colorValue
                        onClicked: folderFormDialog.colorValue = colorValue }
                    FolderSwatch { objectName: "folderSwatch-1"
                        colorValue: root.folderColorPresets[1]
                        selected: folderFormDialog.colorValue === colorValue
                        onClicked: folderFormDialog.colorValue = colorValue }
                    FolderSwatch { objectName: "folderSwatch-2"
                        colorValue: root.folderColorPresets[2]
                        selected: folderFormDialog.colorValue === colorValue
                        onClicked: folderFormDialog.colorValue = colorValue }
                    FolderSwatch { objectName: "folderSwatch-3"
                        colorValue: root.folderColorPresets[3]
                        selected: folderFormDialog.colorValue === colorValue
                        onClicked: folderFormDialog.colorValue = colorValue }
                    FolderSwatch { objectName: "folderSwatch-4"
                        colorValue: root.folderColorPresets[4]
                        selected: folderFormDialog.colorValue === colorValue
                        onClicked: folderFormDialog.colorValue = colorValue }
                    FolderSwatch { objectName: "folderSwatch-5"
                        colorValue: root.folderColorPresets[5]
                        selected: folderFormDialog.colorValue === colorValue
                        onClicked: folderFormDialog.colorValue = colorValue }
                    FolderSwatch { objectName: "folderSwatch-6"
                        colorValue: root.folderColorPresets[6]
                        selected: folderFormDialog.colorValue === colorValue
                        onClicked: folderFormDialog.colorValue = colorValue }
                    FolderSwatch { objectName: "folderSwatch-7"
                        colorValue: root.folderColorPresets[7]
                        selected: folderFormDialog.colorValue === colorValue
                        onClicked: folderFormDialog.colorValue = colorValue }
                }
                Rectangle {
                    objectName: "folderPreviewChip"
                    Layout.alignment: Qt.AlignLeft
                    color: Qt.rgba(folderFormDialog.selectedColor.r, folderFormDialog.selectedColor.g,
                        folderFormDialog.selectedColor.b, 0.12)
                    radius: 8
                    border.width: 1
                    border.color: root.colors["border-subtle"]
                    height: 33
                    width: folderPreviewRow.implicitWidth + 24
                    RowLayout {
                        id: folderPreviewRow
                        anchors.centerIn: parent
                        spacing: 8
                        Image {
                            source: "qrc:/workspace/icons/folder.svg"
                            sourceSize: Qt.size(16, 16)
                            Accessible.ignored: true
                        }
                        Text {
                            objectName: "folderPreviewText"
                            text: folderNameField.text.trim() || qsTr("Preview")
                            textFormat: Text.PlainText
                            font.pixelSize: 12
                            color: root.colors["text-muted"]
                        }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Item { Layout.fillWidth: true }
                StatusActionButton {
                    objectName: "folderFormCancel"
                    horizontalPadding: 16
                    text: qsTr("Cancel")
                    enabled: !folderFormDialog.pending
                    onClicked: folderFormDialog.close()
                }
                StatusActionButton {
                    id: folderFormSubmit
                    objectName: "folderFormSubmit"
                    horizontalPadding: 16
                    text: root.folderFormMode === "create" ? qsTr("Create") : qsTr("Save")
                    primary: true
                    enabled: !folderFormDialog.pending && folderFormDialog.hasName
                        && !folderFormDialog.noChanges
                    contentItem: RowLayout {
                        spacing: 8
                        opacity: folderFormSubmit.enabled ? 1 : 0.5
                        StatusSpinner { visible: folderFormDialog.pending; Layout.preferredWidth: 14; Layout.preferredHeight: 14 }
                        Text {
                            text: folderFormSubmit.text; textFormat: Text.PlainText
                            color: root.colors["on-accent"]; font.pixelSize: 14; font.weight: Font.Medium
                        }
                    }
                    onClicked: folderFormDialog.submit()
                }
            }
        }
    }

    Popup {
        id: folderDeleteDialog
        objectName: "folderDeleteDialog"
        property string targetId: ""
        property string targetTitle: ""
        property bool submitted: false
        readonly property bool pending: Boolean(root.controller.folderMutation.pending)
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(448, root.width - 32)
        padding: 24
        modal: true
        dim: true
        focus: true
        closePolicy: pending ? Popup.NoAutoClose : Popup.CloseOnEscape | Popup.CloseOnPressOutside
        Overlay.modal: Rectangle { color: "#80000000" }
        background: Rectangle {
            radius: 10
            color: root.colors["bg-elevated"]
            border.color: root.colors["border-subtle"]
        }
        onClosed: submitted = false
        contentItem: Column {
            spacing: 16
            Text {
                objectName: "folderDeleteTitle"
                width: parent.width
                text: qsTr("Delete folder?")
                textFormat: Text.PlainText; wrapMode: Text.Wrap
                font.pixelSize: 18; font.weight: Font.DemiBold
                color: root.colors["text-primary"]
            }
            Text {
                objectName: "folderDeleteDescription"
                width: parent.width
                text: qsTr("Are you sure you want to delete \"%1\"?").arg(folderDeleteDialog.targetTitle)
                textFormat: Text.PlainText; wrapMode: Text.Wrap
                font.pixelSize: 14
                color: root.colors["text-primary"]
            }
            Row {
                anchors.right: parent.right
                spacing: 8
                StatusActionButton {
                    objectName: "folderDeleteCancel"
                    horizontalPadding: 16
                    text: qsTr("Cancel")
                    enabled: !folderDeleteDialog.pending
                    onClicked: folderDeleteDialog.close()
                }
                Button {
                    id: folderDeleteConfirm
                    objectName: "folderDeleteConfirm"
                    text: qsTr("Delete")
                    enabled: !folderDeleteDialog.pending
                    hoverEnabled: true
                    padding: 0
                    contentItem: RowLayout {
                        spacing: 8
                        opacity: folderDeleteConfirm.enabled ? 1 : 0.6
                        StatusSpinner { visible: folderDeleteDialog.pending; Layout.preferredWidth: 14; Layout.preferredHeight: 14 }
                        Text {
                            text: folderDeleteConfirm.text; textFormat: Text.PlainText
                            color: root.colors["notice-base"]; font.pixelSize: 14; font.weight: Font.Medium
                        }
                    }
                    background: Rectangle {
                        radius: 8
                        color: Qt.rgba(root.colors["notice-base"].r, root.colors["notice-base"].g,
                            root.colors["notice-base"].b, folderDeleteConfirm.hovered && folderDeleteConfirm.enabled ? 0.2 : 0.1)
                    }
                    leftPadding: 16; rightPadding: 16; topPadding: 8; bottomPadding: 8
                    onClicked: {
                        if (folderDeleteDialog.pending) return;
                        folderDeleteDialog.submitted = true;
                        root.controller.deleteFolder(folderDeleteDialog.targetId);
                    }
                }
            }
        }
    }

    Popup {
        id: folderErrorToast
        objectName: "folderErrorToast"
        readonly property color noticeColor: root.colors["notice-base"]
        parent: Overlay.overlay
        x: root.width - width - 16
        y: 16
        z: 10000
        width: Math.min(384, root.width - 32)
        leftPadding: 16; rightPadding: 16; topPadding: 12; bottomPadding: 12
        closePolicy: Popup.NoAutoClose
        modal: false
        focus: false
        background: Rectangle {
            radius: 8
            color: Qt.rgba(folderErrorToast.noticeColor.r, folderErrorToast.noticeColor.g,
                folderErrorToast.noticeColor.b, 0.1)
            border.color: Qt.rgba(folderErrorToast.noticeColor.r, folderErrorToast.noticeColor.g,
                folderErrorToast.noticeColor.b, 0.4)
        }
        contentItem: RowLayout {
            spacing: 8
            Text {
                objectName: "folderErrorText"
                text: root.folderErrorNotice
                textFormat: Text.PlainText; wrapMode: Text.Wrap
                color: root.colors["notice-base"]; font.pixelSize: 14
                Layout.fillWidth: true
                Accessible.role: Accessible.AlertMessage
                Accessible.name: text
            }
            NativeIconButton {
                objectName: "folderDismissError"
                colors: root.colors; asset: "close.svg"
                implicitWidth: 20; implicitHeight: 20
                help: qsTr("Dismiss")
                glyphSize: 16
                iconColor: folderErrorToast.noticeColor
                onClicked: folderErrorToast.close()
            }
        }
        Timer { interval: 5000; running: folderErrorToast.visible; onTriggered: folderErrorToast.close() }
    }

    Menu {
        id: folderContextMenu
        objectName: "folderContextMenu"
        width: 240
        focus: true
        topPadding: 5
        bottomPadding: 5
        leftPadding: 1
        rightPadding: 1
        modal: false
        dim: false
        background: Rectangle {
            radius: 6
            color: root.colors["bg-elevated"]
            border.color: root.colors["border-subtle"]
        }
        // folder-rail-context-menu.lib.ts port: rename and delete (both
        // disabled for system folders) plus the two always-enabled display
        // toggles of issue #85; each label states the action its click
        // performs, so the horizontal layout menu offers "Display vertically"
        // and vice versa (buildFolderContextMenuItems).
        MessageMenuAction {
            objectName: "folderMenuRename"
            implicitWidth: 238
            text: qsTr("Rename")
            asset: "border_color.svg"
            enabled: root.folderMenuCanEdit
            onTriggered: root.openFolderRename()
        }
        MessageMenuAction {
            objectName: "folderMenuToggleLayout"
            implicitWidth: 238
            text: root.folderRailHorizontalMode ? qsTr("Display vertically") : qsTr("Display horizontally")
            asset: "folders.svg"
            onTriggered: root.preferences.folderRailLayout =
                root.folderRailHorizontalMode ? "vertical" : "horizontal"
        }
        MessageMenuAction {
            objectName: "folderMenuToggleSystemFolders"
            implicitWidth: 238
            text: root.preferences.showSystemFolders
                ? qsTr("Hide system folders") : qsTr("Show system folders")
            asset: "folder.svg"
            onTriggered: root.preferences.showSystemFolders = !root.preferences.showSystemFolders
        }
        MessageMenuAction {
            objectName: "folderMenuDelete"
            implicitWidth: 238
            text: qsTr("Delete")
            asset: "close.svg"
            palette.windowText: root.colors["notice-base"]
            enabled: root.folderMenuCanEdit
            onTriggered: root.openFolderDelete()
        }
    }

    function chatNavigationHasInputFocus() {
        for (let item = root.activeFocusItem; item; item = item.parent) {
            if (item instanceof TextInput || item instanceof TextEdit || item instanceof ComboBox)
                return true;
        }
        return false;
    }

    function handleChatEscape(event) {
        event.accepted = conversation.visible && root.controller.hasConversation
            && !root.controller.favoritesVisible && !aboutDrawer.visible
            && !root.controller.mediaViewer.open && !root.chatNavigationHasInputFocus();
        if (event.accepted) root.controller.showInbox();
    }

    RowLayout {
        id: workspace
        Keys.onEscapePressed: event => root.handleChatEscape(event)
        visible: root.controller.authenticated
        anchors.top: topBar.bottom
        anchors.topMargin: 4
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.rightMargin: aboutDrawer.visible ? aboutDrawer.width + 4 : 0
        spacing: 4

        Item {
            id: folderRail
            objectName: "folderRail"
            // Issue #85: in horizontal mode the rail remounts inside the
            // sidebar (sidebar-shell.ui.tsx activityPanelBottomSlot), so the
            // 72 px column collapses completely.
            visible: !root.folderRailHorizontalMode
            Layout.preferredWidth: root.folderRailHorizontalMode ? 0 : 72
            Layout.fillHeight: true
            // Issue #83: real folders (docs/NATIVE_DESKTOP_FOLDERS_RAIL.md).
            // Measured from workspace_ui 61dacef7 folder-rail-vertical-view.ui.tsx:
            // 72px rail, py-2, gap 2px; the All slot pins above the scrollable
            // group; the add-folder button is the last scroll item. The
            // overflow quick-list is a documented follow-up. Issue #85: the
            // group iterates the system-folder-filtered projection.
            readonly property var allFolder: root.folderRailRows.length > 0
                && String(root.folderRailRows[0].key) === "all"
                ? root.folderRailRows[0] : null
            readonly property var scrollRows:
                root.folderRailRows.slice(folderRail.allFolder ? 1 : 0)
            Column {
                anchors.fill: parent
                anchors.topMargin: 8
                anchors.bottomMargin: 8
                spacing: 2
                Repeater {
                    model: folderRail.allFolder ? [folderRail.allFolder] : []
                    delegate: folderCellComponent
                }
                ListView {
                    id: folderScroll
                    width: parent.width
                    height: Math.max(0, parent.height - (folderRail.allFolder ? 74 : 0))
                    spacing: 2
                    clip: true
                    // Filtered projection: delegates read their row directly.
                    model: folderRail.scrollRows
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                    footer: Item {
                        width: folderScroll.width
                        height: 40
                        NativeIconButton {
                            objectName: "folderAddButton"
                            colors: root.colors
                            anchors.horizontalCenter: parent.horizontalCenter
                            y: 8
                            asset: "add.svg"
                            help: qsTr("Add folder")
                            width: 32
                            height: 32
                            cornerRadius: 8
                            glyphSize: 28
                            // The creation flow is issue #84; the button only announces.
                            onClicked: root.controller.requestAddFolder()
                        }
                    }
                    delegate: folderCellComponent
                }
            }
        }

        Rectangle {
            id: sidebar
            objectName: "sidebar"
            Layout.preferredWidth: root.sidebarWidth
            Layout.fillHeight: true
            color: root.colors["bg-elevated"]
            radius: 6
            NativeField {
                id: sidebarSearch
                objectName: "sidebarSearch"
                colors: root.colors
                x: 12
                y: 16
                width: parent.width - 64
                height: 32
                leftPadding: 9
                rightPadding: sidebarSearch.text.length ? 59 : 35
                topPadding: 3
                bottomPadding: 3
                font.pixelSize: 16
                placeholderText: qsTr("Find")
                Accessible.name: qsTr("Search")
                text: root.sidebarQuery
                onTextEdited: root.sidebarQuery = text
                background: Rectangle {
                    radius: 6
                    color: root.colors["text-field-bg"]
                    border.color: sidebarSearch.activeFocus ? root.colors.accent : root.colors["border-subtle"]
                }
                NativeIconButton {
                    colors: root.colors
                    asset: "search.svg"
                    glyphSize: 18
                    icon.color: root.colors["text-muted"]
                    x: parent.width - 27
                    y: 7
                    width: 18
                    height: 18
                    enabled: false
                    background: Item {}
                    Accessible.ignored: true
                }
                NativeIconButton {
                    objectName: "clearSidebarSearch"
                    colors: root.colors
                    visible: sidebarSearch.text.length > 0
                    asset: "close.svg"
                    glyphSize: 14
                    x: parent.width - 55
                    y: 6
                    width: 20
                    height: 20
                    help: qsTr("Clear search")
                    onClicked: root.sidebarQuery = ""
                }
            }
            NativeIconButton {
                id: createChatButton
                objectName: "createChatButton"
                colors: root.colors
                x: parent.width - 44
                y: 16
                width: 32
                height: 32
                glyphSize: 21
                asset: "new_window.svg"
                enabled: root.controller.authenticated
                help: qsTr("New chat")
                onClicked: root.openChannelForm(visualFocus)
            }
            ColumnLayout {
                id: sidebarActivityPanel
                objectName: "sidebarActivityPanel"
                x: 8
                y: 64
                width: parent.width - 16
                spacing: 0
                visible: !root.normalizedSidebarQuery.length
                readonly property bool open: root.controller.activityOpen
                // Push-down applied to streamList while the expanded list
                // replaces the 32 px compact rail (reference flow: the chat
                // list follows the panel, sidebar-activity-view.ui.tsx).
                // #101 review: gate on the ACTUAL visible state, not the raw
                // open flag - a search hides this panel, and a hidden panel
                // must not keep its gap above the search results.
                readonly property int expandedDelta:
                    open && visible ? Math.round(sidebarActivityExpanded.implicitHeight) - 32 : 0
                RowLayout {
                    id: sidebarActivity
                    objectName: "sidebarActivity"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 32
                    visible: !sidebarActivityPanel.open
                    spacing: 8
                    Repeater {
                        model: [
                            { id: "inbox", asset: "mail_activity_compact.svg", size: 20, name: qsTr("Inbox") },
                            { id: "favorites", asset: "home.svg", size: 18, name: qsTr("Favorites") },
                            { id: "starred", asset: "marker_outline.svg", size: 18, name: qsTr("Starred messages") },
                            { id: "mentions", asset: "alternate_email.svg", size: 20, name: qsTr("Mentions") },
                            { id: "drafts", asset: "drafts_compact.svg", size: 18, name: qsTr("Drafts") }
                        ]
                        delegate: NativeIconButton {
                            id: railItem
                            required property var modelData
                            objectName: "activity-" + modelData.id
                            colors: root.colors
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.preferredHeight: 32
                            enabled: true
                            selected: modelData.id === "drafts" && root.controller.draftsVisible
                                || modelData.id === "mentions" && root.controller.mentionsVisible
                                || modelData.id === "inbox" && root.controller.inboxVisible
                                || modelData.id === "starred" && root.controller.starredVisible
                                || modelData.id === "favorites" && root.controller.favoritesVisible
                            asset: modelData.asset
                            glyphSize: modelData.size
                            help: modelData.name
                            onClicked: {
                                if (modelData.id === "drafts") root.controller.showDrafts();
                                else if (modelData.id === "mentions") root.controller.showMentions();
                                else if (modelData.id === "inbox") root.controller.showInbox();
                                else if (modelData.id === "starred") root.controller.showStarred();
                                else if (modelData.id === "favorites") root.controller.showFavorites();
                            }
                            background: Rectangle {
                                radius: 6
                                color: root.colors[parent.selected ? "card-bg-active" : "card-bg"]
                                border.width: parent.visualFocus ? 1 : 0
                                border.color: root.colors.accent
                            }
                            // Compact count badge (#101): only inbox and
                            // mentions carry counts (sidebar-activity-view
                            // countForActivityKey + the workspace container
                            // passing drafts/markedMessages null); hidden at
                            // zero and while unknown (-1), pinned top-right,
                            // Badge size sm with the 1000 cap.
                            readonly property int activityCount:
                                modelData.id === "inbox" ? root.controller.activityInboxCount
                                    : modelData.id === "mentions" ? root.controller.activityMentionsCount : -1
                            Rectangle {
                                objectName: "activityBadge-" + railItem.modelData.id
                                anchors.right: parent.right
                                anchors.top: parent.top
                                visible: railItem.activityCount > 0
                                height: 16
                                width: Math.max(16, railBadgeLabel.implicitWidth + 8)
                                radius: 8
                                color: root.colors["sidebar-unread"]
                                Text {
                                    id: railBadgeLabel
                                    anchors.centerIn: parent
                                    text: root.controller.activityBadgeLabel(railItem.activityCount)
                                    font.pixelSize: 10
                                    color: root.colors["text-primary"]
                                }
                            }
                        }
                    }
                    NativeIconButton {
                        objectName: "activityExpandButton"
                        colors: root.colors
                        Layout.preferredWidth: 32
                        Layout.preferredHeight: 32
                        enabled: root.controller.authenticated
                        asset: "chevron-down.svg"
                        glyphSize: 16
                        icon.color: root.colors["text-muted"]
                        help: qsTr("My Activity")
                        onClicked: root.controller.activityOpen = true
                        background: Rectangle {
                            radius: 6
                            color: root.colors["card-bg"]
                        }
                    }
                }
                ColumnLayout {
                    id: sidebarActivityExpanded
                    objectName: "sidebarActivityExpanded"
                    Layout.fillWidth: true
                    spacing: 0
                    visible: sidebarActivityPanel.open
                    Item {
                        // Header button: label + chevron-up collapses the
                        // panel (reference t("nav.activity") text-base
                        // font-semibold row, px-1 py-1).
                        id: activityHeader
                        objectName: "activityHeader"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 32
                        MouseArea {
                            anchors.fill: parent
                            onClicked: root.controller.activityOpen = false
                        }
                        RowLayout {
                            anchors.fill: parent
                            spacing: 4
                            Text {
                                text: qsTr("My Activity")
                                font.pixelSize: 16
                                font.weight: Font.DemiBold
                                color: root.colors["text-primary"]
                                Layout.leftMargin: 4
                            }
                            Item { Layout.fillWidth: true }
                            NativeIconButton {
                                objectName: "activityCollapseButton"
                                colors: root.colors
                                Layout.preferredWidth: 24
                                Layout.preferredHeight: 24
                                Layout.rightMargin: 4
                                asset: "chevron-up.svg"
                                glyphSize: 16
                                icon.color: root.colors["text-muted"]
                                help: qsTr("My Activity")
                                background: Item {}
                                onClicked: root.controller.activityOpen = false
                            }
                        }
                    }
                    ColumnLayout {
                        objectName: "sidebarActivityList"
                        Layout.fillWidth: true
                        Layout.topMargin: 8
                        spacing: 4
                        Repeater {
                            model: [
                                { id: "inbox", asset: "mail_activity.svg", chip: "indicator-purple", name: qsTr("Inbox") },
                                { id: "favorites", asset: "home_filled.svg", chip: "indicator-blue", name: qsTr("Favorites") },
                                { id: "starred", asset: "marker.svg", chip: "indicator-red", name: qsTr("Marked messages") },
                                { id: "mentions", asset: "alternate_email.svg", chip: "indicator-yellow", name: qsTr("Mentions") },
                                { id: "drafts", asset: "drafts.svg", chip: "indicator-pink", name: qsTr("Drafts") }
                            ]
                            delegate: Rectangle {
                                // Expanded row (reference rounded-lg bg-card-bg
                                // p-2 gap-3; active route bg-bg-elevated/60;
                                // 30x30 chip with a 24 px white glyph; badge
                                // only for inbox/mentions with the 1000 cap).
                                id: activityRow
                                required property var modelData
                                objectName: "activityRow-" + modelData.id
                                Layout.fillWidth: true
                                Layout.preferredHeight: 46
                                radius: 8
                                readonly property bool enabledItem: true
                                readonly property bool routeActive: modelData.id === "inbox" && root.controller.inboxVisible
                                    || modelData.id === "mentions" && root.controller.mentionsVisible
                                    || modelData.id === "drafts" && root.controller.draftsVisible
                                    || modelData.id === "starred" && root.controller.starredVisible
                                    || modelData.id === "favorites" && root.controller.favoritesVisible
                                readonly property int activityCount:
                                    modelData.id === "inbox" ? root.controller.activityInboxCount
                                        : modelData.id === "mentions" ? root.controller.activityMentionsCount : -1
                                enabled: enabledItem
                                opacity: enabledItem ? 1 : 0.7
                                color: routeActive ? Qt.alpha(root.colors["bg-elevated"], 0.6)
                                    : rowPointer.containsMouse && enabledItem ? root.colors["card-bg-active"]
                                    : root.colors["card-bg"]
                                function route() {
                                    if (modelData.id === "inbox") root.controller.showInbox();
                                    else if (modelData.id === "mentions") root.controller.showMentions();
                                    else if (modelData.id === "drafts") root.controller.showDrafts();
                                    else if (modelData.id === "starred") root.controller.showStarred();
                                    else if (modelData.id === "favorites") root.controller.showFavorites();
                                }
                                MouseArea {
                                    id: rowPointer
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    enabled: activityRow.enabledItem
                                    onClicked: activityRow.route()
                                }
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.margins: 8
                                    spacing: 12
                                    NativeIconButton {
                                        // Icon chip: the white 24 px glyph
                                        // matches the reference text-white.
                                        objectName: "activityChip-" + activityRow.modelData.id
                                        colors: root.colors
                                        Layout.preferredWidth: 30
                                        Layout.preferredHeight: 30
                                        padding: 0
                                        glyphSize: 24
                                        asset: activityRow.modelData.asset
                                        icon.color: "#ffffff"
                                        enabled: activityRow.enabledItem
                                        help: activityRow.modelData.name
                                        background: Rectangle {
                                            objectName: "activityChipBg-" + activityRow.modelData.id
                                            radius: 15
                                            color: root.colors[activityRow.modelData.chip]
                                        }
                                        onClicked: activityRow.route()
                                    }
                                    Text {
                                        objectName: "activityRowLabel-" + activityRow.modelData.id
                                        text: activityRow.modelData.name
                                        color: root.colors["text-primary"]
                                        font.pixelSize: 14
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                    }
                                    Rectangle {
                                        objectName: "activityRowBadge-" + activityRow.modelData.id
                                        visible: activityRow.activityCount > 0
                                        Layout.preferredWidth: Math.max(20, activityRowBadgeLabel.implicitWidth + 12)
                                        Layout.preferredHeight: 20
                                        radius: 10
                                        color: root.colors["sidebar-unread"]
                                        Text {
                                            id: activityRowBadgeLabel
                                            anchors.centerIn: parent
                                            text: root.controller.activityBadgeLabel(activityRow.activityCount)
                                            font.pixelSize: 11
                                            color: root.colors["badge-text"]
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            // Issue #100: reference sidebar-workspace.ui.tsx:827-870 at
            // workspace_ui 61dacef. Loading | blocking error | empty render as
            // one dashed card in the flow above the rows; the partial-refresh
            // banner is a solid card above the rows while they stay visible.
            // When the partial banner and the empty card are both visible the
            // card stacks directly below the banner, never under it.
            // Geometry: wrapper px-3 py-4 (12/16, banner px-3 pb-2), dashed
            // card radius 8 border 1 border-subtle bg bg-elevated/40 (banner
            // bg-elevated/60, solid border); loading card px-3 py-6 gap-3,
            // error/empty card px-3 py-5 gap-2 with 18 px icons, 14 px medium
            // titles and max-w-[220px] 12 px muted second lines.
            // Issue #85 (docs/NATIVE_DESKTOP_FOLDERS_RAIL_LAYOUT.md): the
            // horizontal rail (sidebar-shell.ui.tsx activityPanelBottomSlot)
            // mounts below the activity panel and above the chat list: the
            // 44 px strip (h-11: px-2 py-1, gap-1, 32 px chips, horizontally
            // scrollable) followed by the reference divider block (my-2:
            // 8 px + 1 px border-subtle line + 8 px). Everything below
            // (state cards, stream rows) shifts by folderRailInset.
            readonly property real folderRailInset: folderRailHorizontal.visible ? 61 : 0
            Item {
                id: folderRailHorizontal
                objectName: "folderRailHorizontal"
                x: 0
                width: parent.width
                height: 44
                visible: root.folderRailHorizontalMode
                y: (root.normalizedSidebarQuery.length ? 60 : 104)
                    + (root.normalizedSidebarQuery.length ? 0 : sidebarActivityPanel.expandedDelta)
                // The strip-level pointer owns drag and clicks like the React
                // rail's capture-phase handlers: left button only, a 3 px
                // movement threshold gates the scroll, one click after a drag
                // is suppressed, and the add-folder button (which grabs its
                // own press) neither starts a drag nor gets suppressed.
                property real dragStartX: 0
                property real dragStartContentX: 0
                property bool dragging: false
                property string pressedKey: ""
                readonly property string hoverKey: folderRailStripPointer.containsMouse
                    ? chipKeyAt(folderRailStripPointer.mouseX, folderRailStripPointer.mouseY) : ""
                function chipKeyAt(x, y) {
                    // This Qt kit's QQuickItem::childAt is depth-one, so
                    // descend manually to the deepest item under the point.
                    let item = folderRailStrip.contentItem
                    let px = x + folderRailStrip.contentX
                    let py = y
                    for (let guard = 0; guard < 32; ++guard) {
                        const nxt = item.childAt(px, py)
                        if (!nxt || nxt === item)
                            break
                        const local = nxt.mapFromItem(item, px, py)
                        px = local.x
                        py = local.y
                        item = nxt
                    }
                    while (item && !String(item.objectName).startsWith("folder-"))
                        item = item.parent
                    return item ? String(item.objectName).substring(7) : ""
                }
                MouseArea {
                    id: folderRailStripPointer
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    hoverEnabled: true
                    cursorShape: folderRailHorizontal.dragging ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                    onPressed: (mouse) => {
                        if (mouse.button === Qt.RightButton) {
                            const key = folderRailHorizontal.chipKeyAt(mouse.x, mouse.y)
                            if (key.length)
                                root.openFolderMenu(key, mapToItem(root.contentItem, mouse.x, mouse.y))
                            return;
                        }
                        folderRailHorizontal.pressedKey = folderRailHorizontal.chipKeyAt(mouse.x, mouse.y)
                        folderRailHorizontal.dragging = false
                        folderRailHorizontal.dragStartX = mouse.x
                        folderRailHorizontal.dragStartContentX = folderRailStrip.contentX
                    }
                    onPositionChanged: (mouse) => {
                        // Hover motion (including the synthetic move Qt
                        // delivers ahead of a button press) must not touch the
                        // drag bookkeeping; only a held left button drags.
                        if (!folderRailStripPointer.containsMouse || !folderRailStripPointer.pressed)
                            return
                        const deltaX = mouse.x - folderRailHorizontal.dragStartX
                        if (!folderRailHorizontal.dragging && Math.abs(deltaX) >= 3)
                            folderRailHorizontal.dragging = true
                        if (!folderRailHorizontal.dragging) return
                        folderRailStrip.contentX = Math.max(0, Math.min(
                            Math.max(0, folderRailStrip.contentWidth - folderRailStrip.width),
                            folderRailHorizontal.dragStartContentX - deltaX))
                    }
                    onReleased: {
                        // A gesture that crossed the 3 px threshold consumes
                        // the click that ended it (exactly one suppressed
                        // click); a plain press selects the pressed chip,
                        // drafts-closing included like the vertical cell.
                        if (!folderRailHorizontal.dragging && folderRailHorizontal.pressedKey.length) {
                            root.controller.closeDrafts()
                            root.controller.selectFolder(folderRailHorizontal.pressedKey)
                        }
                        folderRailHorizontal.dragging = false
                        folderRailHorizontal.pressedKey = ""
                    }
                    onCanceled: {
                        folderRailHorizontal.dragging = false
                        folderRailHorizontal.pressedKey = ""
                    }
                    onWheel: (wheel) => {
                        const deltaX = wheel.angleDelta.x
                        if (!deltaX) return
                        folderRailStrip.contentX = Math.max(0, Math.min(
                            Math.max(0, folderRailStrip.contentWidth - folderRailStrip.width),
                            folderRailStrip.contentX - deltaX))
                        wheel.accepted = true
                    }
                }
                Flickable {
                    id: folderRailStrip
                    objectName: "folderRailStrip"
                    anchors.fill: parent
                    interactive: false
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    contentWidth: Math.max(width, folderRailRow.width + 16)
                    contentHeight: height
                    Row {
                        id: folderRailRow
                        x: 8
                        y: (folderRailStrip.height - height) / 2
                        spacing: 4
                        Repeater {
                            model: root.folderRailRows
                            delegate: FolderRailChip {}
                        }
                        NativeIconButton {
                            objectName: "folderAddButton"
                            colors: root.colors
                            width: 32
                            height: 32
                            cornerRadius: 8
                            asset: "add.svg"
                            glyphSize: 24
                            help: qsTr("Add folder")
                            onClicked: root.controller.requestAddFolder()
                        }
                    }
                }
            }
            Rectangle {
                objectName: "folderRailDivider"
                x: 0
                width: parent.width
                height: 1
                color: root.colors["border-subtle"]
                visible: folderRailHorizontal.visible
                y: folderRailHorizontal.y + folderRailHorizontal.height + 8
            }
            readonly property real sidebarLoadingInset: sidebarLoadingCard.visible ? sidebarLoadingCard.height + 32 : 0
            readonly property real sidebarErrorInset: sidebarErrorCard.visible ? sidebarErrorCard.height + 32 : 0
            readonly property real sidebarEmptyInset: sidebarEmptyCard.visible ? sidebarEmptyCard.height + 32 : 0
            readonly property real sidebarPartialInset: sidebarPartialBanner.visible ? sidebarPartialBanner.height + 8 : 0
            Item {
                id: sidebarStateBlock
                objectName: "sidebarStateBlock"
                x: 0
                width: parent.width
                y: (root.normalizedSidebarQuery.length ? 60 : 104) + sidebar.folderRailInset
                height: Math.max(sidebarLoadingCard.visible ? sidebarLoadingCard.y + sidebarLoadingCard.height + 16 : 0,
                    sidebarErrorCard.visible ? sidebarErrorCard.y + sidebarErrorCard.height + 16 : 0,
                    sidebarEmptyCard.visible ? sidebarEmptyCard.y + sidebarEmptyCard.height + 16 : 0,
                    sidebarPartialBanner.visible ? sidebarPartialBanner.height + 8 : 0)
                Accessible.ignored: true
                Item {
                    id: sidebarLoadingCard
                    objectName: "sidebarLoadingCard"
                    visible: Boolean(root.sidebarCatalogState.showLoading)
                    x: 12
                    y: 16
                    width: sidebarStateBlock.width - 24
                    height: sidebarLoadingContent.implicitHeight + 48
                    Canvas {
                        // border-dashed border-subtle rounded-lg bg-bg-elevated/40
                        // (Qt Rectangle has no dashed border; Canvas draws it).
                        anchors.fill: parent
                        onPaint: {
                            const c = getContext("2d")
                            c.reset()
                            c.fillStyle = Qt.alpha(root.colors["bg-elevated"], 0.4)
                            c.strokeStyle = root.colors["border-subtle"]
                            c.lineWidth = 1
                            c.setLineDash([4, 3])
                            // Qt's Canvas 2D subset has no roundRect: arcTo path.
                            const w = parent.width - 1
                            const h = parent.height - 1
                            const r = 8
                            c.beginPath()
                            c.moveTo(0.5 + r, 0.5)
                            c.lineTo(w - r + 0.5, 0.5)
                            c.arcTo(w + 0.5, 0.5, w + 0.5, r + 0.5, r)
                            c.lineTo(w + 0.5, h - r + 0.5)
                            c.arcTo(w + 0.5, h + 0.5, w - r + 0.5, h + 0.5, r)
                            c.lineTo(r + 0.5, h + 0.5)
                            c.arcTo(0.5, h + 0.5, 0.5, h - r + 0.5, r)
                            c.lineTo(0.5, r + 0.5)
                            c.arcTo(0.5, 0.5, r + 0.5, 0.5, r)
                            c.fill()
                            c.stroke()
                        }
                    }
                    Accessible.role: Accessible.StatusBar
                    Accessible.name: String(root.sidebarCatalogState.loadingText || "")
                    ColumnLayout {
                        id: sidebarLoadingContent
                        anchors.top: parent.top
                        anchors.topMargin: 24
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 12
                        Accessible.ignored: true
                        Canvas {
                            // Spinner size=lg: 32 px ring, 2 px stroke, full
                            // ring border-subtle + top accent arc (the CSS
                            // border-t-accent edge), 1 s rotation.
                            id: sidebarLoadingSpinner
                            objectName: "sidebarLoadingSpinner"
                            width: 32
                            height: 32
                            Layout.alignment: Qt.AlignHCenter
                            Accessible.ignored: true
                            onPaint: {
                                const c = getContext("2d")
                                c.reset()
                                c.lineWidth = 2
                                c.strokeStyle = root.colors["border-subtle"]
                                c.beginPath()
                                c.arc(16, 16, 15, 0, Math.PI * 2)
                                c.stroke()
                                c.strokeStyle = root.colors["accent"]
                                c.beginPath()
                                c.arc(16, 16, 15, Math.PI, Math.PI * 2)
                                c.stroke()
                            }
                            RotationAnimator on rotation {
                                from: 0
                                to: 360
                                duration: 1000
                                loops: Animation.Infinite
                                running: sidebarLoadingCard.visible
                            }
                        }
                        Text {
                            id: sidebarLoadingText
                            objectName: "sidebarLoadingText"
                            Layout.alignment: Qt.AlignHCenter
                            text: String(root.sidebarCatalogState.loadingText || "")
                            font.pixelSize: 14
                            color: root.colors["text-muted"]
                            Accessible.ignored: true
                        }
                    }
                }
                Item {
                    id: sidebarErrorCard
                    objectName: "sidebarErrorCard"
                    visible: Boolean(root.sidebarCatalogState.showBlockingError)
                    x: 12
                    y: 16
                    width: sidebarStateBlock.width - 24
                    height: sidebarErrorContent.implicitHeight + 40
                    Canvas {
                        // border-dashed border-subtle rounded-lg bg-bg-elevated/40
                        // (Qt Rectangle has no dashed border; Canvas draws it).
                        anchors.fill: parent
                        onPaint: {
                            const c = getContext("2d")
                            c.reset()
                            c.fillStyle = Qt.alpha(root.colors["bg-elevated"], 0.4)
                            c.strokeStyle = root.colors["border-subtle"]
                            c.lineWidth = 1
                            c.setLineDash([4, 3])
                            // Qt's Canvas 2D subset has no roundRect: arcTo path.
                            const w = parent.width - 1
                            const h = parent.height - 1
                            const r = 8
                            c.beginPath()
                            c.moveTo(0.5 + r, 0.5)
                            c.lineTo(w - r + 0.5, 0.5)
                            c.arcTo(w + 0.5, 0.5, w + 0.5, r + 0.5, r)
                            c.lineTo(w + 0.5, h - r + 0.5)
                            c.arcTo(w + 0.5, h + 0.5, w - r + 0.5, h + 0.5, r)
                            c.lineTo(r + 0.5, h + 0.5)
                            c.arcTo(0.5, h + 0.5, 0.5, h - r + 0.5, r)
                            c.lineTo(0.5, r + 0.5)
                            c.arcTo(0.5, 0.5, r + 0.5, 0.5, r)
                            c.fill()
                            c.stroke()
                        }
                    }
                    Accessible.role: Accessible.AlertMessage
                    Accessible.name: [String(root.sidebarCatalogState.errorTitle || ""),
                        String(root.sidebarCatalogState.errorText || "")].filter(part => part.length).join(" ")
                    ColumnLayout {
                        id: sidebarErrorContent
                        anchors.top: parent.top
                        anchors.topMargin: 20
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 8
                        Accessible.ignored: true
                        Image {
                            objectName: "sidebarErrorIcon"
                            width: 18
                            height: 18
                            Layout.alignment: Qt.AlignHCenter
                            source: "qrc:/workspace/icons/info.svg"
                            sourceSize.width: 18
                            sourceSize.height: 18
                            asynchronous: false
                            layer.enabled: true
                            layer.effect: MultiEffect {
                                colorizationColor: root.colors["notice-base"]
                                colorization: 1.0
                            }
                            Accessible.ignored: true
                        }
                        Text {
                            objectName: "sidebarErrorTitle"
                            Layout.alignment: Qt.AlignHCenter
                            text: String(root.sidebarCatalogState.errorTitle || "")
                            font.pixelSize: 14
                            font.weight: Font.Medium
                            color: root.colors["text-primary"]
                            Accessible.ignored: true
                        }
                        Text {
                            objectName: "sidebarErrorBody"
                            Layout.alignment: Qt.AlignHCenter
                            width: Math.min(220, sidebarStateBlock.width - 48)
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            text: String(root.sidebarCatalogState.errorText || "")
                            font.pixelSize: 12
                            color: root.colors["text-muted"]
                            Accessible.ignored: true
                        }
                    }
                }
                Rectangle {
                    id: sidebarPartialBanner
                    objectName: "sidebarPartialBanner"
                    visible: Boolean(root.sidebarCatalogState.showPartialError)
                    x: 12
                    y: 0
                    width: sidebarStateBlock.width - 24
                    height: sidebarPartialRow.implicitHeight + 16
                    radius: 8
                    color: Qt.alpha(root.colors["bg-elevated"], 0.6)
                    border.width: 1
                    border.color: root.colors["border-subtle"]
                    Accessible.role: Accessible.AlertMessage
                    Accessible.name: [String(root.sidebarCatalogState.partialTitle || ""),
                        String(root.sidebarCatalogState.errorText || "")].filter(part => part.length).join(" ")
                    RowLayout {
                        id: sidebarPartialRow
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        anchors.topMargin: 8
                        spacing: 8
                        Accessible.ignored: true
                        Image {
                            objectName: "sidebarPartialIcon"
                            width: 16
                            height: 16
                            Layout.topMargin: 2
                            Layout.alignment: Qt.AlignTop
                            source: "qrc:/workspace/icons/info.svg"
                            sourceSize.width: 16
                            sourceSize.height: 16
                            asynchronous: false
                            layer.enabled: true
                            layer.effect: MultiEffect {
                                colorizationColor: root.colors["notice-base"]
                                colorization: 1.0
                            }
                            Accessible.ignored: true
                        }
                        ColumnLayout {
                            id: sidebarPartialColumn
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            spacing: 0
                            Accessible.ignored: true
                            Text {
                                objectName: "sidebarPartialTitle"
                                Layout.fillWidth: true
                                text: String(root.sidebarCatalogState.partialTitle || "")
                                font.pixelSize: 12
                                font.weight: Font.Medium
                                color: root.colors["text-primary"]
                                Accessible.ignored: true
                            }
                            Text {
                                objectName: "sidebarPartialLine"
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                text: String(root.sidebarCatalogState.errorText || "")
                                font.pixelSize: 12
                                color: root.colors["text-muted"]
                                Accessible.ignored: true
                            }
                        }
                    }
                }
                Item {
                    id: sidebarEmptyCard
                    objectName: "sidebarEmptyCard"
                    visible: Boolean(root.sidebarCatalogState.showEmpty)
                    x: 12
                    // Partial refresh error plus an empty (e.g. no-match query)
                    // list shows both blocks together: stack the card below the
                    // banner instead of painting over it (review #389 finding 1).
                    y: sidebarPartialBanner.visible ? sidebarPartialBanner.height + 8 : 16
                    width: sidebarStateBlock.width - 24
                    height: sidebarEmptyContent.implicitHeight + 40
                    Canvas {
                        // border-dashed border-subtle rounded-lg bg-bg-elevated/40
                        // (Qt Rectangle has no dashed border; Canvas draws it).
                        anchors.fill: parent
                        onPaint: {
                            const c = getContext("2d")
                            c.reset()
                            c.fillStyle = Qt.alpha(root.colors["bg-elevated"], 0.4)
                            c.strokeStyle = root.colors["border-subtle"]
                            c.lineWidth = 1
                            c.setLineDash([4, 3])
                            // Qt's Canvas 2D subset has no roundRect: arcTo path.
                            const w = parent.width - 1
                            const h = parent.height - 1
                            const r = 8
                            c.beginPath()
                            c.moveTo(0.5 + r, 0.5)
                            c.lineTo(w - r + 0.5, 0.5)
                            c.arcTo(w + 0.5, 0.5, w + 0.5, r + 0.5, r)
                            c.lineTo(w + 0.5, h - r + 0.5)
                            c.arcTo(w + 0.5, h + 0.5, w - r + 0.5, h + 0.5, r)
                            c.lineTo(r + 0.5, h + 0.5)
                            c.arcTo(0.5, h + 0.5, 0.5, h - r + 0.5, r)
                            c.lineTo(0.5, r + 0.5)
                            c.arcTo(0.5, 0.5, r + 0.5, 0.5, r)
                            c.fill()
                            c.stroke()
                        }
                    }
                    Accessible.role: Accessible.StaticText
                    Accessible.name: [String(root.sidebarCatalogState.emptyTitle || ""),
                        String(root.sidebarCatalogState.emptyHint || "")].filter(part => part.length).join(" ")
                    ColumnLayout {
                        id: sidebarEmptyContent
                        anchors.top: parent.top
                        anchors.topMargin: 20
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 8
                        Accessible.ignored: true
                        Image {
                            objectName: "sidebarEmptyIcon"
                            width: 18
                            height: 18
                            Layout.alignment: Qt.AlignHCenter
                            source: "qrc:/workspace/icons/chat_bubble.svg"
                            sourceSize.width: 18
                            sourceSize.height: 18
                            asynchronous: false
                            layer.enabled: true
                            layer.effect: MultiEffect {
                                colorizationColor: root.colors["text-muted"]
                                colorization: 1.0
                            }
                            Accessible.ignored: true
                        }
                        Text {
                            objectName: "sidebarEmptyTitle"
                            Layout.alignment: Qt.AlignHCenter
                            text: String(root.sidebarCatalogState.emptyTitle || "")
                            font.pixelSize: 14
                            font.weight: Font.Medium
                            color: root.colors["text-primary"]
                            Accessible.ignored: true
                        }
                        Text {
                            objectName: "sidebarEmptyHint"
                            Layout.alignment: Qt.AlignHCenter
                            width: Math.min(220, sidebarStateBlock.width - 48)
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            text: String(root.sidebarCatalogState.emptyHint || "")
                            font.pixelSize: 12
                            color: root.colors["text-muted"]
                            Accessible.ignored: true
                        }
                    }
                }
            }
            ListView {
                id: streamList
                objectName: "streamList"
                anchors.top: parent.top
                anchors.topMargin: (root.normalizedSidebarQuery.length ? 60 : 104)
                    + sidebar.sidebarLoadingInset + sidebar.sidebarErrorInset
                    + sidebar.sidebarEmptyInset + sidebar.sidebarPartialInset
                    + sidebar.folderRailInset
                    + sidebarActivityPanel.expandedDelta
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 2
                clip: true
                model: root.controller.sidebarStreams
                MouseArea {
                    // Issue #142: sidebar wheel scroller (expanded topic rows
                    // included - they are delegate children) using the same
                    // measured reference contract as messageList; at an edge
                    // the wheel propagates.
                    anchors.fill: parent
                    acceptedButtons: Qt.NoButton
                    propagateComposedEvents: true
                    onWheel: function (wheel) {
                        wheel.accepted = root.scrollFlickableWithWheel(streamList, wheel)
                    }
                }
                ScrollBar.vertical: ScrollBar {
                    policy: ScrollBar.AsNeeded
                }
                delegate: Column {
                    id: streamDelegate
                    required property var modelData
                    // Issue #102 (docs/NATIVE_DESKTOP_CHAT_LIST_DENSITY.md): the two
                    // persisted reference settings drive the row ladder and the click
                    // target; section 1.1/1.2 of the design doc holds the measurements.
                    readonly property bool compactDensity: root.preferences.chatListDensity === "compact"
                    readonly property bool opensLatestTopic: root.preferences.chatClickAction === "latestTopic"
                    readonly property string previewTopic: String(streamDelegate.modelData.previewTopic || "")
                    // workspace_ui 61dacef sidebar-message-preview.ui.tsx: sender in the
                    // sidebar-sender token + ": " + body in text-secondary, one 20 px line.
                    property var sidebarTopicState: ({ known: false, count: 0, rows: [] })
                    readonly property bool expanded: root.controller.expandedSidebarStreams.indexOf(modelData.id) >= 0
                    readonly property var allTopics: sidebarTopicState.rows || []
                    function refreshTopics() {
                        sidebarTopicState = root.controller.sidebarTopicState(modelData.id, expanded);
                    }
                    onExpandedChanged: {
                        if (!expanded && root.sidebarTopicRevealLevels[modelData.id] && root.sidebarTopicRevealLevels[modelData.id] !== "collapsed")
                            root.setSidebarTopicReveal(modelData.id, "collapsed");
                        refreshTopics();
                    }
                    onModelDataChanged: refreshTopics()
                    Component.onCompleted: refreshTopics()
                    Connections {
                        target: root.controller
                        function onSidebarTopicsChanged(stream) {
                            if (!stream.length || stream === streamDelegate.modelData.id) streamDelegate.refreshTopics();
                        }
                    }
                    readonly property string reveal: root.sidebarTopicRevealLevels[modelData.id] || "collapsed"
                    readonly property int unfinishedCount: allTopics.filter(topic => !Boolean(topic.done)).length
                    readonly property int collapsedCount: Math.min(3, allTopics.length)
                    readonly property int visibleCount: reveal === "all" ? allTopics.length
                        : reveal === "unfinished" ? Math.max(collapsedCount, unfinishedCount) : collapsedCount
                    readonly property int hiddenUnfinished: Math.max(0, unfinishedCount - visibleCount)
                    readonly property int hiddenCompleted: Math.max(0, allTopics.length - Math.max(unfinishedCount, visibleCount))
                    readonly property string nextAction: hiddenUnfinished > 0 ? "more"
                        : hiddenCompleted > 0 ? "completed" : visibleCount > collapsedCount ? "collapse" : ""
                    readonly property int hiddenCount: nextAction === "more" ? hiddenUnfinished : hiddenCompleted
                    readonly property string previewBody: Boolean(modelData.previewLocked) ? qsTr("Key required") : String(modelData.previewText || "")
                    readonly property string previewSenderLabel: {
                        const sender = String(modelData.previewSender || "")
                        return sender.length && previewBody.length ? sender + ": " : ""
                    }
                    width: streamList.width
                    spacing: 2
                    NativeButton {
                        id: streamButton
                        colors: root.colors
                        objectName: "stream-" + streamDelegate.modelData.id
                        width: parent.width
                        // Issue #97 (docs/NATIVE_DESKTOP_SIDEBAR_MUTE_INDICATORS.md):
                        // the reference applies opacity-70 to a fully-muted stream
                        // row surface (isWorkspaceStreamFullyMuted, precomputed as
                        // modelData.fullyMuted by the controller).
                        opacity: Boolean(streamDelegate.modelData.fullyMuted) ? 0.7 : 1.0
                        // Compact: 6 px (py-1.5) + the tallest content line; the avatar
                        // only occupies the line box in latestTopic mode at the sm size
                        // (6 + 32 + 6), the plain compact box is 6 + 20 + 6 (design doc 1.1).
                        height: streamDelegate.compactDensity ? (streamDelegate.opensLatestTopic ? 44 : 32) : 60
                        padding: streamDelegate.compactDensity ? 6 : 10
                        horizontalPadding: streamDelegate.compactDensity ? 8 : 10
                        verticalPadding: streamDelegate.compactDensity ? 6 : 10
                        onClicked: {
                            root.controller.closeDrafts();
                            // Reference sidebar-workspace.ui.tsx:455: the latestTopic body
                            // link is `stream.preview?.route ?? stream.route`; the native
                            // last-message topic pointer plays the route role.
                            if (streamDelegate.opensLatestTopic && streamDelegate.previewTopic.length)
                                root.controller.openTopic(streamDelegate.previewTopic, streamDelegate.modelData.id);
                            else
                                root.controller.selectStream(streamDelegate.modelData.id);
                        }
                        onHoveredChanged: if (hovered)
                            root.controller.requestSidebarTopics(streamDelegate.modelData.id)
                        onActiveFocusChanged: if (activeFocus)
                            root.controller.requestSidebarTopics(streamDelegate.modelData.id)
                        Keys.onMenuPressed: event => {
                            root.openStreamMenu(streamDelegate.modelData.id, streamButton, streamButton.mapToItem(root.contentItem, streamButton.width, streamButton.height / 2));
                            event.accepted = true;
                        }
                        Keys.onPressed: event => {
                            if (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier)) {
                                root.openStreamMenu(streamDelegate.modelData.id, streamButton, streamButton.mapToItem(root.contentItem, streamButton.width, streamButton.height / 2));
                                event.accepted = true;
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            acceptedButtons: Qt.RightButton
                            onClicked: mouse => root.openStreamMenu(streamDelegate.modelData.id, streamButton, mapToItem(root.contentItem, mouse.x, mouse.y))
                        }
                        background: Rectangle {
                            radius: 6
                            color: root.controller.selectedStream === streamDelegate.modelData.id && (root.controller.selectedTopic.length === 0 || !(streamDelegate.expanded || (root.normalizedSidebarQuery.length && (streamDelegate.modelData.searchTopics || []).length > 0))) ? root.colors["card-bg-active"] : streamButton.hovered ? root.colors["sidebar-item-hover"] : root.colors["card-bg"]
                            border.width: streamButton.visualFocus ? 1 : 0
                            border.color: root.colors.accent
                        }
                        contentItem: RowLayout {
                            objectName: "streamRowLayout-" + streamDelegate.modelData.id
                            spacing: streamDelegate.compactDensity ? 8 : 12
                            Rectangle {
                                objectName: "streamAvatarFace-" + streamDelegate.modelData.id
                                // showAvatar: !compact in the general click config and always
                                // true in latestTopic, where the avatar is the sm size in
                                // compact density (sidebar-workspace.ui.tsx:417,452,457).
                                visible: !streamDelegate.compactDensity || streamDelegate.opensLatestTopic
                                implicitWidth: streamDelegate.compactDensity ? 32 : 40
                                implicitHeight: streamDelegate.compactDensity ? 32 : 40
                                radius: streamDelegate.compactDensity ? 16 : 20
                                // Issue #176 (docs/NATIVE_DESKTOP_SIDEBAR_STREAM_AVATARS.md):
                                // reference sidebar-workspace.ui.tsx:512-519 paints the
                                // non-direct row with resolveAvatarBackgroundColor(stream.color)
                                // and falls back to the Avatar bg-bg token; direct rows stay
                                // photo/letter (WorkspaceAvatar path). Same binding as
                                // NativeDraftsPage.qml:182-185.
                                color: !Boolean(streamDelegate.modelData.isDirect)
                                    && String(streamDelegate.modelData.avatarColor || "").length
                                    ? streamDelegate.modelData.avatarColor : root.colors.bg
                                border.color: root.colors["border-subtle"]
                                Accessible.ignored: true
                                NativeAvatarImage {
                                    id: streamPhoto
                                    anchors.fill: parent
                                    reference: streamDelegate.modelData.isDirect ? String(streamDelegate.modelData.avatar || "") : ""
                                    loadActive: root.visible && root.controller.authenticated
                                }
                                PresenceBadge {
                                    objectName: "presence-stream-" + streamDelegate.modelData.id
                                    presence: root.controller.authenticated && Boolean(streamDelegate.modelData.isDirect)
                                        ? String(streamDelegate.modelData.presence || "") : ""
                                }
                                NativeIconButton {
                                    anchors.centerIn: parent
                                    visible: Boolean(streamDelegate.modelData.encrypted) && !streamPhoto.replacesFallback
                                    colors: root.colors; asset: "chat_lock.svg"; glyphSize: 24
                                    width: 24; height: 24; padding: 0
                                    enabled: false; opacity: 1; Accessible.ignored: true
                                    icon.color: root.colors["text-primary"]
                                }
                                Text {
                                    anchors.centerIn: parent
                                    visible: !Boolean(streamDelegate.modelData.encrypted) && !streamPhoto.replacesFallback
                                    objectName: "streamInitial-" + streamDelegate.modelData.id
                                    text: root.streamDisplayName(streamDelegate.modelData).slice(0, 1)
                                    textFormat: Text.PlainText
                                    color: root.colors["text-primary"]
                                    font.pixelSize: 12
                                    font.weight: Font.DemiBold
                                }
                                Rectangle {
                                    // Keyboard focus ring for the avatar action below, the
                                    // same pattern as the row button (streamButton shows its
                                    // focus with a 1 px accent border). A separate ring item
                                    // so the face's own 1 px border-subtle ring (the photo is
                                    // inset inside it) is untouched.
                                    anchors.fill: parent
                                    radius: parent.radius + 1
                                    color: "transparent"
                                    border.width: 1
                                    border.color: root.colors.accent
                                    visible: streamAvatarLink.visualFocus
                                }
                                ToolButton {
                                    id: streamAvatarLink
                                    objectName: "streamAvatar-" + streamDelegate.modelData.id
                                    // PR #396 review: the avatar action was a Rectangle +
                                    // MouseArea, which only answered the pointer. The reference
                                    // renders the avatar as a focusable <a> with title and
                                    // aria-label `chat.generalChat: title`
                                    // (sidebar-workspace.ui.tsx:455-462), so the action becomes a
                                    // transparent AbstractButton raised over the face: focus
                                    // policy, Space/Enter activation and the accessible Press
                                    // action come from the control itself (Qt.StrongFocus matches
                                    // the NativeIconButton idiom, e.g. line 8256). It fills the
                                    // face, so the avatar geometry (32 px compact sm / 40 px md,
                                    // 44/60 px rows) is untouched, and left-click still only
                                    // opens the stream (reference Link to stream.route at z-10,
                                    // line 494-501); right-click falls through to the row menu.
                                    anchors.fill: parent
                                    background: null
                                    padding: 0
                                    focusPolicy: Qt.StrongFocus
                                    Accessible.role: Accessible.Button
                                    Accessible.name: qsTr("General Chat: %1").arg(root.streamDisplayName(streamDelegate.modelData))
                                    onClicked: {
                                        root.controller.closeDrafts();
                                        root.controller.selectStream(streamDelegate.modelData.id);
                                    }
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                spacing: 0
                                Row {
                                    // Issue #96 (docs/NATIVE_DESKTOP_SIDEBAR_ROW_STATES.md):
                                    // reference sidebar-chat-title-with-status.ui.tsx is a
                                    // 4 px flex line where the direct status text carries
                                    // shrink-[9999]: it absorbs nearly all overflow and
                                    // truncates before the name does; the emoji is shrink-0.
                                    // The priority is reproduced with explicit widths: the
                                    // status takes only what the name and emoji leave over,
                                    // and the name takes back whatever the status gave up.
                                    id: streamTitleLine
                                    objectName: "streamTitleLine-" + streamDelegate.modelData.id
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignTop
                                    spacing: 4
                                    readonly property real nameNeed: streamTitleName.implicitWidth
                                    readonly property real emojiNeed: streamTitleEmoji.visible
                                        ? streamTitleEmoji.implicitWidth : 0
                                    readonly property real statusNeed: streamTitleStatus.visible
                                        ? streamTitleStatus.implicitWidth : 0
                                    readonly property real gapNeed: 4 * Math.max(0, 1
                                        + (streamTitleEmoji.visible ? 1 : 0)
                                        + (streamTitleStatus.visible ? 1 : 0) - 1)
                                    readonly property real statusWidth: streamTitleStatus.visible
                                        ? Math.max(0, Math.min(statusNeed,
                                            width - gapNeed - emojiNeed - nameNeed)) : 0
                                    readonly property real nameWidth: Math.min(nameNeed,
                                        Math.max(0, width - gapNeed - emojiNeed - statusWidth))
                                    Text {
                                        id: streamTitleName
                                        objectName: "streamTitleName-" + streamDelegate.modelData.id
                                        width: streamTitleLine.nameWidth
                                        text: root.streamDisplayName(streamDelegate.modelData)
                                        textFormat: Text.PlainText
                                        color: root.colors["text-primary"]
                                        font.pixelSize: 14
                                        font.weight: Font.Medium
                                        lineHeightMode: Text.FixedHeight
                                        lineHeight: 20
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        id: streamTitleEmoji
                                        objectName: "streamTitleEmoji-" + streamDelegate.modelData.id
                                        // Direct rows only (sidebar-workspace.ui.tsx:430);
                                        // the controller already filtered the raw DTO emoji
                                        // through the resolveWorkspaceStatusEmojiDisplay port.
                                        visible: text.length > 0
                                        text: Boolean(streamDelegate.modelData.isDirect)
                                            ? String(streamDelegate.modelData.statusEmoji || "") : ""
                                        textFormat: Text.PlainText
                                        color: root.colors["text-primary"]
                                        font.pixelSize: 12
                                        lineHeightMode: Text.FixedHeight
                                        lineHeight: 20
                                        Accessible.ignored: true
                                    }
                                    Text {
                                        id: streamTitleStatus
                                        objectName: "streamTitleStatus-" + streamDelegate.modelData.id
                                        visible: text.length > 0
                                        width: streamTitleLine.statusWidth
                                        text: Boolean(streamDelegate.modelData.isDirect)
                                            ? String(streamDelegate.modelData.statusText || "") : ""
                                        textFormat: Text.PlainText
                                        color: root.colors["text-muted"]
                                        font.pixelSize: 12
                                        lineHeightMode: Text.FixedHeight
                                        lineHeight: 20
                                        elide: Text.ElideRight
                                    }
                                }
                                Item {
                                    Layout.fillHeight: true
                                }
                                Row {
                                    // The line box stays mounted empty so every row keeps the
                                    // measured 60 px height (reference min-h-5 rule).
                                    // In compact density the reference drops the preview
                                    // entirely (sidebar-workspace.ui.tsx:536).
                                    id: streamPreviewLine
                                    objectName: "streamPreviewLine-" + streamDelegate.modelData.id
                                    visible: !streamDelegate.compactDensity
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 20
                                    spacing: 0
                                    Text {
                                        id: streamPreviewSenderText
                                        objectName: "streamPreviewSender-" + streamDelegate.modelData.id
                                        text: streamDelegate.previewSenderLabel
                                        textFormat: Text.PlainText
                                        color: root.colors["sidebar-sender"]
                                        font.pixelSize: 12
                                        lineHeightMode: Text.FixedHeight
                                        lineHeight: 20
                                    }
                                    Text {
                                        objectName: "streamPreviewText-" + streamDelegate.modelData.id
                                        x: streamPreviewSenderText.width
                                        width: Math.max(0, streamPreviewLine.width - streamPreviewSenderText.implicitWidth)
                                        text: streamDelegate.previewBody
                                        textFormat: Text.PlainText
                                        color: root.colors["text-secondary"]
                                        font.pixelSize: 12
                                        lineHeightMode: Text.FixedHeight
                                        lineHeight: 20
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                            ColumnLayout {
                                // workspace_ui 61dacef sidebar-chat-row-meta.ui.tsx: a 40 px
                                // min-width column, badge on top, timestamp at the bottom;
                                // compact switches it to min-w-8 and suppresses the time line
                                // (showTime = !compact).
                                Layout.minimumWidth: streamDelegate.compactDensity ? 32 : 40
                                Layout.fillHeight: true
                                spacing: 0
                                Item {
                                    Layout.alignment: Qt.AlignTop | Qt.AlignRight
                                    implicitWidth: streamMetaRow.implicitWidth
                                    implicitHeight: 20
                                    // Issue #96 (docs/NATIVE_DESKTOP_SIDEBAR_ROW_STATES.md):
                                    // the reference meta top row (sidebar-chat-row-meta
                                    // .ui.tsx) flows pin (12 px text-text-muted), then the
                                    // mention badge, then the unread/chevron action slot,
                                    // gap-1. The badges flow as trailing Row children; the
                                    // existing slot keeps its anchors.right internals.
                                    Row {
                                        id: streamMetaRow
                                        spacing: 4
                                        Item {
                                            objectName: "streamNotif-" + streamDelegate.modelData.id
                                            // Issue #97 (docs/NATIVE_DESKTOP_SIDEBAR_MUTE_INDICATORS.md):
                                            // the reference meta line's first child - a 20 px slot
                                            // with a centered 16 px muted glyph (workspace-notification-mode
                                            // .ui.tsx), rendered only when the stream's own mode is muted.
                                            visible: String(streamDelegate.modelData.notificationMode || "") === "muted"
                                            width: 20; height: 20
                                            Accessible.ignored: true
                                            Image {
                                                objectName: "streamNotifGlyph-" + streamDelegate.modelData.id
                                                anchors.centerIn: parent
                                                width: 16; height: 16
                                                source: "qrc:/workspace/icons/bell_off.svg"
                                                sourceSize.width: 16
                                                sourceSize.height: 16
                                                asynchronous: false
                                                layer.enabled: true
                                                layer.effect: MultiEffect {
                                                    colorizationColor: root.colors["text-muted"]
                                                    colorization: 1.0
                                                }
                                                Accessible.ignored: true
                                            }
                                        }
                                        Image {
                                            objectName: "streamPin-" + streamDelegate.modelData.id
                                            visible: Boolean(streamDelegate.modelData.pinned)
                                            width: 12; height: 12
                                            source: "qrc:/workspace/icons/pin.svg"
                                            sourceSize.width: 12
                                            sourceSize.height: 12
                                            asynchronous: false
                                            layer.enabled: true
                                            layer.effect: MultiEffect {
                                                colorizationColor: root.colors["text-muted"]
                                                colorization: 1.0
                                            }
                                            Accessible.ignored: true
                                        }
                                        Rectangle {
                                            objectName: "streamMention-" + streamDelegate.modelData.id
                                            // MentionBadge default size (mention-badge.tsx):
                                            // h-5 min-w-[20px] px-1.5 text-[11px], glyph @,
                                            // aria-hidden, untranslated title.
                                            visible: Boolean(streamDelegate.modelData.hasMention)
                                            width: Math.max(20, streamMentionGlyph.implicitWidth + 12)
                                            height: 20
                                            radius: 10
                                            color: root.colors["badge-bg"]
                                            Accessible.ignored: true
                                            Text {
                                                id: streamMentionGlyph
                                                objectName: "streamMentionGlyph-" + streamDelegate.modelData.id
                                                anchors.centerIn: parent
                                                text: "@"
                                                textFormat: Text.PlainText
                                                color: root.colors["badge-text"]
                                                font.pixelSize: 11
                                            }
                                        }
                                        Item {
                                            implicitWidth: Math.max(20, streamUnread.implicitWidth + 8)
                                            implicitHeight: 20
                                            Rectangle {
                                                objectName: "streamUnreadBadge-" + streamDelegate.modelData.id
                                                anchors.right: parent.right
                                                width: parent.width; height: 20; radius: 10
                                                visible: Number(streamDelegate.modelData.displayedUnread || 0) > 0
                                                    && !expandTopics.visible
                                                // resolveWorkspaceDisplayedUnread: a passive
                                                // (muted) unread renders with the gray
                                                // notice-disable badge variant, active stays
                                                // the accent one.
                                                color: Boolean(streamDelegate.modelData.unreadPassive)
                                                    ? root.colors["notice-disable"] : root.colors["badge-bg"]
                                                Text {
                                                    id: streamUnread
                                                    anchors.centerIn: parent
                                                    text: root.countText(Number(streamDelegate.modelData.displayedUnread || 0))
                                                    textFormat: Text.PlainText
                                                    color: root.colors["badge-text"]
                                                    font.pixelSize: 11
                                                }
                                            }
                                            NativeIconButton {
                                                id: expandTopics
                                                objectName: "expandTopics-" + streamDelegate.modelData.id
                                                anchors.right: parent.right
                                                width: 20; height: 20; glyphSize: 16
                                                colors: root.colors; asset: "chevron-down.svg"
                                                rotation: streamDelegate.expanded ? 180 : 0
                                                help: streamDelegate.expanded ? qsTr("Collapse topics") : qsTr("Expand topics")
                                                visible: !root.normalizedSidebarQuery.length && Boolean(streamDelegate.sidebarTopicState.known) && Number(streamDelegate.sidebarTopicState.count) > 0
                                                    && (streamButton.hovered || streamButton.activeFocus || activeFocus || hovered)
                                                onClicked: {
                                                    if (streamDelegate.expanded) root.setSidebarTopicReveal(streamDelegate.modelData.id, "collapsed");
                                                    root.controller.toggleSidebarStream(streamDelegate.modelData.id);
                                                }
                                            }
                                        }
                                    }
                                }
                                Item {
                                    Layout.fillHeight: true
                                }
                                Text {
                                    objectName: "streamTime-" + streamDelegate.modelData.id
                                    Layout.alignment: Qt.AlignBottom | Qt.AlignRight
                                    visible: text.length > 0 && !streamDelegate.compactDensity
                                    text: String(streamDelegate.modelData.rowTime || "")
                                    textFormat: Text.PlainText
                                    color: root.colors["text-muted"]
                                    font.pixelSize: 12
                                }
                            }
                        }
                    }
                    Repeater {
                        id: sidebarTopicRepeater
                        model: root.normalizedSidebarQuery.length
                            ? (streamDelegate.modelData.searchTopics || [])
                            : streamDelegate.expanded ? streamDelegate.allTopics.slice(0, streamDelegate.visibleCount) : []
                        delegate: NativeButton {
                            id: topicButton
                            required property var modelData
                            readonly property string previewBody: Boolean(modelData.previewLocked) ? qsTr("Key required") : String(modelData.previewText || "")
                            readonly property string previewSenderLabel: {
                                const sender = String(modelData.previewSender || "")
                                return sender.length && previewBody.length ? sender + ": " : ""
                            }
                            colors: root.colors
                            objectName: "topic-" + modelData.id
                            width: streamList.width
                            // Issue #97 (docs/NATIVE_DESKTOP_SIDEBAR_MUTE_INDICATORS.md):
                            // effectively-muted topic rows (per-topic mute, or default
                            // under a muted stream) render at opacity-70.
                            opacity: Boolean(modelData.effectivelyMuted) ? 0.7 : 1.0
                            // Compact: py-1.5 (6 px) + the single 20 px title line, the
                            // preview line is dropped by the reference (sidebar-workspace.ui.tsx:231).
                            height: streamDelegate.compactDensity ? 32 : 56
                            padding: 0
                            horizontalPadding: 0
                            verticalPadding: 0
                            onClicked: {
                                root.controller.closeDrafts();
                                if (root.normalizedSidebarQuery.length)
                                    root.controller.openTopic(modelData.id, streamDelegate.modelData.id);
                                else if (root.controller.selectedStream === streamDelegate.modelData.id)
                                    root.controller.selectTopic(modelData.id);
                                else
                                    root.controller.openTopic(modelData.id, streamDelegate.modelData.id);
                            }
                            background: Rectangle {
                                radius: 6
                                color: root.controller.selectedTopic === topicButton.modelData.id ? root.colors["card-bg-active"] : topicButton.hovered ? root.colors["sidebar-item-hover"] : root.colors["card-bg"]
                                border.width: topicButton.visualFocus ? 1 : 0
                                border.color: root.colors.accent
                            }
                            contentItem: Item {
                                id: topicRowSurface
                                // The reference meta column is min-w-10 with the badge on top
                                // and the timestamp at the bottom; both share one width slot.
                                // Issue #97: the mute indicator slot (20 px + the 4 px gap)
                                // flows ahead of the unread badge and extends the top line.
                                readonly property real metaWidth: Math.max(
                                    (topicUnread.visible ? topicUnread.width : 0)
                                        + (topicNotif.visible ? 24 : 0),
                                    topicTime.visible ? topicTime.implicitWidth : 0)
                                Rectangle {
                                    objectName: "topicBar-" + topicButton.modelData.id
                                    // pl-[38px] / pl-9 with the 3 px bar self-stretching to
                                    // the text block height (36 + 3 + 12 = 53 normal,
                                    // 36 + 3 + 8 = 47 compact, sidebarTopicRowLinkClass).
                                    x: streamDelegate.compactDensity ? 36 : 38
                                    width: 3
                                    height: streamDelegate.compactDensity ? 20 : 40
                                    anchors.verticalCenter: parent.verticalCenter
                                    radius: 2
                                    color: root.colors["icon-base"]
                                }
                                Text {
                                    objectName: "topicTitle-" + topicButton.modelData.id
                                    x: streamDelegate.compactDensity ? 47 : 53
                                    y: streamDelegate.compactDensity ? 6 : 8
                                    // pr-2 (8 px) stays the right inset in both densities.
                                    width: topicRowSurface.width - (streamDelegate.compactDensity ? 55 : 61)
                                        - topicRowSurface.metaWidth
                                    text: String(topicButton.modelData.name).startsWith("#") ? topicButton.modelData.name : "#" + topicButton.modelData.name
                                    textFormat: Text.PlainText
                                    font.pixelSize: 14
                                    font.weight: Font.Medium
                                    lineHeightMode: Text.FixedHeight
                                    lineHeight: 20
                                    font.strikeout: Boolean(topicButton.modelData.done)
                                    color: root.colors["text-primary"]
                                    elide: Text.ElideRight
                                }
                                Row {
                                    // Same preview contract as the stream row: the 20 px line
                                    // box stays mounted when the row has no last message, and
                                    // the whole line is suppressed in compact density.
                                    id: topicPreviewLine
                                    objectName: "topicPreviewLine-" + topicButton.modelData.id
                                    visible: !streamDelegate.compactDensity
                                    x: 53
                                    y: 28
                                    width: topicRowSurface.width - 61 - topicRowSurface.metaWidth
                                    height: 20
                                    spacing: 0
                                    Text {
                                        id: topicPreviewSenderText
                                        objectName: "topicPreviewSender-" + topicButton.modelData.id
                                        text: topicButton.previewSenderLabel
                                        textFormat: Text.PlainText
                                        color: root.colors["sidebar-sender"]
                                        font.pixelSize: 12
                                        lineHeightMode: Text.FixedHeight
                                        lineHeight: 20
                                    }
                                    Text {
                                        objectName: "topicPreviewText-" + topicButton.modelData.id
                                        x: topicPreviewSenderText.width
                                        width: Math.max(0, topicPreviewLine.width - topicPreviewSenderText.implicitWidth)
                                        text: topicButton.previewBody
                                        textFormat: Text.PlainText
                                        color: root.colors["text-secondary"]
                                        font.pixelSize: 12
                                        lineHeightMode: Text.FixedHeight
                                        lineHeight: 20
                                        elide: Text.ElideRight
                                    }
                                }
                                Rectangle {
                                    id: topicUnread
                                    anchors.right: parent.right
                                    anchors.rightMargin: 8
                                    y: streamDelegate.compactDensity ? 6 : 8
                                    width: visible ? Math.max(20, topicUnreadText.implicitWidth + 8) : 0
                                    height: 20
                                    visible: Number(topicButton.modelData.unread) > 0
                                    radius: 10
                                    color: root.colors["badge-bg"]
                                    Text {
                                        id: topicUnreadText
                                        anchors.centerIn: parent
                                        text: root.countText(Number(topicButton.modelData.unread))
                                        textFormat: Text.PlainText
                                        font.pixelSize: 11
                                        color: root.colors["badge-text"]
                                    }
                                }
                                Item {
                                    id: topicNotif
                                    objectName: "topicNotif-" + topicButton.modelData.id
                                    // Issue #97 (docs/NATIVE_DESKTOP_SIDEBAR_MUTE_INDICATORS.md):
                                    // the reference indicator on the topic meta line's first
                                    // slot, ahead of the unread badge (flex gap-1 flows it to
                                    // the row edge when no badge/chevron is visible). Glyph per
                                    // mapWorkspaceTopicNotificationModeToLevel; the reference
                                    // isDirectPrivate port is the stream row's isDirect.
                                    readonly property string glyph: {
                                        const mode = String(topicButton.modelData.notificationMode || "")
                                        if (mode === "mute") return "bell_off.svg"
                                        if (mode === "unmute")
                                            return Boolean(streamDelegate.modelData.isDirect)
                                                ? "topic_follow.svg" : "at.svg"
                                        if (mode === "follow") return "topic_follow.svg"
                                        return ""
                                    }
                                    visible: glyph.length > 0
                                    width: 20; height: 20
                                    y: streamDelegate.compactDensity ? 6 : 8
                                    anchors.right: topicUnread.visible ? topicUnread.left : topicRowSurface.right
                                    anchors.rightMargin: topicUnread.visible ? 4 : 8
                                    Accessible.ignored: true
                                    Image {
                                        objectName: "topicNotifGlyph-" + topicButton.modelData.id
                                        anchors.centerIn: parent
                                        width: 16; height: 16
                                        source: topicNotif.glyph.length
                                            ? "qrc:/workspace/icons/" + topicNotif.glyph : ""
                                        sourceSize.width: 16
                                        sourceSize.height: 16
                                        asynchronous: false
                                        layer.enabled: true
                                        layer.effect: MultiEffect {
                                            colorizationColor: root.colors["text-muted"]
                                            colorization: 1.0
                                        }
                                        Accessible.ignored: true
                                    }
                                }
                                Text {
                                    id: topicTime
                                    objectName: "topicTime-" + topicButton.modelData.id
                                    anchors.right: topicRowSurface.right
                                    anchors.rightMargin: 8
                                    y: 28
                                    // showTime = !compact (sidebar-chat-row-meta.ui.tsx).
                                    visible: text.length > 0 && !streamDelegate.compactDensity
                                    text: String(topicButton.modelData.rowTime || "")
                                    textFormat: Text.PlainText
                                    color: root.colors["text-muted"]
                                    font.pixelSize: 12
                                }
                            }
                        }
                    }
                    NativeButton {
                        id: showMoreTopics
                        objectName: "showMoreTopics-" + streamDelegate.modelData.id
                        visible: !root.normalizedSidebarQuery.length && streamDelegate.expanded && streamDelegate.nextAction.length > 0
                        width: streamList.width; height: visible ? (streamDelegate.compactDensity ? 32 : 36) : 0
                        colors: root.colors
                        text: streamDelegate.nextAction === "collapse" ? qsTr("Hide extra topics")
                            : streamDelegate.nextAction === "completed" ? qsTr("Show completed topics (%1)").arg(streamDelegate.hiddenCount)
                            : qsTr("Show more topics (%1)").arg(streamDelegate.hiddenCount)
                        Accessible.name: text
                        padding: 0
                        background: Rectangle {
                            color: showMoreTopics.hovered ? root.colors["sidebar-item-hover"] : "transparent"
                            border.width: showMoreTopics.visualFocus ? 1 : 0
                            border.color: root.colors.accent
                        }
                        contentItem: Item {
                            Text {
                                // pl-6 (24) + the bar spacer (3) + gap-3 / gap-2.
                                x: streamDelegate.compactDensity ? 35 : 39
                                width: parent.width - 70; height: parent.height
                                text: showMoreTopics.text; textFormat: Text.PlainText
                                color: root.colors["text-primary"]; font.pixelSize: 14; font.weight: Font.Medium
                                verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
                            }
                            NativeIconButton {
                                anchors.right: parent.right; anchors.rightMargin: 8; anchors.verticalCenter: parent.verticalCenter
                                width: 16; height: 16; glyphSize: 16
                                colors: root.colors; asset: "chevron-down.svg"
                                rotation: streamDelegate.nextAction === "collapse" ? 180 : 0
                                enabled: false; opacity: 1; Accessible.ignored: true
                                icon.color: root.colors["text-primary"]
                            }
                        }
                        onClicked: {
                            const priorCount = streamDelegate.visibleCount;
                            const action = streamDelegate.nextAction;
                            root.setSidebarTopicReveal(streamDelegate.modelData.id,
                                action === "more" ? "unfinished" : action === "completed" ? "all" : "collapsed");
                            if (action !== "collapse") Qt.callLater(function() {
                                const nextTopic = sidebarTopicRepeater.itemAt(priorCount);
                                if (nextTopic) nextTopic.forceActiveFocus(Qt.TabFocusReason);
                            });
                        }
                    }
                }
            }
            MouseArea {
                objectName: "sidebarResizeHandle"
                width: 8
                x: parent.width - 4
                height: parent.height
                cursorShape: Qt.SplitHCursor
                property real startX: 0
                property real startWidth: 0
                onPressed: mouse => {
                    startX = mapToItem(root.contentItem, mouse.x, 0).x;
                    startWidth = root.sidebarWidth;
                }
                onPositionChanged: mouse => {
                    if (pressed)
                        root.preferences.sidebarPreferredWidth = Math.max(root.sidebarMinimum, Math.min(root.sidebarMaximum, startWidth + mapToItem(root.contentItem, mouse.x, 0).x - startX));
                }
            }
        }

        ColumnLayout {
            id: conversation
            objectName: "conversationPanel"
            // Issue #355: the workspace has exactly one visible content page.
            // The Feed page is a sibling fillWidth child, so it must take part
            // in this expression: while Feed is open the conversation header,
            // message list and composer are hidden (their shared visibility
            // gates then suspend visible-message read reporting and keyboard
            // interaction, docs/NATIVE_DESKTOP_FEED.md).
            visible: !root.controller.draftsVisible && !root.controller.mentionsVisible
                && !root.controller.inboxVisible && !root.controller.feedVisible
                && !root.controller.starredVisible
                && !root.controller.reactionsVisible
                && !(root.controller.favoritesVisible && root.controller.favoritesStatus !== "ready")
                && root.controller.section === "chat"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.rightMargin: rightDrawer.visible ? rightDrawer.width + 4 : 0
            spacing: 0
            Rectangle {
                id: chatHeaderRoot
                objectName: "conversationHeader"
                Layout.fillWidth: true
                Layout.preferredHeight: root.controller.isDirectConversation ? 56 : root.encryptedConversation ? 62 : 54
                radius: 6
                color: root.colors["bg-elevated"]
                Binding {
                    // Issue #438 (issue #89 scoping fix): the controller may
                    // fetch the header member page only for the conversation
                    // whose header is actually on screen - the same discipline
                    // #434 applied to the binding-event NOTIFY. The earlier
                    // Connections handlers named onSelectedStreamChanged/
                    // onAuthenticatedChanged, but both properties use
                    // stateChanged as their NOTIFY, so every stateChanged
                    // re-fired the fetch (the #438 traffic storms and abort).
                    // Only the item's own visibility feeds in: controller
                    // state (favorites included) is folded in by the setter,
                    // so the value expression can never loop on stateChanged.
                    target: root.controller
                    property: "chatHeaderVisible"
                    value: chatHeaderRoot.visible
                }
                // Favorites presentation (issue #70): ChatFavoritesHeader shows only
                // the h1 title - text-base font-semibold text-text-primary
                // (chat-header-favorites.ui.tsx:5-10); the ordinary topic/direct/
                // encrypted titles and the header actions stay hidden.
                Text {
                    objectName: "favoritesConversationHeader"
                    visible: root.controller.favoritesVisible
                    anchors.left: parent.left
                    anchors.leftMargin: 20
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, parent.width - 40)
                    text: qsTr("Favorites")
                    textFormat: Text.PlainText
                    color: root.colors["text-primary"]
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                RowLayout {
                    visible: !root.controller.isDirectConversation && !root.encryptedConversation
                        && !root.controller.favoritesVisible
                    x: 20
                    y: 8
                    height: 20
                    width: parent.width - 100
                    spacing: 0
                    Text {
                        Layout.maximumWidth: streamTitle.visible ? parent.width * 0.65 : parent.width
                        text: root.selectedName(root.controller.topics, root.controller.selectedTopic) || root.selectedName(root.controller.streams, root.controller.selectedStream) || qsTr("Choose a conversation")
                        textFormat: Text.PlainText
                        color: root.colors["text-primary"]
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                    Text {
                        id: streamTitle
                        Layout.fillWidth: true
                        visible: root.controller.selectedTopic.length > 0
                        text: " · " + root.selectedName(root.controller.streams, root.controller.selectedStream)
                        textFormat: Text.PlainText
                        color: root.colors["text-muted"]
                        font.pixelSize: 14
                        elide: Text.ElideRight
                    }
                }
                Text {
                    objectName: "chatHeaderCounters"
                    // Issue #89: text-xs text-text-muted at mt-0.5 under the
                    // 14px title (chat-header-channel.ui.tsx); hidden for
                    // direct chats and before the projection arrives (a bound
                    // creator means a live stream always has >= 1 member).
                    visible: !root.controller.isDirectConversation && !root.encryptedConversation
                        && !root.controller.favoritesVisible && root.controller.chatHeaderMemberCount > 0
                    x: 20
                    y: 30
                    width: parent.width - 100
                    text: root.chatHeaderCountersText()
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                    textFormat: Text.PlainText
                    font.pixelSize: 12
                    color: root.colors["text-muted"]
                    elide: Text.ElideRight
                }
                RowLayout {
                    objectName: "encryptedConversationHeader"
                    visible: root.encryptedConversation && !root.controller.isDirectConversation
                        && !root.controller.favoritesVisible
                    x: 20; y: 12; height: 42
                    // Source shell: horizontal20 plus existing side-panel36.
                    width: parent.width - 76
                    spacing: 12
                    Rectangle {
                        implicitWidth: 32; implicitHeight: 32; radius: 16
                        Layout.alignment: Qt.AlignVCenter
                        // Issue #90: resolveAvatarBackgroundColor(stream.color),
                        // falling back to the Avatar bg-bg token only when the
                        // stream carries no color (avatar-color.lib.ts:2-7).
                        color: root.encryptedHeaderAvatarColor.length ? root.encryptedHeaderAvatarColor : root.colors.bg
                        border.width: 1
                        border.color: root.colors["border-subtle"]
                        NativeIconButton {
                            anchors.centerIn: parent
                            colors: root.colors; asset: "chat_lock.svg"; glyphSize: 20
                            width: 24; height: 24; padding: 0
                            enabled: false; opacity: 1; Accessible.ignored: true
                            icon.color: root.colors["text-primary"]
                        }
                    }
                    Item {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 42
                        Row {
                            id: encryptedTitleLine
                            width: parent.width; height: 20; spacing: 20
                            Item {
                                id: encryptedTitleGroup
                                readonly property bool topicShown: root.controller.selectedTopic.length > 0
                                readonly property real naturalWidth: encryptedTitle.implicitWidth + (topicShown ? 8 + encryptedStreamTitle.implicitWidth : 0)
                                width: Math.max(0, Math.min(naturalWidth, encryptedTitleLine.width - protectedChatInfo.implicitWidth - encryptedTitleLine.spacing))
                                height: 20
                                clip: true
                                Text {
                                    id: encryptedTitle
                                    objectName: "encryptedConversationTitle"
                                    width: Math.min(implicitWidth, parent.width)
                                    height: 20
                                    text: root.selectedName(root.controller.topics, root.controller.selectedTopic) || root.encryptedStreamName
                                    textFormat: Text.PlainText
                                    color: root.colors["text-primary"]
                                    font.pixelSize: 16; font.weight: Font.Medium
                                    lineHeightMode: Text.FixedHeight; lineHeight: 20
                                    elide: Text.ElideRight
                                }
                                Text {
                                    id: encryptedStreamTitle
                                    x: encryptedTitle.width + 8
                                    width: Math.max(0, parent.width - x)
                                    height: 20
                                    visible: encryptedTitleGroup.topicShown
                                    text: root.encryptedStreamName
                                    textFormat: Text.PlainText; elide: Text.ElideRight
                                    font.pixelSize: 16; color: root.colors["text-muted"]
                                    lineHeightMode: Text.FixedHeight; lineHeight: 20
                                }
                            }
                            ToolButton {
                                id: protectedChatInfo
                                objectName: "protectedChatInformation"
                                width: implicitWidth; height: 20
                                padding: 0; spacing: 6; hoverEnabled: true
                                // The reference trigger is a real tabbable button
                                // (chat-header-channel.ui.tsx:100-102), and its
                                // focus alone opens the tooltip
                                // (app-tooltip.ui.tsx:97-100).
                                focusPolicy: Qt.TabFocus
                                text: qsTr("Protected chat")
                                icon.source: "qrc:/workspace/icons/info.svg"
                                icon.width: 14; icon.height: 14; icon.color: root.colors.accent
                                font.pixelSize: 16; palette.buttonText: root.colors.accent
                                background: Rectangle { color: "transparent"; radius: 2; border.width: protectedChatInfo.visualFocus ? 2 : 0; border.color: root.colors["accent-soft"] }
                                Accessible.name: qsTr("Protected chat information")
                                // Issue #90 (docs/NATIVE_DESKTOP_E2EE_HEADER_AVATAR.md):
                                // the reference AppTooltip (chat-header-channel.ui.tsx:86-110)
                                // shows the e2ee.chatFeatures title plus the five
                                // chatFeatures* bullets (en.json:1632-1637,
                                // ru.json:1645-1650), on hover AND focus
                                // (app-tooltip.ui.tsx:91-107). Surface: w-80 px-4 py-3
                                // rounded-xl border-border-subtle bg-bg-elevated text-sm
                                // leading-relaxed text-text-secondary (app-tooltip.ui.tsx:208-215);
                                // body rhythm space-y-2 (8 px), list-disc pl-4 (16 px).
                                ToolTip {
                                    id: protectedChatTooltip
                                    visible: protectedChatInfo.hovered || protectedChatInfo.visualFocus
                                    delay: 0
                                    timeout: 0
                                    padding: 16
                                    topPadding: 12
                                    bottomPadding: 12
                                    // w-80 is a fixed width in the reference, not a
                                    // content-derived implicit width, so the popup
                                    // pins 320 and the paragraphs wrap inside it.
                                    width: 320
                                    background: Rectangle {
                                        objectName: "protectedChatTooltipSurface"
                                        radius: 12
                                        color: root.colors["bg-elevated"]
                                        border.width: 1
                                        border.color: root.colors["border-subtle"]
                                    }
                                    contentItem: ColumnLayout {
                                        // The popup opens as an overlay; the test-observable
                                        // nodes are the reparented content and surface items.
                                        objectName: "protectedChatTooltip"
                                        spacing: 8 // space-y-2.
                                        Text {
                                            objectName: "protectedChatTooltipTitle"
                                            Layout.fillWidth: true
                                            text: qsTr("Protected chat information")
                                            textFormat: Text.PlainText
                                            font.pixelSize: 14; font.weight: Font.Medium // font-medium text-text-primary.
                                            lineHeightMode: Text.FixedHeight; lineHeight: 23 // leading-relaxed.
                                            color: root.colors["text-primary"]
                                            wrapMode: Text.Wrap
                                        }
                                        Repeater {
                                            model: [
                                                qsTr("Message text and all links within it are protected by end-to-end encryption."),
                                                qsTr("Keys are stored on the device. Different periods of chat history may require different keys."),
                                                qsTr("Without the matching key, a message cannot be read even if it has already been downloaded to the device."),
                                                qsTr("Images, videos, and files are stored without end-to-end encryption and can be accessed by the server and storage."),
                                                qsTr("To open an attachment from the chat, you need to decrypt the message containing its link.")
                                            ]
                                            delegate: RowLayout {
                                                required property int index
                                                required property string modelData
                                                objectName: "protectedChatTooltipFeature-" + index
                                                Layout.fillWidth: true
                                                spacing: 0
                                                Text {
                                                    objectName: "protectedChatTooltipBullet-" + index
                                                    text: "\u2022"
                                                    Layout.preferredWidth: 16 // list-disc pl-4.
                                                    Layout.alignment: Qt.AlignTop
                                                    textFormat: Text.PlainText
                                                    font.pixelSize: 14
                                                    lineHeightMode: Text.FixedHeight; lineHeight: 23
                                                    color: root.colors["text-secondary"]
                                                }
                                                Text {
                                                    objectName: "protectedChatTooltipFeatureText-" + index
                                                    Layout.fillWidth: true
                                                    text: modelData
                                                    textFormat: Text.PlainText
                                                    font.pixelSize: 14
                                                    lineHeightMode: Text.FixedHeight; lineHeight: 23 // text-sm leading-relaxed.
                                                    color: root.colors["text-secondary"]
                                                    wrapMode: Text.Wrap
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        Item {
                            objectName: "encryptedParticipantsPlaceholder"
                            y: 24; width: parent.width; height: 18
                            Text {
                                objectName: "encryptedChatHeaderCounters"
                                // Issue #89: the mt-1 leading-[18px] text-xs
                                // text-text-muted caption of the encrypted
                                // header variant (chat-header-channel.ui.tsx).
                                visible: root.controller.chatHeaderMemberCount > 0
                                anchors.left: parent.left
                                anchors.right: parent.right
                                height: 18
                                text: root.chatHeaderCountersText()
                                Accessible.role: Accessible.StaticText
                                Accessible.name: text
                                textFormat: Text.PlainText
                                font.pixelSize: 12
                                lineHeightMode: Text.FixedHeight; lineHeight: 18
                                color: root.colors["text-muted"]
                                elide: Text.ElideRight
                            }
                        }
                    }
                }
                RowLayout {
                    id: directConversationRow
                    objectName: "directConversationHeader"
                    visible: root.controller.isDirectConversation && !root.controller.favoritesVisible
                    x: 20
                    y: 8
                    height: 40
                    width: parent.width - 100
                    spacing: 12
                    Rectangle {
                        implicitWidth: 40
                        implicitHeight: 40
                        radius: 20
                        color: root.colors["bg-elevated"]
                        border.color: root.colors["border-subtle"]
                        NativeAvatarImage {
                            id: directHeaderPhoto
                            anchors.fill: parent
                            reference: String(root.controller.directPeer.avatar || "")
                            loadActive: root.visible && root.controller.authenticated
                        }
                        PresenceBadge {
                            objectName: "presence-direct-header"
                            // Reference avatar.tsx:53,84-95 swaps the dot for
                            // the deactivated badge: presence stays projected
                            // in the data, only the dot is suppressed.
                            presence: root.controller.authenticated
                                && root.controller.directPeer.accountDeactivated !== true
                                ? String(root.controller.directPeer.presence || "") : ""
                        }
                        // Issue #177 (reference 61dacef7 avatar.tsx:20-23,
                        // presence-indicator.tsx:40-50): bg-card-bg disc with a
                        // 2px ring-bg ring and a 14px block glyph in
                        // text-text-muted, on the same 14.65 percent inset
                        // center as PresenceBadge (18px box -> -9 centering).
                        Item {
                            objectName: "deactivated-direct-header"
                            width: 18; height: 18
                            x: parent ? parent.width * 0.8535 - 9 : 0
                            y: parent ? parent.height * 0.8535 - 9 : 0
                            z: 1
                            visible: root.controller.authenticated
                                && root.controller.directPeer.accountDeactivated === true
                            Accessible.role: Accessible.StatusBar
                            Accessible.name: root.accountDeactivatedLabel()
                            Accessible.ignored: !visible
                            Rectangle {
                                objectName: "deactivated-ring"
                                anchors.fill: parent
                                radius: 9
                                color: root.colors.bg
                                Accessible.ignored: true
                            }
                            Rectangle {
                                objectName: "deactivated-fill"
                                x: 2; y: 2; width: 14; height: 14; radius: 7
                                color: root.colors["card-bg"]
                                Accessible.ignored: true
                            }
                            NativeIconButton {
                                objectName: "deactivated-glyph"
                                x: 2; y: 2; width: 14; height: 14; padding: 0
                                colors: root.colors; asset: "block.svg"; glyphSize: 14
                                enabled: false; opacity: 1
                                icon.color: root.colors["text-muted"]
                                Accessible.ignored: true
                            }
                        }
                        Text {
                            visible: !directHeaderPhoto.replacesFallback
                            anchors.centerIn: parent
                            objectName: "directHeaderInitial"
                            text: String(root.controller.directPeer.displayName || "").slice(0, 1).toUpperCase()
                            textFormat: Text.PlainText
                            color: root.colors["text-muted"]
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        spacing: 0
                        Text {
                            objectName: "directConversationName"
                            Layout.fillWidth: true
                            text: String(root.controller.directPeer.displayName || "")
                            textFormat: Text.PlainText
                            color: root.colors["text-primary"]
                            font.pixelSize: 14
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                            lineHeightMode: Text.FixedHeight; lineHeight: 20
                        }
                        Text {
                            objectName: "directPresenceStatus"
                            Layout.fillWidth: true
                            // An unknown user has no dot; Source's actual header
                            // still displays its offline caption, not online.
                            // Issue #88: the whole caption follows the
                            // resolveDmStatusText chain (chat-header.lib.ts:4-27).
                            text: root.directStatusCaption()
                            textFormat: Text.PlainText
                            color: root.colors["text-muted"]
                            font.pixelSize: 12
                            elide: Text.ElideRight
                            lineHeightMode: Text.FixedHeight; lineHeight: 16
                        }
                    }
                }
                AbstractButton {
                    // Issue #88 (reference chat-header-direct.ui.tsx:7-8,
                    // 47-55): the whole partner block is one overlay button
                    // (aria-label a11y.openUserProfile) that opens the partner
                    // profile drawer already wired from #134. The reference
                    // renders the overlay only with a profile handler; here
                    // that precondition is a known direct peer id.
                    // Exordy review PRRT_kwDOVmpjls65SUMo: a real focusable
                    // control instead of a MouseArea, so Tab reaches it and
                    // both Enter and Space activate it. With no background and
                    // no contentItem it paints nothing: pixel-identical to
                    // the previous transparent overlay.
                    id: directPartnerProfileAction
                    objectName: "directPartnerProfileAction"
                    anchors.fill: directConversationRow
                    visible: directConversationRow.visible
                        && String(root.controller.directPeer.id || "").length > 0
                    enabled: visible
                    focusPolicy: Qt.TabFocus | Qt.ClickFocus
                    padding: 0
                    spacing: 0
                    // Reference overlay classes: rounded-lg plus
                    // focus-visible:ring-2 ring-accent-soft
                    // (chat-header-direct.ui.tsx:10). The ring paints only
                    // with keyboard focus, so idle pixels stay identical.
                    background: Rectangle {
                        radius: 8
                        color: "transparent"
                        border.width: directPartnerProfileAction.visualFocus ? 2 : 0
                        border.color: root.colors["accent-soft"]
                    }
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Open profile: %1")
                        .arg(String(root.controller.directPeer.displayName || ""))
                    Accessible.ignored: !visible
                    onClicked: {
                        const peer = String(root.controller.directPeer.id || "")
                        if (peer.length > 0)
                            rightDrawer.openWorkspaceUserProfile(peer)
                    }
                }
                Row {
                    // The favorites header carries no actions (ChatHeaderShell renders
                    // the empty ChatHeaderActions for ChatFavoritesHeader, issue #70).
                    visible: !root.controller.favoritesVisible
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 4
                    Text {
                        objectName: "callError"
                        visible: root.controller.callError.length > 0
                        text: root.controller.callError
                        textFormat: Text.PlainText
                        color: root.colors.danger
                        font.pixelSize: 13
                        anchors.verticalCenter: parent.verticalCenter
                        Accessible.role: Accessible.AlertMessage
                    }
                    NativeIconButton {
                        objectName: "headerCallButton"
                        // Reference: rendered only when the Workspace meet host
                        // is configured (chat-page-workspace.ui.tsx:4100).
                        visible: root.controller.canStartCall
                        colors: root.colors
                        asset: "phone.svg"
                        glyphSize: 20
                        help: qsTr("Call")
                        onClicked: root.controller.startCall()
                    }
                    NativeIconButton {
                        colors: root.colors
                        asset: "side_panel.svg"
                        glyphSize: 20
                        // Issue #134: on direct chats the affordance opens the
                        // partner profile; ordinary channels keep the placeholder.
                        enabled: String(root.controller.directPeer.id || "").length > 0
                        help: String(root.controller.directPeer.id || "").length > 0
                            ? qsTr("Information")
                            : qsTr("Conversation information is not implemented yet")
                        onClicked: {
                            const peer = String(root.controller.directPeer.id || "")
                            if (peer.length > 0)
                                rightDrawer.openWorkspaceUserProfile(peer)
                        }
                    }
                }
            }
            Rectangle {
                visible: root.controller.error.length > 0
                Layout.fillWidth: true
                Layout.preferredHeight: Math.max(40, errorText.implicitHeight + 16)
                color: root.colors["bg-elevated"]
                radius: 6
                Text {
                    id: errorText
                    anchors.left: parent.left
                    anchors.right: cacheRetry.visible ? cacheRetry.left : parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    text: root.controller.error
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: root.colors.danger
                    font.pixelSize: 13
                    Accessible.role: Accessible.AlertMessage
                }
                NativeButton {
                    id: cacheRetry
                    objectName: "cacheRetryButton"
                    colors: root.colors
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    visible: root.controller.cacheStale
                    enabled: !root.controller.busy
                    implicitHeight: 32
                    verticalPadding: 6
                    font.pixelSize: 12
                    text: qsTr("Retry")
                    onClicked: root.controller.refresh()
                }
            }
            Rectangle {
                visible: root.controller.notice.length > 0
                Layout.fillWidth: true
                Layout.preferredHeight: Math.max(36, transferNotice.implicitHeight + 16)
                color: root.colors["bg-elevated"]
                radius: 6
                Text {
                    id: transferNotice
                    objectName: "fileTransferNotice"
                    anchors.fill: parent
                    anchors.margins: 8
                    text: root.controller.notice
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: root.colors["text-secondary"]
                    font.pixelSize: 13
                    Accessible.role: Accessible.StatusBar
                }
            }
            NativeButton {
                colors: root.colors
                visible: root.controller.canLoadOlder
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("Load older messages")
                enabled: !root.controller.busy
                onClicked: root.controller.loadOlder()
            }
            ListView {
                id: messageList
                objectName: "messageList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                leftMargin: 12
                rightMargin: 12
                topMargin: 16
                bottomMargin: 16
                spacing: 4
                clip: true
                // Keep delegates stable when a verified QVariantList snapshot
                // changes, then restore the visible UUID after layout.
                model: renderedMessages
                // Scroll-triggered older paging (docs/NATIVE_DESKTOP_AUTO_PAGING.md,
                // reserve policy for #156 in docs/NATIVE_DESKTOP_HISTORY_PREFETCH.md):
                // reference workspace-message-list-scroll.hook.ts LOAD_MORE_THRESHOLD
                // = 100 px (:22) with handleScroll marking trusted user scrolls
                // (:1145-1149) and the re-arm (:1172-1174) and trigger (:1185-1197)
                // sequence at 61dacef78dab2f5d0be76f283bebf49fe29b84af.
                // distanceFromTop is the DOM scrollTop analogue for these margins;
                // the reference programmaticScrollRef maps to `positioning`. The
                // reference fires the boundary request at 100 px; #156 keeps that
                // floor inside NativeController::noteScrollReserve and starts the
                // request one viewport earlier, so the view attributes user scrolling
                // here and forwards geometry - arming, coalescing and end-of-history
                // guards live in the controller (testable without a window).
                property bool userScrollSeen: false
                // Issue #253 review: followSentMessage arms the persistent
                // intent too, so a resize capture can already hold
                // userScrollSeen=true and a later wheel has no false-to-true
                // edge left to make. Every intent writer bumps this counter,
                // every viewport snapshot records it (captureViewport), and
                // the resize hand-off compares captured vs current instead
                // of the boolean edge.
                property int userScrollGeneration: 0
                property bool anchorJumpPending: false
                // Read the current position inside the scroll signal. A dependent
                // binding can still hold the previous Y during contentYChanged.
                function distanceFromTop() { return contentY - originY + topMargin }
                // Drag/fling is user scrolling too (handleTouchMove analogue).
                onMovementStarted: { userScrollSeen = true; ++userScrollGeneration; takeOpeningControl() }
                MouseArea {
                    // Wheel observer mirroring the reference handleWheel: it
                    // marks the scroll intent, then scrolls the measured
                    // reference distance itself (root.scrollFlickableWithWheel,
                    // issue #142) and only propagates the event when the list
                    // did not move. A WheelHandler inside a ListView would
                    // swallow the wheel instead; verified against the wheel
                    // tests.
                    // Share the delegate content plane: nested controls get
                    // first refusal, while ordinary wheels still reach this
                    // observer before the ListView's default flick handling.
                    parent: messageList.contentItem
                    x: messageList.contentX
                    y: messageList.contentY
                    width: messageList.width
                    height: messageList.height
                    acceptedButtons: Qt.NoButton
                    propagateComposedEvents: true
                    onWheel: function (wheel) {
                        messageList.userScrollSeen = true
                        ++messageList.userScrollGeneration
                        messageList.takeOpeningControl()
                        wheel.accepted = root.scrollFlickableWithWheel(messageList, wheel)
                    }
                }
                section.property: "dayKey"
                section.criteria: ViewSection.FullString
                section.labelPositioning: ViewSection.InlineLabels | ViewSection.CurrentLabelAtStart
                section.delegate: Item {
                    required property string section
                    width: messageList.width - 24
                    height: section.length ? 42 : 0
                    z: 2
                    Rectangle {
                        objectName: "messageDay-" + parent.section
                        property string text: root.dayLabel(parent.section)
                        property color surfaceColor: root.colors["bg-elevated"]
                        visible: parent.section.length > 0
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: 4
                        width: dayText.implicitWidth + 26
                        height: 26
                        radius: 13
                        color: Qt.rgba(surfaceColor.r, surfaceColor.g, surfaceColor.b, 0.9)
                        border.color: root.colors["border-subtle"]
                        Accessible.name: text
                        Accessible.role: Accessible.StaticText
                        Text {
                            id: dayText
                            anchors.centerIn: parent
                            text: parent.text
                            textFormat: Text.PlainText
                            font.pixelSize: 12
                            font.weight: Font.Medium
                            lineHeightMode: Text.FixedHeight
                            lineHeight: 16
                            color: root.colors["text-muted"]
                            Accessible.ignored: true
                        }
                    }
                }
                ListModel {
                    id: renderedMessages
                    dynamicRoles: true
                }
                // View-only preferences follow actual UUIDs through split/merge.
                // Controller remains the sole canonical history collection.
                property var messageSnapshot: []
                readonly property var readRequestCoverage: {
                    const boundaries = root.controller.readRequestBoundaryIds
                    const covered = {}
                    if (!boundaries.length) return covered
                    const boundaryIds = {}, topics = {}
                    for (const id of boundaries) boundaryIds[id] = true
                    // The canonical snapshot is in Core order, including
                    // microsecond timestamps and UUID tie breaks.
                    for (let i = messageSnapshot.length - 1; i >= 0; --i) {
                        const row = messageSnapshot[i]
                        if (boundaryIds[row.id]) topics[row.topicId] = true
                        if (topics[row.topicId]) covered[row.id] = true
                    }
                    return covered
                }
                property var serviceSession: ({ scope: "", ready: false, seen: {}, latest: null, preferences: {}, revision: 0, focusKey: "" })
                property var serviceGroups: []
                property var serviceAliases: ({})
                property var consumedHistoryCommit: -1
                property int serviceInspectionTicket: 0
                // Native has no selection/focus-target entry yet. These reducer
                // inputs are explicit unwired boundaries, not enabled features.
                property bool serviceSelectionMode: false
                property var serviceTextSelectionIds: []
                property var serviceFocusedTarget: null
                readonly property bool serviceLiveEnabled: root.controller.messageHistoryReady && !root.controller.cacheStale
                readonly property bool serviceCollapseEnabled: serviceLiveEnabled && !positioning && !root.controller.busy && !serviceSelectionMode
                function serviceScope() {
                    return JSON.stringify([root.controller.authenticated, root.controller.ownerKey,
                        String(root.controller.account.uuid || ""), root.controller.selectedStream, root.controller.selectedTopic]);
                }
                function isService(entry) {
                    return entry && ["key_request", "key_grant", "key_reject", "key_announced"].indexOf(entry.kind) >= 0;
                }
                function compareEntries(a, b) {
                    const first = Date.parse(a.createdAt), second = Date.parse(b.createdAt);
                    const dateOrder = isNaN(first) || isNaN(second) ? String(a.createdAt).localeCompare(String(b.createdAt)) : first - second;
                    return dateOrder || String(a.id).localeCompare(String(b.id));
                }
                function accountRelevant(entry, requests, entries, visiting) {
                    if (!isService(entry))
                        return true;
                    if (entry.kind === "key_announced")
                        return false;
                    const viewer = String(root.controller.account.uuid || "");
                    if (viewer.length && String(entry.authorId) === viewer)
                        return true;
                    if (entry.kind === "key_request") {
                        const request = requests[String(entry.id)] || {};
                        return Boolean(request.canManageKeys) || Boolean(request.offerGrant) || Boolean(request.offerReject);
                    }
                    const response = entry.keyServiceResponse || {};
                    if (!Boolean(response.recipientKnown))
                        return false;
                    if (viewer.length && String(response.recipientId) === viewer)
                        return true;
                    const id = String(response.requestId || "");
                    if (!id.length || visiting[id])
                        return false;
                    // Correlation is already exact/typed. A prepared outside-window
                    // R may establish account relevance without a fabricated DTO.
                    const prepared = requests[id];
                    if (prepared && (String(prepared.authorId || "") === viewer || Boolean(prepared.canManageKeys)
                        || Boolean(prepared.offerGrant) || Boolean(prepared.offerReject)))
                        return true;
                    const request = entries[id];
                    if (!request || request.kind !== "key_request")
                        return false;
                    const next = Object.assign({}, visiting);
                    next[id] = true;
                    return accountRelevant(request, requests, entries, next);
                }
                function servicePreference(group, session) {
                    let result = null;
                    for (const id of group.ids) {
                        const candidate = session.preferences[id];
                        if (candidate && (!result || candidate.source === "manual" && result.source !== "manual"
                            || candidate.source === result.source && candidate.revision > result.revision))
                            result = candidate;
                    }
                    return result;
                }
                function withServicePreference(session, groups, source, expanded) {
                    const preferences = Object.assign({}, session.preferences);
                    const preference = { source: source, expanded: expanded, revision: session.revision + 1 };
                    for (const group of groups)
                        for (const id of group.ids)
                            preferences[id] = preference;
                    return Object.assign({}, session, { preferences: preferences, revision: preference.revision });
                }
                function serviceGroupStatus(entries) {
                    let waiting = false, unavailable = false;
                    for (const entry of entries) {
                        const status = String(entry.serviceStatus || "");
                        if (status === "action-required") return status;
                        waiting = waiting || status === "waiting";
                        unavailable = unavailable || status === "unavailable";
                    }
                    return waiting ? "waiting" : unavailable ? "unavailable" : "";
                }
                function serviceCaption(count, status) {
                    const countLabel = count === 1 ? qsTr("%n event", "", count) : qsTr("%n events", "", count);
                    const label = serviceStatusLabel(status);
                    return countLabel + (label.length ? " · " + label : "");
                }
                function serviceStatusLabel(status) {
                    return status === "action-required" ? qsTr("Action required")
                        : status === "waiting" ? qsTr("Awaiting response")
                        : status === "unavailable" ? qsTr("Status unavailable") : "";
                }
                function projectServiceRows(rows, historical) {
                    const scope = serviceScope();
                    let session = serviceSession.scope === scope ? serviceSession
                        : { scope: scope, ready: false, seen: {}, latest: null, preferences: {}, revision: 0, focusKey: "" };
                    const sorted = rows.slice().sort(compareEntries);
                    const entries = {}, readOrder = {};
                    for (const entry of sorted) entries[String(entry.id)] = entry;
                    // Preserve exact Core chronology for read_up_to only. Public
                    // createdAt and Source visual sorting have millisecond precision.
                    for (let i = 0; i < rows.length; ++i) readOrder[String(rows[i].id)] = i;
                    const requests = root.incomingKeyRequestsById;
                    const visible = root.selectedStreamEncrypted ? sorted
                        : sorted.filter(entry => accountRelevant(entry, requests, entries, {}));
                    let latestOrdinary = null;
                    for (const entry of visible) if (!isService(entry)) latestOrdinary = entry;
                    const groups = [];
                    for (let i = 0; i < visible.length;) {
                        const first = visible[i];
                        if (!isService(first)) { ++i; continue; }
                        const members = [first];
                        let end = i + 1;
                        while (end < visible.length && isService(visible[end]) && visible[end].topicId === first.topicId
                            && root.messageDay(visible[end].createdAt) === root.messageDay(first.createdAt))
                            members.push(visible[end++]);
                        let readId = String(first.id);
                        for (const member of members)
                            if (readOrder[String(member.id)] > readOrder[readId]) readId = String(member.id);
                        const group = { id: String(first.id), ids: members.map(entry => String(entry.id)), entries: members, readId: readId,
                            first: first, last: members[members.length - 1], status: serviceGroupStatus(members),
                            laterOrdinary: Boolean(latestOrdinary && compareEntries(latestOrdinary, members[members.length - 1]) > 0) };
                        let preference = servicePreference(group, session);
                        const target = serviceFocusedTarget;
                        const focusKey = target ? [target.intentId, target.messageUuid, target.focusAttempt].join(":") : "";
                        if (target && session.focusKey !== focusKey && group.ids.indexOf(String(target.messageUuid)) >= 0) {
                            session = withServicePreference(session, [group], "manual", true);
                            session = Object.assign({}, session, { focusKey: focusKey });
                            preference = servicePreference(group, session);
                        } else if (!preference && !historical && session.ready && serviceLiveEnabled
                            && group.ids.every(id => !session.seen[id]) && (!session.latest || compareEntries(first, session.latest) > 0)) {
                            session = withServicePreference(session, [group], "auto", true);
                            preference = servicePreference(group, session);
                        }
                        if (preference) {
                            const preferences = Object.assign({}, session.preferences);
                            for (const id of group.ids) preferences[id] = preference;
                            session = Object.assign({}, session, { preferences: preferences });
                        }
                        group.expanded = serviceSelectionMode || Boolean(preference && preference.expanded);
                        groups.push(group);
                        i = end;
                    }
                    const byId = {};
                    for (const group of groups) for (const id of group.ids) byId[id] = group;
                    const seen = Object.assign({}, session.seen);
                    let disclosureRevision = session.revision;
                    // Track logical Source body mount, not virtual delegate life.
                    // Existing seen membership remains truthy for live ordering.
                    for (const id of Object.keys(seen)) {
                        const previous = seen[id];
                        if (previous && previous.expanded && (!byId[id] || !byId[id].expanded))
                            seen[id] = Object.assign({}, previous, { expanded: false });
                    }
                    for (const entry of visible) {
                        const id = String(entry.id);
                        if (entry.kind !== "key_grant" && entry.kind !== "key_reject") {
                            seen[id] = true;
                            continue;
                        }
                        const previous = session.seen[id];
                        const expanded = Boolean(byId[id] && byId[id].expanded);
                        seen[id] = { expanded: expanded, disclosureSerial: expanded && !(previous && previous.expanded)
                            ? ++disclosureRevision : Number(previous && previous.disclosureSerial || 0) };
                    }
                    const latest = visible.length ? visible[visible.length - 1] : null;
                    session = Object.assign({}, session, { seen: seen, revision: disclosureRevision,
                        latest: latest && (!session.latest || compareEntries(latest, session.latest) > 0) ? latest : session.latest });
                    serviceSession = session;
                    const aliases = {}, projected = [];
                    for (let i = 0; i < visible.length; ++i) {
                        const entry = visible[i], group = byId[String(entry.id)];
                        if (group && !group.expanded && String(entry.id) !== group.id) continue;
                        const model = rowModel(visible, i);
                        model.serviceGroupId = group ? group.id : "";
                        model.serviceFirst = Boolean(group && String(entry.id) === group.id);
                        model.serviceLast = Boolean(group && (String(entry.id) === String(group.last.id) || !group.expanded));
                        model.serviceCollapsed = Boolean(group && !group.expanded);
                        model.serviceCount = group ? group.ids.length : 0;
                        model.serviceStatus = group ? group.status : "";
                        model.serviceExpanded = Boolean(group && group.expanded);
                        model.serviceDisclosureSerial = Number(seen[String(entry.id)] && seen[String(entry.id)].disclosureSerial || 0);
                        // All members share topic/day; retain the maximum exact
                        // Core boundary even when Source visual UUID order differs.
                        model.serviceReadId = group && !group.expanded ? group.readId : String(entry.id);
                        if (group && !group.expanded) for (const id of group.ids) aliases[id] = group.id;
                        if (group) group.lastDisplayIndex = projected.length;
                        projected.push(model);
                    }
                    serviceGroups = groups;
                    serviceAliases = aliases;
                    return projected;
                }
                function finishServiceHistoryReady() {
                    if (root.controller.messageHistoryReady && serviceSession.scope === serviceScope() && !serviceSession.ready)
                        serviceSession = Object.assign({}, serviceSession, { ready: true });
                    scheduleServiceInspection();
                }
                function updateServiceContext() {
                    if (serviceSession.scope !== serviceScope()) {
                        if (positionRuntime === root.controller.conversationOpening.runtime) rememberViewport();
                        ++serviceInspectionTicket;
                        serviceSession = { scope: serviceScope(), ready: false, seen: {}, latest: null, preferences: {}, revision: 0, focusKey: "" };
                        consumedHistoryCommit = -1;
                        // State changes can precede route-clearing messagesChanged.
                        // Never re-project the previous route's saved snapshot.
                        messageSnapshot = [];
                        serviceGroups = [];
                        serviceAliases = {};
                    }
                    finishServiceHistoryReady();
                }
                function toggleServiceGroup(id) {
                    if (serviceSelectionMode || serviceSession.scope !== serviceScope()) return;
                    const group = serviceGroups.find(group => group.id === id);
                    if (!group) return;
                    const preference = servicePreference(group, serviceSession);
                    serviceSession = withServicePreference(serviceSession, [group], "manual", !(preference && preference.expanded));
                    rebuildServiceView();
                }
                function rebuildServiceView() {
                    if (displayedRoute !== routeKey() || serviceSession.scope !== serviceScope()) {
                        updateServiceContext();
                        return;
                    }
                    replaceMessages(messageSnapshot, true);
                }
                function groupHasInteraction(group) {
                    if (serviceTextSelectionIds.some(id => group.ids.indexOf(String(id)) >= 0)) return true;
                    let focus = root.activeFocusItem;
                    while (focus) {
                        const row = focus as MessageBoundaryRow;
                        if (row && row.serviceGroupId === group.id) return true;
                        focus = focus.parent;
                    }
                    return false;
                }
                function inspectServiceGroups(ticket, revision, scope) {
                    if (ticket !== serviceInspectionTicket || scope !== serviceScope() || serviceSession.scope !== scope
                        || revision !== serviceSession.revision || !serviceCollapseEnabled || !serviceSession.ready) return;
                    let firstVisible = count;
                    const rows = {};
                    for (const child of contentItem.children) {
                        const row = child as MessageBoundaryRow;
                        if (!row || !row.visible) continue;
                        const top = row.mapToItem(messageList, 0, 0).y;
                        if (top < height && top + row.height > 0) firstVisible = Math.min(firstVisible, row.displayIndex);
                        if (row.serviceGroupId.length && (!rows[row.serviceGroupId] || rows[row.serviceGroupId].index < row.displayIndex))
                            rows[row.serviceGroupId] = { index: row.displayIndex, bottom: top + row.height };
                    }
                    const candidates = serviceGroups.filter(group => {
                        const preference = servicePreference(group, serviceSession);
                        return preference && preference.source === "auto" && preference.expanded && !group.status.length
                            && group.laterOrdinary && !groupHasInteraction(group)
                            && (firstVisible < count && group.lastDisplayIndex < firstVisible
                                || rows[group.id] && rows[group.id].index === group.lastDisplayIndex && rows[group.id].bottom <= 0);
                    });
                    if (!candidates.length) return;
                    const snapshot = captureViewport();
                    if (!snapshot.anchorId.length || positioning || revision !== serviceSession.revision || scope !== serviceScope()) return;
                    pendingViewport = snapshot;
                    serviceSession = withServicePreference(serviceSession, candidates, "auto", false);
                    rebuildServiceView();
                }
                function scheduleServiceInspection() {
                    const ticket = ++serviceInspectionTicket, revision = serviceSession.revision, scope = serviceScope();
                    if (serviceCollapseEnabled && serviceSession.ready)
                        Qt.callLater(() => inspectServiceGroups(ticket, revision, scope));
                }
                onServiceSelectionModeChanged: rebuildServiceView()
                onServiceFocusedTargetChanged: rebuildServiceView()
                onServiceTextSelectionIdsChanged: scheduleServiceInspection()
                onServiceCollapseEnabledChanged: scheduleServiceInspection()
                property string displayedRoute: ""
                property int layoutTicket: 0
                property bool positioning: false
                // Issue #253: persistent tail-follow intent, the analogue of
                // the reference wasAtBottomRef/userScrolledAwayFromBottomRef
                // pair. The instantaneous captureViewport heuristic cannot be
                // trusted during a resize (the viewport height is already new
                // while contentHeight still reflects pre-reflow rows), so the
                // value is kept here and only re-derived from geometry after
                // non-programmatic movement. It is not a user-scroll proxy:
                // the #156 wheel observer remains the only userScrollSeen
                // attribution.
                property bool tailFollow: true
                property var pendingViewport: null
                property string openingContext: ""
                property bool openingApplied: false
                property var openingPosition: null
                property string positionRuntime: ""
                property string displayedPositionRoute: ""
                property var conversationPositions: ({})
                property var positionOrder: []
                property bool positionSaveAllowed: true
                property string unreadAnchorId: ""
                property bool unreadDividerDismissed: false
                readonly property int unreadCount: messageSnapshot.filter(row => !row.read).length
                readonly property bool readActive: openingApplied && !anchorTransitionLayer.visible && root.controller.conversationOpening.ready && root.active && root.visible && root.visibility !== Window.Minimized && root.controller.authenticated && !root.controller.busy && !root.controller.cacheStale && !positioning && root.controller.hasConversation && visible && width > 0 && height > 0 && !accountPopup.visible && !ownStatusDialog.visible && !aboutDrawer.visible && !messageMenu.visible && !reopenTopicMenu.visible && !streamMenu.visible && !chatActionDialog.visible && !createTopicDialog.visible && !createChatDialog.visible && !deleteDialog.visible && !outgoingDiscard.visible && !clipboardFilesDialog.visible && !referenceNotice.visible && !attachmentPicker.visible && !attachmentSaveDialog.visible && !reactionPicker.visible && !forwardDialog.visible && !root.controller.mediaViewer.open
                function routeKey() {
                    return serviceScope();
                }
                function positionKey() {
                    return JSON.stringify([root.controller.conversationOpening.runtime,
                        root.controller.selectedStream, root.controller.selectedTopic]);
                }
                function rememberViewport() {
                    if (!positionSaveAllowed || !openingApplied || !displayedPositionRoute.length || !messageSnapshot.length || positioning)
                        return;
                    const snapshot = captureViewport();
                    const position = snapshot.followTail
                        ? { kind: "bottom", messageId: String(messageSnapshot[messageSnapshot.length - 1].id) }
                        : { kind: "anchor", messageId: snapshot.anchorId, offset: snapshot.offset };
                    if (!position.messageId.length) return;
                    const positions = Object.assign({}, conversationPositions);
                    const order = positionOrder.filter(key => key !== displayedPositionRoute);
                    positions[displayedPositionRoute] = position;
                    order.push(displayedPositionRoute);
                    while (order.length > 12) delete positions[order.shift()];
                    conversationPositions = positions;
                    positionOrder = order;
                }
                function forgetPositions(streamUuid, topicUuid) {
                    const affected = key => {
                        const route = JSON.parse(key);
                        return route[0] === root.controller.conversationOpening.runtime && route[1] === streamUuid
                            && (!topicUuid.length || !route[2].length || route[2] === topicUuid);
                    };
                    const positions = Object.assign({}, conversationPositions);
                    positionOrder = positionOrder.filter(key => {
                        if (!affected(key)) return true;
                        delete positions[key];
                        return false;
                    });
                    conversationPositions = positions;
                    if (displayedPositionRoute.length && affected(displayedPositionRoute)) {
                        // A later route cleanup must not save this old viewport
                        // again. Fresh user movement re-arms position recording.
                        positionSaveAllowed = false;
                        openingPosition = null;
                        // Wait for the accepted read projection to finish before
                        // restarting a retired saved-position opening.
                        Qt.callLater(continueOpening);
                    }
                }
                function continueOpening() {
                    if (openingApplied || displayedRoute !== routeKey() || !root.controller.messageHistoryReady
                        || root.controller.busy || positioning)
                        return;
                    root.controller.prepareConversationOpening(openingPosition ? openingPosition.messageId : "");
                    if (!openingApplied && !positioning && root.controller.conversationOpening.ready)
                        replaceMessages(messageSnapshot, true);
                }
                function takeOpeningControl() {
                    positionSaveAllowed = true;
                    if (openingApplied && root.controller.conversationOpening.ready) return;
                    openingApplied = true;
                    pendingViewport = null;
                    positioning = false;
                    root.controller.cancelConversationOpening();
                }
                function dismissPassedUnreadDivider() {
                    if (positioning || !openingApplied || unreadDividerDismissed || !unreadAnchorId.length) return;
                    let dividerIndex = -1, firstVisible = -1;
                    for (let i = 0; i < count; ++i) {
                        if (renderedMessages.get(i).unreadStart) dividerIndex = i;
                        const row = itemAtIndex(i) as MessageBoundaryRow;
                        if (!row) continue;
                        if (row.unreadDivider) {
                            if (row.unreadDivider.mapToItem(messageList, 0, row.unreadDivider.height).y <= 0)
                                unreadDividerDismissed = true;
                            return;
                        }
                        if (firstVisible < 0 && row.readBoundary) {
                            const top = row.readBoundary.mapToItem(messageList, 0, 0).y;
                            if (top < height && top + row.readBoundary.height > 0) firstVisible = i;
                        }
                    }
                    // A large wheel jump can recycle the divider's delegate
                    // before this pass. The rendered row order still locates it.
                    if (dividerIndex >= 0 && firstVisible > dividerIndex) unreadDividerDismissed = true;
                }
                function scrollToLatest() {
                    takeOpeningControl();
                    const snapshot = captureViewport();
                    snapshot.followTail = true;
                    snapshot.userGenerationAtCapture = ++userScrollGeneration;
                    pendingViewport = snapshot;
                    tailFollow = true;
                    positioning = true;
                    if (root.controller.canLoadNewer) root.controller.loadLatestMessages();
                    const ticket = ++layoutTicket;
                    Qt.callLater(() => restoreViewport(ticket));
                }
                // The reference zero-visible-window effect (workspace-message-list
                // .ui.tsx:998-1022 at 61dacef78dab2f5d0be76f283bebf49fe29b84af):
                // "an invisible page has no scroll surface from which to request
                // the next one" - when every loaded row is filtered out by account
                // relevance the list advances automatically, newer direction first,
                // deduplicated per (scope, direction, window boundary) exactly like
                // the reference hiddenPageLoadRef key.
                property string hiddenAdvanceKey: ""
                function maybeAdvanceHiddenWindow() {
                    if (renderedMessages.count > 0) {
                        hiddenAdvanceKey = ""
                        return
                    }
                    if (positioning || messageSnapshot.length === 0 || !root.controller.messageHistoryReady)
                        return
                    const direction = root.controller.canLoadNewer ? "newer"
                        : root.controller.canLoadOlder ? "older" : ""
                    if (!direction.length)
                        return
                    const key = [routeKey(), direction, String(messageSnapshot[0].id),
                        String(messageSnapshot[messageSnapshot.length - 1].id)].join(":")
                    if (hiddenAdvanceKey === key)
                        return
                    hiddenAdvanceKey = key
                    if (direction === "newer")
                        root.controller.loadNewer()
                    else
                        root.controller.loadOlder()
                }
                function captureViewport() {
                    forceLayout();
                    const snapshot = {
                        anchorId: "",
                        offset: 0,
                        scrollY: contentY,
                        // Issue #253 review: stamp the wheel-intent
                        // generation with the geometry so the hand-off gate
                        // can tell intent that arrived after this capture
                        // from intent that predates it.
                        userGenerationAtCapture: userScrollGeneration,
                        followTail: count === 0 || atYEnd || originY + contentHeight + bottomMargin - contentY - height <= 80
                    };
                    for (let i = 0; i < count; ++i) {
                        const row = itemAtIndex(i) as MessageBoundaryRow;
                        if (!row || !row.visible || !row.readBoundary)
                            continue;
                        const boundary = row.readBoundary;
                        const point = boundary.mapToItem(messageList, 0, 0);
                        if (point.y < height && point.y + boundary.height > 0) {
                            snapshot.anchorId = row.readId;
                            snapshot.offset = point.y;
                            break;
                        }
                    }
                    return snapshot;
                }
                function sameAuthorGroup(left, right) {
                    if (!left || !right)
                        return false;
                    // Ordinary groups follow real identities, never a display
                    // name or an ownership flag shared by different authors.
                    const ordinaryLeft = left.kind === "markdown" || left.kind === "e2ee";
                    const ordinaryRight = right.kind === "markdown" || right.kind === "e2ee";
                    return ordinaryLeft && ordinaryRight && left.authorId === right.authorId && left.topicId === right.topicId && root.messageDay(left.createdAt) === root.messageDay(right.createdAt);
                }
                function rowModel(rows, index) {
                    const entry = rows[index];
                    let unreadStart = false;
                    if (unreadAnchorId.length && !unreadDividerDismissed
                        && (index === 0 || !sameAuthorGroup(rows[index - 1], entry))) {
                        for (let i = index; i < rows.length && (i === index || sameAuthorGroup(rows[i - 1], rows[i])); ++i)
                            if (String(rows[i].id) === unreadAnchorId) { unreadStart = true; break; }
                    }
                    return {
                        entry: entry,
                        unreadStart: unreadStart,
                        dayKey: root.messageDay(entry.createdAt),
                        groupFirst: index === 0 || !sameAuthorGroup(rows[index - 1], entry),
                        groupLast: index + 1 === rows.length || !sameAuthorGroup(entry, rows[index + 1]),
                        topicRunStart: index > 0 && rows[index - 1].topicId !== entry.topicId
                    };
                }
                function replaceMessages(rows, reproject) {
                    const route = routeKey();
                    const opening = root.controller.conversationOpening;
                    const sameRoute = displayedRoute === route;
                    if (!sameRoute || positionRuntime !== opening.runtime) {
                        if (positionRuntime === opening.runtime) rememberViewport();
                        else { conversationPositions = ({}); positionOrder = []; }
                        positionRuntime = opening.runtime;
                        displayedPositionRoute = positionKey();
                        openingPosition = conversationPositions[displayedPositionRoute] || null;
                        positionSaveAllowed = true;
                        openingApplied = false;
                        unreadAnchorId = "";
                        unreadDividerDismissed = false;
                        userScrollSeen = false;
                    }
                    if (openingContext !== opening.context) {
                        openingContext = opening.context;
                        openingApplied = false;
                    }
                    if (!reproject) {
                        const snapshot = [];
                        const length = rows.length;
                        for (let i = 0; i < length; ++i) snapshot.push(rows[i]);
                        messageSnapshot = snapshot;
                    }
                    const history = Number(root.controller.historyCommitRevision);
                    const historical = !reproject && history !== consumedHistoryCommit;
                    if (!reproject) consumedHistoryCommit = history;
                    const snapshot = sameRoute && pendingViewport ? pendingViewport : captureViewport();
                    if (!sameRoute)
                        snapshot.followTail = true;
                    if (!openingApplied && opening.ready && root.controller.messageHistoryReady) {
                        let target = String(opening.messageId || "");
                        const firstUnread = messageSnapshot.find(row => !row.read);
                        unreadAnchorId = firstUnread ? String(firstUnread.id) : "";
                        if (openingPosition && openingPosition.kind === "bottom") {
                            const previous = messageSnapshot.findIndex(row => String(row.id) === openingPosition.messageId);
                            const fresh = messageSnapshot.slice(previous + 1).find(row => !row.read);
                            target = fresh ? String(fresh.id) : root.controller.canLoadNewer ? target : "";
                        }
                        snapshot.initialOpening = true;
                        snapshot.followTail = !target.length;
                        snapshot.anchorId = target;
                        snapshot.center = Boolean(target.length && (!openingPosition || openingPosition.kind !== "anchor"));
                        snapshot.offset = openingPosition && openingPosition.kind === "anchor" ? openingPosition.offset : 0;
                    }
                    const projection = projectServiceRows(messageSnapshot, historical);
                    positioning = true;
                    visibleReadTimer.stop();
                    pendingViewport = snapshot;
                    displayedRoute = route;
                    for (let i = 0; i < projection.length; ++i) {
                        const model = projection[i];
                        const id = String(model.entry.id);
                        if (i >= renderedMessages.count) {
                            renderedMessages.append(model);
                        } else {
                            if (String(renderedMessages.get(i).entry.id) !== id) {
                                let previous = -1;
                                for (let j = i + 1; j < renderedMessages.count; ++j) {
                                    if (String(renderedMessages.get(j).entry.id) === id) {
                                        previous = j;
                                        break;
                                    }
                                }
                                if (previous >= 0)
                                    renderedMessages.move(previous, i, 1);
                                else
                                    renderedMessages.insert(i, model);
                            }
                            renderedMessages.set(i, model);
                        }
                    }
                    if (renderedMessages.count > projection.length)
                        renderedMessages.remove(projection.length, renderedMessages.count - projection.length);
                    const ticket = ++layoutTicket;
                    Qt.callLater(() => restoreViewport(ticket));
                }
                function forceMeasuredLayout() {
                    forceLayout();
                    // Rich text and Column positioning have their own polish
                    // passes. Resolve the instantiated rows before estimating
                    // the tail or capturing an anchor; offscreen rows stay virtual.
                    for (let i = 0; i < count; ++i) {
                        const row = itemAtIndex(i) as MessageBoundaryRow;
                        if (row)
                            row.settleGeometry();
                    }
                    forceLayout();
                }
                function restoreViewport(ticket, finalPass) {
                    if (ticket !== layoutTicket || displayedRoute !== routeKey() || !pendingViewport)
                        return;
                    // Issue #253: a real wheel/touch that arrived while this
                    // chain was queued belongs to the user, not the reflow.
                    // Reference analogue: pinTailToBottom's requestAnimationFrame
                    // pass returns when userScrolledAwayFromBottomRef fired
                    // (hook :294-307). Hand the chain over instead of
                    // overwriting the user's movement. Issue #253 review:
                    // compare the captured wheel generation with the current
                    // one - the boolean cannot express "new intent" once
                    // followSentMessage has already armed it pre-capture.
                    if (pendingViewport.followTail
                        && userScrollGeneration > pendingViewport.userGenerationAtCapture) {
                        pendingViewport = null;
                        positioning = false;
                        tailFollow = originY + contentHeight + bottomMargin - contentY - height <= 80
                        // Issue #279: this hand-off fired because the user's own
                        // wheel/drag took the chain over, but that takeover
                        // scrolled while `positioning` was still raised, so its
                        // contentY passes were dropped by the reference gates in
                        // onContentYChanged and no attributed pass follows (the
                        // wheel already parked the viewport wherever it stops).
                        // A fast climb to the loaded top therefore lost the
                        // older-page trigger for good: the reserve was still
                        // full, yet only a re-arm - which never requests - ran
                        // at dTop <= reserve. The resting position here is the
                        // user's own scroll position, so it must be evaluated
                        // with user attribution (trigger + re-arm), mirroring
                        // the reference handleScroll, which scores a user wheel
                        // scroll event regardless of a pending pin pass. The
                        // arm/coalescing/end-of-history guards inside
                        // noteScrollReserve keep this from ever duplicating or
                        // storming a request.
                        root.controller.noteScrollReserve(distanceFromTop(), height)
                        finishServiceHistoryReady();
                        scheduleVisibleRead();
                        return;
                    }
                    forceMeasuredLayout();
                    const snapshot = pendingViewport;
                    if (snapshot.followTail) {
                        tailFollow = true;
                        positionViewAtEnd();
                        forceMeasuredLayout();
                        const tail = itemAtIndex(count - 1);
                        if (tail)
                            contentY = Math.max(originY - topMargin, contentY + tail.mapToItem(messageList, 0, tail.height).y + bottomMargin - height);
                    } else {
                        tailFollow = false;
                        let anchorIndex = -1;
                        const anchorId = serviceAliases[snapshot.anchorId] || snapshot.anchorId;
                        for (let i = 0; i < count; ++i) {
                            if (String(renderedMessages.get(i).entry.id) === anchorId) {
                                anchorIndex = i;
                                break;
                            }
                        }
                        if (anchorIndex >= 0) {
                            positionViewAtIndex(anchorIndex, ListView.Beginning);
                            forceMeasuredLayout();
                            const row = itemAtIndex(anchorIndex) as MessageBoundaryRow;
                            if (row && row.readBoundary)
                                contentY += row.readBoundary.mapToItem(messageList, 0, 0).y
                                    - (snapshot.center ? (height - row.readBoundary.height) / 2 : snapshot.offset);
                        } else {
                            contentY = snapshot.scrollY;
                        }
                    }
                    forceMeasuredLayout();
                    if (!finalPass) {
                        Qt.callLater(() => restoreViewport(ticket, true));
                        return;
                    }
                    pendingViewport = null;
                    if (snapshot.initialOpening) openingApplied = true;
                    positioning = false;
                    // Issue #212 review: the swap suppressed every scroll note
                    // (positioning gate), so re-arm the controller's prefetch
                    // from the restored geometry - re-arm only; this call can
                    // never start a request, keeping programmatic movement
                    // unattributed as user scrolling.
                    root.controller.noteScrollRearm(distanceFromTop(), height)
                    finishServiceHistoryReady();
                    scheduleVisibleRead();
                    maybeAdvanceHiddenWindow();
                    continueOpening();
                    dismissPassedUnreadDivider();
                }
                function anchorToMessage(id) {
                    if (!id)
                        return;
                    const wanted = serviceAliases[id] || id;
                    for (let i = 0; i < count; ++i) {
                        if (String(renderedMessages.get(i).entry.id) === wanted) {
                            // Reference analogue: handleScroll returns while
                            // anchorHandoffPending, so the highlight jump itself
                            // must not mark user scrolling or fire the 100 px
                            // auto-page trigger.
                            anchorJumpPending = true
                            positionViewAtIndex(i, ListView.Contain);
                            forceMeasuredLayout();
                            Qt.callLater(() => { anchorJumpPending = false })
                            return;
                        }
                    }
                }
                Connections {
                    target: root.controller
                    function onAnchorChanged() {
                        if (root.controller.anchorMessageId) {
                            messageList.takeOpeningControl();
                            Qt.callLater(() => {
                                messageList.anchorToMessage(root.controller.anchorMessageId);
                                // Explicit navigation cancels the opening layout.
                                // A filtered service anchor still needs the normal
                                // empty-window paging pass after that hand-off.
                                messageList.maybeAdvanceHiddenWindow();
                            });
                        }
                    }
                }
                function followSentMessage(id) {
                    for (let i = 0; i < count; ++i) {
                        if (String(renderedMessages.get(i).entry.id) !== id)
                            continue;
                        // Reference pinTailToBottom-on-send marks the scroll
                        // intent unconditionally (hook :573-584).
                        userScrollSeen = true
                        ++userScrollGeneration
                        tailFollow = true
                        const snapshot = pendingViewport || captureViewport();
                        snapshot.followTail = true;
                        // The send's own intent bump must not re-arm the
                        // hand-off against this very pin: record the
                        // generation this pin was posted from.
                        snapshot.userGenerationAtCapture = userScrollGeneration;
                        pendingViewport = snapshot;
                        positioning = true;
                        const ticket = ++layoutTicket;
                        Qt.callLater(() => restoreViewport(ticket));
                        return;
                    }
                }
                function scheduleVisibleRead() {
                    if (readActive)
                        visibleReadTimer.restart();
                    else
                        visibleReadTimer.stop();
                }
                function markVisibleRead() {
                    if (!readActive)
                        return;
                    // Cached delegates can exist outside the clipped viewport.
                    // Resolve geometry at dispatch time so a focus/route change
                    // cannot send a boundary captured in another view.
                    forceLayout();
                    const ids = [];
                    for (const child of contentItem.children) {
                        const row = child as MessageBoundaryRow;
                        if (!row || !row.visible || !row.readBoundary)
                            continue;
                        const boundary = row.readBoundary;
                        const point = boundary.mapToItem(messageList, 0, 0);
                        if (point.y < height && point.y + boundary.height > 0 && point.x < width && point.x + boundary.width > 0)
                            ids.push(row.dispatchReadId.length ? row.dispatchReadId : row.readId);
                        if (ids.length === 256)
                            break;
                    }
                    if (ids.length)
                        root.controller.markVisibleMessagesRead(ids);
                    if (atYEnd && !anchorJumpPending && !root.controller.canLoadNewer)
                        root.controller.markTopicReadAtBottom();
                }
                function avatarPlacement(row, index) {
                    // One rail avatar follows the visible end of a peer group.
                    // Rows remain virtual even for a very long author run.
                    const geometry = contentY + contentHeight + row.y + row.height;
                    if (row.own || positioning || !row.readBoundary || !isFinite(geometry))
                        return { shown: false, y: 0 };
                    const bubble = row.readBoundary;
                    const point = bubble.mapToItem(messageList, 0, 0);
                    if (point.y >= height || point.y + bubble.height <= 0)
                        return { shown: false, y: 0 };
                    if (!row.groupLast) {
                        const next = itemAtIndex(index + 1) as MessageBoundaryRow;
                        if (next && next.readBoundary && next.readBoundary.mapToItem(messageList, 0, 0).y < height)
                            return { shown: false, y: 0 };
                    }
                    const bottom = Math.min(height - 8, row.groupLast ? point.y + bubble.height - 8 : height - 8);
                    const top = Math.max(row.groupFirst ? point.y : -Infinity, bottom - 48);
                    return { shown: true, y: top - row.mapToItem(messageList, 0, 0).y };
                }
                onReadActiveChanged: scheduleVisibleRead()
                onContentYChanged: {
                    scheduleVisibleRead(); scheduleServiceInspection();
                    dismissPassedUnreadDivider();
                    Qt.callLater(dismissPassedUnreadDivider);
                    // Issue #156: user-attributed scroll passes hand the reserve
                    // decision to the controller (reference gates preserved:
                    // userScrollSeen, positioning, anchorJumpPending).
                    if (userScrollSeen && !positioning && !anchorJumpPending)
                        root.controller.noteScrollReserve(distanceFromTop(), height)
                    // Issue #253: same reference syncAtBottomFromElement analogue
                    // that keeps the resize-time tail intent current outside the
                    // programmatic capture/restore passes. Programmatic movement
                    // must not overwrite it (positioning/anchorJump gates).
                    if (!positioning && !anchorJumpPending)
                        tailFollow = originY + contentHeight + bottomMargin - contentY - height <= 80
                }
                // Issue #253: the reference ResizeObserver effect
                // (workspace-message-list-scroll.hook.ts:956-1016) re-runs its
                // tail pin or anchor restore whenever the scroll container (or
                // a row) resizes. A window resize changes every delegate width
                // and the wrapped row heights with it, so the captured viewport
                // must be restored against the reflowed layout; without this
                // hook contentY stayed put while rows above the viewport grew
                // and the conversation drifted away from its tail or anchor.
                function resizeViewport() {
                    // Reference gates: never interrupt an in-flight capture/
                    // restore or an anchor highlight handoff, and never run on
                    // an empty or not-yet-laid-out list.
                    if (positioning || anchorJumpPending || count === 0 || width <= 0 || height <= 0)
                        return;
                    const snapshot = captureViewport();
                    // The persistent intent decides, exactly like the
                    // reference wasAtBottomRef && !userScrolledAway check:
                    // during a resize the geometry heuristic reads the new
                    // viewport against pre-reflow content, so either signal
                    // alone would misfire - here a taller viewport can shrink
                    // the bottom distance below 80 for a parked reader.
                    snapshot.followTail = tailFollow;
                    // The reference pinTailToBottom second pass re-checks
                    // userScrolledAwayFromBottomRef before re-pinning; the
                    // wheel observer moving userScrollSeen/userScrollGeneration
                    // during this chain is the native wheel-intent analogue
                    // of that check. captureViewport stamped the generation
                    // this snapshot must be compared against.
                    if (!snapshot.followTail && !snapshot.anchorId.length)
                        return;
                    positioning = true;
                    visibleReadTimer.stop();
                    pendingViewport = snapshot;
                    const ticket = ++layoutTicket;
                    Qt.callLater(() => restoreViewport(ticket));
                }
                onWidthChanged: { resizeViewport(); scheduleVisibleRead(); scheduleServiceInspection(); }
                onHeightChanged: { resizeViewport(); scheduleVisibleRead(); scheduleServiceInspection(); }
                onMovementEnded: { scheduleVisibleRead(); scheduleServiceInspection(); }
                Connections {
                    target: root
                    function onActiveFocusItemChanged() { messageList.scheduleServiceInspection(); }
                    function onSelectedStreamEncryptedChanged() { messageList.rebuildServiceView(); }
                }
                Component.onCompleted: replaceMessages(root.controller.messages)
                Timer {
                    id: visibleReadTimer
                    interval: 250
                    onTriggered: messageList.markVisibleRead()
                }
                ScrollBar.vertical: ScrollBar {
                    policy: ScrollBar.AsNeeded
                }
                NativeIconButton {
                    id: scrollToBottomButton
                    objectName: "scrollToBottomButton"
                    parent: messageList
                    z: 10
                    x: messageList.width - width - 16
                    y: messageList.height - height - 16
                    width: 40; height: 40; glyphSize: 20; cornerRadius: 8
                    colors: root.colors; asset: "chevron-down.svg"
                    visible: messageList.count > 0 && !messageList.positioning
                        && (!messageList.tailFollow || root.controller.canLoadNewer || root.controller.conversationOpening.failed)
                    enabled: !root.controller.busy && !root.controller.conversationOpening.pending
                    help: messageList.unreadCount > 0
                        ? qsTr("Scroll to bottom, %1 unread").arg(root.countText(messageList.unreadCount)) : qsTr("Scroll to bottom")
                    background: Rectangle {
                        radius: 8; color: root.colors["bg-elevated"]
                        border.width: scrollToBottomButton.visualFocus ? 2 : 1
                        border.color: root.colors[scrollToBottomButton.visualFocus ? "accent-soft" : "border-subtle"]
                    }
                    onClicked: messageList.scrollToLatest()
                    Rectangle {
                        objectName: "scrollToBottomUnreadBadge"
                        visible: messageList.unreadCount > 0
                        anchors.right: parent.right; anchors.rightMargin: -8
                        anchors.top: parent.top; anchors.topMargin: -14
                        width: Math.max(20, scrollUnreadText.implicitWidth + 8); height: 20; radius: 10
                        color: root.colors["badge-bg"]
                        Text {
                            id: scrollUnreadText
                            anchors.centerIn: parent; text: root.countText(messageList.unreadCount)
                            font.pixelSize: 12; color: root.colors["badge-text"]
                        }
                    }
                }
                // Issue #107 (docs/NATIVE_DESKTOP_ANCHOR_TRANSITION.md): the reference
                // anchor transition layer (workspace_ui 61dacef7
                // workspace-message-anchor-transition.ui.tsx) covers the list while a
                // message-link anchor resolves: skeleton preview zones after a 100 ms
                // delay, and a retryable in-list alert when the jump failed.
                component AnchorSkeletonBubble: Rectangle {
                    id: skeletonBubble
                    required property bool shimmer
                    // Static fallback keeps the rows and drops the pulse
                    // (reference :29-35 prefers-reduced-motion).
                    readonly property bool shimmerRunning: shimmer
                    height: 76
                    radius: 18
                    color: Qt.alpha(root.colors["border-subtle"], 0.40)
                    Accessible.ignored: true
                    SequentialAnimation on opacity {
                        id: anchorSkeletonPulse
                        objectName: "anchorSkeletonPulse"
                        loops: Animation.Infinite
                        running: skeletonBubble.shimmer
                        PropertyAnimation { to: 0.5; duration: 1000; easing.type: Easing.InOutSine }
                        PropertyAnimation { to: 1.0; duration: 1000; easing.type: Easing.InOutSine }
                    }
                    Column {
                        x: 12; y: 12
                        width: parent.width - 24
                        spacing: 8
                        Rectangle { width: parent.width / 3; height: 12; radius: 6; color: Qt.alpha(root.colors["border-subtle"], 0.60) }
                        Rectangle { width: parent.width; height: 12; radius: 6; color: Qt.alpha(root.colors["border-subtle"], 0.60) }
                        Rectangle { width: parent.width * 2 / 3; height: 12; radius: 6; color: Qt.alpha(root.colors["border-subtle"], 0.60) }
                    }
                }
                Item {
                    id: anchorTransitionLayer
                    objectName: "anchorTransitionLayer"
                    parent: messageList
                    anchors.fill: parent
                    z: 30
                    readonly property bool failure: root.controller.anchorNavigationError.length > 0
                    readonly property bool pending: root.controller.anchorTransitionPending
                    readonly property bool shimmerActive: !root.prefersReducedMotion
                    property bool anchorSkeletonsVisible: false
                    visible: pending || failure
                    Rectangle {
                        anchors.fill: parent
                        color: root.colors.bg
                    }
                    // Reference :122-131: skeleton rows appear 100 ms after the jump
                    // intent, re-armed per intent, so an instant jump never flashes them.
                    Timer {
                        id: anchorSkeletonDelay
                        interval: 100
                        onTriggered: anchorTransitionLayer.anchorSkeletonsVisible = true
                    }
                    Connections {
                        target: root.controller
                        function onAnchorChanged() {
                            if (root.controller.anchorTransitionPending && !anchorTransitionLayer.failure) {
                                anchorTransitionLayer.anchorSkeletonsVisible = false;
                                anchorSkeletonDelay.restart();
                            }
                        }
                    }
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 16; anchors.rightMargin: 16
                        anchors.topMargin: 24; anchors.bottomMargin: 24
                        spacing: 16
                        Item { Layout.fillWidth: true; Layout.fillHeight: true }
                        ColumnLayout {
                            objectName: "anchorSkeletonTop"
                            Layout.fillWidth: true
                            spacing: 12
                            AnchorSkeletonBubble {
                                objectName: "anchorSkeletonBubbleTopPeer"
                                visible: anchorTransitionLayer.anchorSkeletonsVisible && !anchorTransitionLayer.failure
                                Layout.alignment: Qt.AlignLeft
                                Layout.preferredWidth: Math.min(anchorTransitionLayer.width * 0.72, 512)
                                shimmer: anchorTransitionLayer.shimmerActive && anchorTransitionLayer.anchorSkeletonsVisible
                            }
                            AnchorSkeletonBubble {
                                objectName: "anchorSkeletonBubbleTopOwn"
                                visible: anchorTransitionLayer.anchorSkeletonsVisible && !anchorTransitionLayer.failure
                                Layout.alignment: Qt.AlignRight
                                Layout.preferredWidth: Math.min(anchorTransitionLayer.width * 0.72, 512)
                                shimmer: anchorTransitionLayer.shimmerActive && anchorTransitionLayer.anchorSkeletonsVisible
                            }
                        }
                        // Reference data-message-preview-placeholder: an empty
                        // min-h-20 column until a resolved preview bubble
                        // exists - the placeholder itself never carries a
                        // skeleton bubble.
                        Item {
                            objectName: "anchorPreviewPlaceholder"
                            Layout.alignment: Qt.AlignHCenter
                            Layout.fillWidth: true
                            Layout.maximumWidth: 672
                            Layout.preferredHeight: 80
                            Layout.minimumHeight: 80
                        }
                        ColumnLayout {
                            objectName: "anchorSkeletonBottom"
                            Layout.fillWidth: true
                            spacing: 12
                            AnchorSkeletonBubble {
                                objectName: "anchorSkeletonBubbleBottomOwn"
                                visible: anchorTransitionLayer.anchorSkeletonsVisible && !anchorTransitionLayer.failure
                                Layout.alignment: Qt.AlignRight
                                Layout.preferredWidth: Math.min(anchorTransitionLayer.width * 0.72, 512)
                                shimmer: anchorTransitionLayer.shimmerActive && anchorTransitionLayer.anchorSkeletonsVisible
                            }
                            AnchorSkeletonBubble {
                                objectName: "anchorSkeletonBubbleBottomPeer"
                                visible: anchorTransitionLayer.anchorSkeletonsVisible && !anchorTransitionLayer.failure
                                Layout.alignment: Qt.AlignLeft
                                Layout.preferredWidth: Math.min(anchorTransitionLayer.width * 0.72, 512)
                                shimmer: anchorTransitionLayer.shimmerActive && anchorTransitionLayer.anchorSkeletonsVisible
                            }
                        }
                        Item { Layout.fillWidth: true; Layout.fillHeight: true }
                    }
                    Text {
                        objectName: "anchorLoadingStatus"
                        visible: anchorTransitionLayer.pending && !anchorTransitionLayer.failure
                        x: 0; y: 0
                        width: 1; height: 1
                        opacity: 0 // reference sr-only role=status
                        text: qsTr("Loading message context")
                        Accessible.name: text
                        Accessible.role: Accessible.StaticText
                    }
                    Column {
                        objectName: "anchorFailedActions"
                        visible: anchorTransitionLayer.failure
                        anchors.left: parent.left; anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.leftMargin: 16; anchors.rightMargin: 16; anchors.bottomMargin: 16
                        spacing: 8
                        Rectangle {
                            id: anchorErrorOverlay
                            objectName: "anchorErrorOverlay"
                            width: parent.width
                            radius: 8
                            color: root.colors["bg-elevated"]
                            border.width: 1
                            border.color: root.colors["border-subtle"]
                            height: Math.max(anchorErrorDetail.y + anchorErrorDetail.height + 8,
                                anchorRetryButton.implicitHeight + 16)
                            Accessible.role: Accessible.AlertMessage
                            Accessible.name: anchorErrorTitle.text + ". " + anchorErrorDetail.text
                            Text {
                                id: anchorErrorTitle
                                objectName: "anchorErrorTitle"
                                x: 12; y: 8
                                width: Math.max(0, anchorErrorOverlay.width - anchorRetryButton.implicitWidth - 36)
                                text: qsTr("Could not open the selected message")
                                textFormat: Text.PlainText
                                font.pixelSize: 14
                                font.weight: Font.Medium
                                color: root.colors["notice-base"]
                                elide: Text.ElideRight
                            }
                            Text {
                                id: anchorErrorDetail
                                objectName: "anchorErrorDetail"
                                x: 12
                                anchors.top: anchorErrorTitle.bottom
                                width: anchorErrorTitle.width
                                text: root.controller.anchorNavigationError.length
                                    ? root.controller.anchorNavigationError
                                    : qsTr("The selected message is no longer available in the current workspace.")
                                textFormat: Text.PlainText
                                font.pixelSize: 12
                                color: root.colors["notice-base"]
                                wrapMode: Text.WordWrap
                            }
                            Button {
                                id: anchorRetryButton
                                objectName: "anchorRetryButton"
                                anchors.right: parent.right; anchors.rightMargin: 12
                                anchors.verticalCenter: parent.verticalCenter
                                padding: 0
                                text: qsTr("Retry opening message")
                                contentItem: Text {
                                    text: anchorRetryButton.text
                                    textFormat: Text.PlainText
                                    font.pixelSize: 12
                                    font.weight: Font.Medium
                                    color: root.colors["on-accent"]
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                                background: Rectangle {
                                    radius: 8
                                    color: root.colors.accent
                                    border.width: anchorRetryButton.visualFocus ? 2 : 0
                                    border.color: root.colors["accent-soft"]
                                }
                                implicitWidth: contentItem.implicitWidth + 24
                                implicitHeight: contentItem.implicitHeight + 12
                                onClicked: root.controller.retryAnchorNavigation()
                                Accessible.name: text
                            }
                        }
                        Item {
                            width: parent.width
                            height: 40
                            NativeIconButton {
                                objectName: "anchorScrollToBottom"
                                anchors.right: parent.right
                                width: 40; height: 40; glyphSize: 20; cornerRadius: 8
                                colors: root.colors; asset: "chevron-down.svg"
                                help: qsTr("Scroll to bottom")
                                background: Rectangle {
                                    radius: 8; color: root.colors["bg-elevated"]
                                    border.width: 1
                                    border.color: root.colors["border-subtle"]
                                }
                                icon.color: root.colors["text-primary"]
                                onClicked: {
                                    root.controller.dismissAnchorNavigation();
                                    messageList.scrollToLatest();
                                }
                            }
                        }
                    }
                }
                Text {
                    visible: messageList.count === 0
                    anchors.centerIn: parent
                    width: Math.max(0, parent.width - 40)
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: root.controller.busy ? qsTr("Loading…") : qsTr("No messages to display")
                    textFormat: Text.PlainText
                    color: root.colors["text-muted"]
                    font.pixelSize: 14
                }
                delegate: MessageBoundaryRow {
                    id: viewRow
                    objectName: "serviceViewRow-" + entry.id
                    required property var entry
                    required property int index
                    required property bool groupFirst
                    required property bool groupLast
                    required property bool topicRunStart
                    required property bool unreadStart
                    required serviceGroupId
                    required property bool serviceFirst
                    required property bool serviceLast
                    required property bool serviceCollapsed
                    required property int serviceCount
                    required property string serviceStatus
                    required property bool serviceExpanded
                    required property real serviceDisclosureSerial
                    required property string serviceReadId
                    displayIndex: index
                    readonly property bool own: Boolean(entry.own)
                    width: messageList.width - 24
                    // The capsule can extend into another delegate. Its local
                    // z cannot cross the Loader/list-row stacking boundary.
                    z: messageBodyLoader.item ? messageBodyLoader.item.quickReactionsLayer : 1
                    readonly property Item unreadDivider: unreadRule.visible ? unreadRule : null
                    readonly property real bodyTop: unreadRule.height + (serviceFirst ? serviceTopicDivider.height + 4 + serviceHeader.height : 0)
                    height: bodyTop + (messageBodyLoader.active && messageBodyLoader.item ? messageBodyLoader.item.height : 0) + (serviceLast ? (index + 1 < messageList.count ? 8 : 4) : 0)
                    readId: String(entry.id)
                    dispatchReadId: serviceReadId
                    readBoundary: serviceCollapsed ? collapsedReadBoundary : messageBodyLoader.item ? messageBodyLoader.item.readBoundary : null
                    function settleGeometry() {
                        if (messageBodyLoader.item) messageBodyLoader.item.settleGeometry();
                    }
                    Item {
                        id: unreadRule
                        objectName: "unreadDivider"
                        visible: viewRow.unreadStart && !messageList.unreadDividerDismissed
                        width: parent.width; height: visible ? 24 : 0
                        property string text: messageList.unreadCount > 0
                            ? qsTr("Unread messages • %1").arg(root.countText(messageList.unreadCount)) : qsTr("Unread messages")
                        Accessible.name: text; Accessible.role: Accessible.StaticText
                        RowLayout {
                            anchors.fill: parent; anchors.leftMargin: 16; anchors.rightMargin: 16
                            spacing: 8
                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.colors["notice-base"]; opacity: 0.3 }
                            Text { text: unreadRule.text; textFormat: Text.PlainText; font.pixelSize: 12; lineHeightMode: Text.FixedHeight; lineHeight: 16; color: root.colors["notice-base"] }
                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.colors["notice-base"]; opacity: 0.3 }
                        }
                    }
                    Item {
                        id: serviceTopicDivider
                        y: unreadRule.height
                        objectName: "serviceTopicDivider-" + viewRow.entry.id
                        visible: viewRow.serviceFirst && root.aggregateConversation && viewRow.topicRunStart
                        width: parent.width
                        height: visible ? (root.topicLabel(viewRow.entry.topicName).length ? 24 : 9) + 8 : 0
                        RowLayout {
                            anchors.fill: parent; anchors.leftMargin: 16; anchors.rightMargin: 16
                            spacing: 8
                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.colors["border-subtle"] }
                            Button {
                                text: root.topicLabel(viewRow.entry.topicName)
                                visible: text.length > 0
                                padding: 0; horizontalPadding: 0; verticalPadding: 0
                                implicitHeight: 16; implicitWidth: contentItem.implicitWidth
                                contentItem: Text { text: parent.text; textFormat: Text.PlainText; font.pixelSize: 12; color: root.colors["text-muted"] }
                                background: Rectangle { color: "transparent"; radius: 2; border.width: parent.visualFocus ? 2 : 0; border.color: root.colors["accent-soft"] }
                                onClicked: root.controller.selectTopic(viewRow.entry.topicId)
                            }
                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.colors["border-subtle"] }
                        }
                    }
                    Item {
                        id: serviceHeader
                        objectName: "serviceGroupHeader-" + viewRow.entry.id
                        visible: viewRow.serviceFirst
                        y: unreadRule.height + serviceTopicDivider.height + 4
                        width: parent.width
                        height: visible ? Math.max(28, serviceToggle.implicitHeight) + 8 : 0
                        readonly property color tone: root.colors[viewRow.serviceStatus === "action-required" ? "notice-base" : "text-muted"]
                        readonly property color rule: root.colors[viewRow.serviceStatus === "action-required" ? "notice-base" : "border-subtle"]
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 16; anchors.rightMargin: 16
                            anchors.topMargin: 4; anchors.bottomMargin: 4
                            spacing: 8
                            Rectangle {
                                Layout.fillWidth: true; Layout.preferredHeight: 1
                                color: serviceHeader.rule
                                opacity: viewRow.serviceStatus === "action-required" ? 0.3 : 1
                            }
                            Button {
                                id: serviceToggle
                                objectName: "serviceGroupToggle-" + viewRow.entry.id
                                property bool expanded: viewRow.serviceExpanded
                                property string status: viewRow.serviceStatus
                                Layout.maximumWidth: Math.max(0, serviceHeader.width - 48)
                                implicitHeight: Math.max(28, contentItem.implicitHeight)
                                implicitWidth: contentItem.implicitWidth + 8
                                padding: 0; horizontalPadding: 4; verticalPadding: 0
                                enabled: !messageList.serviceSelectionMode
                                hoverEnabled: true
                                text: messageList.serviceCaption(viewRow.serviceCount, viewRow.serviceStatus)
                                Accessible.name: text
                                Accessible.checkable: true
                                Accessible.checked: expanded
                                contentItem: RowLayout {
                                    spacing: 12
                                    NativeIconButton {
                                        objectName: "serviceGroupChevron-" + viewRow.entry.id
                                        colors: root.colors; asset: "chevron-down.svg"
                                        Layout.preferredWidth: 12; Layout.preferredHeight: 12
                                        padding: 0; horizontalPadding: 0
                                        glyphWidth: 12; glyphHeight: 6; glyphSize: 12
                                        enabled: false; opacity: 1; icon.color: serviceHeader.tone
                                        rotation: serviceToggle.expanded ? 0 : -90
                                        Accessible.ignored: true
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true; spacing: 4
                                        Text {
                                            objectName: "serviceGroupCaption-" + viewRow.entry.id
                                            text: messageList.serviceCaption(viewRow.serviceCount, ""); textFormat: Text.PlainText
                                            wrapMode: Text.Wrap; font.pixelSize: 12
                                            lineHeightMode: Text.FixedHeight; lineHeight: 16
                                            color: serviceHeader.tone
                                        }
                                        Text {
                                            objectName: "serviceGroupStatus-" + viewRow.entry.id
                                            visible: viewRow.serviceStatus.length > 0
                                            text: "· " + messageList.serviceStatusLabel(viewRow.serviceStatus); textFormat: Text.PlainText
                                            wrapMode: Text.Wrap; font.pixelSize: 12
                                            lineHeightMode: Text.FixedHeight; lineHeight: 16
                                            color: serviceHeader.tone
                                            Accessible.role: Accessible.StatusBar
                                        }
                                    }
                                }
                                background: Rectangle {
                                    radius: 4
                                    color: serviceToggle.hovered ? root.colors["card-bg-active"] : "transparent"
                                    border.width: serviceToggle.visualFocus ? 2 : 0
                                    border.color: root.colors["accent-soft"]
                                }
                                onClicked: messageList.toggleServiceGroup(viewRow.serviceGroupId)
                            }
                            Rectangle {
                                Layout.fillWidth: true; Layout.preferredHeight: 1
                                color: serviceHeader.rule
                                opacity: viewRow.serviceStatus === "action-required" ? 0.3 : 1
                            }
                        }
                        Item {
                            id: collapsedReadBoundary
                            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                            height: 1
                        }
                    }
                    Loader {
                        id: messageBodyLoader
                        objectName: "serviceBodyLoader-" + viewRow.entry.id
                        y: viewRow.bodyTop
                        width: parent.width
                        active: !viewRow.serviceCollapsed
                        sourceComponent: Component {
                            MessageContentRow {
                                entry: viewRow.entry; index: viewRow.index
                                groupFirst: viewRow.groupFirst; groupLast: viewRow.groupLast
                                topicRunStart: viewRow.topicRunStart
                                serviceGroupId: viewRow.serviceGroupId
                                serviceDisclosureSerial: viewRow.serviceDisclosureSerial
                            }
                        }
                    }
                }
                component MessageContentRow: MessageBoundaryRow {
                    id: messageRow
                    readonly property real quickReactionsLayer: quickReactions.expanded || quickReactions.hasFocus || quickReply.visualFocus ? 3 : quickReactions.visible || quickReply.hovered ? 2 : 1
                    required property var entry
                    required property int index
                    required property bool groupFirst
                    required property bool groupLast
                    required property bool topicRunStart
                    required property real serviceDisclosureSerial
                    readonly property var modelData: entry
                    readonly property bool encryptedBody: modelData.kind === "e2ee"
                    readonly property bool encryptedLocked: encryptedBody && root.encryptedPreparationState(modelData) !== "ready"
                    readonly property bool lockedInlineMeta: encryptedLocked && root.encryptedPreparationState(modelData) !== "error" && !Boolean(modelData.reactionBusy) && !(modelData.reactions || []).some(reaction => Number(reaction.count) > 0 || Boolean(reaction.pending))
                    readonly property string controlLabel: root.encryptionControlLabel(String(modelData.kind))
                    readonly property bool keyControl: controlLabel.length > 0
                    readonly property bool hasPoll: Boolean(modelData.poll)
                        && !keyControl && !encryptedLocked
                    readonly property bool keyResponse: modelData.kind === "key_grant" || modelData.kind === "key_reject"
                    // Only an instantiated, expanded G/J body supplies demand.
                    // Core derives R and owns runtime/owner/cache revision guards.
                    property bool keyServiceBodyMounted: false
                    property var keyServiceDemandController: null
                    property string keyServiceDemandScope: ""
                    property string keyServiceDemandId: ""
                    function demandKeyServiceRequest() {
                        if (!keyServiceBodyMounted || !keyResponse || !root.encryptedConversation
                            || keyServiceDemandController !== root.controller
                            || keyServiceDemandScope !== messageList.serviceScope()
                            || messageList.displayedRoute !== keyServiceDemandScope
                            || keyServiceDemandId !== String(modelData.id || "")) return;
                        root.controller.resolveKeyServiceRequest(keyServiceDemandId, serviceDisclosureSerial);
                    }
                    function bindKeyServiceRequest() {
                        if (!keyServiceBodyMounted || messageList.displayedRoute !== messageList.serviceScope()) return;
                        keyServiceDemandController = root.controller;
                        keyServiceDemandScope = messageList.serviceScope();
                        keyServiceDemandId = keyResponse ? String(modelData.id || "") : "";
                        demandKeyServiceRequest();
                    }
                    onEntryChanged: bindKeyServiceRequest()
                    onServiceDisclosureSerialChanged: demandKeyServiceRequest()
                    Connections {
                        target: root.controller
                        enabled: messageRow.keyResponse
                        // A genuine Core/vault reset publishes stateChanged even
                        // when the still-expanded row needs no QML reconstruction.
                        function onStateChanged() { messageRow.demandKeyServiceRequest(); }
                    }
                    readonly property var incomingKeyRequest: String(modelData.kind) === "key_request" ? root.incomingKeyRequestsById[String(modelData.id)] || null : null
                    readonly property bool externalAuthor: encryptedBody && !own && groupFirst
                    readonly property string displayedTopic: root.aggregateConversation && groupFirst ? root.topicLabel(modelData.topicName) : ""
                    readonly property bool hasDivider: root.aggregateConversation && topicRunStart && !keyControl
                    readonly property real bubbleOffset: hasDivider ? topicDivider.height + 8 : 0
                    readonly property var avatar: messageList.avatarPlacement(messageRow, index)
                    // displayContent === "" is a valid empty answer (quote-only message);
                    // only a missing key falls back to the raw content (review #119).
                    readonly property var rendered: root.markdown.render(encryptedLocked ? "" : keyControl ? controlLabel
                        : String(modelData.displayContent !== undefined ? modelData.displayContent : modelData.content))
                    readonly property var imageParts: rendered.parts || []
                    // Issue #109 (docs/NATIVE_DESKTOP_MENTION_CHIPS.md): the
                    // ordered urn:user references the renderer recognised in
                    // this body (ordered as they appear in the text).
                    readonly property var mentionUrns: rendered.mentions || []
                    readonly property bool hasInlineImages: !!root.messageImages && !encryptedLocked && !keyControl
                        && incomingKeyRequest === null && !keyResponse && modelData.kind !== "key_announced"
                        && (modelData.kind === "markdown" || modelData.kind === "e2ee" && Boolean(modelData.decrypted))
                        && imageParts.some(part => part.kind === "image")
                    readonly property string tailHtml: hasInlineImages
                        ? imageParts.length && imageParts[imageParts.length - 1].kind === "text" ? String(imageParts[imageParts.length - 1].html) : ""
                        : String(rendered.html)
                    readonly property bool inlineMeta: !encryptedLocked && !keyControl && !(modelData.reactions || []).some(reaction => Number(reaction.count) > 0) && root.trailingMessageParagraph(tailHtml) >= 0
                    // Issue #109 (docs/NATIVE_DESKTOP_MENTION_CHIPS.md): the
                    // chip's visible painting is the rich-text anchor itself;
                    // these transparent items supply the real hit target,
                    // pointer cursor and accessibility for each mention urn.
                    // Geometry comes from scanning the body TextEdit's
                    // linkAt map (only bodies that actually carry mentions).
                    // Inline-image bodies keep their per-part link handling.
                    // Issue #109 reviews F4/F6 (cid 4639284127/4639284139):
                    // the locator never samples the body area with linkAt.
                    // Each occurrence is located by its plain label inside
                    // the item's own laid-out text, and its geometry is read
                    // from the text layout (positionToRectangle) while
                    // walking only that label's characters. One rectangle is
                    // produced per occurrence and line, bounded by the actual
                    // link bounds. Review F2 (cid 4639284117): inline-image
                    // bodies locate the mentions of every visible text part.
                    readonly property var mentionLabels: rendered.mentionLabels || []
                    // Issue #109 review N4 (cid 4641051847): the renderer
                    // reports each mention anchor's own range in the plain
                    // text. When that list aligns with the urn order, the
                    // locator takes the anchor position directly, so a
                    // plain-text copy of the label earlier in the body can
                    // never steal the chip's hit target.
                    readonly property var mentionSpans: rendered.mentionSpans || []
                    readonly property bool mentionSpansAligned: messageRow.mentionSpans.length > 0
                        && messageRow.mentionSpans.length === messageRow.mentionUrns.length
                    property var mentionPairs: messageRow.mentionUrns.map(
                        (urn, index) => ({ urn: urn, label: String(messageRow.mentionLabels[index] || ""),
                            anchorStart: messageRow.mentionSpansAligned
                                ? Number(messageRow.mentionSpans[index].start) : -1,
                            anchorLength: messageRow.mentionSpansAligned
                                ? Number(messageRow.mentionSpans[index].length) : 0 }))
                    property var mentionTargets: locateMentionTargets()
                    function mentionRectsIn(item, pairs, overlay) {
                        const out = [];
                        if (!item || !item.visible || item.width <= 0 || pairs.length === 0)
                            return out;
                        const full = String(item.getText(0, item.length));
                        let cursor = 0;
                        for (const pair of pairs) {
                            const label = pair.label;
                            if (!label.length)
                                continue;
                            let found = -1;
                            let end = -1;
                            if (Number.isInteger(pair.anchorStart) && pair.anchorStart >= 0
                                && pair.anchorStart + pair.anchorLength <= full.length) {
                                found = pair.anchorStart;
                                end = found + pair.anchorLength;
                            } else {
                                found = full.indexOf(label, cursor);
                                if (found < 0)
                                    found = full.indexOf(label);
                                if (found < 0)
                                    continue;
                                end = found + label.length;
                            }
                            cursor = end;
                            let i = found;
                            while (i < end) {
                                const top = item.positionToRectangle(i);
                                let j = i;
                                while (j + 1 < end && Math.abs(item.positionToRectangle(j + 1).y - top.y) < 0.5)
                                    ++j;
                                // Issue #109 review N3 (cid 4641051840): the
                                // right edge is the layout cursor just after
                                // this line's last character when that cursor
                                // still sits on the line; when the run wraps,
                                // it is the last character's own rectangle
                                // edge. The old fallback shrank every line
                                // rect to about 8 px at the line start.
                                const after = item.positionToRectangle(j + 1);
                                const lastChar = j > i ? item.positionToRectangle(j) : top;
                                const right = Math.abs(after.y - top.y) < 0.5
                                    ? after.x : lastChar.x + lastChar.width;
                                const origin = item.mapToItem(overlay, top.x, top.y);
                                out.push({ urn: pair.urn, label: label,
                                    x: origin.x, y: origin.y,
                                    w: Math.max(6, right - top.x + 1), h: Math.max(12, top.height) });
                                i = j + 1;
                            }
                        }
                        return out;
                    }
                    function locateMentionTargets() {
                        if (!messageRow.hasInlineImages)
                            return mentionRectsIn(messageBody, messageRow.mentionPairs, bodyMentionOverlay);
                        const out = [];
                        if (!mediaMessageBody.visible || mediaMessageBody.width <= 0
                            || messageRow.mentionPairs.length === 0)
                            return out;
                        let consumed = 0;
                        for (let i = 0; i < imageRuns.count && consumed < messageRow.mentionPairs.length; ++i) {
                            const run = imageRuns.itemAt(i);
                            const part = run ? run.part : null;
                            if (!part || String(part.kind) !== "text")
                                continue;
                            const count = String(part.html || "").split("<a href=\"urn:user:").length - 1;
                            if (count <= 0)
                                continue;
                            // The TextEdit for each part starts at zero; translate
                            // anchor positions from the original document.
                            const pairs = messageRow.mentionPairs.slice(consumed, consumed + count)
                                .map(pair => ({ urn: pair.urn, label: pair.label,
                                    anchorStart: pair.anchorStart - part.start, anchorLength: pair.anchorLength }));
                            consumed += count;
                            for (const rect of mentionRectsIn(run.textPart, pairs, mediaMentionOverlay))
                                out.push(rect);
                        }
                        return out;
                    }
                    function imagePartHtml(part, index) {
                        const html = root.mentionChipHtml(String(part.html || "").replace(/<p style="/g, '<p style="line-height:20px;-qt-line-height-type:fixed;'));
                        return inlineMeta && index === imageParts.length - 1 ? root.reserveMessageTime(html, modelData.time) : html;
                    }
                    // Text.lineHeight does not affect Qt RichText. Apply the
                    // desktop paragraph leading only to renderer-owned p styles.
                    readonly property string bodyHtml: root.mentionChipHtml(String(rendered.html).replace(/<p style="/g, '<p style="line-height:20px;-qt-line-height-type:fixed;'))
                    readonly property string displayHtml: inlineMeta ? root.reserveMessageTime(bodyHtml, modelData.time) : bodyHtml
                    readonly property string callUrl: !keyControl && !encryptedLocked
                        && modelData.kind === "markdown" && !hasInlineImages
                        ? String(root.controller.callUrlInText(String(modelData.displayContent !== undefined
                            ? modelData.displayContent : modelData.content))) : ""
                    readonly property bool isCallCard: callUrl.length > 0
                    FontMetrics {
                        id: reactionNameMetrics
                        font.pixelSize: 14
                    }
                    FontMetrics {
                        id: reactionCountMetrics
                        font.pixelSize: 11
                    }
                    function reactionPreferredWidth(reaction) {
                        const people = reaction.participants || [];
                        const complete = Number(reaction.count) > 0 && people.length === Number(reaction.count);
                        const labelWidth = String(reaction.unicode).length ? 20 : Math.min(160, reactionNameMetrics.advanceWidth(String(reaction.emojiName)));
                        const countWidth = complete ? 20 + (people.length - 1) * 16 : reactionCountMetrics.advanceWidth(String(reaction.count));
                        return Math.max(48, labelWidth + 4 + countWidth + 16);
                    }
                    readonly property real footerPreferredWidth: {
                        const rows = (modelData.reactions || []).filter(reaction => Number(reaction.count) > 0 || Boolean(reaction.pending));
                        let result = 0;
                        for (let i = 0; i < rows.length; ++i)
                            result += reactionPreferredWidth(rows[i]);
                        return result + Math.max(0, rows.length - 1) * 4 + (rows.length ? 8 : 0) + messageTime.implicitWidth;
                    }
                    function settleGeometry() {
                        naturalText.forceLayout();
                        // Quote text and its nested Column have independent
                        // polish passes. Finish them before the outer Column
                        // supplies a height to ListView's viewport restoration.
                        if (messageQuoteFrame.visible) {
                            quoteAuthor.forceLayout();
                            if (quoteSelection.visible) quoteSelection.forceLayout();
                            if (quoteBody.visible) quoteBody.forceLayout();
                            messageQuoteContents.forceLayout();
                        }
                        // Issue #218: the body is a TextEdit now (no forceLayout
                        // slot); its document lays out inside the same polish
                        // pass that settles the content Column below.
                        if (messageBody.forceLayout) messageBody.forceLayout();
                        mediaMessageBody.forceLayout();
                        bubbleContent.forceLayout();
                        // Issue #109 review F4/F6: the locator reads the laid-out
                        // text, so rebind it after the forced layout passes settle.
                        messageRow.mentionTargets = Qt.binding(() => locateMentionTargets());
                    }
                    Component.onCompleted: {
                        settleGeometry();
                        keyServiceBodyMounted = true;
                        bindKeyServiceRequest();
                    }
                    Component.onDestruction: keyServiceBodyMounted = false
                    readBoundary: bubble
                    readId: String(modelData.id)
                    readonly property bool own: Boolean(modelData.own)
                    // Issue #113 read-request pulse (docs/NATIVE_DESKTOP_READ_REQUEST_PULSE.md):
                    // The list passes boundary coverage to each unread peer bubble
                    // (workspace-message-list.ui.tsx:661).
                    readonly property bool readRequestPending: !own && !Boolean(modelData.read)
                        && !!messageList.readRequestCoverage[modelData.id]
                    readonly property bool lastInOwnGroup: groupLast
                    width: messageList.width - 24
                    height: bubbleOffset + (externalAuthor ? 24 : 0) + bubble.height + (groupLast && !keyControl ? 4 : 0)
                    HoverHandler {
                        id: messageHover
                    }
                    Text {
                        id: naturalText
                        visible: false
                        text: messageRow.hasInlineImages ? messageRow.imageParts.map((part, index) => part.kind === "text" ? messageRow.imagePartHtml(part, index) : "").join("") : messageRow.displayHtml
                        textFormat: Text.RichText
                        font.pixelSize: 14
                        Accessible.ignored: true
                    }
                    Item {
                        id: topicDivider
                        objectName: "message-divider-" + messageRow.modelData.id
                        visible: messageRow.hasDivider
                        width: parent.width
                        height: root.topicLabel(messageRow.modelData.topicName).length ? 24 : 9
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 16
                            anchors.rightMargin: 16
                            spacing: 8
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 1
                                color: root.colors["border-subtle"]
                            }
                            Button {
                                id: dividerLink
                                objectName: "message-divider-link-" + messageRow.modelData.id
                                visible: text.length > 0
                                text: root.topicLabel(messageRow.modelData.topicName)
                                padding: 0
                                horizontalPadding: 0
                                verticalPadding: 0
                                implicitWidth: contentItem.implicitWidth
                                implicitHeight: 16
                                hoverEnabled: true
                                Accessible.name: text
                                background: Rectangle {
                                    color: "transparent"
                                    radius: 2
                                    border.width: dividerLink.visualFocus ? 2 : 0
                                    border.color: root.colors["accent-soft"]
                                }
                                contentItem: Text {
                                    text: dividerLink.text
                                    textFormat: Text.PlainText
                                    font.pixelSize: 12
                                    color: root.colors[dividerLink.hovered ? "text-primary" : "text-muted"]
                                    font.underline: dividerLink.hovered
                                }
                                onClicked: root.controller.selectTopic(messageRow.modelData.topicId)
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 1
                                color: root.colors["border-subtle"]
                            }
                        }
                    }
                    Rectangle {
                        objectName: "message-avatar-" + messageRow.modelData.id
                        visible: !messageRow.keyControl && messageRow.avatar.shown
                            && root.messageSelectionCount === 0
                        x: 0
                        y: messageRow.avatar.y
                        width: 48
                        height: 48
                        radius: 24
                        color: root.colors["bg-elevated"]
                        border.color: root.colors["border-subtle"]
                        NativeAvatarImage {
                            id: messagePhoto
                            anchors.fill: parent
                            reference: String(messageRow.modelData.authorAvatar || "")
                            loadActive: root.visible && root.controller.authenticated
                        }
                        PresenceBadge {
                            objectName: "presence-author-" + messageRow.modelData.id
                            presence: root.controller.authenticated ? String(messageRow.modelData.authorPresence || "") : ""
                        }
                        Text {
                            visible: !messagePhoto.replacesFallback
                            objectName: "message-avatar-initial-" + messageRow.modelData.id
                            anchors.centerIn: parent
                            text: String(messageRow.modelData.author).slice(0, 1)
                            textFormat: Text.PlainText
                            color: root.colors["text-primary"]
                            font.pixelSize: 18
                            font.weight: Font.DemiBold
                        }
                    }
                    Row {
                        visible: messageRow.externalAuthor
                        x: bubble.x; y: messageRow.bubbleOffset
                        height: 16; width: bubble.width; spacing: 6
                        Text {
                            text: messageRow.modelData.author; textFormat: Text.PlainText
                            width: Math.min(implicitWidth, parent.width * 0.65)
                            elide: Text.ElideRight; font.pixelSize: 12; font.weight: Font.Medium
                            color: root.colors["text-muted"]
                        }
                        Button {
                            visible: messageRow.displayedTopic.length > 0
                            height: 16; padding: 0
                            text: messageRow.displayedTopic
                            contentItem: Text { text: parent.text; textFormat: Text.PlainText; font.pixelSize: 12; color: root.colors.accent; elide: Text.ElideRight }
                            background: null
                            onClicked: root.controller.selectTopic(messageRow.modelData.topicId)
                        }
                    }
                    Rectangle {
                        objectName: "selectMessage-" + messageRow.modelData.id
                        // Reference WorkspaceMessageSelectionControl: a 32 px
                        // hit area bottom-anchored to the bubble (2 px inset),
                        // a 16 px visual with 4 px radius and 1 px muted
                        // border; checked = accent fill + 12 px check glyph in
                        // bg colour. The checkboxes appear only in selection
                        // mode (workspace-message-list.ui.tsx:503,545-553).
                        visible: root.messageSelectionCount > 0
                            && !messageRow.keyControl && root.messageSelectionSurface
                        x: 12
                        y: bubble.y + bubble.height - height - 2
                        width: 32
                        height: 32
                        color: "transparent"
                        Accessible.role: Accessible.CheckBox
                        Accessible.name: root.isMessageSelected(messageRow.modelData.id)
                            ? qsTr("Deselect") : qsTr("Select")
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.toggleMessageSelection(messageRow.modelData.id)
                        }
                        Rectangle {
                            objectName: "selectMessageBox-" + messageRow.modelData.id
                            anchors.centerIn: parent
                            width: 16
                            height: 16
                            radius: 4
                            color: root.isMessageSelected(messageRow.modelData.id) ? root.colors.accent : "transparent"
                            border.width: 1
                            border.color: root.isMessageSelected(messageRow.modelData.id)
                                ? root.colors.accent : root.colors["text-muted"]
                            Image {
                                objectName: "selectMessageCheck-" + messageRow.modelData.id
                                anchors.centerIn: parent
                                width: 12
                                height: 12
                                visible: root.isMessageSelected(messageRow.modelData.id)
                                source: "qrc:/workspace/icons/check.svg"
                                layer.enabled: true
                                layer.effect: MultiEffect {
                                    colorization: 1.0
                                    colorizationColor: root.colors.bg
                                }
                            }
                        }
                    }
                    Rectangle {
                        objectName: "messageHighlight-" + messageRow.modelData.id
                        Accessible.ignored: true
                        visible: root.controller.anchorMessageId === messageRow.modelData.id
                        z: 1
                        x: bubble.x - 3
                        y: bubble.y - 3
                        width: bubble.width + 6
                        height: bubble.height + 6
                        radius: 21
                        color: "transparent"
                        border.width: 2
                        border.color: root.colors.accent
                    }
                    Rectangle {
                        id: bubble
                        objectName: "message-" + messageRow.modelData.id
                        activeFocusOnTab: true
                        KeyNavigation.tab: quickReply.visible ? quickReply : null
                        width: messageRow.keyControl ? messageRow.width : messageRow.hasPoll
                            ? Math.min(360, Math.max(24, (messageRow.width - (messageRow.own ? 0 : 56)) * 0.88))
                            : Math.min(Math.max(messageRow.encryptedLocked ? Math.min(480, root.width * 0.6) : naturalText.implicitWidth, messageRow.hasInlineImages ? mediaMessageBody.imagePreferredWidth : 0, authorHeader.preferredWidth, messageRow.inlineMeta ? 0 : messageRow.footerPreferredWidth) + 24, Math.max(24, (messageRow.width - (messageRow.own ? 0 : 56)) * 0.88))
                        height: bubbleContent.implicitHeight + (messageRow.keyControl ? 8 : 16)
                        x: messageRow.keyControl ? 0 : messageRow.own ? messageRow.width - width : 56
                        y: messageRow.bubbleOffset + (messageRow.externalAuthor ? 24 : 0)
                        color: messageRow.keyControl ? "transparent" : messageRow.own ? root.colors["msg-own-bg"] : root.colors["msg-bg"]
                        topLeftRadius: !messageRow.own && !messageRow.groupLast ? 10 : 18
                        topRightRadius: !messageRow.own || messageRow.groupLast ? 18 : 10
                        bottomLeftRadius: messageRow.own ? 18 : messageRow.groupLast ? 6 : 10
                        bottomRightRadius: messageRow.own ? (messageRow.lastInOwnGroup ? 6 : 10) : 18
                        // Issue #113 read-request pulse (docs/NATIVE_DESKTOP_READ_REQUEST_PULSE.md):
                        // native port of workspace-message-read-request-pulse::before
                        // (app.styles.css:128-156) - an accent wash between the bubble
                        // fill and its content. z: 0 keeps the wash above the parent
                        // Rectangle's fill (a negative z paints behind it and the fill
                        // hides the pulse - exordy review finding 4639122138) while the
                        // later bubbleContent sibling still paints fully opaque on top.
                        // Radii mirrored from the bubble (border-radius: inherit),
                        // opacity 0 -> 0.2 -> 0 per 1.6 s ease-in-out cycle while the
                        // bubble awaits its read receipt. root.active is the Window.active
                        // stand-in for the reference activity gate, and reduced motion
                        // freezes the wash at the full 0.2 (app.styles.css:269-286).
                        // No visible text: the reference overlay is purely visual.
                        Rectangle {
                            id: readRequestOverlay
                            objectName: "message-read-pulse-" + String(messageRow.modelData.id)
                            anchors.fill: parent
                            visible: messageRow.readRequestPending
                            z: 0
                            color: root.colors.accent
                            opacity: root.prefersReducedMotion ? 0.2 : 0
                            topLeftRadius: bubble.topLeftRadius
                            topRightRadius: bubble.topRightRadius
                            bottomLeftRadius: bubble.bottomLeftRadius
                            bottomRightRadius: bubble.bottomRightRadius
                            Accessible.ignored: true
                            SequentialAnimation {
                                objectName: "message-read-pulse-anim-" + String(messageRow.modelData.id)
                                loops: Animation.Infinite
                                running: messageRow.readRequestPending && root.active
                                    && !root.prefersReducedMotion
                                // CSS applies the timing function per keyframe
                                // segment, so each 800 ms leg eases in-out.
                                NumberAnimation { target: readRequestOverlay; property: "opacity"; from: 0.0; to: 0.2; duration: 800; easing.type: Easing.InOutQuad }
                                NumberAnimation { target: readRequestOverlay; property: "opacity"; from: 0.2; to: 0.0; duration: 800; easing.type: Easing.InOutQuad }
                            }
                        }
                        Column {
                            id: bubbleContent
                            x: 12
                            y: messageRow.keyControl ? 4 : 8
                            width: parent.width - 24
                            spacing: 4
                            // Meeting-URL messages render as the call card in
                            // place of the text body (issue #161, reference
                            // workspace-message-bubble-presentation.ui.tsx:543+906).
                            NativeJitsiCard {
                                id: callCard
                                objectName: "call-card-" + messageRow.modelData.id
                                visible: messageRow.isCallCard && !messageRow.keyControl
                                width: parent.width
                                colors: root.colors
                                own: messageRow.own
                                callUrl: messageRow.callUrl
                                roomName: root.controller.callRoomName(messageRow.callUrl)
                                locationName: ""
                                authorName: String(messageRow.modelData.author || "")
                                onJoinRequested: meetingUrl => root.controller.joinCall(meetingUrl)
                            }
                            // Poll card (issue #216, docs/NATIVE_DESKTOP_POLLS_BUBBLE.md): the
                            // reference reserves a 336 px column inside the 360 px bubble
                            // (workspace-message-poll.css:4-9); voting, results, marks and the
                            // inline add-option input follow workspace-message-poll-question.ui.tsx.
                            NativePollCard {
                                visible: messageRow.hasPoll
                                width: parent.width
                                colors: root.colors
                                controller: root.controller
                                readOnly: !root.pollInteractiveStream
                                messageUuid: String(messageRow.modelData.id || "")
                                poll: messageRow.modelData.poll
                                onVoteRequested: function (questionUuid, optionUuid, selected) {
                                    root.controller.votePollOption(messageUuid, questionUuid, optionUuid, selected)
                                }
                                onAddOptionRequested: function (questionUuid, optionUuid, optionText) {
                                    root.controller.addPollOption(messageUuid, questionUuid, optionUuid, optionText)
                                }
                            }
                            // Quote frame (docs/NATIVE_DESKTOP_REPLIES.md). Source message-surface
                            // frame (workspace_ui 61dacef workspace-message-quote.ui.tsx + frame.ui.tsx:8-18):
                            // bg at 35 % alpha, radius 6 px, 2 px accent left bar, vertical margin
                            // 4 px, padding 8 x / 6 y; header 12 px / 16 px weight-500 accent
                            // (fallback label, else "Quote"), muted "Message unavailable" when the
                            // origin is gone, 12 px muted "Loading messages" while resolving, body
                            // 14 px / 20 px; clicking the frame jumps to the origin (quote.ui.tsx:353-397).
                            Rectangle {
                                id: messageQuoteFrame
                                objectName: "message-quote-" + messageRow.modelData.id
                                visible: String(messageRow.modelData.quoteId || "").length > 0
                                    && !messageRow.keyControl && !messageRow.encryptedLocked
                                width: parent.width
                                // Media preview leading the quote frame (issue #217,
                                // docs/NATIVE_DESKTOP_QUOTE_MEDIA_THUMBNAILS.md):
                                // first image, else first video, never for a
                                // selected-text quote (quote.ui.tsx:257-294).
                                readonly property bool quoteMediaAllowed:
                                    String(messageRow.modelData.quoteStatus) === "ready"
                                    && String(messageRow.modelData.quoteText || "").length === 0
                                readonly property var quoteMedia: quoteMediaAllowed
                                    ? root.controller.mediaPreviewReference(String(messageRow.modelData.quoteContent || ""))
                                    : ({})
                                readonly property string quoteMediaUrn: String(quoteMedia.urn || "")
                                readonly property string quoteMediaKind: String(quoteMedia.kind || "")
                                readonly property bool quoteMediaShown: messageQuoteThumbnail.visible
                                    && messageQuoteThumbnail.mediaStatus === "ready"
                                height: Math.max(messageQuoteContents.height,
                                    messageQuoteThumbnail.visible ? 48 : 0) + 12
                                radius: 6
                                color: Qt.alpha(root.colors["bg"], 0.35)
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.controller.openMessageInChat(messageRow.modelData.quoteId)
                                }
                                Rectangle {
                                    objectName: "quoteAccentBar-" + messageRow.modelData.id
                                    width: 2
                                    height: parent.height
                                    radius: 1
                                    color: root.colors.accent
                                }
                                NativeQuoteMediaThumbnail {
                                    id: messageQuoteThumbnail
                                    objectName: "quoteMediaThumbnail-" + messageRow.modelData.id
                                    // Demand only: the component hides itself
                                    // on a failed load (issue #217 exo-review).
                                    demand: messageQuoteFrame.quoteMediaUrn.length > 0
                                    colors: root.colors
                                    images: root.messageImages
                                    urn: messageQuoteFrame.quoteMediaUrn
                                    kind: messageQuoteFrame.quoteMediaKind
                                    x: 10
                                    y: 6
                                    height: parent.height - 12
                                }
                                readonly property string quoteHtml: String(messageRow.modelData.quoteStatus) === "ready"
                                    && String(messageRow.modelData.quoteText || "").length === 0
                                    ? String(root.markdown.render(String(messageRow.modelData.quoteContent),
                                        messageQuoteFrame.quoteMediaShown).html)
                                        .replace(/<p style="/g, '<p style="line-height:20px;-qt-line-height-type:fixed;')
                                    : ""
                                Column {
                                    id: messageQuoteContents
                                    x: messageQuoteThumbnail.visible ? 66 : 10
                                    width: parent.width - (messageQuoteThumbnail.visible ? 74 : 18)
                                    y: 6
                                    spacing: 2
                                    Text {
                                        id: quoteAuthor
                                        objectName: "quoteHeaderText-" + messageRow.modelData.id
                                        width: parent.width
                                        text: String(messageRow.modelData.quoteStatus) === "unavailable"
                                            ? qsTr("Message unavailable")
                                            : String(messageRow.modelData.quoteAuthorName || messageRow.modelData.quoteLabel || "") || qsTr("Quote")
                                        textFormat: Text.PlainText
                                        font.pixelSize: 12
                                        font.weight: Font.Medium
                                        lineHeightMode: Text.FixedHeight
                                        lineHeight: 16
                                        elide: Text.ElideRight
                                        color: String(messageRow.modelData.quoteStatus) === "unavailable"
                                            ? root.colors["text-muted"] : root.colors.accent
                                    }
                                    Text {
                                        visible: String(messageRow.modelData.quoteStatus) === "loading"
                                        text: qsTr("Loading messages")
                                        textFormat: Text.PlainText
                                        font.pixelSize: 12
                                        color: root.colors["text-muted"]
                                    }
                                    Text {
                                        id: quoteSelection
                                        visible: String(messageRow.modelData.quoteStatus) === "ready"
                                            && String(messageRow.modelData.quoteText || "").length > 0
                                        width: parent.width
                                        text: String(messageRow.modelData.quoteText || "")
                                        textFormat: Text.PlainText
                                        font.pixelSize: 14
                                        lineHeightMode: Text.FixedHeight
                                        lineHeight: 20
                                        wrapMode: Text.Wrap
                                        color: root.colors["text-primary"]
                                    }
                                    Text {
                                        id: quoteBody
                                        objectName: "quoteBodyHtml-" + messageRow.modelData.id
                                        visible: messageQuoteFrame.quoteHtml.length > 0
                                        width: parent.width
                                        text: messageQuoteFrame.quoteHtml
                                        textFormat: Text.RichText
                                        font.pixelSize: 14
                                        lineHeightMode: Text.FixedHeight
                                        lineHeight: 20
                                        wrapMode: Text.Wrap
                                        linkColor: root.colors.accent
                                        onLinkActivated: link => root.openReference(link)
                                        color: root.colors["text-primary"]
                                    }
                                    // Issue #222: decrypt-aware quote sub-states (reference
                                    // workspace_ui 230e02b workspace-message-quote.ui.tsx:123-143 +
                                    // encrypted-message-placeholder.ui.tsx geometry), mirrored from
                                    // the bubble's encrypted-placeholder-* row without restyle.
                                    // "preparing" renders the author header with no body, so it
                                    // needs no node here.
                                    Item {
                                        id: quoteLockedPlaceholder
                                        objectName: "quote-locked-" + messageRow.modelData.id
                                        visible: String(messageRow.modelData.quoteStatus) === "locked"
                                        width: Math.min(480, root.width * 0.6, parent.width)
                                        height: 36
                                        clip: true
                                        Accessible.role: Accessible.StaticText
                                        Accessible.name: qsTr("Key required")
                                        Item {
                                            anchors.fill: parent
                                            readonly property bool blurSupported: GraphicsInfo.api !== GraphicsInfo.Software && GraphicsInfo.api !== GraphicsInfo.OpenVG && GraphicsInfo.api !== GraphicsInfo.Unknown && GraphicsInfo.api !== GraphicsInfo.Null
                                            layer.enabled: visible && blurSupported
                                            layer.effect: MultiEffect { objectName: "quote-locked-effect-" + messageRow.modelData.id; blurEnabled: true; blurMax: 36; blur: 1 }
                                            Rectangle { objectName: "quote-locked-bar-wide-" + messageRow.modelData.id; y: 6; width: parent.width * 0.92; height: 8; radius: 4; color: root.colors["text-muted"]; opacity: 0.35 }
                                            Rectangle { objectName: "quote-locked-bar-narrow-" + messageRow.modelData.id; y: 22; width: parent.width * 0.65; height: 8; radius: 4; color: root.colors["text-muted"]; opacity: 0.35 }
                                            Accessible.ignored: true
                                        }
                                    }
                                    Row {
                                        id: quoteDecryptErrorRow
                                        objectName: "quote-error-" + messageRow.modelData.id
                                        visible: String(messageRow.modelData.quoteStatus) === "error"
                                        width: parent.width; height: Math.max(36, quoteDecryptErrorText.implicitHeight); spacing: 8
                                        NativeIconButton {
                                            colors: root.colors; asset: "warning.svg"; glyphSize: 16
                                            width: 16; height: 20; padding: 0; enabled: false; opacity: 1
                                            icon.color: root.colors["text-muted"]; Accessible.ignored: true
                                        }
                                        Text {
                                            id: quoteDecryptErrorText
                                            objectName: "quote-error-text-" + messageRow.modelData.id
                                            width: Math.max(0, parent.width - 32 - (quoteDecryptRetry.visible ? quoteDecryptRetry.implicitWidth : 0))
                                            text: qsTr("Could not decrypt message"); textFormat: Text.PlainText
                                            font.pixelSize: 14; color: root.colors["text-muted"]; wrapMode: Text.Wrap
                                            Accessible.role: Accessible.AlertMessage
                                        }
                                        // Issue #222: the inline Retry re-runs the origin's per-message
                                        // decrypt (#221 path) through the shared controller guard, so a
                                        // bubble Retry and this one can never double-decrypt; disabled
                                        // while the uuid is in flight.
                                        InlineDecryptRetryAction {
                                            id: quoteDecryptRetry
                                            objectName: "quote-retry-" + messageRow.modelData.id
                                            text: qsTr("Retry")
                                            Accessible.name: text
                                            enabled: root.controller.decryptRetryPendingUuids.indexOf(String(messageRow.modelData.quoteId)) < 0
                                            onClicked: root.controller.retryMessageDecrypt(String(messageRow.modelData.quoteId))
                                        }
                                    }
                                }
                            }
                            Row {
                                id: authorHeader
                                visible: !messageRow.keyControl && !messageRow.externalAuthor && messageRow.groupFirst && (!messageRow.own || messageRow.displayedTopic.length > 0)
                                readonly property real preferredWidth: visible ? (authorLabel.visible ? authorLabel.implicitWidth : 0) + (topicLink.visible ? topicLink.contentItem.implicitWidth : 0) + (authorLabel.visible && topicLink.visible ? 6 : 0) : 0
                                width: parent.width
                                height: 16
                                spacing: 6
                                Text {
                                    id: authorLabel
                                    objectName: "message-author-" + messageRow.modelData.id
                                    visible: !messageRow.own && messageRow.groupFirst
                                    width: Math.min(implicitWidth, topicLink.visible ? parent.width * 0.5 : parent.width)
                                    text: messageRow.modelData.author
                                    textFormat: Text.PlainText
                                    font.pixelSize: 12
                                    font.weight: Font.Medium
                                    lineHeightMode: Text.FixedHeight
                                    lineHeight: 16
                                    color: root.colors["text-muted"]
                                    elide: Text.ElideRight
                                }
                                Button {
                                    id: topicLink
                                    objectName: "message-topic-" + messageRow.modelData.id
                                    visible: messageRow.displayedTopic.length > 0
                                    width: Math.min(contentItem.implicitWidth, Math.max(0, parent.width - (authorLabel.visible ? authorLabel.width + 6 : 0)))
                                    height: 16
                                    padding: 0
                                    horizontalPadding: 0
                                    verticalPadding: 0
                                    hoverEnabled: true
                                    text: messageRow.displayedTopic
                                    Accessible.name: text
                                    contentItem: Text {
                                        text: topicLink.text
                                        textFormat: Text.PlainText
                                        font.pixelSize: 12
                                        font.weight: Font.Medium
                                        lineHeightMode: Text.FixedHeight
                                        lineHeight: 16
                                        color: root.colors[topicLink.hovered ? "text-primary" : "accent"]
                                        font.underline: topicLink.hovered
                                        elide: Text.ElideRight
                                    }
                                    background: Rectangle {
                                        color: "transparent"
                                        radius: 2
                                        border.width: topicLink.visualFocus ? 2 : 0
                                        border.color: root.colors["accent-soft"]
                                    }
                                    onClicked: root.controller.selectTopic(messageRow.modelData.topicId)
                                }
                            }
                            Item {
                                objectName: "encrypted-placeholder-" + messageRow.modelData.id
                                visible: messageRow.encryptedLocked && root.encryptedPreparationState(messageRow.modelData) !== "error"
                                width: Math.min(480, root.width * 0.6, parent.width)
                                height: 36
                                clip: true
                                Accessible.role: Accessible.StaticText
                                Accessible.name: root.encryptedPreparationState(messageRow.modelData) === "preparing" ? qsTr("Decrypting message") : root.encryptedPreparationState(messageRow.modelData) === "unavailable" ? qsTr("Encryption is unavailable for this chat or local key storage could not be opened.") : qsTr("Key required")
                                Item {
                                    anchors.fill: parent
                                    readonly property bool blurSupported: GraphicsInfo.api !== GraphicsInfo.Software && GraphicsInfo.api !== GraphicsInfo.OpenVG && GraphicsInfo.api !== GraphicsInfo.Unknown && GraphicsInfo.api !== GraphicsInfo.Null
                                    layer.enabled: visible && blurSupported
                                    layer.effect: MultiEffect { objectName: "encrypted-placeholder-effect-" + messageRow.modelData.id; blurEnabled: true; blurMax: 36; blur: 1 }
                                    Rectangle { y: 6; width: parent.width * 0.92; height: 8; radius: 4; color: root.colors["text-muted"]; opacity: 0.35 }
                                    Rectangle { y: 22; width: parent.width * 0.65; height: 8; radius: 4; color: root.colors["text-muted"]; opacity: 0.35 }
                                    Accessible.ignored: true
                                }
                            }
                            Row {
                                objectName: "encrypted-error-" + messageRow.modelData.id
                                visible: messageRow.encryptedBody && root.encryptedPreparationState(messageRow.modelData) === "error"
                                width: parent.width; height: Math.max(36, encryptedErrorText.implicitHeight); spacing: 8
                                NativeIconButton {
                                    colors: root.colors; asset: "warning.svg"; glyphSize: 16
                                    width: 16; height: 20; padding: 0; enabled: false; opacity: 1
                                    icon.color: root.colors["text-muted"]; Accessible.ignored: true
                                }
                                Text {
                                    id: encryptedErrorText
                                    width: Math.max(0, parent.width - 32 - (messageDecryptRetry.visible ? messageDecryptRetry.implicitWidth : 0))
                                    text: qsTr("Could not decrypt message"); textFormat: Text.PlainText
                                    font.pixelSize: 14; color: root.colors["text-muted"]; wrapMode: Text.Wrap
                                    Accessible.role: Accessible.AlertMessage
                                }
                                // Issue #221: inline ghost Retry re-runs this message's decrypt
                                // (workspace_ui 230e02b workspace-message-content-bubble.ui.tsx:49-55);
                                // disabled while the controller reports the uuid in flight.
                                InlineDecryptRetryAction {
                                    id: messageDecryptRetry
                                    objectName: "encrypted-retry-" + messageRow.modelData.id
                                    text: qsTr("Retry")
                                    Accessible.name: text
                                    enabled: root.controller.decryptRetryPendingUuids.indexOf(String(messageRow.modelData.id)) < 0
                                    onClicked: root.controller.retryMessageDecrypt(String(messageRow.modelData.id))
                                }
                            }
                            Column {
                                id: keyRequestService
                                objectName: "keyRequestService-" + messageRow.modelData.id
                                visible: messageRow.incomingKeyRequest !== null
                                readonly property var request: messageRow.incomingKeyRequest || ({})
                                readonly property bool own: root.ownsKeyRequest(request)
                                readonly property string title: root.incomingKeyRequestTitle(request)
                                readonly property string description: root.incomingKeyRequestDescription(request)
                                readonly property string status: root.incomingKeyRequestStatus(request)
                                readonly property string actor: own ? "" : String(request.name || "")
                                readonly property var responseIntent: {
                                    const saved = root.chatEncryption.keyResponse || ({});
                                    return String(saved.requestId || "") === String(messageRow.modelData.id) ? saved : ({});
                                }
                                readonly property bool retainedResponse: String(responseIntent.requestId || "").length > 0
                                    && responseIntent.phase !== "ready"
                                function responseEnabled(action) {
                                    if (!root.controller.authenticated || !root.controller.hasConversation
                                            || String(request.id || "") !== String(messageRow.modelData.id)
                                            || Boolean(request.ownCurrentDevice) || Boolean(root.chatEncryption.busy))
                                        return false;
                                    if (action !== "grant" && action !== "reject")
                                        return false;
                                    const offered = action === "grant" ? Boolean(request.offerGrant) : Boolean(request.offerReject);
                                    if (!offered)
                                        return false;
                                    if (retainedResponse)
                                        return String(responseIntent.action || "") === action
                                            && responseIntent.phase === "uncertain" && !Boolean(responseIntent.busy)
                                            && Boolean(responseIntent.canRetry);
                                    return action === "grant" ? Boolean(request.canGrant) : Boolean(request.canReject);
                                }
                                function respond(action) {
                                    if (!responseEnabled(action))
                                        return;
                                    const id = String(messageRow.modelData.id);
                                    if (retainedResponse)
                                        root.controller.retryChatKeyResponse(id);
                                    else if (action === "grant")
                                        root.controller.grantChatKey(id);
                                    else
                                        root.controller.rejectChatKey(id);
                                }
                                readonly property string accessibleSummary: [actor, title, description, status].filter(part => part.length > 0).join(" ")
                                width: parent.width; spacing: 4
                                Flow {
                                    id: keyRequestLine
                                    readonly property real naturalWidth: keyRequestTitle.implicitWidth
                                        + (keyRequestActor.visible ? keyRequestActor.implicitWidth + 4 : 0)
                                        + (grantRequestKey.visible ? grantRequestKey.implicitWidth + 4 : 0)
                                        + (rejectRequestKey.visible ? rejectRequestKey.implicitWidth + 4 : 0)
                                    width: Math.min(parent.width, naturalWidth)
                                    x: (parent.width - width) / 2
                                    height: implicitHeight; spacing: 4
                                    function centeredOffset(item) {
                                        if (!item.visible || item.width <= 0 || item.height <= 0)
                                            return Qt.point(0, 0);
                                        const parts = [keyRequestActor, keyRequestTitle, grantRequestKey, rejectRequestKey];
                                        let left = width, right = 0, rowHeight = 0;
                                        for (let i = 0; i < parts.length; ++i) {
                                            const part = parts[i];
                                            // Flow owns raw x/y. Translate never changes this row witness.
                                            if (!part.visible || part.width <= 0 || part.height <= 0 || part.y !== item.y)
                                                continue;
                                            left = Math.min(left, part.x);
                                            right = Math.max(right, part.x + part.width);
                                            rowHeight = Math.max(rowHeight, part.height);
                                        }
                                        return Qt.point((width - (right - left)) / 2 - left, (rowHeight - item.height) / 2);
                                    }
                                    Text {
                                        id: keyRequestActor
                                        transform: Translate {
                                            readonly property point offset: keyRequestLine.centeredOffset(keyRequestActor)
                                            x: offset.x; y: offset.y
                                        }
                                        horizontalAlignment: Text.AlignHCenter
                                        visible: keyRequestService.actor.length > 0
                                        text: keyRequestService.actor; textFormat: Text.PlainText
                                        width: Math.min(implicitWidth, keyRequestLine.width)
                                        height: contentHeight; wrapMode: Text.Wrap
                                        font.pixelSize: 14; color: root.colors["text-primary"]
                                        lineHeightMode: Text.FixedHeight; lineHeight: 20
                                    }
                                    Text {
                                        id: keyRequestTitle
                                        transform: Translate {
                                            readonly property point offset: keyRequestLine.centeredOffset(keyRequestTitle)
                                            x: offset.x; y: offset.y
                                        }
                                        horizontalAlignment: Text.AlignHCenter
                                        text: keyRequestService.title; textFormat: Text.PlainText
                                        width: Math.min(implicitWidth, keyRequestLine.width)
                                        height: contentHeight; wrapMode: Text.Wrap
                                        font.pixelSize: 14; color: root.colors.accent
                                        lineHeightMode: Text.FixedHeight; lineHeight: 20
                                    }
                                    InlineKeyRequestAction {
                                        id: grantRequestKey
                                        transform: Translate {
                                            readonly property point offset: keyRequestLine.centeredOffset(grantRequestKey)
                                            x: offset.x; y: offset.y
                                        }
                                        objectName: "grantChatKeyButton-" + messageRow.modelData.id
                                        visible: !Boolean(keyRequestService.request.ownCurrentDevice) && Boolean(keyRequestService.request.offerGrant)
                                        enabled: keyRequestService.responseEnabled("grant")
                                        text: qsTr("Send key")
                                        onClicked: keyRequestService.respond("grant")
                                    }
                                    InlineKeyRequestAction {
                                        id: rejectRequestKey
                                        transform: Translate {
                                            readonly property point offset: keyRequestLine.centeredOffset(rejectRequestKey)
                                            x: offset.x; y: offset.y
                                        }
                                        objectName: "rejectChatKeyButton-" + messageRow.modelData.id
                                        visible: !Boolean(keyRequestService.request.ownCurrentDevice) && Boolean(keyRequestService.request.offerReject)
                                        enabled: keyRequestService.responseEnabled("reject")
                                        text: qsTr("Reject")
                                        onClicked: keyRequestService.respond("reject")
                                    }
                                }
                                Text {
                                    visible: text.length > 0
                                    text: keyRequestService.description; textFormat: Text.PlainText
                                    width: parent.width; height: contentHeight; wrapMode: Text.Wrap
                                    horizontalAlignment: Text.AlignHCenter
                                    font.pixelSize: 14; color: root.colors.accent
                                    lineHeightMode: Text.FixedHeight; lineHeight: 20
                                }
                                Text {
                                    visible: text.length > 0
                                    text: keyRequestService.status; textFormat: Text.PlainText
                                    width: parent.width; height: contentHeight; wrapMode: Text.Wrap
                                    horizontalAlignment: Text.AlignHCenter
                                    font.pixelSize: 14; color: root.colors.accent
                                    lineHeightMode: Text.FixedHeight; lineHeight: 20
                                    Accessible.role: Accessible.StatusBar
                                }
                            }
                            Column {
                                id: keyServiceResponse
                                objectName: "keyServiceResponse-" + messageRow.modelData.id
                                visible: messageRow.keyResponse
                                readonly property var response: messageRow.modelData.keyServiceResponse || ({})
                                readonly property bool grant: messageRow.modelData.kind === "key_grant"
                                readonly property bool recipientKnown: Boolean(response.recipientKnown) && String(response.recipientId || "").length > 0
                                readonly property bool ownCurrentDevice: recipientKnown && Boolean(response.ownCurrentDevice)
                                readonly property string title: root.keyServiceResponseTitle(messageRow.modelData, response)
                                readonly property string description: recipientKnown ? "" : qsTr("Recipient information is unavailable.")
                                readonly property bool offerRequest: !grant && ownCurrentDevice && Boolean(response.offerRequest) && String(response.keyUuid || "").length > 0
                                readonly property string status: grant && ownCurrentDevice && Boolean(response.hasKey) ? qsTr("The key is available on this device") : ""
                                readonly property string accessibleSummary: [title, description, status].filter(part => part.length > 0).join(" ")
                                width: parent.width; spacing: 4
                                RowLayout {
                                    id: responseTitleLine
                                    width: Math.min(parent.width, Math.ceil(responseTitle.implicitWidth) + 28)
                                    x: (parent.width - width) / 2
                                    height: implicitHeight; spacing: 4
                                    NativeIconButton {
                                        objectName: "keyServiceResponseIcon-" + messageRow.modelData.id
                                        colors: root.colors; asset: keyServiceResponse.grant ? "key-granted.svg" : "key-rejected.svg"
                                        glyphSize: 24; Layout.preferredWidth: 24; Layout.preferredHeight: 24
                                        Layout.minimumWidth: 24; Layout.maximumWidth: 24
                                        Layout.minimumHeight: 24; Layout.maximumHeight: 24
                                        Layout.alignment: Qt.AlignVCenter
                                        padding: 0; enabled: false; opacity: 1; icon.color: root.colors.accent
                                        Accessible.ignored: true
                                    }
                                    Text {
                                        id: responseTitle
                                        objectName: "keyServiceResponseText-" + messageRow.modelData.id
                                        Layout.fillWidth: true; Layout.minimumWidth: 0
                                        Layout.alignment: Qt.AlignVCenter
                                        text: keyServiceResponse.title + (keyServiceResponse.description.length ? " " + keyServiceResponse.description : "")
                                        textFormat: Text.PlainText; wrapMode: Text.Wrap
                                        horizontalAlignment: Text.AlignHCenter
                                        font.pixelSize: 14; color: root.colors.accent
                                        lineHeightMode: Text.FixedHeight; lineHeight: 20
                                    }
                                }
                                GridLayout {
                                    id: responseRequestLine
                                    visible: keyServiceResponse.offerRequest
                                    readonly property real naturalWidth: responseRetryText.implicitWidth + 4 + responseRequestButton.implicitWidth
                                    readonly property bool inlineActions: naturalWidth <= parent.width
                                    width: inlineActions ? naturalWidth : parent.width
                                    x: (parent.width - width) / 2
                                    height: implicitHeight
                                    columns: inlineActions ? 2 : 1; columnSpacing: 4; rowSpacing: 4
                                    Text {
                                        id: responseRetryText
                                        Layout.preferredWidth: Math.min(implicitWidth, responseRequestLine.width)
                                        Layout.alignment: Qt.AlignHCenter | Qt.AlignVCenter
                                        text: qsTr("You can request the key again"); textFormat: Text.PlainText
                                        horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap
                                        font.pixelSize: 14; color: root.colors.accent
                                        lineHeightMode: Text.FixedHeight; lineHeight: 20
                                    }
                                    InlineKeyRequestAction {
                                        id: responseRequestButton
                                        objectName: "requestRejectedChatKeyButton-" + messageRow.modelData.id
                                        Layout.alignment: Qt.AlignHCenter | Qt.AlignVCenter
                                        enabled: Boolean(keyServiceResponse.response.canRequest) && !Boolean(root.chatEncryption.busy)
                                        text: qsTr("Request")
                                        onClicked: {
                                            const response = keyServiceResponse.response;
                                            const key = String(response.keyUuid || "");
                                            if (!keyServiceResponse.grant && keyServiceResponse.recipientKnown && keyServiceResponse.ownCurrentDevice
                                                    && Boolean(response.offerRequest) && Boolean(response.canRequest) && key.length)
                                                root.controller.requestChatKey(key);
                                        }
                                    }
                                }
                                Text {
                                    visible: text.length > 0
                                    text: keyServiceResponse.status; textFormat: Text.PlainText
                                    width: parent.width; height: contentHeight; wrapMode: Text.Wrap
                                    horizontalAlignment: Text.AlignHCenter
                                    font.pixelSize: 14; color: root.colors.accent
                                    lineHeightMode: Text.FixedHeight; lineHeight: 20
                                    Accessible.role: Accessible.StatusBar
                                }
                            }
                            Column {
                                id: keyAnnouncement
                                objectName: "keyAnnouncementService-" + messageRow.modelData.id
                                visible: messageRow.modelData.kind === "key_announced"
                                readonly property var announcement: messageRow.modelData.keyServiceAnnouncement || ({})
                                width: parent.width; spacing: 4
                                Flow {
                                    id: announcementHeading
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    width: Math.min(parent.width, announcementTitle.implicitWidth + 28)
                                    height: implicitHeight
                                    spacing: 4
                                    function lineOffset(item) {
                                        const parts = [announcementIcon, announcementTitle].filter(part => part.visible && Math.abs(part.y - item.y) < 0.5);
                                        let left = Infinity, right = -Infinity, maxHeight = 0;
                                        for (const part of parts) {
                                            left = Math.min(left, part.x); right = Math.max(right, part.x + part.width);
                                            maxHeight = Math.max(maxHeight, part.height);
                                        }
                                        return { x: (width - (right - left)) / 2 - left, y: (maxHeight - item.height) / 2 };
                                    }
                                    NativeIconButton {
                                        id: announcementIcon
                                        objectName: "keyAnnouncementIcon-" + messageRow.modelData.id
                                        colors: root.colors; asset: "warning.svg"
                                        width: 24; height: 24
                                        glyphSize: 24; padding: 0; horizontalPadding: 0
                                        enabled: false; opacity: 1; icon.color: root.colors.accent
                                        Accessible.ignored: true
                                        transform: Translate {
                                            x: announcementHeading.lineOffset(announcementIcon).x
                                            y: announcementHeading.lineOffset(announcementIcon).y
                                        }
                                    }
                                    Text {
                                        id: announcementTitle
                                        objectName: "keyAnnouncementTitle-" + messageRow.modelData.id
                                        width: Math.min(implicitWidth, announcementHeading.width)
                                        height: contentHeight
                                        text: qsTr("The security key was updated"); textFormat: Text.PlainText
                                        wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter
                                        font.pixelSize: 14; color: root.colors.accent
                                        lineHeightMode: Text.FixedHeight; lineHeight: 20
                                        transform: Translate {
                                            x: announcementHeading.lineOffset(announcementTitle).x
                                            y: announcementHeading.lineOffset(announcementTitle).y
                                        }
                                    }
                                }
                                Flow {
                                    id: announcementDescription
                                    visible: Boolean(keyAnnouncement.announcement.offerRequest)
                                    readonly property real naturalWidth: announcementDescriptionLabel.implicitWidth + announcementRequest.implicitWidth + 4
                                    width: Math.min(parent.width, naturalWidth)
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    height: implicitHeight; spacing: 4
                                    function lineOffset(item) {
                                        // Same bounded centering rule as incoming R:
                                        // Flow's untransformed packing is authoritative.
                                        const parts = [announcementDescriptionLabel, announcementRequest].filter(part => part.visible && Math.abs(part.y - item.y) < 0.5);
                                        let left = Infinity, right = -Infinity, maxHeight = 0;
                                        for (const part of parts) {
                                            left = Math.min(left, part.x); right = Math.max(right, part.x + part.width);
                                            maxHeight = Math.max(maxHeight, part.height);
                                        }
                                        return { x: (width - (right - left)) / 2 - left, y: (maxHeight - item.height) / 2 };
                                    }
                                    Text {
                                        id: announcementDescriptionLabel
                                        text: qsTr("Request the key to read the messages"); textFormat: Text.PlainText
                                        width: Math.min(implicitWidth, announcementDescription.width)
                                        height: contentHeight; wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter
                                        font.pixelSize: 14; color: root.colors.accent
                                        lineHeightMode: Text.FixedHeight; lineHeight: 20
                                        transform: Translate {
                                            x: announcementDescription.lineOffset(announcementDescriptionLabel).x
                                            y: announcementDescription.lineOffset(announcementDescriptionLabel).y
                                        }
                                    }
                                    InlineKeyRequestAction {
                                        id: announcementRequest
                                        objectName: "requestAnnouncedChatKeyButton-" + messageRow.modelData.id
                                        text: qsTr("Request key")
                                        enabled: Boolean(keyAnnouncement.announcement.canRequest) && !Boolean(root.chatEncryption.busy)
                                        transform: Translate {
                                            x: announcementDescription.lineOffset(announcementRequest).x
                                            y: announcementDescription.lineOffset(announcementRequest).y
                                        }
                                        onClicked: {
                                            const facts = keyAnnouncement.announcement;
                                            const key = String(facts.keyUuid || "");
                                            if (Boolean(facts.offerRequest) && Boolean(facts.canRequest) && key.length && !Boolean(root.chatEncryption.busy))
                                                root.controller.requestChatKey(key);
                                        }
                                    }
                                }
                            }
                            TextEdit {
                                id: messageBody
                                visible: !messageRow.hasInlineImages && !messageRow.encryptedLocked && messageRow.incomingKeyRequest === null && !messageRow.keyResponse && messageRow.modelData.kind !== "key_announced"
                                    && !messageRow.isCallCard
                                horizontalAlignment: messageRow.keyControl ? Text.AlignHCenter : Text.AlignLeft
                                objectName: messageRow.hasInlineImages ? "" : "message-content-" + messageRow.modelData.id
                                width: parent.width
                                text: messageRow.keyControl ? messageRow.controlLabel
                                    // The reference body keeps a fixed 20 px line box
                                    // (Text.lineHeightMode: FixedHeight). TextEdit has no
                                    // lineHeight property, so the box travels with the
                                    // rich-text document as an inherited CSS line-height.
                                    : "<div style=\"line-height:20px\">" + messageRow.displayHtml + "</div>"
                                textFormat: messageRow.keyControl ? TextEdit.PlainText : TextEdit.RichText
                                // Issue #218: a read-only selectable body mirrors the
                                // reference DOM selection (workspace-message-body
                                // -interactions.hook.ts:97-116); centered control
                                // labels stay non-selectable.
                                readOnly: true
                                selectByMouse: !messageRow.keyControl
                                cursorVisible: false
                                persistentSelection: true
                                // Issue #218 review r2: a selection becoming
                                // active here clears the other bubbles'
                                // persistent selections (noteBodySelection).
                                onSelectedTextChanged: root.noteBodySelection(messageRow.modelData.id, messageBody)
                                Component.onDestruction: root.forgetBodySelection(messageRow.modelData.id)
                                bottomPadding: messageRow.inlineMeta ? 2 : 0
                                height: contentHeight + topPadding + bottomPadding
                                Accessible.ignored: true
                                wrapMode: TextEdit.Wrap
                                font.pixelSize: 14
                                color: messageRow.keyControl ? root.colors.accent : root.colors["text-primary"]
                                palette.link: root.colors.accent
                                onLinkActivated: link => root.openReference(link)
                                // Test seam (issue #109 review F6): counts the
                                // linkAt samples the mention locator performs
                                // for this body, so window cases can assert
                                // the locator's work stays bounded.
                                property int linkAtProbes: 0
                                // Issue #109: transparent per-mention hit
                                // targets aligned with the rich-text anchors
                                // (see docs/NATIVE_DESKTOP_MENTION_CHIPS.md).
                                // Children of the body item keep the bubble
                                // Column layout untouched.
                                Item {
                                    id: bodyMentionOverlay
                                    objectName: messageRow.hasInlineImages
                                        ? "" : "message-mentions-" + String(messageRow.modelData.id)
                                    visible: !messageRow.hasInlineImages && messageRow.mentionTargets.length > 0
                                    width: 0
                                    height: 0
                                    z: 2
                                    Repeater {
                                        model: messageRow.mentionTargets
                                        delegate: NativeMentionChip {}
                                    }
                                }
                            }
                            Column {
                                id: mediaMessageBody
                                objectName: messageRow.hasInlineImages ? "message-content-" + messageRow.modelData.id : ""
                                visible: messageRow.hasInlineImages
                                width: parent.width
                                // A standalone Markdown image has an explicit
                                // blank paragraph boundary (Source: 0.7 * 20px).
                                spacing: 14
                                readonly property string text: messageRow.displayHtml
                                readonly property real contentHeight: implicitHeight
                                readonly property real imagePreferredWidth: {
                                    let width = 0;
                                    for (let i = 0; i < imageRuns.count; ++i) {
                                        const run = imageRuns.itemAt(i);
                                        if (run)
                                            width = Math.max(width, run.imageNaturalWidth);
                                    }
                                    return width;
                                }
                                Repeater {
                                    id: imageRuns
                                    model: messageRow.hasInlineImages ? messageRow.imageParts : []
                                    delegate: Item {
                                        id: imageRun
                                        required property var modelData
                                        required property int index
                                        readonly property var part: modelData
                                        readonly property Item textPart: textRun
                                        // Issue #110 review r3 (#460/F3): the text
                                        // run's laid-out document. The reference
                                        // resolves right-clicked anchors and code
                                        // block offsets against the rendered part
                                        // itself; this hidden TextEdit mirrors the
                                        // visible Text layout (same width, wrap and
                                        // fixed 20 px leading on every styled block)
                                        // so linkAt/positionToRectangle apply to it.
                                        readonly property var layoutProbe: part.kind === "text" ? mediaLayoutProbe : null
                                        readonly property string layoutHtml: part.kind === "text"
                                            ? messageRow.imagePartHtml(part, index).replace(/ style="/g,
                                                ' style="line-height:20px;-qt-line-height-type:fixed;') : ""
                                        readonly property real imageNaturalWidth: part.kind === "image" ? picture.implicitWidth + 2 : 0
                                        readonly property real imageWrapperHeight: Math.min(180, picture.height + 10)
                                        width: mediaMessageBody.width
                                        height: part.kind === "image" ? imageWrapperHeight + 8 : textRun.contentHeight + textRun.bottomPadding
                                        TextEdit {
                                            id: textRun
                                            objectName: "mediaTextRun-" + messageRow.modelData.id + "-" + index
                                            visible: imageRun.part.kind === "text"
                                            width: parent.width
                                            height: contentHeight + bottomPadding
                                            text: messageRow.imagePartHtml(imageRun.part, imageRun.index)
                                            textFormat: TextEdit.RichText
                                            readOnly: true
                                            cursorVisible: false
                                            wrapMode: TextEdit.Wrap
                                            font.pixelSize: 14
                                            bottomPadding: messageRow.inlineMeta && imageRun.index === messageRow.imageParts.length - 1 ? 2 : 0
                                            palette.link: root.colors.accent
                                            onLinkActivated: link => root.openReference(link)
                                            Accessible.ignored: true
                                        }
                                        TextEdit {
                                            id: mediaLayoutProbe
                                            objectName: "mediaLayoutProbe-" + messageRow.modelData.id + "-" + imageRun.index
                                            visible: false
                                            readOnly: true
                                            x: 0
                                            y: 0
                                            width: textRun.width
                                            text: imageRun.layoutHtml
                                            textFormat: TextEdit.RichText
                                            wrapMode: TextEdit.Wrap
                                            font.pixelSize: 14
                                            color: root.colors["text-primary"]
                                            Accessible.ignored: true
                                        }
                                        NativeMessageImage {
                                            id: picture
                                            objectName: "inline-image-" + messageRow.modelData.id
                                            visible: imageRun.part.kind === "image"
                                            colors: root.colors
                                            images: root.messageImages
                                            viewport: messageList
                                            reference: String(imageRun.part.reference || "")
                                            label: String(imageRun.part.label || "")
                                            hintWidth: Number(imageRun.part.width || 0)
                                            hintHeight: Number(imageRun.part.height || 0)
                                            // Source keeps a transparent 1px
                                            // border and two 4px margin pairs.
                                            x: 1; y: 4 + (imageRun.imageWrapperHeight - height) / 2
                                            width: Math.min(Math.max(0, parent.width - 2), implicitWidth)
                                            bodyActive: mediaMessageBody.visible && !messageList.positioning
                                                && messageList.displayedRoute === messageList.serviceScope()
                                                && (!root.controller.messageHistoryReady || messageList.openingApplied)
                                            onOpened: root.openReference(reference, picture.failed)
                                        }
                                    }
                                }
                                Item {
                                    id: mediaMentionOverlay
                                    objectName: messageRow.hasInlineImages
                                        ? "message-mentions-" + String(messageRow.modelData.id) : ""
                                    anchors.left: parent.left
                                    anchors.top: parent.top
                                    visible: messageRow.hasInlineImages && messageRow.mentionTargets.length > 0
                                    width: 0
                                    height: 0
                                    z: 2
                                    Repeater {
                                        model: messageRow.mentionTargets
                                        delegate: NativeMentionChip {}
                                    }
                                }
                            }
                            Item {
                                id: messageFooter
                                visible: !messageRow.keyControl && (messageRow.isCallCard
                                    ? reactionFlow.implicitHeight > 0 : !messageRow.inlineMeta && !messageRow.lockedInlineMeta)
                                width: parent.width
                                height: Math.max(reactionFlow.implicitHeight, messageTime.height)
                                Flow {
                                    id: reactionFlow
                                    width: Math.max(0, parent.width - messageTime.implicitWidth - 8)
                                    y: parent.height - implicitHeight
                                    spacing: 4
                                    Repeater {
                                        model: (messageRow.modelData.reactions || []).slice().sort((a, b) => String(a.emojiName).localeCompare(String(b.emojiName)))
                                        delegate: CheckBox {
                                            id: reactionChip
                                            required property var modelData
                                            readonly property bool owned: Boolean(modelData.ownKnown) && Boolean(modelData.own)
                                            readonly property bool known: String(modelData.unicode).length > 0
                                            readonly property var participants: modelData.participants || []
                                            readonly property bool showParticipants: Number(modelData.count) > 0 && participants.length === Number(modelData.count)
                                            readonly property real participantWidth: participants.length > 0 ? 20 + (participants.length - 1) * 16 : 0
                                            readonly property string label: (known ? String(modelData.unicode) : String(modelData.emojiName)) + " " + (showParticipants ? participants.map(person => String(person.name)).join(", ") : String(modelData.count))
                                            readonly property color accentColor: root.colors.accent
                                            objectName: "reaction-" + messageRow.modelData.id + "-" + modelData.emojiName
                                            visible: Number(modelData.count) > 0 || Boolean(modelData.pending)
                                            width: Math.min(reactionFlow.width, messageRow.reactionPreferredWidth(modelData))
                                            height: 26
                                            leftPadding: 8
                                            rightPadding: 8
                                            topPadding: 2
                                            bottomPadding: 2
                                            indicator: null
                                            hoverEnabled: true
                                            checked: owned
                                            nextCheckState: () => owned ? Qt.Checked : Qt.Unchecked
                                            opacity: enabled || modelData.pending ? 1 : 0.5
                                            enabled: Boolean(messageRow.modelData.canReact) && !Boolean(messageRow.modelData.reactionBusy) && (known || owned || !Boolean(modelData.ownKnown))
                                            Accessible.name: label
                                            Accessible.description: !modelData.ownKnown ? qsTr("Check your reaction ownership") : owned ? qsTr("Remove your reaction") : known ? qsTr("Add your reaction") : qsTr("This provider reaction cannot be added")
                                            Accessible.role: Accessible.Button
                                            Accessible.checkable: true
                                            Accessible.checked: owned
                                            ToolTip {
                                                id: reactionTooltip
                                                objectName: "reactionTooltip-" + messageRow.modelData.id + "-" + reactionChip.modelData.emojiName
                                                visible: reactionChip.hovered
                                                text: reactionChip.label
                                                contentItem: Text {
                                                    objectName: "reactionTooltipText-" + messageRow.modelData.id + "-" + reactionChip.modelData.emojiName
                                                    text: reactionTooltip.text
                                                    textFormat: Text.PlainText
                                                    font: reactionTooltip.font
                                                    color: reactionTooltip.palette.toolTipText
                                                    wrapMode: Text.Wrap
                                                }
                                            }
                                            background: Rectangle {
                                                radius: 8
                                                color: reactionChip.owned ? Qt.rgba(reactionChip.accentColor.r, reactionChip.accentColor.g, reactionChip.accentColor.b, reactionChip.hovered ? 0.25 : 0.15) : root.colors[reactionChip.hovered ? "card-bg-active" : "card-bg"]
                                                border.width: reactionChip.visualFocus ? 2 : 1
                                                border.color: reactionChip.visualFocus ? reactionChip.accentColor : reactionChip.owned ? Qt.rgba(reactionChip.accentColor.r, reactionChip.accentColor.g, reactionChip.accentColor.b, reactionChip.hovered ? 0.5 : 0.4) : root.colors["border-subtle"]
                                            }
                                            contentItem: RowLayout {
                                                spacing: 4
                                                Text {
                                                    id: reactionGlyph
                                                    objectName: "reaction-label-" + messageRow.modelData.id + "-" + reactionChip.modelData.emojiName
                                                    text: reactionChip.known ? reactionChip.modelData.unicode : reactionChip.modelData.emojiName
                                                    textFormat: Text.PlainText
                                                    Layout.preferredWidth: reactionChip.known ? 20 : Math.min(160, implicitWidth)
                                                    Layout.maximumWidth: Math.max(12, reactionChip.availableWidth - (reactionChip.showParticipants ? Math.min(20, reactionChip.participantWidth) : reactionCount.implicitWidth) - 4)
                                                    Layout.preferredHeight: 20
                                                    verticalAlignment: Text.AlignVCenter
                                                    horizontalAlignment: reactionChip.known ? Text.AlignHCenter : Text.AlignLeft
                                                    elide: Text.ElideRight
                                                    color: root.colors["text-primary"]
                                                    font.pixelSize: reactionChip.known ? 16 : 14
                                                }
                                                Text {
                                                    id: reactionCount
                                                    objectName: "reaction-count-" + messageRow.modelData.id + "-" + reactionChip.modelData.emojiName
                                                    visible: !reactionChip.showParticipants
                                                    text: String(reactionChip.modelData.count)
                                                    textFormat: Text.PlainText
                                                    font.pixelSize: 11
                                                    color: root.colors["text-muted"]
                                                }
                                                Item {
                                                    id: participantStrip
                                                    visible: reactionChip.showParticipants
                                                    Layout.preferredWidth: Math.min(reactionChip.participantWidth, Math.max(0, reactionChip.availableWidth - reactionGlyph.width - 4))
                                                    Layout.preferredHeight: 20
                                                    clip: true
                                                    Repeater {
                                                        model: reactionChip.showParticipants ? reactionChip.participants.slice(0, Math.max(0, Math.ceil(participantStrip.width / 16))) : []
                                                        delegate: Rectangle {
                                                            id: participantAvatar
                                                            required property var modelData
                                                            required property int index
                                                            objectName: "reaction-participant-" + messageRow.modelData.id + "-" + reactionChip.modelData.emojiName + "-" + modelData.id
                                                            x: index * 16
                                                            width: 20
                                                            height: 20
                                                            radius: 10
                                                            color: root.colors.bg
                                                            border.color: root.colors["border-subtle"]
                                                            Text {
                                                                objectName: "reaction-participant-name-" + messageRow.modelData.id + "-" + reactionChip.modelData.emojiName + "-" + participantAvatar.modelData.id
                                                                anchors.centerIn: parent
                                                                text: String(participantAvatar.modelData.name).slice(0, 1)
                                                                textFormat: Text.PlainText
                                                                font.pixelSize: 9
                                                                font.weight: Font.DemiBold
                                                                color: root.colors["text-primary"]
                                                            }
                                                            HoverHandler {
                                                                id: participantHover
                                                            }
                                                            ToolTip {
                                                                id: participantTooltip
                                                                visible: participantHover.hovered
                                                                text: String(participantAvatar.modelData.name)
                                                                contentItem: Text {
                                                                    text: participantTooltip.text
                                                                    textFormat: Text.PlainText
                                                                    font: participantTooltip.font
                                                                    color: participantTooltip.palette.toolTipText
                                                                    wrapMode: Text.Wrap
                                                                }
                                                            }
                                                            Accessible.ignored: true
                                                        }
                                                    }
                                                }
                                            }
                                            onClicked: root.controller.toggleReaction(messageRow.modelData.id, modelData.emojiName)
                                        }
                                    }
                                }
                            }
                            Column {
                                width: parent.width
                                visible: Boolean(messageRow.modelData.reactionBusy)
                                spacing: 4
                                Text {
                                    width: parent.width
                                    text: messageRow.modelData.reactionUncertain ? qsTr("Reaction not confirmed") : qsTr("Updating reaction…")
                                    textFormat: Text.PlainText
                                    wrapMode: Text.Wrap
                                    color: root.colors["text-muted"]
                                    font.pixelSize: 11
                                    Accessible.role: Accessible.StatusBar
                                }
                                NativeButton {
                                    objectName: "check-reaction-" + messageRow.modelData.id
                                    colors: root.colors
                                    visible: Boolean(messageRow.modelData.reactionUncertain)
                                    enabled: Boolean(messageRow.modelData.canReact)
                                    text: qsTr("Check status")
                                    ToolTip.visible: hovered
                                    ToolTip.text: qsTr("Check without sending the reaction again")
                                    onClicked: root.checkReaction(messageRow.modelData.id)
                                }
                            }
                        }
                        Button {
                            id: quickReply
                            objectName: "quickReply-" + messageRow.modelData.id
                            x: messageRow.own ? 12 : parent.width - width - 12
                            y: -12
                            z: 3
                            width: implicitWidth
                            height: 24
                            horizontalPadding: 10
                            verticalPadding: 0
                            text: qsTr("Reply")
                            Accessible.name: text
                            hoverEnabled: true
                            visible: root.composerAvailable && !root.controller.topicClosed && !root.encryptedConversation
                                && messageRow.modelData.kind === "markdown" && !root.controller.cacheStale
                                && root.messageSelectionCount === 0 && !root.aggregateConversation
                                && (messageHover.hovered || bubble.activeFocus || visualFocus || hovered
                                    || quickReactions.hasFocus || messageBody.activeFocus)
                            enabled: visible && !root.controller.busy
                            KeyNavigation.backtab: bubble
                            font.pixelSize: 12
                            contentItem: Text {
                                text: quickReply.text
                                font: quickReply.font
                                color: root.colors["text-primary"]
                                textFormat: Text.PlainText
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                radius: height / 2
                                color: quickReply.hovered ? root.colors["card-bg-active"] : root.colors["card-bg"]
                                border.color: quickReply.visualFocus ? root.colors.accent : root.colors["border-subtle"]
                            }
                            onClicked: root.controller.addReplyTab(messageRow.modelData.id,
                                String(messageBody.selectedText || "").replace(/[\u2028\u2029]/g, "\n"))
                        }
                        NativeQuickReactions {
                            id: quickReactions
                            colors: root.colors
                            choices: root.quickReactionChoices
                            reactions: messageRow.modelData.reactions || []
                            messageId: messageRow.modelData.id
                            allowed: Boolean(messageRow.modelData.canReact) && !Boolean(messageRow.modelData.reactionBusy)
                            emojiFamily: fullReactionPicker.emojiFamily
                            emojiPixelSize: Qt.platform.os === "linux" && fullReactionPicker.emojiFamily === "Noto Color Emoji" ? 28 : 24
                            x: messageRow.own ? 0 : parent.width
                            y: parent.height + 4
                            visible: messageHover.hovered || bubble.activeFocus || hasFocus || expanded
                            onAboutToOpen: {
                                root.quickUsage = root.preferences.quickReactionUsage;
                                quickReactions.availableHeight = Math.max(40, quickReactions.mapToItem(messageList, 0, 0).y - 4);
                            }
                            onChosen: emojiName => root.chooseQuickReaction(messageRow.modelData.id, emojiName)
                        }
                        MouseArea {
                            anchors.fill: parent
                            acceptedButtons: Qt.RightButton
                            onClicked: mouse => {
                                // Issue #110: TextEdit.linkAt identifies the
                                // anchor under the right-click (reference
                                // target.closest("a[href]") resolver,
                                // workspace-message-body-interactions.hook.ts:266-280).
                                const bodyPoint = messageBody.mapFromItem(bubble, mouse.x, mouse.y);
                                let bodyLink = messageBody.visible && messageBody.enabled
                                    ? messageBody.linkAt(bodyPoint.x, bodyPoint.y) : "";
                                // Issue #110 review r3 (#460): a media-split body
                                // renders its text in the mediaMessageBody runs, so
                                // the hit-test travels each text run's laid-out
                                // probe document when the ordinary body is hidden.
                                if (!bodyLink.length && mediaMessageBody.visible) {
                                    for (let i = 0; i < imageRuns.count && !bodyLink.length; ++i) {
                                        const run = imageRuns.itemAt(i);
                                        const probe = run ? run.layoutProbe : null;
                                        if (!probe || !String(probe.text).length)
                                            continue;
                                        const probePoint = probe.mapFromItem(bubble, mouse.x, mouse.y);
                                        if (probePoint.x < 0 || probePoint.y < 0
                                            || probePoint.x > probe.width || probePoint.y > probe.height)
                                            continue;
                                        bodyLink = probe.linkAt(probePoint.x, probePoint.y);
                                    }
                                }
                                root.openMessageActions(messageRow.modelData, bubble, messageBody.selectedText, bodyLink);
                            }
                        }
                        // Issue #110 (docs/NATIVE_DESKTOP_BODY_COPY_AFFORDANCES.md):
                        // a copy button is mounted into every rendered code block
                        // (workspace-message-body-interactions.hook.ts:137-214).
                        // The label swaps to Copied / Failed to copy and resets
                        // after CODE_COPY_RESET_MS = 1200 ms (hook.ts:25).
                        // Review r3 (F4): the mounted control is a real accessible
                        // button: Accessible.Button role with a name, tab focus and
                        // Enter/Space activation through the same exported
                        // performCopy() path as the pointer. Only messages with
                        // fenced code gain one, so the pinned bubble focus order
                        // (reaction fixtures contain no fences) stays untouched.
                        // Review r3 (F3): media-split bodies carry their per-text-part
                        // code blocks too; a part's button lays out against that
                        // part's hidden text probe instead of the ordinary body.
                        readonly property var codeCopyTargets: {
                            const targets = [];
                            if (messageBody.visible) {
                                (messageRow.rendered.codeBlocks || []).forEach((block, nth) =>
                                    targets.push({start: block.start, text: block.text, partIndex: -1, nth: nth}));
                            } else if (messageRow.hasInlineImages) {
                                messageRow.imageParts.forEach((part, partIndex) => {
                                    if (part.kind !== "text") return;
                                    (part.codeBlocks || []).forEach((block, nth) =>
                                        targets.push({start: block.start, text: block.text, partIndex: partIndex, nth: nth}));
                                });
                            }
                            return targets;
                        }
                        Component {
                            id: codeCopyButtonTemplate
                            Rectangle {
                                id: codeCopyButton
                                required property var modelData
                                readonly property string codeText: String(modelData.text || "")
                                readonly property int mountPart: Number(modelData.partIndex)
                                readonly property var layoutTarget: mountPart < 0 ? messageBody
                                    : (imageRuns.count > mountPart ? imageRuns.itemAt(mountPart).layoutProbe : null)
                                property string copyState: "idle"
                                objectName: "codeCopyButton-" + messageRow.modelData.id
                                    + (mountPart < 0 ? "-" : "-p" + mountPart + "-") + Number(modelData.nth)
                                width: codeCopyLabel.implicitWidth + 16
                                height: 20
                                radius: 5
                                visible: codeText.trim().length > 0
                                activeFocusOnTab: true
                                Accessible.role: Accessible.Button
                                Accessible.name: codeCopyLabel.text
                                Accessible.onPressAction: codeCopyButton.performCopy()
                                x: bubbleContent.x + (mountPart < 0 ? messageBody.width : mediaMessageBody.width) - width - 2
                                // runShift is only a re-layout dependency: the
                                // image loads after delegate creation and shifts
                                // the media runs, so the y expression must be
                                // re-evaluated once the run settles (mapToItem
                                // already carries the full path to bubble).
                                readonly property real runShift: mountPart < 0 ? 0
                                    : (imageRuns.count > mountPart ? imageRuns.itemAt(mountPart).y : 0)
                                y: layoutTarget && layoutTarget.length > Number(modelData.start)
                                    ? layoutTarget.mapToItem(bubble, 0,
                                        layoutTarget.positionToRectangle(Number(modelData.start)).y).y + 2
                                        + runShift - runShift : 0
                                color: codeCopyMouse.containsMouse || codeCopyButton.activeFocus
                                    ? root.colors["bg-elevated"] : "transparent"
                                border.width: codeCopyMouse.containsMouse || codeCopyButton.activeFocus ? 1 : 0
                                border.color: root.colors["border-subtle"]
                                Keys.onPressed: event => {
                                    if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                                        codeCopyButton.performCopy();
                                        event.accepted = true;
                                    }
                                }
                                Text {
                                    id: codeCopyLabel
                                    objectName: "codeCopyButtonText-" + messageRow.modelData.id
                                        + (codeCopyButton.mountPart < 0 ? "-" : "-p" + codeCopyButton.mountPart + "-")
                                        + Number(codeCopyButton.modelData.nth)
                                    anchors.centerIn: parent
                                    font.pixelSize: 11
                                    color: root.colors["text-muted"]
                                    text: codeCopyButton.copyState === "copied" ? qsTr("Copied")
                                        : codeCopyButton.copyState === "failed" ? qsTr("Failed to copy") : qsTr("Copy")
                                }
                                // Exported so pointer, keyboard and accessibility
                                // activation share one code path (the reference
                                // mounts a real button).
                                function performCopy() {
                                    if (!codeText.trim().length)
                                        return;
                                    copyState = root.controller.copyToClipboard(codeText) ? "copied" : "failed";
                                    codeCopyReset.restart();
                                }
                                MouseArea {
                                    id: codeCopyMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: codeCopyButton.performCopy()
                                }
                                Timer {
                                    id: codeCopyReset
                                    interval: 1200
                                    onTriggered: codeCopyButton.copyState = "idle"
                                }
                            }
                        }
                        Repeater {
                            model: bubble.codeCopyTargets
                            delegate: codeCopyButtonTemplate
                        }
                        Keys.onPressed: event => {
                            if (event.key === Qt.Key_Menu || (event.key === Qt.Key_F10 && event.modifiers & Qt.ShiftModifier)) {
                                root.openMessageActions(messageRow.modelData, bubble, messageBody.selectedText, "");
                                event.accepted = true;
                            }
                        }
                        Accessible.role: Accessible.ListItem
                        Text {
                            id: messageTime
                            visible: !messageRow.keyControl
                            objectName: "message-time-" + messageRow.modelData.id
                            parent: messageRow.isCallCard ? callCard.metadataRow : bubble
                            x: parent.width - (messageRow.isCallCard ? 0 : 12) - implicitWidth
                            y: messageRow.isCallCard ? (parent.height - height) / 2
                                : messageRow.lockedInlineMeta ? bubble.height - 8 - height : bubbleContent.y + (messageRow.inlineMeta ? messageRow.hasInlineImages ? mediaMessageBody.y + mediaMessageBody.height : messageBody.y + messageBody.height : messageFooter.y + messageFooter.height) - height
                            height: 16
                            text: messageRow.modelData.time
                            textFormat: Text.PlainText
                            font.pixelSize: 12
                            lineHeightMode: Text.FixedHeight
                            lineHeight: 16
                            color: root.colors["text-muted"]
                            // #108: the time chip is the trigger of the read-only
                            // timestamp viewer (reference hook :566-572 click and
                            // :490-496 Enter/Space on the focused [datetime] element).
                            activeFocusOnTab: true
                            Accessible.role: Accessible.Button
                            Accessible.name: text
                            function openViewer() {
                                root.openMessageTimeView(messageRow.modelData.createdAt, messageTime)
                            }
                            Accessible.onPressAction: openViewer()
                            MouseArea {
                                anchors.fill: parent
                                anchors.margins: -2
                                acceptedButtons: Qt.LeftButton
                                cursorShape: Qt.PointingHandCursor
                                onClicked: messageTime.openViewer()
                            }
                            Keys.onPressed: event => {
                                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                                    event.accepted = true
                                    messageTime.openViewer()
                                }
                            }
                        }
                        Accessible.name: messageRow.modelData.kind === "key_announced" ? announcementTitle.text : messageRow.keyResponse ? keyServiceResponse.accessibleSummary : messageRow.incomingKeyRequest !== null ? keyRequestService.accessibleSummary : messageRow.encryptedLocked ? root.encryptedPreparationState(messageRow.modelData) === "error" ? qsTr("Could not decrypt message") : root.encryptedPreparationState(messageRow.modelData) === "preparing" ? qsTr("Decrypting message") : qsTr("Key required") : String(messageRow.modelData.author) + ": " + String(messageRow.rendered.plain)
                    }
                }
            }
            NativeButton {
                colors: root.colors
                visible: root.controller.canLoadNewer
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("Load newer messages")
                enabled: !root.controller.busy
                onClicked: root.controller.loadNewer()
            }
            Rectangle {
                id: outgoingStatus
                objectName: "pendingOutgoingStatus"
                visible: !root.encryptedConversation && (String(root.pendingOutgoing.id || "").length > 0 || String(root.pendingOutgoing.error || "").length > 0)
                Layout.fillWidth: true
                Layout.preferredHeight: outgoingStatusContent.implicitHeight + 20
                color: root.colors["composer-outer"]
                ColumnLayout {
                    id: outgoingStatusContent
                    anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                    anchors.margins: 10
                    Text {
                        Layout.fillWidth: true
                        text: root.pendingOutgoing.busy ? qsTr("Checking pending message…") : String(root.pendingOutgoing.error || qsTr("This message is waiting for confirmation."))
                        color: root.colors["text-muted"]
                        wrapMode: Text.Wrap; textFormat: Text.PlainText
                        Accessible.role: Accessible.StaticText
                        Accessible.name: text
                    }
                    RowLayout {
                        NativeButton {
                            objectName: "checkPendingOutgoingButton"; colors: root.colors
                            text: qsTr("Check status"); enabled: Boolean(root.pendingOutgoing.canCheck)
                            onClicked: root.controller.checkPendingSend()
                        }
                        NativeButton {
                            objectName: "retryPendingOutgoingButton"; colors: root.colors
                            text: qsTr("Retry"); enabled: Boolean(root.pendingOutgoing.canRetry)
                            onClicked: root.controller.retryPendingSend()
                        }
                        NativeButton {
                            objectName: "discardPendingOutgoingButton"; colors: root.colors
                            text: qsTr("Discard"); enabled: Boolean(root.pendingOutgoing.canDiscard)
                            onClicked: outgoingDiscard.openForPending()
                        }
                    }
                }
            }
            Rectangle {
                id: encryptionStatus
                objectName: "encryptedReadStatus"
                readonly property bool inlineKeyNotice: root.encryptedKeyNoticeVisible && root.chatEncryption.available
                    && root.chatEncryption.state !== "unresolved" && String(root.chatEncryption.error || "").length === 0
                    && String(root.encryptedPendingSend.id || "").length === 0
                readonly property real keyNoticeActionsWidth: (requestCurrentKeyControl.visible ? requestCurrentKeyControl.implicitWidth + 8 : 0) + 24
                visible: root.encryptedConversation && (!root.encryptedComposerHasKey || root.encryptedKeyNoticeVisible || String(root.encryptedPendingSend.id || "").length > 0)
                Layout.fillWidth: true
                Layout.preferredHeight: encryptionStatusContent.implicitHeight + (inlineKeyNotice ? 12 : root.encryptedComposerHasKey ? 40 : 41)
                color: inlineKeyNotice ? root.colors["composer-outer"] : "transparent"
                radius: inlineKeyNotice ? 12 : 0
                clip: inlineKeyNotice
                // The source shell rounds only the top and joins the same surface below.
                Rectangle {
                    visible: encryptionStatus.inlineKeyNotice
                    anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                    height: Math.min(12, parent.height); color: parent.color
                }
                Rectangle { visible: !encryptionStatus.inlineKeyNotice; width: parent.width; height: 1; color: root.colors["border-subtle"] }
                GridLayout {
                    id: encryptionStatusContent
                    readonly property real availableWidth: parent.width - (encryptionStatus.inlineKeyNotice ? 24 : 32)
                    readonly property bool controlsInline: encryptionStatus.inlineKeyNotice
                        && encryptedReadStatusText.implicitWidth + 20 + encryptionStatus.keyNoticeActionsWidth <= availableWidth
                    x: encryptionStatus.inlineKeyNotice ? 12 : 16
                    y: encryptionStatus.inlineKeyNotice ? 12 : root.encryptedComposerHasKey ? 20 : 21
                    width: controlsInline ? encryptedReadStatusText.implicitWidth + 20 + encryptionStatus.keyNoticeActionsWidth : availableWidth
                    columns: controlsInline ? 2 : 1
                    columnSpacing: 20
                    rowSpacing: encryptionStatus.inlineKeyNotice ? 8 : root.encryptedComposerHasKey ? 8 : 12
                    Text {
                        id: encryptedReadStatusText
                        objectName: "encryptedReadStatusText"
                        visible: text.length > 0
                        Layout.row: 0; Layout.column: 0
                        Layout.preferredWidth: encryptionStatus.inlineKeyNotice ? Math.min(implicitWidth, encryptionStatusContent.width) : encryptionStatusContent.width
                        Layout.maximumWidth: encryptionStatusContent.width
                        Layout.fillWidth: !encryptionStatus.inlineKeyNotice
                        Layout.alignment: Qt.AlignLeft | (encryptionStatus.inlineKeyNotice ? Qt.AlignVCenter : Qt.AlignTop)
                        wrapMode: Text.Wrap; textFormat: Text.PlainText
                        font.pixelSize: 14; color: root.colors[encryptionStatus.inlineKeyNotice ? "accent" : "text-muted"]
                        lineHeightMode: Text.FixedHeight; lineHeight: 20
                        Accessible.role: Accessible.StatusBar
                        text: String(root.encryptedPendingSend.id || "").length > 0 ? (root.encryptedPendingSend.busy ? qsTr("Loading…") : String(root.encryptedPendingSend.error || ""))
                            : root.encryptedKeyNoticeVisible ? qsTr("The security key has changed. Request the key from the channel owner to read new messages")
                            : root.chatEncryption.storeOpening ? qsTr("Loading…")
                            : !root.chatEncryption.available ? qsTr("Encryption is unavailable for this chat or local key storage could not be opened.")
                            : root.chatEncryption.busy ? qsTr("Loading…")
                            : root.chatEncryption.state === "requested" ? qsTr("Waiting for a response")
                            : root.chatEncryption.state === "rejected" ? qsTr("The key request was rejected. You can send another request.")
                            : root.encryptedReadSummary.missing > 0 ? qsTr("Request the key for this generation to read the message.")
                            : root.encryptedReadSummary.error > 0 ? qsTr("Could not decrypt message")
                            : root.encryptedReadSummary.preparing > 0 ? qsTr("Decrypting message")
                            : root.encryptedReadSummary.currentReady ? qsTr("The key is available on this device")
                            : qsTr("Request the key for this generation to read the message.")
                    }
                    Text {
                        id: encryptionStatusError
                        objectName: "encryptedReadError"
                        visible: String(root.chatEncryption.error || "").length > 0
                        Layout.row: encryptedReadStatusText.text.length > 0 ? 1 : 0
                        Layout.column: 0; Layout.fillWidth: true
                        Layout.preferredWidth: encryptionStatusContent.width
                        textFormat: Text.PlainText; wrapMode: Text.Wrap
                        font.pixelSize: 14; color: root.colors.danger
                        text: String(root.chatEncryption.error || "")
                        Accessible.role: Accessible.AlertMessage
                    }
                    Flow {
                        Layout.row: encryptionStatus.inlineKeyNotice ? (encryptionStatusContent.controlsInline ? 0 : 1)
                            : (encryptedReadStatusText.text.length > 0 ? 1 : 0) + (encryptionStatusError.text.length > 0 ? 1 : 0)
                        Layout.column: encryptionStatusContent.controlsInline ? 1 : 0
                        Layout.preferredWidth: encryptionStatus.inlineKeyNotice ? encryptionStatus.keyNoticeActionsWidth : encryptionStatusContent.width
                        Layout.minimumWidth: encryptionStatus.inlineKeyNotice ? encryptionStatus.keyNoticeActionsWidth : 0
                        Layout.maximumWidth: encryptionStatus.inlineKeyNotice ? encryptionStatus.keyNoticeActionsWidth : encryptionStatusContent.width
                        Layout.fillWidth: !encryptionStatus.inlineKeyNotice
                        Layout.alignment: Qt.AlignLeft | (encryptionStatus.inlineKeyNotice ? Qt.AlignVCenter : Qt.AlignTop)
                        spacing: 8
                        NativeButton {
                            objectName: "retryChatKeyStoreButton"
                            colors: root.colors
                            visible: !root.chatEncryption.available && !root.chatEncryption.storeOpening
                            enabled: root.controller.authenticated && !root.chatEncryption.storeOpening
                            text: qsTr("Retry")
                            Accessible.name: qsTr("Retry opening secure key storage")
                            onClicked: root.controller.retryChatKeyStore()
                        }
                        NativeButton {
                            id: requestCurrentKeyControl
                            objectName: "requestCurrentChatKeyButton"
                            colors: root.colors
                            primary: encryptionStatus.inlineKeyNotice
                            readonly property color sourceAccent: root.colors.accent
                            implicitHeight: root.encryptedComposerHasKey ? 32 : 36
                            horizontalPadding: root.encryptedComposerHasKey ? 12 : 16
                            implicitWidth: contentItem.implicitWidth + 2 * horizontalPadding
                            font.pixelSize: root.encryptedComposerHasKey ? 12 : 14
                            font.weight: root.encryptedComposerHasKey ? Font.Normal : Font.Medium
                            background: Rectangle {
                                radius: 6
                                color: encryptionStatus.inlineKeyNotice
                                    ? Qt.rgba(requestCurrentKeyControl.sourceAccent.r, requestCurrentKeyControl.sourceAccent.g, requestCurrentKeyControl.sourceAccent.b, requestCurrentKeyControl.down ? 0.8 : requestCurrentKeyControl.hovered ? 0.9 : 1)
                                    : requestCurrentKeyControl.hovered ? root.colors["bg-elevated"] : root.colors["card-bg-active"]
                                border.width: requestCurrentKeyControl.visualFocus ? 2 : 1
                                border.color: requestCurrentKeyControl.visualFocus ? root.colors.accent : encryptionStatus.inlineKeyNotice ? "transparent" : requestCurrentKeyControl.hovered ? root.colors["border-subtle"] : "transparent"
                                opacity: requestCurrentKeyControl.enabled ? 1 : 0.5
                            }
                            visible: (!root.encryptedComposerHasKey || root.encryptedKeyNoticeVisible) && root.chatEncryption.state !== "unresolved" && String(root.chatEncryption.currentKeyUuid || "").length > 0 && (Boolean(root.chatEncryption.canRequestCurrentKey) || (root.chatEncryption.keyUuid === root.chatEncryption.currentKeyUuid && (root.chatEncryption.busy || root.chatEncryption.state === "requested" || root.chatEncryption.state === "unresolved")))
                            enabled: Boolean(root.chatEncryption.canRequestCurrentKey) && !root.chatEncryption.busy
                            text: root.chatEncryption.state === "requested" && root.chatEncryption.keyUuid === root.chatEncryption.currentKeyUuid ? qsTr("Waiting for a response") : qsTr("Request key")
                            onClicked: root.controller.requestChatKey(String(root.chatEncryption.currentKeyUuid))
                        }
                        NativeButton {
                            objectName: "checkChatKeyRequestButton"
                            colors: root.colors
                            visible: (!root.encryptedComposerHasKey || root.encryptedKeyNoticeVisible) && root.chatEncryption.state === "unresolved" && String(root.chatEncryption.keyUuid || "").length > 0
                            enabled: root.chatEncryption.canRequest && !root.chatEncryption.busy
                            text: qsTr("Check status")
                            onClicked: root.controller.checkChatKeyRequest(String(root.chatEncryption.keyUuid))
                        }
                        NativeButton {
                            objectName: "retryChatKeyRequestButton"
                            colors: root.colors
                            visible: (!root.encryptedComposerHasKey || root.encryptedKeyNoticeVisible) && root.chatEncryption.state === "unresolved" && String(root.chatEncryption.keyUuid || "").length > 0
                            enabled: root.chatEncryption.canRequest && !root.chatEncryption.busy
                            text: qsTr("Retry")
                            Accessible.name: qsTr("Retry sending the same key request")
                            onClicked: root.controller.retryChatKeyRequest(String(root.chatEncryption.keyUuid))
                        }
                        Item {
                            visible: root.encryptedKeyNoticeVisible
                            width: 24; height: encryptionStatus.inlineKeyNotice && requestCurrentKeyControl.visible ? requestCurrentKeyControl.height : 24
                            NativeIconButton {
                                objectName: "dismissEncryptedKeyNoticeButton"
                                colors: root.colors; asset: "close.svg"; glyphSize: 16
                                iconColor: root.colors[encryptionStatus.inlineKeyNotice ? "accent" : "icon-base"]
                                visible: root.encryptedKeyNoticeVisible
                                anchors.centerIn: parent
                                width: 24; height: 24; padding: 0
                                Accessible.name: qsTr("Close")
                                onClicked: root.dismissedEncryptionKeyNotice = root.encryptionKeyNoticeId
                            }
                        }
                        NativeButton {
                            objectName: "checkPendingEncryptedSendButton"
                            colors: root.colors
                            visible: String(root.encryptedPendingSend.id || "").length > 0
                            enabled: Boolean(root.encryptedPendingSend.canCheck) && !root.encryptedPendingSend.busy
                            text: qsTr("Check status")
                            Accessible.name: qsTr("Check message status")
                            onClicked: root.controller.checkPendingSend()
                        }
                        NativeButton {
                            objectName: "retryPendingEncryptedSendButton"
                            colors: root.colors
                            visible: String(root.encryptedPendingSend.id || "").length > 0
                            enabled: Boolean(root.encryptedPendingSend.canRetry) && !root.encryptedPendingSend.busy
                            text: qsTr("Retry")
                            Accessible.name: qsTr("Retry sending the same message")
                            onClicked: root.controller.retryPendingSend()
                        }
                        NativeButton {
                            objectName: "discardPendingEncryptedSendButton"
                            colors: root.colors; text: qsTr("Discard")
                            visible: String(root.pendingOutgoing.id || "").length > 0
                            enabled: Boolean(root.pendingOutgoing.canDiscard)
                            onClicked: outgoingDiscard.openForPending()
                        }
                    }
                }
            }
            Rectangle {
                objectName: "topicClosedPrompt"
                visible: root.controller.authenticated && root.controller.topicClosed && !root.encryptedConversation
                Layout.fillWidth: true
                Layout.preferredHeight: Math.max(56, closedTopicContent.implicitHeight + 20)
                color: "transparent"
                Rectangle {
                    width: parent.width
                    height: 1
                    color: root.colors["border-subtle"]
                }
                Flow {
                    id: closedTopicContent
                    anchors.centerIn: parent
                    width: Math.min(implicitWidth, parent.width - 32)
                    spacing: 4
                    Text {
                        text: qsTr("This topic is resolved.")
                        textFormat: Text.PlainText
                        color: root.colors["text-primary"]
                        font.pixelSize: 16
                    }
                    Button {
                        id: reopenTopicButton
                        objectName: "reopenTopicButton"
                        enabled: root.controller.canReopenTopic
                        text: qsTr("Remove the mark")
                        padding: 0
                        implicitWidth: contentItem.implicitWidth
                        implicitHeight: 24
                        hoverEnabled: true
                        Accessible.name: text
                        background: Rectangle {
                            color: "transparent"
                            radius: 2
                            border.width: reopenTopicButton.visualFocus ? 2 : 0
                            border.color: root.colors["accent-soft"]
                        }
                        contentItem: Text {
                            text: reopenTopicButton.text
                            textFormat: Text.PlainText
                            font.pixelSize: 16
                            font.underline: true
                            verticalAlignment: Text.AlignVCenter
                            color: root.colors[reopenTopicButton.hovered ? "accent-soft" : "accent"]
                            opacity: reopenTopicButton.enabled ? 1 : 0.6
                        }
                        onClicked: {
                            root.reopenOwner = root.controller.ownerKey;
                            root.reopenStream = root.controller.selectedStream;
                            root.reopenTopic = root.controller.selectedTopic;
                            reopenTopicMenu.open();
                        }
                        Menu {
                            id: reopenTopicMenu
                            x: (parent.width - width) / 2
                            y: -height - 8
                            width: 180
                            MenuItem {
                                objectName: "reopenTopicAction"
                                text: qsTr("Remove the mark")
                                icon.source: "qrc:/workspace/icons/check.svg"
                                icon.width: 16
                                icon.height: 16
                                icon.color: root.colors["text-primary"]
                                enabled: root.reopenAllowed
                                onTriggered: if (root.reopenAllowed)
                                    root.controller.reopenTopic()
                            }
                        }
                    }
                    Text {
                        text: qsTr("to send a message")
                        textFormat: Text.PlainText
                        color: root.colors["text-primary"]
                        font.pixelSize: 16
                    }
                }
            }
            Rectangle {
                objectName: "streamTopicPrompt"
                visible: root.aggregateConversation && !root.encryptedConversation
                Layout.fillWidth: true
                Layout.preferredHeight: 57
                color: "transparent"
                Rectangle {
                    width: parent.width
                    height: 1
                    color: root.colors["border-subtle"]
                }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 16
                    anchors.rightMargin: 16
                    anchors.topMargin: 11
                    anchors.bottomMargin: 10
                    spacing: 12
                    Text {
                        Layout.maximumWidth: root.aggregateTopics.length ? 256 : -1
                        Layout.fillWidth: !root.aggregateTopics.length
                        text: qsTr("Select topic:")
                        textFormat: Text.PlainText
                        horizontalAlignment: root.aggregateTopics.length ? Text.AlignLeft : Text.AlignHCenter
                        font.pixelSize: 14
                        lineHeightMode: Text.FixedHeight
                        lineHeight: 20
                        color: root.colors["text-muted"]
                        elide: Text.ElideRight
                    }
                    ListView {
                        id: topicRail
                        objectName: "streamTopicRail"
                        visible: root.aggregateTopics.length > 0
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        orientation: ListView.Horizontal
                        spacing: 6
                        clip: true
                        model: root.aggregateTopics
                        Accessible.name: qsTr("Select topic:")
                        boundsBehavior: Flickable.StopAtBounds
                        WheelHandler {
                            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                            onWheel: event => {
                                // Keep horizontal input as well as the source's
                                // vertical-to-horizontal wheel translation.
                                const horizontal = event.pixelDelta.x || event.angleDelta.x;
                                const modified = event.modifiers & (Qt.ControlModifier | Qt.MetaModifier | Qt.AltModifier | Qt.ShiftModifier);
                                if (!modified && topicRail.contentWidth > topicRail.width) {
                                    const delta = horizontal || event.pixelDelta.y || event.angleDelta.y;
                                    const previous = topicRail.contentX;
                                    topicRail.contentX = Math.max(0, Math.min(topicRail.contentWidth - topicRail.width, topicRail.contentX - delta));
                                    event.accepted = previous !== topicRail.contentX;
                                } else {
                                    event.accepted = false;
                                }
                            }
                        }
                        delegate: Button {
                            id: topicShortcut
                            required property var modelData
                            required property int index
                            objectName: "topicShortcut-" + modelData.id
                            text: "# " + String(modelData.name).trim()
                            height: 36
                            // RowLayout rounds its text allocation to pixels.
                            // Round the cell outwards so a fractional advance
                            // cannot elide an otherwise short catalog name.
                            width: Math.ceil(Math.min(224, topicShortcutLabel.implicitWidth + (shortcutBadge.visible ? shortcutBadge.width + 8 : 0) + 26))
                            padding: 0
                            horizontalPadding: 13
                            hoverEnabled: true
                            Accessible.name: text
                            onClicked: root.controller.selectTopic(modelData.id)
                            onActiveFocusChanged: {
                                if (activeFocus)
                                    topicRail.positionViewAtIndex(index, ListView.Contain);
                            }
                            background: Rectangle {
                                radius: 6
                                color: root.colors[topicShortcut.hovered ? "card-bg-active" : "card-bg"]
                                border.width: topicShortcut.visualFocus ? 2 : 1
                                border.color: root.colors[topicShortcut.visualFocus || topicShortcut.hovered ? "accent" : "border-subtle"]
                            }
                            contentItem: RowLayout {
                                spacing: 8
                                Text {
                                    id: topicShortcutLabel
                                    objectName: "topicShortcutText-" + topicShortcut.modelData.id
                                    Layout.fillWidth: true
                                    text: topicShortcut.text
                                    textFormat: Text.PlainText
                                    font.pixelSize: 14
                                    lineHeightMode: Text.FixedHeight
                                    lineHeight: 20
                                    font.strikeout: Boolean(topicShortcut.modelData.done)
                                    color: root.colors[topicShortcut.hovered ? "accent" : "text-primary"]
                                    opacity: topicShortcut.modelData.done ? 0.7 : 1
                                    elide: Text.ElideRight
                                }
                                Rectangle {
                                    id: shortcutBadge
                                    visible: Number(topicShortcut.modelData.unread) > 0
                                    Layout.preferredWidth: Math.max(20, shortcutUnread.implicitWidth + 8)
                                    Layout.preferredHeight: 20
                                    radius: 10
                                    color: root.colors["badge-bg"]
                                    Text {
                                        id: shortcutUnread
                                        anchors.centerIn: parent
                                        text: root.countText(Number(topicShortcut.modelData.unread))
                                        textFormat: Text.PlainText
                                        color: root.colors["badge-text"]
                                        font.pixelSize: 11
                                    }
                                }
                            }
                        }
                    }
                }
            }
            Rectangle {
                id: messageComposerShell
                objectName: "messageComposerShell"
                visible: root.encryptedConversation ? root.encryptedComposerHasKey : !root.controller.topicClosed && !root.aggregateConversation
                Layout.fillWidth: true
                // Source measured geometry: the composer box plus the 33 px edit notice
                // (6+20+6 padding and line box, plus the 1 px rule) when a session is open.
                Layout.preferredHeight: Math.min(185, Math.max(57, composerText.contentHeight + 33))
                    + (composerToolbarRow.visible ? composerToolbarRow.Layout.preferredHeight + composerInputColumn.spacing : 0)
                    + (messageSelectionBar.visible ? messageSelectionBar.height : 0)
                    + (editMessageNotice.visible ? editMessageNotice.height : 0)
                    + replyTabStrip.height
                    + (replyNotice.visible ? replyNotice.height : 0)
                    + editAttachmentStrip.height
                    + uploadCardStrip.height
                color: root.colors["composer-outer"]
                radius: 10
                Rectangle {
                    width: parent.width
                    height: 1
                    color: root.colors["border-subtle"]
                }
                Rectangle {
                    id: messageSelectionBar
                    objectName: "messageSelectionBar"
                    // Reference ChatPageSelectionBar: only visible with a
                    // non-empty selection; a danger notice marker (4 px, 10 px
                    // vertical inset, rounded right edge), "Selected: N" in
                    // text-sm medium text-primary, then Cancel, Reply and Forward.
                    // The 52 px keeps the native 40 px button inside the strip
                    // with the reference's 6 px vertical padding.
                    width: parent.width
                    height: 52
                    visible: root.messageSelectionCount > 0
                    color: root.colors["composer-outer"]
                    Accessible.role: Accessible.ToolBar
                    Accessible.name: selectionCountText.text
                    Rectangle {
                        objectName: "selectionNoticeMarker"
                        x: 0
                        y: 10
                        width: 4
                        height: parent.height - 20
                        radius: 2
                        color: root.colors.danger
                    }
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 1
                        color: root.colors["border-subtle"]
                    }
                    RowLayout {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 24
                        anchors.rightMargin: 24
                        spacing: 12
                        Text {
                            id: selectionCountText
                            objectName: "messageSelectionCountText"
                            Layout.alignment: Qt.AlignVCenter
                            text: qsTr("Selected: %1").arg(root.messageSelectionCount)
                            textFormat: Text.PlainText
                            font.pixelSize: 14
                            font.weight: Font.Medium
                            color: root.colors["text-primary"]
                        }
                        Item { Layout.fillWidth: true }
                        NativeButton {
                            objectName: "selectionCancelButton"
                            colors: root.colors
                            text: qsTr("Cancel")
                            onClicked: root.clearMessageSelection()
                        }
                        component SelectionActionButton: NativeButton {
                            id: action
                            icon.width: 28
                            icon.height: 28
                            icon.color: root.colors["text-primary"]
                            contentItem: ControlsImpl.IconLabel {
                                icon: action.icon
                                text: action.text
                                font: action.font
                                color: root.colors["text-primary"]
                                spacing: 6
                                opacity: action.enabled ? 1 : 0.5
                            }
                        }
                        SelectionActionButton {
                            objectName: "selectionReplyButton"
                            colors: root.colors
                            text: qsTr("Reply")
                            icon.source: "qrc:/workspace/icons/reply.svg"
                            enabled: root.messageSelectionCount > 0 && root.composerAvailable
                                && !root.encryptedConversation && !root.controller.busy && !root.controller.cacheStale
                            onClicked: {
                                if (root.controller.replyToSelection(root.messageSelectionIds))
                                    root.clearMessageSelection()
                            }
                        }
                        SelectionActionButton {
                            objectName: "selectionForwardButton"
                            icon.source: "qrc:/workspace/icons/forward.svg"
                            colors: root.colors
                            text: qsTr("Forward")
                            // reference forwardDisabled = selectedCount === 0
                            // (chat-page-workspace.ui.tsx:4759); the bar only
                            // exists with a non-empty selection.
                            enabled: root.messageSelectionCount > 0
                            onClicked: forwardDialog.open()
                        }
                    }
                }
                Rectangle {
                    id: editMessageNotice
                    objectName: "editMessageNotice"
                    y: messageSelectionBar.visible ? messageSelectionBar.height : 0
                    width: parent.width
                    height: 33
                    color: root.colors["composer-outer"]
                    visible: root.controller.editingMessageId.length > 0
                    Accessible.role: Accessible.Notification
                    Accessible.name: editMessageNoticeText.text
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 1
                        color: root.colors["border-subtle"]
                    }
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 12
                        Text {
                            id: editMessageNoticeText
                            objectName: "editMessageNoticeText"
                            Layout.fillWidth: true
                            text: qsTr("Edit message")
                            textFormat: Text.PlainText
                            font.pixelSize: 14
                            lineHeightMode: Text.FixedHeight
                            lineHeight: 20
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                            color: root.colors["text-primary"]
                        }
                        Rectangle {
                            id: cancelEditNoticeControl
                            Layout.preferredWidth: Math.max(40, cancelEditNoticeText.implicitWidth + 24)
                            Layout.preferredHeight: 28
                            radius: 8
                            color: cancelEditNoticeArea.containsMouse ? Qt.alpha(root.colors["text-primary"], 0.08) : "transparent"
                            Text {
                                id: cancelEditNoticeText
                                objectName: "cancelEditButton"
                                anchors.centerIn: parent
                                text: qsTr("Cancel")
                                textFormat: Text.PlainText
                                font.pixelSize: 14
                                color: root.colors["text-muted"]
                            }
                            MouseArea {
                                id: cancelEditNoticeArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.controller.cancelEditMessage()
                            }
                        }
                    }
                }
                // Multi-reply tab strip (docs/NATIVE_DESKTOP_MULTI_REPLY_TABS.md).
                // Source measures (workspace_ui 61dacef workspace-reply.ui.tsx +
                // message-composer-preface.ui.tsx:231-243): visible only when more
                // than one tab exists (chat-page-composer-section.ui.tsx:135-142);
                // row insets 8 px with top 8 / bottom 4, pills spaced 6 px; each pill
                // is 32 px tall, rounded-xl (12 px) with a 1 px border, sender label
                // px-3 capped at 184 px of elided text-sm (14 px) medium text; the
                // active pill fills accent/15 with text-primary, the rest stay muted;
                // the close button is 32 x 32 with a 14 px icon; the dismiss moves
                // into this row while the strip is visible.
                Rectangle {
                    id: replyTabStrip
                    objectName: "replyTabStrip"
                    y: (messageSelectionBar.visible ? messageSelectionBar.height : 0)
                        + (editMessageNotice.visible ? editMessageNotice.height : 0)
                    width: parent.width
                    // Height-collapsed rather than visible-toggled: a hidden view never
                    // materializes its delegates, so the strip collapses to 0 px instead
                    // (reference overflow-x-auto scroll row).
                    height: replyTabRow.count > 1 ? 44 : 0
                    clip: true
                    color: root.colors["composer-outer"]
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        anchors.topMargin: 8
                        anchors.bottomMargin: 4
                        spacing: 6
                        ListView {
                            id: replyTabRow
                            Layout.fillWidth: true
                            Layout.preferredHeight: 32
                            height: 32
                            orientation: ListView.Horizontal
                            boundsBehavior: Flickable.StopAtBounds
                            spacing: 6
                            clip: true
                            model: root.controller.replyTabs
                            delegate: Rectangle {
                                    required property var modelData
                                    objectName: "replyTab-" + modelData.id
                                        width: replyTabContents.implicitWidth
                                        height: 32
                                        radius: 12
                                        border.width: 1
                                        border.color: root.colors["border-subtle"]
                                        color: modelData.active
                                            ? Qt.alpha(root.colors.accent, 0.15)
                                            : Qt.alpha(root.colors["bg-elevated"], 0.5)
                                        Accessible.role: Accessible.PageTab
                                        Accessible.name: modelData.label + ": " + qsTr("Reply")
                                        RowLayout {
                                            id: replyTabContents
                                            anchors.fill: parent
                                            anchors.leftMargin: 12
                                            spacing: 0
                                            Text {
                                                Layout.maximumWidth: 184
                                                Layout.preferredWidth: Math.min(implicitWidth, 184)
                                                Layout.alignment: Qt.AlignVCenter
                                                text: modelData.author
                                                textFormat: Text.PlainText
                                                font.pixelSize: 14
                                                font.weight: Font.Medium
                                                elide: Text.ElideRight
                                                color: modelData.active
                                                    ? root.colors["text-primary"]
                                                    : root.colors["text-muted"]
                                            }
                                            NativeIconButton {
                                                objectName: "replyTabClose-" + modelData.id
                                                Layout.preferredWidth: 32
                                                Layout.preferredHeight: 32
                                                implicitWidth: 32
                                                implicitHeight: 32
                                                colors: root.colors
                                                asset: "close.svg"
                                                glyphSize: 14
                                                cornerRadius: 12
                                                help: qsTr("Close: %1").arg(modelData.author)
                                                onClicked: root.controller.removeReplyTab(modelData.id)
                                            }
                                        }
                                        MouseArea {
                                            anchors.fill: parent
                                            // The 32 px close button starts at x = width - 32;
                                            // leave 1 px of overlap so the pill centre (and the
                                            // author text) always selects the tab.
                                            anchors.rightMargin: 31
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: root.controller.selectReplyTab(modelData.id)
                                        }
                                    }
                            }
                        Rectangle {
                            // The dismiss shares the tab row while tabs are visible
                            // (clearReplyOnTabsRow, message-composer-preface.ui.tsx:237-241).
                            visible: replyTabStrip.height > 0
                            objectName: replyTabStrip.height > 0 ? "cancelReplyButton" : ""
                            Layout.alignment: Qt.AlignTop
                            Layout.preferredWidth: Math.max(40, cancelReplyStripText.implicitWidth + 24)
                            Layout.preferredHeight: 28
                            radius: 8
                            color: cancelReplyStripArea.containsMouse ? Qt.alpha(root.colors["text-primary"], 0.08) : "transparent"
                            Text {
                                id: cancelReplyStripText
                                anchors.centerIn: parent
                                text: qsTr("Cancel")
                                textFormat: Text.PlainText
                                font.pixelSize: 14
                                color: root.colors["text-muted"]
                            }
                            MouseArea {
                                id: cancelReplyStripArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.controller.cancelReply()
                            }
                        }
                    }
                }
                // Reply preview (docs/NATIVE_DESKTOP_REPLIES.md). Source measures
                // (workspace_ui 61dacef message-composer-reply-quote-preview.ui.tsx +
                // shared/ui/workspace-message-quote-frame + message-composer-preface.ui.tsx:222-262):
                // composer surface, row padding top 8 / bottom 4 with 8 px inset, quote
                // frame radius 6 px with a 2 px accent left bar, padding 8 x / 6 y,
                // header 12 px / 16 px weight-500 accent "Reply: <name>", body 14 px /
                // 20 px text-primary clamped to two lines (QUOTE_PREVIEW_MAX 80 summary).
                Rectangle {
                    id: replyNotice
                    objectName: "replyNotice"
                    y: (messageSelectionBar.visible ? messageSelectionBar.height : 0)
                        + (editMessageNotice.visible ? editMessageNotice.height : 0)
                        + replyTabStrip.height
                    width: parent.width
                    height: 12 + replyQuoteFrame.height
                    color: root.colors["composer-outer"]
                    visible: root.controller.replyMessageId.length > 0
                    Accessible.role: Accessible.Notification
                    Accessible.name: replyAuthorText.text
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        anchors.topMargin: 8
                        anchors.bottomMargin: 4
                        spacing: 8
                        Rectangle {
                            id: replyQuoteFrame
                            Layout.fillWidth: true
                            // The 48 px media thumbnail sets the minimum frame
                            // height, exactly like the reference self-stretch box.
                            Layout.preferredHeight: Math.max(replyQuoteContents.height,
                                replyMediaThumbnail.visible ? 48 : 0) + 12
                            radius: 6
                            color: root.colors["composer-outer"]
                            Rectangle {
                                id: replyAccentBar
                                objectName: "replyAccentBar"
                                width: 2
                                height: parent.height
                                radius: 1
                                color: root.colors.accent
                            }
                            NativeQuoteMediaThumbnail {
                                id: replyMediaThumbnail
                                objectName: "replyMediaThumbnail"
                                // Demand only: the component hides itself on a
                                // failed load (issue #217 exo-review).
                                demand: root.controller.replyMediaUrn.length > 0
                                colors: root.colors
                                images: root.messageImages
                                urn: root.controller.replyMediaUrn
                                kind: root.controller.replyMediaKind
                                x: 10
                                y: 6
                                height: parent.height - 12
                            }
                            Column {
                                id: replyQuoteContents
                                x: replyMediaThumbnail.visible ? 66 : 10
                                width: replyQuoteFrame.width - (replyMediaThumbnail.visible ? 74 : 18)
                                y: 6
                                spacing: 2
                                Text {
                                    id: replyAuthorText
                                    objectName: "replyAuthorText"
                                    width: parent.width
                                    text: qsTr("Reply: %1").arg(root.controller.replyAuthor)
                                    textFormat: Text.PlainText
                                    font.pixelSize: 12
                                    font.weight: Font.Medium
                                    lineHeightMode: Text.FixedHeight
                                    lineHeight: 16
                                    elide: Text.ElideRight
                                    color: root.colors.accent
                                }
                                Text {
                                    id: replyExcerptText
                                    objectName: "replyExcerptText"
                                    width: parent.width
                                    visible: text.length > 0
                                    // Once the thumbnail is painted the media token
                                    // leaves the summary (reference
                                    // hiddenWorkspaceMediaFileUuids, issue #217).
                                    text: replyMediaThumbnail.mediaStatus === "ready"
                                        ? root.controller.replyMediaSummary
                                        : root.controller.replyExcerpt
                                    textFormat: Text.PlainText
                                    font.pixelSize: 14
                                    lineHeightMode: Text.FixedHeight
                                    lineHeight: 20
                                    maximumLineCount: 2
                                    elide: Text.ElideRight
                                    wrapMode: Text.Wrap
                                    color: root.colors["text-primary"]
                                }
                            }
                        }
                        Rectangle {
                            id: cancelReplyControl
                            Layout.alignment: Qt.AlignTop
                            Layout.preferredWidth: Math.max(40, cancelReplyText.implicitWidth + 24)
                            Layout.preferredHeight: 28
                            radius: 8
                            color: cancelReplyArea.containsMouse ? Qt.alpha(root.colors["text-primary"], 0.08) : "transparent"
                            Text {
                                id: cancelReplyText
                                objectName: replyTabStrip.height > 0 ? "" : "cancelReplyButton"
                                anchors.centerIn: parent
                                text: qsTr("Cancel")
                                textFormat: Text.PlainText
                                font.pixelSize: 14
                                color: root.colors["text-muted"]
                            }
                            MouseArea {
                                id: cancelReplyArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.controller.cancelReply()
                            }
                        }
                    }
                }
                ListView {
                    id: editAttachmentStrip
                    objectName: "editAttachmentStrip"
                    y: (messageSelectionBar.visible ? messageSelectionBar.height : 0)
                        + (editMessageNotice.visible ? editMessageNotice.height : 0)
                        + replyTabStrip.height + (replyNotice.visible ? replyNotice.height : 0)
                    x: 8
                    width: parent.width - 16
                    height: count > 0 ? 70 : 0
                    clip: true
                    orientation: ListView.Horizontal
                    boundsBehavior: Flickable.StopAtBounds
                    spacing: 8
                    model: root.controller.editAttachments
                    delegate: Rectangle {
                        id: editAttachmentCard
                        required property var modelData
                        objectName: "editAttachment-" + modelData.id
                        y: 8
                        width: Math.min(240, editAttachmentStrip.width)
                        height: 58
                        radius: 8
                        color: root.colors["composer-outer"]
                        border.width: 1
                        border.color: root.colors["border-subtle"]
                        Accessible.role: Accessible.ListItem
                        Accessible.name: modelData.label || qsTr("File")
                        Rectangle {
                            id: editAttachmentFallback
                            objectName: "editAttachmentFallback-" + editAttachmentCard.modelData.id
                            x: 9; y: 9; width: 40; height: 40; radius: 8
                            color: root.colors.bg
                            visible: editAttachmentPreview.imageUrl.length === 0
                            NativeIconButton {
                                anchors.centerIn: parent
                                width: 18; height: 18; glyphSize: 18
                                colors: root.colors
                                iconColor: root.colors["text-muted"]
                                asset: (editAttachmentCard.modelData.kind === "image" ? "images"
                                    : editAttachmentCard.modelData.kind === "video" ? "videos" : "files") + ".svg"
                                focusPolicy: Qt.NoFocus
                                hoverEnabled: false
                                background: null
                                Accessible.ignored: true
                            }
                        }
                        NativeMessageImage {
                            id: editAttachmentPreview
                            objectName: "editAttachmentPreview-" + editAttachmentCard.modelData.id
                            x: 9; y: 9; width: 40; height: 40
                            colors: root.colors
                            viewport: editAttachmentStrip
                            reference: editAttachmentCard.modelData.kind === "image" ? editAttachmentCard.modelData.reference : ""
                            label: editAttachmentCard.modelData.label || ""
                            bodyActive: root.visible && root.controller.editingMessageId.length > 0
                                && editAttachmentCard.modelData.kind === "image"
                            opacity: imageUrl.length > 0 ? 1 : 0
                            imageFillMode: Image.PreserveAspectCrop
                            imageCornerRadius: 8
                        }
                        Column {
                            x: 61; y: 13
                            width: Math.max(0, parent.width - x - 45)
                            spacing: 2
                            Text {
                                width: parent.width
                                text: editAttachmentCard.modelData.label || qsTr("File")
                                textFormat: Text.PlainText
                                font.pixelSize: 12; font.weight: Font.Medium
                                lineHeightMode: Text.FixedHeight; lineHeight: 14
                                elide: Text.ElideRight; color: root.colors["text-primary"]
                            }
                            Text {
                                width: parent.width
                                text: {
                                    const name = String(editAttachmentCard.modelData.label || "")
                                    const extension = name.indexOf(".") >= 0 ? name.split(".").pop().toUpperCase() : ""
                                    const format = extension || (editAttachmentCard.modelData.kind === "image" ? qsTr("Image")
                                        : editAttachmentCard.modelData.kind === "video" ? qsTr("Video") : qsTr("File"))
                                    const bytes = Number(editAttachmentCard.modelData.sizeBytes)
                                    return format + (Number.isFinite(bytes) && bytes > 0
                                        ? " · " + Qt.locale(root.preferences.locale).formattedDataSize(bytes, 1, Locale.DataSizeSIFormat) : "")
                                }
                                textFormat: Text.PlainText
                                font.pixelSize: 12; lineHeightMode: Text.FixedHeight; lineHeight: 15
                                elide: Text.ElideRight; color: root.colors["text-muted"]
                            }
                        }
                        NativeIconButton {
                            objectName: "editAttachmentRemove-" + editAttachmentCard.modelData.id
                            x: parent.width - width - 9; y: 17
                            width: 24; height: 24; glyphSize: 12; cornerRadius: 12
                            colors: root.colors; asset: "close.svg"
                            help: qsTr("Remove %1").arg(editAttachmentCard.modelData.label || qsTr("File"))
                            focusPolicy: Qt.StrongFocus
                            onClicked: root.controller.removeEditAttachment(editAttachmentCard.modelData.id)
                        }
                    }
                    ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
                }
                ListView {
                    id: uploadCardStrip
                    objectName: "uploadCardStrip"
                    y: (editMessageNotice.visible ? editMessageNotice.height : 0)
                        + replyTabStrip.height + (replyNotice.visible ? replyNotice.height : 0) + editAttachmentStrip.height
                    x: 8; width: parent.width - 16
                    height: count > 0 ? 70 : 0
                    clip: true; orientation: ListView.Horizontal
                    boundsBehavior: Flickable.StopAtBounds; spacing: 10
                    model: root.controller.uploadEntries
                    Accessible.role: Accessible.List
                    Accessible.name: qsTr("Attached files")
                    delegate: Rectangle {
                        id: uploadCard
                        required property var modelData
                        readonly property bool pending: modelData.status === "queued" || modelData.status === "validating"
                        readonly property bool uploading: modelData.status === "uploading"
                        readonly property bool failed: modelData.status === "error"
                        readonly property string inlineAlias: String(modelData.alias || "")
                        readonly property bool inlineInText: Boolean(modelData.inText)
                        readonly property int percent: Number(modelData.total) > 0
                            ? Math.max(0, Math.min(100, Math.round(Number(modelData.sent) * 100 / Number(modelData.total)))) : 0
                        objectName: "uploadCard-" + modelData.id
                        y: 8; width: Math.min(240, uploadCardStrip.width); height: 58; radius: failed ? 12 : 8
                        color: root.colors["composer-outer"]
                        border.width: 1; border.color: failed ? root.colors.danger : root.colors["border-subtle"]
                        Accessible.role: Accessible.ListItem
                        Accessible.name: modelData.name
                        Rectangle {
                            x: 9; y: 9; width: 40; height: 40; radius: 8
                            color: root.colors.bg; clip: true
                            Image {
                                anchors.fill: parent
                                source: uploadCard.modelData.status === "ready" && uploadCard.modelData.image ? uploadCard.modelData.path : ""
                                sourceSize.width: 80; sourceSize.height: 80
                                fillMode: Image.PreserveAspectCrop; asynchronous: true
                            }
                            Item {
                                id: imageDragGhost
                                width: 40; height: 40
                                Drag.active: imageDragMouse.drag.active
                                Drag.source: uploadCard
                                Drag.hotSpot.x: width / 2; Drag.hotSpot.y: height / 2
                            }
                            MouseArea {
                                id: imageDragMouse
                                anchors.fill: parent
                                enabled: uploadCard.inlineAlias.length > 0 && !uploadCard.inlineInText
                                cursorShape: Qt.OpenHandCursor
                                Accessible.role: Accessible.Grouping
                                Accessible.name: qsTr("Drag image into message")
                                drag.target: imageDragGhost; drag.threshold: 8
                                onReleased: {
                                    if (drag.active) imageDragGhost.Drag.drop();
                                    imageDragGhost.x = 0; imageDragGhost.y = 0;
                                }
                            }
                            Rectangle {
                                visible: uploadCard.inlineInText
                                anchors.right: parent.right; anchors.bottom: parent.bottom
                                width: 40; height: 20; radius: 10; color: root.colors.accent
                                Text { anchors.left: parent.left; anchors.leftMargin: 3; anchors.verticalCenter: parent.verticalCenter
                                    text: "TX"; color: "white"; font.pixelSize: 9; font.bold: true }
                                NativeIconButton {
                                    objectName: "uploadRemoveInline-" + uploadCard.modelData.id
                                    anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                                    width: 20; height: 20; glyphSize: 10; cornerRadius: 10
                                    colors: root.colors; asset: "close.svg"
                                    help: qsTr("Remove image from text")
                                    onClicked: root.controller.removeUploadFromText(uploadCard.modelData.id)
                                }
                            }
                            NativeIconButton {
                                anchors.centerIn: parent
                                visible: !uploadCard.uploading && !(uploadCard.modelData.status === "ready" && uploadCard.modelData.image)
                                width: 18; height: 18; glyphSize: 18
                                colors: root.colors; iconColor: root.colors["text-muted"]
                                asset: uploadCard.failed ? "attachment-upload-error.svg" : "files.svg"
                                focusPolicy: Qt.NoFocus; hoverEnabled: false; background: null
                                Accessible.ignored: true
                            }
                            Canvas {
                                anchors.centerIn: parent; width: 18; height: 18
                                visible: uploadCard.uploading
                                Accessible.role: Accessible.ProgressBar
                                Accessible.name: qsTr("Uploading %1: %2%").arg(uploadCard.modelData.name).arg(uploadCard.percent)
                                property int value: uploadCard.percent
                                onValueChanged: requestPaint()
                                onPaint: {
                                    const ctx = getContext("2d"); ctx.clearRect(0, 0, width, height);
                                    ctx.lineWidth = 3; ctx.beginPath(); ctx.arc(9, 9, 7.5, 0, 2 * Math.PI);
                                    ctx.strokeStyle = Qt.alpha(root.colors["text-muted"], 0.24); ctx.stroke();
                                    ctx.beginPath(); ctx.arc(9, 9, 7.5, -Math.PI / 2, -Math.PI / 2 + 2 * Math.PI * value / 100);
                                    ctx.strokeStyle = root.colors.accent; ctx.stroke();
                                }
                            }
                        }
                        Column {
                            x: 61; y: 13; width: Math.max(0, parent.width - x - (uploadCard.failed && uploadCard.modelData.retryable ? 57 : 33)); spacing: 2
                            Text {
                                width: parent.width; text: uploadCard.modelData.name
                                textFormat: Text.PlainText; elide: Text.ElideRight
                                color: root.colors["text-primary"]; font.pixelSize: 12; font.weight: Font.Medium
                                lineHeightMode: Text.FixedHeight; lineHeight: 14
                            }
                            Text {
                                objectName: "uploadStatus-" + uploadCard.modelData.id
                                width: parent.width; elide: Text.ElideRight; textFormat: Text.PlainText
                                color: uploadCard.failed ? root.colors.danger : root.colors["text-muted"]; font.pixelSize: 12
                                lineHeightMode: Text.FixedHeight; lineHeight: 15
                                text: {
                                    const status = uploadCard.modelData.status;
                                    if (status === "validating") return qsTr("Checking file");
                                    if (status === "queued") return qsTr("Waiting to upload");
                                    if (status === "uploading") return qsTr("Uploading · %1%").arg(uploadCard.percent);
                                    if (status === "error") return uploadCard.modelData.error;
                                    const name = String(uploadCard.modelData.name);
                                    const dot = name.lastIndexOf(".");
                                    const extension = dot > 0 ? name.slice(dot + 1).toUpperCase().slice(0, 4) : "FILE";
                                    const bytes = Number(uploadCard.modelData.size);
                                    const size = bytes < 1024 ? bytes + " B" : bytes < 1048576 ? Math.round(bytes / 1024) + " KB"
                                        : (Math.round(bytes / 104857.6) / 10) + " MB";
                                    return extension + " · " + size;
                                }
                            }
                        }
                        NativeIconButton {
                            objectName: "uploadRetry-" + uploadCard.modelData.id
                            visible: uploadCard.failed && uploadCard.modelData.retryable
                            x: parent.width - 57; y: 17; width: 24; height: 24; glyphSize: 16; cornerRadius: 12
                            colors: root.colors; asset: "attachment-retry.svg"; focusPolicy: Qt.StrongFocus
                            help: qsTr("Retry upload of %1").arg(uploadCard.modelData.name)
                            onClicked: root.controller.retryUpload(uploadCard.modelData.id)
                        }
                        NativeIconButton {
                            objectName: "uploadCancel-" + uploadCard.modelData.id
                            x: parent.width - width - 9; y: 17
                            width: 24; height: 24; glyphSize: 12; cornerRadius: 12
                            colors: root.colors; asset: "close.svg"; focusPolicy: Qt.StrongFocus
                            help: uploadCard.uploading ? qsTr("Cancel upload of %1").arg(uploadCard.modelData.name)
                                : qsTr("Remove %1").arg(uploadCard.modelData.name)
                            onClicked: root.controller.removeUpload(uploadCard.modelData.id)
                        }
                    }
                    ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
                }
                DropArea {
                    id: composerDropArea
                    objectName: "composerDropArea"
                    property bool externalFiles: false
                    anchors.fill: parent; z: 4
                    onEntered: drag => {
                        if (drag.source && drag.source.inlineAlias) {
                            externalFiles = false;
                            drag.accepted = true;
                            return;
                        }
                        let local = false;
                        if (drag.hasUrls) {
                            for (let i = 0; i < drag.urls.length; ++i)
                                if (String(drag.urls[i]).toLowerCase().startsWith("file:")) local = true;
                        }
                        externalFiles = root.controller.canAttach && local;
                        drag.accepted = externalFiles;
                    }
                    onExited: externalFiles = false
                    onDropped: drop => {
                        externalFiles = false;
                        if (drop.source && drop.source.inlineAlias) {
                            const p = composerDropArea.mapToItem(composerText, drop.x, drop.y);
                            if (p.x >= 0 && p.y >= 0 && p.x <= composerText.width && p.y <= composerText.height
                                && !drop.source.inlineInText && !composerText.readOnly) {
                                const at = composerText.positionAt(p.x, p.y);
                                const alias = drop.source.inlineAlias;
                                const next = composerText.text.slice(0, at) + alias + composerText.text.slice(at);
                                root.controller.draft = next;
                                composerText.forceActiveFocus(); composerText.select(at + alias.length, at + alias.length);
                                drop.accepted = true;
                            } else drop.accepted = false;
                            return;
                        }
                        const files = [];
                        for (let i = 0; i < drop.urls.length; ++i)
                            if (String(drop.urls[i]).toLowerCase().startsWith("file:")) files.push(drop.urls[i]);
                        if (files.length && root.controller.canAttach) {
                            root.controller.attachFiles(files);
                            drop.accepted = true;
                        } else drop.accepted = false;
                    }
                }
                Rectangle {
                    objectName: "composerDropRing"
                    anchors.fill: parent; anchors.margins: 2; radius: 8; z: 5
                    color: "transparent"; border.width: 2; border.color: root.colors.accent
                    visible: composerDropArea.containsDrag && composerDropArea.externalFiles
                }
                ColumnLayout {
                    id: composerInputColumn
                    // Padded composer column: message-composer.ui.tsx:2040
                    // (workspace_ui 61dacef) `relative flex min-h-0 flex-1 flex-col p-2`;
                    // the shell's border-box plus the top rule makes the region 57 px.
                    objectName: "composerInputColumn"
                    anchors.fill: parent
                    anchors.margins: 8
                    anchors.topMargin: 9 + (messageSelectionBar.visible ? messageSelectionBar.height : 0)
                        + (editMessageNotice.visible ? editMessageNotice.height : 0)
                        + replyTabStrip.height
                        + (replyNotice.visible ? replyNotice.height : 0)
                        + editAttachmentStrip.height
                        + uploadCardStrip.height
                    spacing: 4
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        spacing: 12
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            radius: 10
                            color: root.colors.bg
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                spacing: 8
                                NativeIconButton {
                                    objectName: "composerToolbarExpand"
                                    colors: root.colors
                                    // Compact mode keeps a single expand toggle in the
                                    // input row (message-composer.ui.tsx:1925-1939), on
                                    // the same pl-2 axis as the toolbar collapse toggle.
                                    asset: "composer-bottom-panel-open.svg"
                                    help: qsTr("Show formatting tools")
                                    enabled: composerText.enabled
                                    visible: !root.preferences.composerToolbarExpanded
                                    implicitWidth: 32
                                    implicitHeight: 32
                                    glyphSize: 21
                                    Layout.alignment: Qt.AlignBottom
                                    onClicked: root.preferences.composerToolbarExpanded = true
                                }
                                ScrollView {
                                id: composerScroll
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                contentWidth: availableWidth
                                TextArea {
                                    id: composerText
                                    objectName: "messageComposer"
                                    readOnly: root.controller.mediaViewer.open
                                    enabled: root.composerAvailable && !Boolean(root.encryptedPendingSend.busy)
                                    text: root.controller.draft
                                    onTextChanged: {
                                        if (root.composerAvailable && root.controller.draft !== text)
                                            root.controller.draft = text;
                                        Qt.callLater(root.detectComposerMentions);
                                    }
                                    onCursorPositionChanged: Qt.callLater(root.detectComposerMentions)
                                    placeholderText: root.composerAvailable || root.encryptedComposerHasKey ? qsTr("Write a message…") : qsTr("Choose a topic to write a message")
                                    placeholderTextColor: root.colors["composer-icon"]
                                    color: root.colors["text-primary"]
                                    selectionColor: root.colors.accent
                                    font.pixelSize: 16
                                    wrapMode: TextEdit.Wrap
                                    textFormat: TextEdit.PlainText
                                    selectByMouse: true
                                    persistentSelection: true
                                    KeyNavigation.priority: KeyNavigation.BeforeItem
                                    KeyNavigation.tab: root.preferences.composerToolbarExpanded
                                        ? composerFormatLinkButton : null
                                    topPadding: 8
                                    bottomPadding: 8
                                    leftPadding: 0
                                    rightPadding: 0
                                    background: null
                                    Accessible.name: qsTr("Message")
                                    readonly property bool formattingAllowed: enabled && !readOnly && visible && activeFocus
                                        && root.active && !inputMethodComposing
                                        && !ownStatusDialog.visible && !deleteDialog.visible
                                        && !createTopicDialog.visible && !createChatDialog.visible && !referenceNotice.visible
                                        && !attachmentPicker.visible && !attachmentSaveDialog.visible
                                    // message-composer-selection.lib.ts mutateSelection:
                                    // replace the selection, restore the returned selection
                                    // relative to it, and keep the scroll position.
                                    function mutateSelection(mutationFor) {
                                        if (!formattingAllowed)
                                            return;
                                        const owner = root.controller.ownStatusContextKey;
                                        const stream = root.controller.selectedStream, topic = root.controller.selectedTopic;
                                        const start = selectionStart, end = selectionEnd;
                                        const current = text;
                                        const mutation = mutationFor(current.slice(start, end));
                                        const next = current.slice(0, start) + mutation.text + current.slice(end);
                                        const scroll = composerScroll.contentItem;
                                        const scrollY = scroll ? scroll.contentY : 0;
                                        root.controller.draft = next;
                                        // A synchronous draftChanged subscriber can navigate or sign out.
                                        // Keep the text binding and never restore an old route's caret.
                                        if (!formattingAllowed || owner !== root.controller.ownStatusContextKey
                                            || stream !== root.controller.selectedStream || topic !== root.controller.selectedTopic
                                            || root.controller.draft !== next || text !== next)
                                            return;
                                        select(start + mutation.selectionStart, start + mutation.selectionEnd);
                                        if (scroll === composerScroll.contentItem && isFinite(scrollY))
                                            scroll.contentY = scrollY;
                                    }
                                    function wrapFormatting(marker) {
                                        mutateSelection((selected) => selected.length > 0
                                            ? { text: marker + selected + marker,
                                                selectionStart: marker.length + selected.length + marker.length,
                                                selectionEnd: marker.length + selected.length + marker.length }
                                            : { text: marker + marker,
                                                selectionStart: marker.length, selectionEnd: marker.length });
                                    }
                                    // Toolbar list/quote actions prefix each selected line;
                                    // an empty selection inserts the single-line marker.
                                    function prefixSelectedLines(prefixFor) {
                                        mutateSelection((selected) => {
                                            if (selected.length === 0) {
                                                const first = prefixFor(0);
                                                return { text: first,
                                                    selectionStart: first.length, selectionEnd: first.length };
                                            }
                                            const prefixed = selected.split("\n")
                                                .map((line, index) => prefixFor(index) + line).join("\n");
                                            return { text: prefixed,
                                                selectionStart: prefixed.length, selectionEnd: prefixed.length };
                                        });
                                    }
                                    function insertQuote() { prefixSelectedLines((index) => "> "); }
                                    function insertBulletedList() { prefixSelectedLines((index) => "- "); }
                                    function insertNumberedList() { prefixSelectedLines((index) => String(index + 1) + ". "); }
                                    function insertCodeBlock() {
                                        mutateSelection((selected) => selected.length > 0
                                            ? { text: "```\n" + selected + "\n```",
                                                selectionStart: selected.length + 8, selectionEnd: selected.length + 8 }
                                            : { text: "```\n\n```", selectionStart: 4, selectionEnd: 4 });
                                    }
                                    function insertLink() {
                                        mutateSelection((selected) => {
                                            const template = "["
                                                + (selected.length > 0 ? selected : qsTr("link text")) + "](https://)";
                                            // The URL slot is the fixed tail of the template: anchor the
                                            // selection to its end so a label containing "https://" cannot
                                            // move the selection start into the label text.
                                            return { text: template,
                                                selectionStart: template.length - 1 - "https://".length,
                                                selectionEnd: template.length - 1 };
                                        });
                                    }
                                    // message-composer-list-continuation.lib.ts: only with a collapsed
                                    // caret at a line end; an empty marker line is removed instead
                                    // of extended, ordered markers increment.
                                    function listContinuation(current, start, end) {
                                        if (start !== end)
                                            return null;
                                        const lineStart = current.lastIndexOf("\n", Math.max(0, start - 1)) + 1;
                                        const nextNewline = current.indexOf("\n", start);
                                        const lineEnd = nextNewline === -1 ? current.length : nextNewline;
                                        if (start !== lineEnd)
                                            return null;
                                        const line = current.slice(lineStart, lineEnd);
                                        const unordered = /^(\s*)([-*+])\s(.*)$/.exec(line);
                                        if (unordered !== null) {
                                            if (unordered[3].trim().length === 0)
                                                return { text: current.slice(0, lineStart) + current.slice(lineEnd),
                                                    caret: lineStart };
                                            const insertion = "\n" + unordered[1] + unordered[2] + " ";
                                            return { text: current.slice(0, start) + insertion + current.slice(end),
                                                caret: start + insertion.length };
                                        }
                                        const ordered = /^(\s*)(\d+)\.\s(.*)$/.exec(line);
                                        if (ordered !== null) {
                                            if (ordered[3].trim().length === 0)
                                                return { text: current.slice(0, lineStart) + current.slice(lineEnd),
                                                    caret: lineStart };
                                            const insertion = "\n" + ordered[1] + String(Number(ordered[2]) + 1) + ". ";
                                            return { text: current.slice(0, start) + insertion + current.slice(end),
                                                caret: start + insertion.length };
                                        }
                                        return null;
                                    }
                                    // Insert the highlighted visible form in place of
                                    // the mention token and record the pick for frecency
                                    // ranking; the draft keeps the visible name (URNs only
                                    // appear on the wire). visibleText is the serializer's
                                    // key: the bare name for a unique display name, the
                                    // disambiguated "Name (username)" for a shared one.
                                    function insertComposerMention(row) {
                                        const start = composerMentionPopup.tokenStart;
                                        if (start < 0 || !row || typeof row !== "object")
                                            return;
                                        const insertion = row.markdown || ("@" + String(row.visibleText || row.displayName));
                                        const next = text.slice(0, start) + insertion + " " + text.slice(selectionEnd);
                                        const caret = start + insertion.length + 1;
                                        if (!row.markdown)
                                            root.controller.recordMentionPick(String(row.userUuid));
                                        const owner = root.controller.ownStatusContextKey;
                                        const stream = root.controller.selectedStream, topic = root.controller.selectedTopic;
                                        root.controller.draft = next;
                                        // Keep the draft binding alive across a subscriber navigation.
                                        if (formattingAllowed && owner === root.controller.ownStatusContextKey
                                            && stream === root.controller.selectedStream
                                            && topic === root.controller.selectedTopic) {
                                            if (text !== next)
                                                text = next;
                                            select(caret, caret);
                                        }
                                        composerMentionPopup.close();
                                    }
                                    Shortcut {
                                        sequence: "Ctrl+B"; context: Qt.WindowShortcut; autoRepeat: false
                                        enabled: composerText.formattingAllowed
                                        onActivated: composerText.wrapFormatting("**")
                                    }
                                    Shortcut {
                                        sequence: "Ctrl+I"; context: Qt.WindowShortcut; autoRepeat: false
                                        enabled: composerText.formattingAllowed
                                        onActivated: composerText.wrapFormatting("*")
                                    }
                                    Shortcut {
                                        sequence: "Ctrl+E"; context: Qt.WindowShortcut; autoRepeat: false
                                        enabled: composerText.formattingAllowed
                                        onActivated: composerText.wrapFormatting("`")
                                    }
                                    Shortcut {
                                        sequence: "Ctrl+Shift+X"; context: Qt.WindowShortcut; autoRepeat: false
                                        enabled: composerText.formattingAllowed
                                        onActivated: composerText.wrapFormatting("~~")
                                    }
                                    Keys.onShortcutOverride: event => {
                                        // AltGraph belongs to text input, including layouts whose
                                        // platform shortcut translation offers a Latin alternative.
                                        if (event.modifiers & Qt.GroupSwitchModifier)
                                            event.accepted = true;
                                    }
                                    onContentHeightChanged: if (composerEmojiPicker.visible) root.positionComposerEmojiPicker()
                                    Connections {
                                        target: root.controller
                                        // The edit buffer becomes the composer text and the stored
                                        // draft returns with the session, so the draft binding is
                                        // re-established even after direct typing broke it.
                                        function onEditSessionChanged() {
                                            composerText.text = Qt.binding(() => root.controller.draft);
                                            if (root.controller.editingMessageId.length && !composerText.readOnly)
                                                composerText.forceActiveFocus();
                                        }
                                    }
                                    Keys.onPressed: event => {
                                        if (readOnly) {
                                            event.accepted = true;
                                            return;
                                        }
                                        const editing = root.controller.editingMessageId.length > 0;
                                        if (event.matches(StandardKey.Paste)) {
                                            if (clipboardFilesDialog.openForClipboard() || root.controller.pasteAttachments()) {
                                                event.accepted = true;
                                                return;
                                            }
                                            const converted = root.controller.composerReferenceClipboardText();
                                            if (converted.length) {
                                                const at = selectionStart;
                                                remove(at, selectionEnd);
                                                insert(at, converted);
                                                event.accepted = true;
                                            }
                                        } else if (composerMentionPopup.visible && event.key === Qt.Key_Escape) {
                                            // Mention navigation comes first (message-composer-write-body-keydown.lib.ts).
                                            composerMentionPopup.close();
                                            event.accepted = true;
                                        } else if (composerMentionPopup.visible && event.key === Qt.Key_Down) {
                                            composerMentionPopup.activeIndex = Math.min(
                                                Math.max(0, composerMentionPopup.rows.length - 1),
                                                composerMentionPopup.activeIndex + 1);
                                            event.accepted = true;
                                        } else if (composerMentionPopup.visible && event.key === Qt.Key_Up) {
                                            composerMentionPopup.activeIndex = Math.max(0, composerMentionPopup.activeIndex - 1);
                                            event.accepted = true;
                                        } else if (composerMentionPopup.visible
                                                   && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                                   && !(event.modifiers & Qt.ShiftModifier) && !inputMethodComposing
                                                   && composerMentionPopup.rows.length > 0) {
                                            composerText.insertComposerMention(composerMentionPopup.rows[
                                                Math.min(composerMentionPopup.activeIndex, composerMentionPopup.rows.length - 1)]);
                                            event.accepted = true;
                                        } else if (event.key === Qt.Key_Escape && composerEmojiPicker.visible) {
                                            composerEmojiPicker.close();
                                            event.accepted = true;
                                        } else if (event.key === Qt.Key_Escape && editing) {
                                            // Source: Escape leaves the edit mode and is swallowed.
                                            root.controller.cancelEditMessage();
                                            event.accepted = true;
                                        } else if (event.key === Qt.Key_Escape && root.controller.replyMessageId.length > 0) {
                                            // The reply banner mirrors the edit banner: Escape dismisses and is swallowed.
                                            root.controller.cancelReply();
                                            event.accepted = true;
                                        } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                            && (event.modifiers & Qt.ShiftModifier) && !inputMethodComposing) {
                                            // Shift+Enter continues a markdown list item per
                                            // message-composer-list-continuation.lib.ts; a non-list
                                            // caret gets the plain newline inserted here so exactly
                                            // one text mutation happens per press.
                                            const continuation = listContinuation(text, selectionStart, selectionEnd);
                                            const nextText = continuation ? continuation.text
                                                : text.slice(0, selectionStart) + "\n" + text.slice(selectionEnd);
                                            const nextCaret = continuation ? continuation.caret : selectionStart + 1;
                                            const owner = root.controller.ownStatusContextKey;
                                            const stream = root.controller.selectedStream, topic = root.controller.selectedTopic;
                                            root.controller.draft = nextText;
                                            if (formattingAllowed && owner === root.controller.ownStatusContextKey
                                                && stream === root.controller.selectedStream
                                                && topic === root.controller.selectedTopic) {
                                                if (text !== nextText)
                                                    text = nextText;
                                                select(nextCaret, nextCaret);
                                            }
                                            event.accepted = true;
                                        } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                            && !(event.modifiers & Qt.ShiftModifier) && !inputMethodComposing && root.controller.uploadPending) {
                                            event.accepted = true;
                                        } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && !(event.modifiers & Qt.ShiftModifier) && !root.controller.busy && !inputMethodComposing) {
                                            if (editing) {
                                                if (root.controller.canSaveEdit) {
                                                    root.controller.submitEditMessage();
                                                    event.accepted = true;
                                                }
                                            } else if (root.controller.canSend) {
                                                root.controller.send();
                                                event.accepted = true;
                                            }
                                        } else if (event.key === Qt.Key_Up && !(event.modifiers & Qt.ShiftModifier) && !editing
                                                   && text.trim().length === 0 && root.composerAvailable && !root.controller.busy) {
                                            // Up-to-edit-last, the Source composer.editLastMessage hotkey.
                                            root.controller.editLastMessage();
                                            event.accepted = true;
                                        }
                                    }
                                }
                            }
                            RowLayout {
                                spacing: 0
                                Layout.alignment: Qt.AlignBottom
                                NativeIconButton {
                                    objectName: "attachButton"
                                    colors: root.colors
                                    asset: "composer-attach-compact.svg"
                                    help: qsTr("Attach files")
                                    enabled: root.composerAvailable && root.controller.canAttach
                                    implicitWidth: 32
                                    implicitHeight: 32
                                    glyphWidth: 14
                                    glyphHeight: 24
                                    onClicked: {
                                        root.pickerOwner = root.controller.ownerKey;
                                        root.pickerEditSession = root.controller.editSessionKey;
                                        root.pickerStream = root.controller.selectedStream;
                                        root.pickerTopic = root.controller.selectedTopic;
                                        attachmentPicker.open();
                                    }
                                }
                                NativeIconButton {
                                    id: composerEmojiButton
                                    objectName: "composerEmojiButton"
                                    colors: root.colors
                                    asset: "composer-emoji.svg"
                                    help: qsTr("Emoji")
                                    enabled: root.composerAvailable && composerText.enabled
                                    implicitWidth: 32
                                    implicitHeight: 32
                                    glyphSize: 24
                                    onClicked: root.toggleComposerEmojiPicker()
                                }
                                // Issue #215 poll trigger (message-composer.ui.tsx PollButton):
                                // sits after the emoji trigger, hidden while an edit session is
                                // open and on channels without poll support.
                                NativeIconButton {
                                    id: composerPollButton
                                    objectName: "pollButton"
                                    colors: root.colors
                                    asset: "poll.svg"
                                    help: qsTr("Create poll")
                                    visible: root.controller.pollCreationAllowed
                                        && root.controller.editingMessageId.length === 0
                                    enabled: root.composerAvailable
                                    implicitWidth: 32
                                    implicitHeight: 32
                                    glyphSize: 24
                                    onClicked: pollCreateDialog.openForCreate()
                                }
                            }
                        }
                    }
                    NativeIconButton {
                        id: sendControl
                        colors: root.colors
                        // The Source reuses the send control as the save control while an
                        // edit session is open (message-composer.ui.tsx aria-label common.save).
                        objectName: root.controller.editingMessageId.length ? "saveEditButton" : "sendButton"
                        asset: "composer-send.svg"
                        help: root.controller.editingMessageId.length ? qsTr("Save") : qsTr("Send message")
                        iconColor: root.colors["on-accent"]
                        implicitWidth: 40
                        implicitHeight: 40
                        glyphSize: 24
                        glyphHeight: 20
                        enabled: root.controller.editingMessageId.length
                            ? root.controller.canSaveEdit && !root.controller.busy
                            : root.composerAvailable && root.controller.canSend && !root.controller.busy
                        Layout.alignment: Qt.AlignBottom
                        background: Rectangle {
                            radius: 10
                            color: root.colors["composer-send"]
                            opacity: sendControl.enabled ? (sendControl.down ? 0.8 : 1) : 0.5
                        }
                        onClicked: {
                            if (root.controller.editingMessageId.length)
                                root.controller.submitEditMessage();
                            else
                                root.controller.send();
                        }
                    }
                    }
                    // Issue #122: the expandable bottom toolbar (message-composer-toolbar.ui.tsx).
                    // COMPOSER_TOOLBAR_SEND_CLEARANCE mr-[52px] from the composer's right edge
                    // (40px send control + 12px row gap) and the pl-2 leading inset.
                    Item {
                        id: composerToolbarRow
                        objectName: "composerToolbarRow"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 32
                        Layout.rightMargin: 52
                        visible: root.preferences.composerToolbarExpanded
                        RowLayout {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: 8
                            spacing: 8
                            NativeIconButton {
                                objectName: "composerToolbarCollapse"
                                colors: root.colors
                                asset: "composer-bottom-panel-close.svg"
                                help: qsTr("Hide formatting tools")
                                enabled: composerText.enabled
                                implicitWidth: 32
                                implicitHeight: 32
                                glyphSize: 21
                                onClicked: root.preferences.composerToolbarExpanded = false
                            }
                            // message-composer-toolbar.ui.tsx separators: h-7 w-px bg-border-subtle.
                            Rectangle {
                                objectName: "composerToolbarSeparator"
                                Layout.preferredWidth: 1
                                Layout.preferredHeight: 28
                                Layout.alignment: Qt.AlignVCenter
                                color: root.colors["border-subtle"]
                            }
                            // TOOLBAR_TEXT_STYLE_GROUP: link/B/I/S sit flush (gap-0).
                            RowLayout {
                                spacing: 0
                                NativeIconButton {
                                    id: composerFormatLinkButton
                                    objectName: "composerFormatLink"
                                    colors: root.colors
                                    asset: "composer-add-link.svg"
                                    help: qsTr("Link")
                                    focusPolicy: Qt.StrongFocus
                                    enabled: composerText.enabled
                                    implicitWidth: 32
                                    implicitHeight: 32
                                    glyphWidth: 24
                                    glyphHeight: 15
                                    onClicked: { composerText.forceActiveFocus(); composerText.insertLink(); }
                                }
                                NativeIconButton {
                                    objectName: "composerFormatBold"
                                    colors: root.colors
                                    asset: "composer-format-bold.svg"
                                    help: qsTr("Bold")
                                    focusPolicy: Qt.StrongFocus
                                    enabled: composerText.enabled
                                    implicitWidth: 32
                                    implicitHeight: 32
                                    glyphWidth: 11
                                    glyphHeight: 17
                                    onClicked: { composerText.forceActiveFocus(); composerText.wrapFormatting("**"); }
                                }
                                NativeIconButton {
                                    objectName: "composerFormatItalic"
                                    colors: root.colors
                                    asset: "composer-format-italic.svg"
                                    help: qsTr("Italic")
                                    focusPolicy: Qt.StrongFocus
                                    enabled: composerText.enabled
                                    implicitWidth: 32
                                    implicitHeight: 32
                                    glyphWidth: 15
                                    glyphHeight: 17
                                    onClicked: { composerText.forceActiveFocus(); composerText.wrapFormatting("*"); }
                                }
                                NativeIconButton {
                                    objectName: "composerFormatStrikethrough"
                                    colors: root.colors
                                    asset: "composer-strikethrough.svg"
                                    help: qsTr("Strikethrough")
                                    focusPolicy: Qt.StrongFocus
                                    enabled: composerText.enabled
                                    implicitWidth: 32
                                    implicitHeight: 32
                                    glyphWidth: 25
                                    glyphHeight: 20
                                    onClicked: { composerText.forceActiveFocus(); composerText.wrapFormatting("~~"); }
                                }
                            }
                            Rectangle {
                                objectName: "composerToolbarSeparator"
                                Layout.preferredWidth: 1
                                Layout.preferredHeight: 28
                                Layout.alignment: Qt.AlignVCenter
                                color: root.colors["border-subtle"]
                            }
                            NativeIconButton {
                                objectName: "composerFormatNumbered"
                                colors: root.colors
                                asset: "composer-list-numbered.svg"
                                help: qsTr("Numbered list")
                                focusPolicy: Qt.StrongFocus
                                enabled: composerText.enabled
                                implicitWidth: 32
                                implicitHeight: 32
                                glyphWidth: 21
                                glyphHeight: 24
                                onClicked: { composerText.forceActiveFocus(); composerText.insertNumberedList(); }
                            }
                            NativeIconButton {
                                objectName: "composerFormatBulleted"
                                colors: root.colors
                                asset: "composer-list-bulleted.svg"
                                help: qsTr("Bulleted list")
                                focusPolicy: Qt.StrongFocus
                                enabled: composerText.enabled
                                implicitWidth: 32
                                implicitHeight: 32
                                glyphWidth: 21
                                glyphHeight: 20
                                onClicked: { composerText.forceActiveFocus(); composerText.insertBulletedList(); }
                            }
                            Rectangle {
                                objectName: "composerToolbarSeparator"
                                Layout.preferredWidth: 1
                                Layout.preferredHeight: 28
                                Layout.alignment: Qt.AlignVCenter
                                color: root.colors["border-subtle"]
                            }
                            NativeIconButton {
                                objectName: "composerFormatQuote"
                                colors: root.colors
                                asset: "composer-chat-dashed.svg"
                                help: qsTr("Quote")
                                focusPolicy: Qt.StrongFocus
                                enabled: composerText.enabled
                                implicitWidth: 32
                                implicitHeight: 32
                                glyphWidth: 24
                                glyphHeight: 21
                                onClicked: { composerText.forceActiveFocus(); composerText.insertQuote(); }
                            }
                            NativeIconButton {
                                objectName: "composerFormatCodeBlock"
                                colors: root.colors
                                asset: "composer-frame-source.svg"
                                help: qsTr("Code block")
                                focusPolicy: Qt.StrongFocus
                                enabled: composerText.enabled
                                implicitWidth: 32
                                implicitHeight: 32
                                glyphWidth: 21
                                glyphHeight: 21
                                onClicked: { composerText.forceActiveFocus(); composerText.insertCodeBlock(); }
                            }
                            NativeIconButton {
                                objectName: "composerFormatCode"
                                colors: root.colors
                                asset: "composer-inline-code.svg"
                                help: qsTr("Code")
                                focusPolicy: Qt.StrongFocus
                                enabled: composerText.enabled
                                implicitWidth: 32
                                implicitHeight: 32
                                glyphWidth: 22
                                glyphHeight: 18
                                onClicked: { composerText.forceActiveFocus(); composerText.wrapFormatting("`"); }
                            }
                            NativeIconButton {
                                id: composerFormatSpoilerButton
                                objectName: "composerFormatSpoiler"
                                colors: root.colors
                                asset: "composer-visibility.svg"
                                help: qsTr("Spoiler")
                                focusPolicy: Qt.StrongFocus
                                enabled: composerText.enabled
                                implicitWidth: 32
                                implicitHeight: 32
                                glyphWidth: 26
                                glyphHeight: 17
                                onClicked: { composerText.forceActiveFocus(); composerText.wrapFormatting("||"); }
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: 28
                                    height: 1
                                    rotation: -35
                                    color: composerFormatSpoilerButton.enabled
                                        ? composerFormatSpoilerButton.iconColor
                                        : root.colors["icon-disable"]
                                }
                            }
                        }
                    }
                }
            }
        }
        NativeDraftsPage {
            controller: root.controller
            colors: root.colors
            visible: root.controller.draftsVisible
            loadActive: root.visible && root.controller.authenticated
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
        NativeMentionsPage {
            controller: root.controller
            colors: root.colors
            visible: root.controller.mentionsVisible
            loadActive: root.visible && root.controller.authenticated
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
        NativeStarredPage {
            controller: root.controller
            colors: root.colors
            visible: root.controller.starredVisible
            loadActive: root.visible && root.controller.authenticated
            Layout.fillWidth: true
            Layout.fillHeight: true
            onForwardRequested: function (messageUuid) {
                root.starredForwardIds = [messageUuid]
                forwardDialog.open()
            }
        }
        // Activity reactions unsupported state (#43): the reference has no
        // reactions activity collection, so the /activity/reactions route
        // renders the muted single-line state (docs addendum). There is
        // deliberately no sidebar chip: the reference keeps reactions out of
        // VISIBLE_MY_ACTIVITY_KEYS (sidebar.lib.ts:90-96).
        NativeActivityUnsupportedPage {
            controller: root.controller
            colors: root.colors
            visible: root.controller.reactionsVisible
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
        // Favorites self-chat page (#70, docs/NATIVE_DESKTOP_FAVORITES_SELF_CHAT.md):
        // the reference renders the header with either the spinner or the
        // error card (chat-page.ui.tsx:46-70); the ready state renders the
        // conversation column itself with the favorites header presentation.
        NativeFavoritesPage {
            controller: root.controller
            colors: root.colors
            visible: root.controller.favoritesVisible && root.controller.favoritesStatus !== "ready"
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
        NativeInboxPage {
            controller: root.controller
            colors: root.colors
            visible: root.controller.inboxVisible
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
        // Project feed page (#38, docs/NATIVE_DESKTOP_FEED.md): cross-chat
        // "All messages" reached through the /feed route; the reference keeps
        // the sidebar rail entry hidden, so there is deliberately no native
        // rail entry for it either.
        NativeFeedPage {
            controller: root.controller
            colors: root.colors
            visible: root.controller.feedVisible
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
        // Mail section page (issue #35): the unavailable-state fallback card is
        // the only state the shipping desktop product exposes for /mail
        // (workspace_ui 61dacef7 electron main.ts:452); WebView is forbidden.
        NativeSectionUnavailablePage {
            objectName: "mailSectionPage"
            colors: root.colors
            asset: "mail.svg"
            heading: qsTr("Mail")
            body: qsTr("This section is temporarily unavailable in web mode.")
            visible: root.controller.section === "mail"
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
        // Calendar section page (issue #36): like Mail, the packaged desktop
        // reference exposes only the unavailable fallback card for /calendar
        // (workspace_ui 61dacef7 calendar-page.ui.tsx + electron main.ts:452);
        // WebView is forbidden. The shared component already serves any
        // section via heading/body/asset (nav.calendar, app.webModeUnavailable).
        NativeSectionUnavailablePage {
            objectName: "calendarSectionPage"
            colors: root.colors
            asset: "calendar.svg"
            heading: qsTr("Calendar")
            body: qsTr("This section is temporarily unavailable in web mode.")
            visible: root.controller.section === "calendar"
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
        // Recent-calls section page (issue #37): the reference /call and
        // /calls routes render the recent-Jitsi-calls page in its empty
        // state (workspace_ui 61dacef7 calls-page.ui.tsx). Live calling is a
        // separate decision; the top-bar Calls chip stays behind the
        // reference TOP_BAR_CALLS_NAV gate, which the native build settings
        // do not introduce, so no chip is rendered here either.
        NativeCallsPage {
            colors: root.colors
            visible: root.controller.section === "calls"
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
        // Services placeholder hub (issue #45): the reference /services and
        // /all-services routes render the static planned-services hub
        // (workspace_ui 61dacef7 services-page.ui.tsx,
        // app-route-definitions.tsx:166-167,203-204). The top-bar Services
        // button stays behind the reference build gate
        // (top-bar.test.tsx:153), so no chip is rendered here either; the
        // whitelisted deep links are the entry surfaces.
        NativeServicesPage {
            colors: root.colors
            viewportWidth: root.width
            visible: root.controller.section === "services"
            onGoToChats: {
                root.controller.showSection("chat")
                root.controller.closeDrafts()
                root.sidebarQuery = ""
            }
            Layout.fillWidth: true
            Layout.fillHeight: true
        }

        // Open Source Licenses page (issue #46,
        // docs/NATIVE_DESKTOP_LICENSES.md): the reference /licenses route.
        // Close resolves like the reference tests (licenses-page.ui.test.tsx
        // lines 47 and 56): history back when a non-chat section preceded
        // the open, else the messenger home (the native chat section).
        NativeLicensesPage {
            colors: root.colors
            datasetJson: visible ? root.controller.licensesDatasetJson() : ""
            visible: root.controller.section === "licenses"
            canGoBack: root.controller.sectionBeforeLicenses !== "chat"
            Layout.fillWidth: true
            Layout.fillHeight: true
            onBackRequested: root.controller.showSection(root.controller.sectionBeforeLicenses)
            onMessengerHomeRequested: root.controller.showSection("chat")
            onExternalLinkRequested: url => Qt.openUrlExternally(url)
        }
        // Diagnostics page (issue #50, docs/NATIVE_DESKTOP_DIAGNOSTICS_PAGE.md):
        // the reference /settings/logs connection dashboard. The profile menu
        // entry and the whitelisted /logs + settings/logs deep links are the
        // entry surfaces (right-panel-user-menu.ui.tsx:182,372-376); chat
        // navigation resets the section back to the conversation, matching the
        // reference route behaviour.
        NativeDiagnosticsPage {
            controller: root.controller
            colors: root.colors
            visible: root.controller.section === "diagnostics"
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
    }
    component MessageBoundaryRow: Item {
        property Item readBoundary
        property string readId
        property string dispatchReadId
        property string serviceGroupId: ""
        property int displayIndex: -1
        function settleGeometry() {}
    }

    component MessageMenuAction: MenuItem {
        id: action
        property string asset: ""
        implicitWidth: 240
        implicitHeight: 36
        height: visible ? implicitHeight : 0
        leftPadding: 12
        rightPadding: 12
        topPadding: 8
        bottomPadding: 8
        spacing: 8
        font.pixelSize: 14
        font.weight: Font.Normal
        icon.source: asset.length ? "qrc:/workspace/icons/" + asset : ""
        icon.width: 20
        icon.height: 20
        icon.color: root.colors["icon-base"]
        palette.windowText: root.colors["text-primary"]
        opacity: enabled ? 1 : 0.5
        background: Rectangle {
            color: action.highlighted || action.hovered ? root.colors["sidebar-item-hover"] : "transparent"
        }
    }

    component MessageMenuSeparator: MenuSeparator {
        implicitHeight: 9
        height: visible ? implicitHeight : 0
        leftPadding: 8
        rightPadding: 8
        topPadding: 4
        bottomPadding: 4
        contentItem: Rectangle {
            implicitHeight: 1
            color: root.colors["border-subtle"]
        }
    }

    Popup {
        id: composerEmojiPicker
        objectName: "composerEmojiPicker"
        parent: Overlay.overlay
        width: Math.min(320, root.width - 16)
        height: Math.min(322, root.height - 16)
        padding: 1
        modal: false
        dim: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onWidthChanged: if (visible) root.positionComposerEmojiPicker()
        onHeightChanged: if (visible) root.positionComposerEmojiPicker()
        onOpened: composerEmojiChoices.focusSearch()
        onClosed: {
            root.composerEmojiContext = "";
            root.composerEmojiStream = "";
            root.composerEmojiTopic = "";
            root.composerEmojiKind = "";
        }
        background: Rectangle {
            color: root.colors["bg-elevated"]
            radius: 12
            border.color: root.colors["border-subtle"]
        }
        contentItem: NativeReactionPicker {
            id: composerEmojiChoices
            choices: root.controller.reactionChoices
            suggested: root.preferences.suggestedReactions
            dark: theme.dark
            active: composerEmojiPicker.visible
            allowed: root.composerEmojiAllowed && composerText.enabled
            onChosen: name => root.pickComposerEmoji(name)
        }
    }
    Connections {
        target: root
        function onWidthChanged() { if (composerEmojiPicker.visible) root.positionComposerEmojiPicker(); }
        function onHeightChanged() { if (composerEmojiPicker.visible) root.positionComposerEmojiPicker(); }
    }

    // Issue #123: @mention suggestion dropdown above the composer. The composer
    // keeps keyboard focus (focus: false) so caret and typing drive navigation.
    Popup {
        id: composerMentionPopup
        objectName: "composerMentionPopup"
        property bool references: false
        Connections {
            target: root.controller
            enabled: composerMentionPopup.visible && !composerMentionPopup.references
            function onMentionMembershipChanged() {
                const current = composerMentionPopup.rows[composerMentionPopup.activeIndex];
                const rows = root.controller.mentionSuggestions(composerMentionPopup.query);
                composerMentionPopup.rows = rows;
                composerMentionPopup.activeIndex = Math.max(0, rows.findIndex(row => row.userUuid === current?.userUuid));
            }
        }
        property var rows: []
        property int activeIndex: 0
        property string query: ""
        property int tokenStart: -1
        parent: Overlay.overlay
        width: Math.min(320, root.width - 16)
        height: 8 + Math.min(52 * Math.max(rows.length, 1), 192)
        padding: 4
        modal: false
        dim: false
        focus: false
        closePolicy: Popup.CloseOnPressOutside
        onClosed: {
            composerMentionPopup.rows = [];
            composerMentionPopup.activeIndex = 0;
            composerMentionPopup.query = "";
            composerMentionPopup.tokenStart = -1;
        }
        background: Rectangle {
            color: root.colors["bg-elevated"]
            radius: 12
            border.color: root.colors["border-subtle"]
        }
        contentItem: Flickable {
            id: composerMentionList
            objectName: "composerMentionList"
            // Positional Repeater over a Column (the SessionSwitcher popup
            // pattern): delegates stay stable while the candidate list refreshes
            // and always read live candidates through the popup id.
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            contentHeight: composerMentionPopup.rows.length * 52
            Column {
                id: composerMentionRows
                objectName: "composerMentionRows"
                width: composerMentionList.width
                Repeater {
                    model: composerMentionPopup.rows.length
                    delegate: Rectangle {
                        required property int index
                        readonly property var mentionRow: composerMentionPopup.rows[index]
                        objectName: "composerMentionRow" + index
                        width: parent.width
                        height: 52
                        color: index === composerMentionPopup.activeIndex ? root.colors["bg"] : "transparent"
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            anchors.topMargin: 8
                            anchors.bottomMargin: 8
                            spacing: 8
                            // WorkspaceAvatar size="sm": 32px circle, 12px initials,
                            // first character of the display name (username fallback).
                            Rectangle {
                                Layout.preferredWidth: composerMentionPopup.references ? 28 : 32
                                Layout.preferredHeight: composerMentionPopup.references ? 28 : 32
                                Layout.alignment: Qt.AlignVCenter
                                radius: composerMentionPopup.references ? 4 : 16
                                color: root.colors["bg"]
                                Text {
                                    anchors.centerIn: parent
                                    text: mentionRow.kind === "topic" ? "›" : String(root.composerMentionInitials(mentionRow))
                                    font.pixelSize: 12
                                    color: root.colors["text-primary"]
                                }
                                PresenceBadge {
                                    visible: !composerMentionPopup.references
                                    presence: String(mentionRow.status || "")
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 36
                                spacing: 0
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    Text {
                                        objectName: "composerMentionName" + index
                                        Layout.fillWidth: true
                                        // visibleText matches what Enter inserts and what
                                        // the send serializer converts: duplicated display
                                        // names show the "(username)" disambiguation here.
                                        text: String(mentionRow.visibleText || mentionRow.displayName || "")
                                        elide: Text.ElideRight
                                        font.pixelSize: 14
                                        color: root.colors["text-primary"]
                                    }
                                    // `outside channel` chip (text-[10px], px-1.5 py-0.5,
                                    // rounded): rendered only when the fact is known.
                                    Rectangle {
                                        visible: mentionRow.outsideChannel === true
                                        Layout.preferredWidth: mentionOutsideLabel.implicitWidth + 12
                                        Layout.preferredHeight: mentionOutsideLabel.implicitHeight + 4
                                        Layout.alignment: Qt.AlignVCenter
                                        radius: 4
                                        color: root.colors["bg"]
                                        Text {
                                            id: mentionOutsideLabel
                                            objectName: "composerMentionOutsideLabel" + index
                                            anchors.centerIn: parent
                                            text: qsTr("not in channel")
                                            font.pixelSize: 10
                                            color: root.colors["text-muted"]
                                        }
                                    }
                                }
                                // Secondary line: @username when present, else the email.
                                Text {
                                    objectName: "composerMentionSecondary" + index
                                    Layout.fillWidth: true
                                    text: composerMentionPopup.references ? String(mentionRow.secondary || "")
                                        : mentionRow.username ? "@" + String(mentionRow.username)
                                        : String(mentionRow.email || "")
                                    visible: text.length > 0
                                    elide: Text.ElideRight
                                    font.pixelSize: 11
                                    color: root.colors["text-secondary"]
                                }
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            acceptedButtons: Qt.LeftButton
                            hoverEnabled: true
                            onEntered: composerMentionPopup.activeIndex = index
                            onClicked: composerText.insertComposerMention(mentionRow)
                        }
                    }
                }
            }
        }
        Text {
            objectName: "composerMentionNoResults"
            anchors.centerIn: parent
            visible: composerMentionPopup.rows.length === 0
            text: qsTr("No results found")
            font.pixelSize: 12
            color: root.colors["text-muted"]
        }
    }

    Popup {
        id: reactionPicker
        objectName: "reactionPicker"
        parent: Overlay.overlay
        width: Math.min(320, root.width - 16)
        height: Math.min(360, root.height - 16)
        padding: 1
        modal: true
        dim: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: root.colors["bg-elevated"]
            radius: 12
            border.color: root.colors["border-subtle"]
        }
        onClosed: {
            root.reactionMessage = "";
            messageMenu.close();
        }
        contentItem: NativeReactionPicker {
            id: fullReactionPicker
            choices: root.controller.reactionChoices
            suggested: root.preferences.suggestedReactions
            dark: theme.dark
            active: reactionPicker.visible
            allowed: root.reactionPickerAllowed
            onChosen: emojiName => root.pickReaction(emojiName)
        }
    }

    Rectangle {
        parent: messageList
        x: 8
        y: 8
        z: 10
        visible: root.controller.authenticated && root.controller.cacheStale
        width: Math.min(Math.max(0, messageList.width - 16), Math.ceil(cacheStatus.implicitWidth) + 20 + (cacheActivity.visible ? 22 : 0))
        height: 26
        radius: 13
        readonly property color surface: root.colors["bg-elevated"]
        color: Qt.rgba(surface.r, surface.g, surface.b, 0.95)
        border.color: root.colors["border-subtle"]
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 8
            BusyIndicator {
                id: cacheActivity
                objectName: "cacheUpdatingIndicator"
                visible: root.controller.busy
                running: visible
                padding: 0
                palette.dark: root.colors.accent
                Layout.preferredWidth: 14
                Layout.preferredHeight: 14
            }
            Text {
                id: cacheStatus
                objectName: "cacheStatus"
                Layout.fillWidth: true
                text: root.controller.busy ? messageList.messageSnapshot.length > 0 ? qsTr("Showing saved messages · Updating…") : qsTr("Showing saved chats · Updating…") : messageList.messageSnapshot.length > 0 ? qsTr("Showing saved messages · Update failed") : qsTr("Showing saved chats · Update failed")
                textFormat: Text.PlainText
                elide: Text.ElideRight
                font.pixelSize: 12
                color: root.colors["text-muted"]
                Accessible.role: Accessible.StatusBar
                Accessible.name: text
            }
        }
    }

    Dialog {
        id: clipboardFilesDialog
        objectName: "clipboardFilesDialog"
        property string contextKey: ""
        property string stream: ""
        property string topic: ""
        property string editSession: ""
        function openForClipboard() {
            const result = root.controller.prepareClipboardFiles();
            if (result === 2) return true; // recognized files were rejected; consume the paste
            if (result !== 1) return false;
            contextKey = root.controller.ownStatusContextKey;
            stream = root.controller.selectedStream; topic = root.controller.selectedTopic;
            editSession = root.controller.editSessionKey;
            open(); return true;
        }
        function contextMatches() {
            return root.controller.canAttach && contextKey === root.controller.ownStatusContextKey
                && stream === root.controller.selectedStream && topic === root.controller.selectedTopic
                && editSession === root.controller.editSessionKey;
        }
        anchors.centerIn: parent; width: Math.min(root.width - 48, 520); modal: true
        title: qsTr("Upload files from clipboard?")
        contentItem: ScrollView {
            implicitHeight: Math.min(300, paths.implicitHeight)
            Column {
                id: paths; width: clipboardFilesDialog.width - clipboardFilesDialog.leftPadding - clipboardFilesDialog.rightPadding
                spacing: 8
                Label { width: parent.width; text: root.controller.clipboardFilesPreparing ? qsTr("Preparing files…") : qsTr("Check the file paths before uploading."); wrapMode: Text.Wrap }
                Repeater {
                    model: root.controller.clipboardFilesPreview
                    Column {
                        required property var modelData
                        width: paths.width
                        Label { width: parent.width; text: String(parent.modelData.path); wrapMode: Text.WrapAnywhere; textFormat: Text.PlainText }
                        Label { width: parent.width; visible: Boolean(parent.modelData.target); text: String(parent.modelData.target || ""); wrapMode: Text.WrapAnywhere; textFormat: Text.PlainText }
                    }
                }
            }
        }
        footer: RowLayout {
            Item { Layout.fillWidth: true }
            Button { text: qsTr("Cancel"); onClicked: clipboardFilesDialog.reject() }
            Button {
                objectName: "clipboardFilesConfirm"; text: qsTr("Upload")
                enabled: !root.controller.clipboardFilesPreparing && root.controller.clipboardFilesPreview.length > 0
                onClicked: {
                    if (clipboardFilesDialog.contextMatches()) root.controller.attachClipboardFiles();
                    else clipboardFilesDialog.reject();
                }
            }
        }
        onClosed: root.controller.dismissClipboardFiles()
        Connections {
            target: root.controller
            function onStateChanged() {
                if (clipboardFilesDialog.visible && (!clipboardFilesDialog.contextMatches()
                        || (!root.controller.clipboardFilesPreparing && !root.controller.clipboardFilesPreview.length)))
                    clipboardFilesDialog.close();
            }
        }
    }

    NativeDialogs.FileDialog {
        id: attachmentPicker
        objectName: "attachmentPicker"
        title: qsTr("Attach files")
        fileMode: NativeDialogs.FileDialog.OpenFiles
        onAccepted: {
            if (root.controller.canAttach && root.pickerOwner === root.controller.ownerKey && root.pickerEditSession === root.controller.editSessionKey && root.pickerStream === root.controller.selectedStream && root.pickerTopic === root.controller.selectedTopic)
                root.controller.attachFiles(selectedFiles);
        }
    }

    NativeDialogs.FileDialog {
        id: attachmentSaveDialog
        objectName: "attachmentSaveDialog"
        title: qsTr("Save attachment")
        fileMode: NativeDialogs.FileDialog.SaveFile
        onAccepted: {
            if (root.controller.authenticated && root.downloadOwner === root.controller.ownerKey && root.downloadId.length > 0)
                root.controller.downloadFile(root.downloadId, selectedFile);
            root.downloadId = "";
        }
        onRejected: root.downloadId = ""
    }

    Dialog {
        id: outgoingDiscard
        objectName: "discardPendingOutgoingDialog"
        property string pendingId: ""
        property string pendingOwner: ""
        function openForPending() {
            pendingId = String(root.pendingOutgoing.id || ""); pendingOwner = root.controller.ownerKey;
            if (pendingId.length > 0) open();
        }
        anchors.centerIn: parent
        width: Math.min(root.width - 48, 464)
        modal: true; title: qsTr("Discard pending message?")
        standardButtons: Dialog.Cancel | Dialog.Discard
        contentItem: Label {
            text: qsTr("The message may already have been sent. Discard its pending status and keep the current draft?")
            wrapMode: Text.Wrap; textFormat: Text.PlainText
        }
        onDiscarded: {
            if (pendingId === String(root.pendingOutgoing.id || "") && pendingOwner === root.controller.ownerKey)
                root.controller.discardPendingSend();
        }
        Connections {
            target: root.controller
            function onStateChanged() {
                if (outgoingDiscard.visible && (outgoingDiscard.pendingId !== String(root.pendingOutgoing.id || "")
                    || outgoingDiscard.pendingOwner !== root.controller.ownerKey)) outgoingDiscard.close();
            }
        }
    }

    Dialog {
        id: referenceNotice
        anchors.centerIn: parent
        width: Math.min(root.width - 48, 464)
        contentWidth: width - leftPadding - rightPadding
        modal: true
        title: qsTr("Workspace reference")
        standardButtons: Dialog.Ok
        contentItem: Label {
            text: qsTr("Profiles, referenced messages and attachment previews are still under development.")
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }
    }

    // #108 (docs/NATIVE_DESKTOP_MESSAGE_TIMESTAMP_VIEWER.md): read-only view of
    // the instant stored by a message time chip. Parity with the reference
    // AnchoredPopover (rounded-xl border border-subtle bg-elevated shell,
    // role=dialog named by dateTimePicker.label) hosting a read-only
    // DateTimePicker: PANEL_WIDTH 371 (:9), max height viewport - 16 px (:67),
    // seconds only when the stored instant has them (:75), Escape/outside click
    // dismisses and focus returns to the trigger (:63-66).
    Popup {
        id: messageTimePanel
        objectName: "messageTimePanel"
        width: 371
        padding: 0
        modal: false
        dim: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        // Reference panel style: maxHeight calc(100dvh - 16px) (:67).
        height: Math.max(0, Math.min(implicitHeight, root.height - 16))
        x: root.messageTimePanelTopLeft.x
        y: root.messageTimePanelTopLeft.y
        background: Rectangle {
            radius: 12
            color: root.colors["bg-elevated"]
            border.color: root.colors["border-subtle"]
        }
        onClosed: {
            const anchor = root.messageTimeAnchor
            root.messageTimeAnchor = null
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
            Accessible.name: qsTr("Date and time")
            Text {
                objectName: "messageTimePanelDate"
                topPadding: 14
                bottomPadding: 6
                text: root.messageTimeMonthCaption
                textFormat: Text.PlainText
                font.pixelSize: 14
                font.weight: Font.Medium
                color: root.colors["text-primary"]
                // Review finding 4638145299: a screen reader must read the
                // month caption, the highlighted day and the time value.
                Accessible.role: Accessible.StaticText
                Accessible.name: root.messageTimeMonthCaption
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
                        text: Qt.locale(root.preferences.locale).dayName(((index + Qt.locale(root.preferences.locale).firstDayOfWeek - 1) % 7) + 1, Locale.NarrowFormat)
                        textFormat: Text.PlainText
                        font.pixelSize: 12
                        color: root.colors["text-muted"]
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
                    model: root.messageTimeCalendar
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
                            visible: Number(messageTimeDayCell.modelData) === root.messageTimeSelectedDay
                            color: root.colors["accent"]
                        }
                        Text {
                            anchors.centerIn: parent
                            objectName: Number(messageTimeDayCell.modelData) === 0 ? ""
                                : Number(messageTimeDayCell.modelData) === root.messageTimeSelectedDay ? "messageTimePanelSelectedDay"
                                : "messageTimePanelDay-" + Number(messageTimeDayCell.modelData)
                            visible: Number(messageTimeDayCell.modelData) !== 0
                            text: Number(messageTimeDayCell.modelData) === 0 ? "" : String(Number(messageTimeDayCell.modelData))
                            textFormat: Text.PlainText
                            font.pixelSize: 13
                            color: Number(messageTimeDayCell.modelData) === root.messageTimeSelectedDay
                                ? root.colors["bg-elevated"] : root.colors["text-primary"]
                            // Only the highlighted day is announced (finding
                            // 4638145299); blank and unselected cells stay out
                            // of the accessibility tree.
                            Accessible.ignored: Number(messageTimeDayCell.modelData) !== root.messageTimeSelectedDay
                            Accessible.role: Accessible.StaticText
                            Accessible.name: text
                        }
                    }
                }
            }
            Rectangle {
                width: parent.width
                height: 1
                color: root.colors["border-subtle"]
                Accessible.ignored: true
            }
            Row {
                objectName: "messageTimePanelTimeRow"
                topPadding: 12
                bottomPadding: 16
                spacing: 10
                Text {
                    objectName: "messageTimePanelTimeLabel"
                    text: qsTr("Time")
                    textFormat: Text.PlainText
                    font.pixelSize: 13
                    color: root.colors["text-muted"]
                    Accessible.role: Accessible.StaticText
                    Accessible.name: qsTr("Time")
                }
                Text {
                    objectName: "messageTimePanelTime"
                    text: root.messageTimeClock(root.messageTimeInstant)
                    textFormat: Text.PlainText
                    font.pixelSize: 14
                    font.weight: Font.Medium
                    color: root.colors["text-primary"]
                    // Review finding 4638145299: expose the stored time value.
                    Accessible.role: Accessible.StaticText
                    Accessible.name: root.messageTimeClock(root.messageTimeInstant)
                }
            }
            }
        }
    }

    Menu {
        id: streamMenu
        objectName: "streamContextMenu"
        width: 280
        focus: true
        topPadding: 5
        bottomPadding: 5
        leftPadding: 1
        rightPadding: 1
        modal: false
        dim: false
        background: Rectangle {
            radius: 6
            color: root.colors["bg-elevated"]
            border.color: root.colors["border-subtle"]
        }
        // Issue #97 (docs/NATIVE_DESKTOP_SIDEBAR_MUTE_INDICATORS.md): the first
        // section is the stream Notifications level switch (reference
        // notificationPickerItem: heading + size-sm segmented switch, clicking a
        // level closes the menu and persists the wire mode). No hover background:
        // the reference custom item is not a clickable row. Issue #442: like the
        // reference's type-"custom" dropdown entry it must also stay out of the
        // menu's keyboard highlight navigation (arrow keys keep stepping rename
        // -> new topic -> archive). QQuickMenu can only make a MenuItem the
        // current item, so the block is a plain Item delegate wrapper: that
        // removes it from highlight navigation while its segment Buttons stay
        // clickable, which a disabled MenuItem would not. The MouseArea keeps the
        // pinned #97 fallthrough: a press that lands on the heading or on a
        // disabled segment (inert plain-Item area, below the Column in stacking
        // order so live segments win) closes the menu without posting anything.
        Item {
            objectName: "streamNotificationItem"
            implicitWidth: 278
            implicitHeight: streamNotificationColumn.implicitHeight + 8
            Accessible.ignored: true
            MouseArea {
                anchors.fill: parent
                onClicked: streamMenu.close()
            }
            Column {
                id: streamNotificationColumn
                x: 8
                y: 4
                width: parent.width - 16
                spacing: 4
                Text {
                    objectName: "streamNotificationsHeading"
                    text: qsTr("Notifications")
                    textFormat: Text.PlainText
                    font.pixelSize: 10
                    font.weight: Font.Medium
                    color: root.colors["text-muted"]
                }
                Rectangle {
                    objectName: "streamNotificationSwitch"
                    width: parent.width
                    height: 32
                    radius: 6
                    color: root.colors["bg"]
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 2
                        spacing: 2
                        Repeater {
                            model: root.streamMenuNotificationOptions
                            delegate: Button {
                                required property var modelData
                                objectName: modelData.objectName
                                // Review cid 4629685053: a second selection while a
                                // write for this stream is in flight would be silently
                                // rejected, so the whole switch goes inactive for the
                                // busy stream only (per-stream busy flag).
                                enabled: !root.streamMenuNotificationBusy
                                Layout.fillWidth: true
                                Layout.preferredHeight: 28
                                padding: 0
                                display: AbstractButton.IconOnly
                                property bool isCurrent: {
                                    const rows = root.controller.streams;
                                    const stream = root.currentStream(root.streamMenuTarget);
                                    return stream != null
                                        && String(stream.notificationMode || "") === String(modelData.mode);
                                }
                                icon.source: "qrc:/workspace/icons/" + modelData.asset
                                icon.width: 22
                                icon.height: 22
                                icon.color: isCurrent ? root.colors["icon-active"] : root.colors["text-muted"]
                                Accessible.name: modelData.label
                                background: Rectangle {
                                    radius: 6
                                    color: parent.isCurrent ? root.colors["card-bg"] : "transparent"
                                    opacity: parent.enabled ? 1.0 : 0.5
                                }
                                onClicked: {
                                    const target = root.streamMenuTarget
                                    streamMenu.close()
                                    root.controller.setStreamNotificationMode(target, String(modelData.mode))
                                }
                            }
                        }
                    }
                }
            }
        }
        MessageMenuAction {
            objectName: "streamMarkReadAction"
            implicitWidth: 278
            visible: { const stream = root.currentStream(root.streamMenuTarget); return stream && stream.unread > 0; }
            height: visible ? implicitHeight : 0
            text: qsTr("Mark all as read")
            asset: "check.svg"
            enabled: visible && root.streamMenuState.available
            onTriggered: root.openChatAction("read")
        }
        MessageMenuAction {
            objectName: "streamRenameAction"
            implicitWidth: 278
            visible: { const stream = root.currentStream(root.streamMenuTarget); return stream && !stream.isDirect; }
            height: visible ? implicitHeight : 0
            text: qsTr("Rename")
            asset: "border_color.svg"
            enabled: visible && root.streamMenuState.available
            onTriggered: root.openChatAction("rename")
        }
        MessageMenuAction {
            id: streamNewTopicAction
            objectName: "streamNewTopicAction"
            implicitWidth: 278
            leftPadding: 8
            rightPadding: 8
            asset: "plus.svg"
            text: qsTr("New topic")
            enabled: root.streamMenuAvailable
            onTriggered: root.openTopicForm(streamNewTopicAction.visualFocus)
        }
        MessageMenuAction {
            objectName: "streamArchiveAction"
            implicitWidth: 278
            text: qsTranslate("SidebarChatActions", "Archive")
            asset: "folder.svg"
            enabled: root.streamMenuState.canArchive
            onTriggered: root.openChatAction("archive")
        }
    }

    Popup {
        id: chatActionDialog
        objectName: "chatActionDialog"
        parent: Overlay.overlay
        x: Math.round((root.width - width) / 2)
        y: Math.round((root.height - height) / 2)
        width: Math.min(root.width - 48, 448)
        padding: 24
        modal: true; dim: true; focus: true
        closePolicy: root.chatActionState.pending ? Popup.NoAutoClose : Popup.CloseOnEscape | Popup.CloseOnPressOutside
        Overlay.modal: Rectangle { color: "#80000000" }
        background: Rectangle { radius: 10; color: root.colors["bg-elevated"]; border.color: root.colors["border-subtle"] }
        onOpened: {
            if (root.chatActionMode === "rename") { chatRenameInput.forceActiveFocus(Qt.TabFocusReason); chatRenameInput.selectAll(); }
            else chatActionCancel.forceActiveFocus(Qt.TabFocusReason);
        }
        onClosed: {
            root.chatActionTarget = ""; root.chatActionSubmitted = false;
            if (root.streamMenuAnchor) root.streamMenuAnchor.forceActiveFocus(Qt.TabFocusReason);
            messageList.scheduleVisibleRead();
        }
        contentItem: Column {
            spacing: 16
            Text {
                objectName: "chatActionTitle"
                width: parent.width
                text: root.chatActionMode === "archive" ? qsTr("Archive chat?") : root.chatActionMode === "rename" ? qsTr("Rename") : qsTr("Mark all as read")
                textFormat: Text.PlainText; wrapMode: Text.Wrap
                font.pixelSize: 18; font.weight: Font.DemiBold
                color: root.colors["text-primary"]
            }
            Text {
                objectName: "chatArchiveDescription"
                width: parent.width
                visible: root.chatActionMode === "archive"
                text: { const stream = root.currentStream(root.chatActionTarget); return stream && stream.isDirect
                    ? qsTr("Archive your conversation with “%1”? The chat will be archived for both of you.").arg(stream.displayName || stream.name)
                    : qsTr("Archive “%1” for all participants?").arg(stream ? stream.name : ""); }
                textFormat: Text.PlainText; wrapMode: Text.Wrap
                font.pixelSize: 14; color: root.colors["text-primary"]
            }
            Text {
                objectName: "chatArchiveHistoryHint"
                width: parent.width
                visible: root.chatActionMode === "archive"
                text: qsTr("The history will remain available in New chat → Archive. You can restore the chat there.")
                textFormat: Text.PlainText; wrapMode: Text.Wrap
                font.pixelSize: 14; color: root.colors["text-secondary"]
            }
            Column {
                width: parent.width; spacing: 6
                visible: root.chatActionMode === "rename"
                Text { text: qsTr("Channel name"); font.pixelSize: 14; color: root.colors["text-muted"] }
                ChannelField {
                    id: chatRenameInput
                    objectName: "chatRenameInput"
                    width: parent.width
                    enabled: !root.chatActionState.pending
                    Accessible.name: qsTr("Channel name")
                    onAccepted: root.submitChatAction()
                    KeyNavigation.tab: chatActionCancel
                }
            }
            Text {
                objectName: "chatActionError"
                width: parent.width
                visible: root.chatActionSubmitted && root.chatActionState.error.length > 0
                text: visible ? root.chatActionState.error : ""
                textFormat: Text.PlainText; wrapMode: Text.Wrap
                font.pixelSize: 14; color: root.colors.danger
                Accessible.role: Accessible.AlertMessage
            }
            Row {
                anchors.right: parent.right; spacing: 8
                NativeButton {
                    id: chatActionCancel
                    objectName: "chatActionCancel"
                    colors: root.colors
                    text: qsTr("Cancel")
                    enabled: !root.chatActionState.pending
                    onClicked: { root.chatActionTarget = ""; chatActionDialog.close(); }
                    KeyNavigation.tab: chatActionConfirm
                }
                NativeButton {
                    id: chatActionConfirm
                    objectName: "chatActionConfirm"
                    colors: root.colors; primary: true
                    text: root.chatActionState.pending ? qsTr("Please wait…") : root.chatActionMode === "archive" ? qsTranslate("SidebarChatActions", "Archive") : root.chatActionMode === "rename" ? qsTr("Save") : qsTr("Try again")
                    enabled: (root.chatActionMode === "archive" ? root.chatActionState.canArchive : root.chatActionState.available) && (root.chatActionMode !== "rename"
                        || chatRenameInput.text.trim().length > 0 && chatRenameInput.text.trim() !== String((root.currentStream(root.chatActionTarget) || {}).name || ""))
                    onClicked: root.submitChatAction()
                    KeyNavigation.tab: root.chatActionMode === "rename" ? chatRenameInput : chatActionCancel
                }
            }
        }
    }

    // Issue #7 slice 2: the forward destination modal (issue #7 slice 2
    // section of docs/NATIVE_DESKTOP_MULTI_FORWARD.md). A confirmed forward
    // clears the chat selection exactly like the reference onSuccess - but
    // only the selection it was submitted with (review round 1, cid
    // 4586012848): a Check that lands after the modal went away must leave a
    // newer selection alone.
    NativeForwardDialog {
        id: forwardDialog
        colors: root.colors
        controller: root.controller
        sourceIds: root.starredForwardIds.length ? root.starredForwardIds : root.messageSelectionIds
        onClosed: root.starredForwardIds = []
        onForwardSucceeded: function (submittedIds) {
            const submitted = (submittedIds || []).map(String)
            if (submitted.length === 0 || root.messageSelectionCount !== submitted.length)
                return
            for (const id of root.messageSelectionIds)
                if (!submitted.includes(id))
                    return
            root.clearMessageSelection()
        }
    }

    component ArchiveSpinner: Canvas {
        id: archiveSpinner
        width: 14; height: 14
        readonly property color ringColor: root.colors["border-subtle"]
        readonly property color capColor: root.colors.accent
        readonly property bool spinning: visible && createChatDialog.visible && root.visible
        Accessible.ignored: true
        onRingColorChanged: requestPaint()
        onCapColorChanged: requestPaint()
        onSpinningChanged: if (spinning) requestPaint()
        onPaint: {
            const context = getContext("2d"); context.reset(); context.lineWidth = 2;
            context.strokeStyle = ringColor; context.beginPath(); context.arc(7, 7, 6, 0, Math.PI * 2); context.stroke();
            context.strokeStyle = capColor; context.beginPath(); context.arc(7, 7, 6, -Math.PI * 0.75, -Math.PI * 0.25); context.stroke();
        }
        RotationAnimator on rotation {
            from: 0; to: 360; duration: 1000; loops: Animation.Infinite
            running: archiveSpinner.spinning
        }
    }

    component ChannelField: TextField {
        height: 40.84375
        font.pixelSize: 16
        leftPadding: 13
        rightPadding: 13
        topPadding: 9
        bottomPadding: 9
        color: root.colors["text-primary"]
        placeholderTextColor: root.colors["text-muted"]
        selectionColor: root.colors.accent
        selectedTextColor: root.colors["on-accent"]
        selectByMouse: true
        enabled: !root.channelFormFrozen
        onActiveFocusChanged: if (activeFocus) root.revealChannelControl(this)
        background: Rectangle {
            radius: 6
            color: root.colors.bg
            border.color: parent.activeFocus ? root.colors.accent : root.colors["border-subtle"]
        }
    }

    // The web source uses Chromium's intrinsic 16px checkbox paint, independently
    // of the app accent palette. Keep the tick geometry independent of font glyphs.
    component ChannelCheckMark: Canvas {
        anchors.fill: parent
        onVisibleChanged: if (visible) requestPaint()
        Component.onCompleted: requestPaint()
        onPaint: {
            const context = getContext("2d");
            context.clearRect(0, 0, width, height);
            context.strokeStyle = "white";
            context.lineWidth = 2;
            context.lineCap = "butt";
            context.lineJoin = "miter";
            context.beginPath();
            context.moveTo(3.5, 8.5);
            context.lineTo(6.5, 12);
            context.lineTo(12, 3.5);
            context.stroke();
        }
    }

    component ChannelOption: CheckBox {
        id: option
        height: 20
        padding: 0
        leftPadding: 24
        spacing: 8
        hoverEnabled: true
        enabled: !root.channelFormFrozen
        onActiveFocusChanged: if (activeFocus) root.revealChannelControl(option)
        indicator: Rectangle {
            width: 16
            height: 16
            x: 0
            y: (option.height - height) / 2
            radius: 2
            color: option.enabled ? option.checked ? "#0075ff" : "white" : theme.dark ? "#adadae" : "#eff2f4"
            border.color: option.visualFocus ? root.colors.accent : option.enabled ? option.checked ? "#0075ff" : "#767676" : theme.dark ? "#898a8b" : "#c9cbcd"
            ChannelCheckMark { visible: option.checked }
        }
        contentItem: Text {
            text: option.text
            textFormat: Text.PlainText
            color: option.enabled ? root.colors["text-primary"] : root.colors["border-subtle"]
            font.pixelSize: 14
            lineHeightMode: Text.FixedHeight
            lineHeight: 20
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }

    // Issue #234: the Protected channel (E2EE) switch. Geometry mirrors the
    // reference role=switch control in create-chat-dialog.ui.tsx (workspace_ui
    // 230e02b8): 30x18 rounded-full track (bg-border-subtle off, bg-accent on)
    // with a 14px white knob at x = 2 off / x = 14 on, label to the right,
    // 60% opacity while disabled.
    component ChannelSwitch: CheckBox {
        id: control
        height: 20
        padding: 0
        leftPadding: 42
        spacing: 12
        hoverEnabled: true
        opacity: enabled ? 1 : 0.6
        onActiveFocusChanged: if (activeFocus) root.revealChannelControl(control)
        indicator: Rectangle {
            objectName: "channelEncryptionTrack"
            x: 0
            y: (control.height - height) / 2
            width: 30
            height: 18
            radius: 9
            color: control.checked ? root.colors["accent"] : root.colors["border-subtle"]
            border.width: control.visualFocus ? 2 : 0
            border.color: root.colors["accent"]
            Rectangle {
                x: control.checked ? 14 : 2
                y: 2
                width: 14
                height: 14
                radius: 7
                color: "white"
            }
        }
        contentItem: Text {
            objectName: "channelEncryption"
            text: control.text
            textFormat: Text.PlainText
            color: root.colors["text-primary"]
            font.pixelSize: 14
            lineHeightMode: Text.FixedHeight
            lineHeight: 20
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }

    Popup {
        id: createChatDialog
        objectName: "createChatDialog"
        parent: Overlay.overlay
        x: (root.width - width) / 2
        y: (root.height - height) / 2
        width: Math.min(root.width, 896)
        height: chatTabs.currentIndex !== 1 && chatTabs.currentIndex !== 3 ? 512 : Math.max(512, Math.min(root.height * 0.85, 51 + chatTabs.naturalHeight + (chatTabs.currentIndex === 1 ? channelColumn.implicitHeight + 24 : archiveColumn.implicitHeight + 24) + 57 + 2))
        padding: 1
        modal: true
        dim: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        Overlay.modal: Rectangle { color: "#80000000" }
        background: Rectangle {
            objectName: "createChatSurface"
            radius: 10
            color: root.colors["bg-elevated"]
            border.color: root.colors["border-subtle"]
            readonly property bool shadowsSupported: GraphicsInfo.api !== GraphicsInfo.Software && GraphicsInfo.api !== GraphicsInfo.OpenVG && GraphicsInfo.api !== GraphicsInfo.Unknown && GraphicsInfo.api !== GraphicsInfo.Null
            layer.enabled: createChatDialog.visible && shadowsSupported
            layer.effect: MultiEffect {
                shadowEnabled: true
                shadowColor: "black"
                shadowOpacity: 0.16
                shadowVerticalOffset: 12
                shadowScale: 0.99
                shadowBlur: 1
                blurMax: 40
            }
        }
        onOpened: createChatClose.forceActiveFocus(root.channelKeyboard ? Qt.TabFocusReason : Qt.MouseFocusReason)
        onClosed: {
            root.resetChannelView();
            root.resetDirectView();
            root.resetArchiveView();
            root.clearGlobalTopicView();
            createChatButton.forceActiveFocus(root.channelKeyboard ? Qt.TabFocusReason : Qt.MouseFocusReason);
            messageList.scheduleVisibleRead();
        }
        contentItem: Column {
            objectName: "createChatContent"
            Accessible.role: Accessible.Dialog
            Accessible.name: qsTr("New chat")
            Item {
                width: parent.width
                height: 51
                Text {
                    x: 16
                    y: 15
                    width: parent.width - 74
                    height: 20
                    text: qsTr("New chat")
                    textFormat: Text.PlainText
                    color: root.colors["text-primary"]
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                NativeIconButton {
                    id: createChatClose
                    objectName: "createChatClose"
                    x: parent.width - width - 16
                    y: 12
                    width: 26
                    height: 26
                    colors: root.colors
                    asset: "close.svg"
                    glyphSize: 18
                    iconColor: root.colors["text-muted"]
                    help: qsTr("Close")
                    onClicked: createChatDialog.close()
                    KeyNavigation.tab: chatTabs.itemAt(chatTabs.currentIndex)
                    KeyNavigation.backtab: chatTabs.currentIndex === 3 ? channelCancel : chatTabs.currentIndex === 0 ? directRetry.visible && directRetry.enabled ? directRetry : directCheck.visible && directCheck.enabled ? directCheck : channelCancel : chatTabs.currentIndex === 2 ? globalTopicPrimary.enabled ? globalTopicPrimary : channelCancel : channelPrimary.visible && channelPrimary.enabled ? channelPrimary : channelCancel
                }
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 1
                    color: root.colors["border-subtle"]
                }
            }
            TabBar {
                id: chatTabs
                objectName: "createChatTabs"
                width: parent.width
                readonly property real naturalHeight: root.width < 640 ? 35 : 39
                // Match the source flex shrink under intrinsic channel content
                // pressure. The other two forms fit the source 320px minimum body.
                readonly property real naturalMiddle: (currentIndex === 1 ? Math.max(320, channelColumn.implicitHeight + 24) : currentIndex === 3 ? Math.max(320, archiveColumn.implicitHeight + 24) : 320) + 57
                readonly property real overflow: Math.max(0, naturalHeight + naturalMiddle - (createChatDialog.availableHeight - 51))
                height: Math.floor((naturalHeight - overflow * (naturalHeight - 1) / (naturalHeight - 1 + naturalMiddle)) * 64) / 64
                currentIndex: 0
                padding: 0
                spacing: 0
                background: Rectangle {
                    color: "transparent"
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 1
                        color: root.colors["border-subtle"]
                    }
                }
                Repeater {
                    model: [qsTr("Start chat"), qsTr("Create channel"), qsTr("Create topic"), qsTr("Archive")]
                    delegate: TabButton {
                        id: tab
                        required property string modelData
                        required property int index
                        objectName: "createChatTab-" + index
                        width: chatTabs.width / 4
                        implicitHeight: chatTabs.height - 1
                        height: implicitHeight
                        enabled: index >= 0 && index <= 3
                        text: modelData
                        padding: 0
                        Accessible.description: enabled ? "" : qsTr("This section is not implemented yet")
                        contentItem: Text {
                            text: tab.text
                            textFormat: Text.PlainText
                            color: tab.index === chatTabs.currentIndex ? root.colors.accent : root.colors["text-muted"]
                            font.pixelSize: root.width < 640 ? 12 : 14
                            font.weight: Font.Medium
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                        background: Rectangle {
                            color: "transparent"
                            border.width: tab.visualFocus ? 1 : 0
                            border.color: root.colors.accent
                            Rectangle {
                                anchors.bottom: parent.bottom
                                width: parent.width
                                height: tab.index === chatTabs.currentIndex ? 2 : 0
                                color: root.colors.accent
                            }
                        }
                        onClicked: root.chooseChatTab(index)
                        Keys.onPressed: event => {
                            if ([Qt.Key_Left, Qt.Key_Right, Qt.Key_Home, Qt.Key_End].indexOf(event.key) >= 0) {
                                root.chooseChatTab(event.key === Qt.Key_Home ? 0 : event.key === Qt.Key_End ? 3 : (index + (event.key === Qt.Key_Right ? 1 : 3)) % 4);
                                chatTabs.itemAt(chatTabs.currentIndex).forceActiveFocus(Qt.TabFocusReason);
                                event.accepted = true;
                            }
                        }
                        KeyNavigation.tab: chatTabs.currentIndex === 3 ? archiveSearch : chatTabs.currentIndex === 0 ? directChatSearch.enabled ? directChatSearch : channelCancel : chatTabs.currentIndex === 2 ? globalTopicSelector.enabled ? globalTopicSelector : channelCancel : channelNameInput.enabled ? channelNameInput : channelCancel
                        KeyNavigation.backtab: createChatClose
                    }
                }
            }
            Item {
                id: directBody
                objectName: "directChatBody"
                visible: chatTabs.currentIndex === 0
                width: parent.width
                height: createChatDialog.availableHeight - 51 - chatTabs.height - 57
                Column {
                    x: 16
                    y: 16
                    width: parent.width - 32
                    spacing: 12
                    ChannelField {
                        id: directChatSearch
                        objectName: "directChatSearch"
                        width: parent.width
                        placeholderText: qsTr("Search users…")
                        Accessible.name: qsTr("Search users…")
                        enabled: !root.chatMutationBusy && root.controller.directChatDirectoryVerified
                        onTextChanged: root.setChatQuery(text)
                        onActiveFocusChanged: {}
                        KeyNavigation.backtab: chatTabs.itemAt(0)
                        KeyNavigation.tab: directUserList.count ? directUserList.itemAtIndex(0) : channelCancel
                    }
                    Rectangle {
                        objectName: "directChatRoster"
                        width: parent.width
                        height: 240
                        radius: 6
                        color: "transparent"
                        border.color: root.colors["border-subtle"]
                        ListView {
                            id: directUserList
                            objectName: "directChatUserList"
                            anchors.fill: parent
                            anchors.margins: 1
                            model: root.controller.directChatDirectoryVerified ? root.directVisibleUsers : []
                            clip: true
                            boundsBehavior: Flickable.StopAtBounds
                            ScrollBar.vertical: ScrollBar {}
                            delegate: Button {
                                id: directUser
                                required property var modelData
                                required property int index
                                objectName: "direct-user-" + modelData.id
                                width: directUserList.width
                                height: contentItem.secondary.length > 0 ? 51.6875 : 36
                                padding: 0
                                topPadding: 8
                                bottomPadding: 8
                                leftPadding: 12
                                rightPadding: 12
                                enabled: {
                                    const verified = root.controller.directChatDirectoryVerified;
                                    const phase = root.controller.directChatPhase;
                                    return verified && !root.chatMutationBusy && root.controller.canStartDirectChatWith(modelData.id);
                                }
                                Accessible.name: String(modelData.displayName) + (contentItem.secondary ? ", " + contentItem.secondary : "")
                                onClicked: root.startDirectPeer(modelData.id)
                                onActiveFocusChanged: if (activeFocus) directUserList.positionViewAtIndex(index, ListView.Contain)
                                contentItem: ChatPickerRowBody {
                                    user: directUser.modelData
                                    prefix: "direct"
                                }
                                background: Rectangle {
                                    color: directUser.hovered ? root.colors["sidebar-item-hover"] : "transparent"
                                    border.width: directUser.visualFocus ? 1 : 0
                                    border.color: root.colors.accent
                                }
                            }
                        }
                        Text {
                            x: 12
                            y: 16
                            width: parent.width - 24
                            visible: !root.controller.directChatDirectoryVerified || !root.directVisibleUsers.length
                            text: root.controller.directChatDirectoryVerified ? qsTr("No results") : qsTr("User list is currently unavailable.")
                            textFormat: Text.PlainText
                            color: root.colors["text-muted"]
                            font.pixelSize: 14
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }
                    Text {
                        objectName: "directChatStatus"
                        width: parent.width
                        visible: root.directViewIntent && root.controller.directChatBusy
                        text: visible ? qsTr("Opening chat…") : ""
                        textFormat: Text.PlainText
                        color: root.colors["text-secondary"]
                        font.pixelSize: 14
                    }
                    Text {
                        objectName: "directChatError"
                        width: parent.width
                        visible: root.directViewIntent && root.controller.directChatError.length > 0
                        text: visible ? root.controller.directChatError : ""
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: root.colors.danger
                        font.pixelSize: 14
                        Accessible.role: Accessible.AlertMessage
                    }
                }
            }
            Flickable {
                id: channelBody
                objectName: "channelFormBody"
                visible: chatTabs.currentIndex === 1
                width: parent.width
                height: createChatDialog.availableHeight - 51 - chatTabs.height - 57
                contentWidth: width
                contentHeight: channelColumn.implicitHeight + 24
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                Column {
                    id: channelColumn
                    x: 16
                    y: 16
                    width: parent.width - 32
                    spacing: 12
                    Text {
                        width: parent.width
                        height: 20
                        text: qsTr("Channel name")
                        textFormat: Text.PlainText
                        color: root.colors["text-muted"]
                        font.pixelSize: 14
                        lineHeightMode: Text.FixedHeight
                        lineHeight: 20
                    }
                    ChannelField {
                        id: channelNameInput
                        objectName: "channelNameInput"
                        width: parent.width
                        placeholderText: qsTr("Channel name")
                        Accessible.name: qsTr("Channel name")
                    }
                    Text {
                        width: parent.width
                        height: 20
                        text: qsTr("Description (optional)")
                        textFormat: Text.PlainText
                        color: root.colors["text-muted"]
                        font.pixelSize: 14
                        lineHeightMode: Text.FixedHeight
                        lineHeight: 20
                    }
                    ChannelField {
                        id: channelDescriptionInput
                        objectName: "channelDescriptionInput"
                        width: parent.width
                        placeholderText: qsTr("Description (optional)")
                        Accessible.name: qsTr("Description (optional)")
                    }
                    Rectangle {
                        width: parent.width
                        height: channelSettings.implicitHeight + 18
                        radius: 6
                        color: root.colors.bg
                        border.color: root.colors["border-subtle"]
                        Column {
                            id: channelSettings
                            x: 13
                            y: 9
                            width: parent.width - 26
                            spacing: 8
                            ChannelOption {
                                id: channelInviteOnly
                                objectName: "channelInviteOnly"
                                width: parent.width
                                text: qsTr("Invite only")
                                enabled: !root.channelFormFrozen && !root.channelEncryptionChecked
                            }
                            ChannelOption {
                                id: channelAnnounce
                                objectName: "channelAnnounce"
                                width: parent.width
                                text: qsTr("Announce channel creation")
                                enabled: !root.channelFormFrozen && !root.channelEncryptionChecked
                            }
                            ChannelOption {
                                objectName: "channelAnnouncementOnly"
                                width: parent.width
                                text: qsTr("Announcement-only channel")
                                enabled: false
                            }
                            Text {
                                width: parent.width
                                height: implicitHeight
                                text: qsTr("Announcement-only policy is not connected to Workspace yet.")
                                textFormat: Text.PlainText
                                color: root.colors["text-muted"]
                                font.pixelSize: 12
                                lineHeightMode: Text.FixedHeight
                                lineHeight: 16
                                wrapMode: Text.Wrap
                            }
                        }
                    }
                    ChannelSwitch {
                        id: channelEncryptionSwitch
                        objectName: "channelEncryptionSwitch"
                        width: parent.width
                        text: qsTr("Protected channel (E2EE)")
                        Accessible.name: qsTr("Create encrypted chat")
                        checked: root.channelEncryptionChecked
                        enabled: !root.channelFormFrozen
                        onClicked: root.setChannelEncryption(!root.channelEncryptionChecked)
                    }
                    Column {
                        width: parent.width
                        spacing: 12
                        Text {
                            width: parent.width
                            height: 20
                            text: qsTr("Add members")
                            textFormat: Text.PlainText
                            color: root.colors["text-muted"]
                            font.pixelSize: 14
                            lineHeightMode: Text.FixedHeight
                            lineHeight: 20
                        }
                        ChannelField {
                            id: channelMemberSearch
                            objectName: "channelMemberSearch"
                            onTextChanged: root.setChatQuery(text)
                            width: parent.width
                            placeholderText: qsTr("Search users…")
                            Accessible.name: qsTr("Search users…")
                            enabled: !root.channelFormFrozen && root.controller.channelMembersVerified
                        }
                        Rectangle {
                            width: parent.width
                            height: 160
                            radius: 6
                            color: "transparent"
                            border.color: root.colors["border-subtle"]
                            ListView {
                                id: channelMemberList
                                objectName: "channelMemberList"
                                anchors.fill: parent
                                anchors.margins: 1
                                model: root.channelVisibleMembers
                                clip: true
                                boundsBehavior: Flickable.StopAtBounds
                                ScrollBar.vertical: ScrollBar {}
                                delegate: CheckBox {
                                    id: member
                                    required property var modelData
                                    required property int index
                                    objectName: "channel-member-" + modelData.id
                                    width: channelMemberList.width
                                    height: contentItem.secondary.length > 0 ? 51.6875 : 36
                                    padding: 0
                                    topPadding: 8
                                    bottomPadding: 8
                                    leftPadding: 36
                                    rightPadding: 12
                                    enabled: !root.channelFormFrozen && root.controller.channelMembersVerified
                                    checked: root.channelSelectedMembers.indexOf(modelData.id) >= 0
                                    Accessible.name: String(modelData.displayName) + (contentItem.secondary ? ", " + contentItem.secondary : "")
                                    onClicked: root.toggleChannelMember(modelData.id)
                                    onActiveFocusChanged: {
                                        if (activeFocus) {
                                            channelMemberList.positionViewAtIndex(index, ListView.Contain);
                                            root.revealChannelControl(member);
                                        }
                                    }
                                    indicator: Rectangle {
                                        x: 12
                                        y: (member.height - height) / 2
                                        width: 16
                                        height: 16
                                        radius: 2
                                        color: member.checked ? "#0075ff" : "white"
                                        border.color: member.visualFocus ? root.colors.accent : member.checked ? "#0075ff" : "#767676"
                                        ChannelCheckMark { visible: member.checked }
                                    }
                                    contentItem: ChatPickerRowBody {
                                        user: member.modelData
                                        prefix: "channel"
                                    }
                                    background: Rectangle { color: member.hovered ? root.colors["sidebar-item-hover"] : "transparent" }
                                }
                            }
                            Text {
                                x: 12
                                y: 16
                                width: parent.width - 24
                                visible: !root.channelVisibleMembers.length
                                text: root.controller.channelMembersVerified ? qsTr("No results") : qsTr("Loading members…")
                                textFormat: Text.PlainText
                                color: root.colors["text-muted"]
                                font.pixelSize: 14
                                horizontalAlignment: Text.AlignHCenter
                            }
                        }
                    }
                    Text {
                        objectName: "channelCreationStatus"
                        width: parent.width
                        visible: root.channelPartialMembers
                        text: qsTr("The channel exists. Complete adding the selected members.")
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: root.colors["text-secondary"]
                        font.pixelSize: 14
                        lineHeightMode: Text.FixedHeight
                        lineHeight: 20
                    }
                    Text {
                        objectName: "channelCreationError"
                        width: parent.width
                        visible: root.channelSubmitted && root.controller.channelCreationError.length > 0
                        text: visible ? (root.controller.channelCreateRequestFailed
                            ? qsTr("Could not create chat. Try again.") : root.controller.channelCreationError) : ""
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: root.colors.danger
                        font.pixelSize: 14
                        lineHeightMode: Text.FixedHeight
                        lineHeight: 20
                        Accessible.role: Accessible.AlertMessage
                    }
                    Text {
                        objectName: "channelCreationRecoveryDetail"
                        width: parent.width
                        visible: root.channelSubmitted && root.controller.channelCreateRequestFailed && root.channelPhase === "uncertain"
                        text: visible ? root.controller.channelCreationError : ""
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: root.colors["text-secondary"]
                        font.pixelSize: 14
                    }
                    Text {
                        objectName: "channelProfileLoading"
                        width: parent.width
                        visible: !root.controller.channelMembersVerified && root.channelPrimaryAction === "create"
                        text: qsTr("Profile is still loading. Try again in a moment.")
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: root.colors["text-muted"]
                        font.pixelSize: 12
                    }
                    // Issue #234 (review finding 4576938732, R9): when an
                    // encrypted create is rejected because the durable create
                    // budget is held by unresolved attempts, the user must be
                    // able to reach that inventory from this error surface.
                    Button {
                        objectName: "channelStageReviewButton"
                        visible: root.channelSubmitted && root.controller.channelCreationError.length > 0
                            && root.controller.authenticated
                            && root.controller.unresolvedChannelCreateStages().length > 0
                        text: qsTr("Review unresolved encrypted channel creations")
                        font.pixelSize: 14
                        Accessible.name: text
                        onClicked: unresolvedChannelCreationsDialog.open()
                    }
                }
            }
            Item {
                objectName: "globalTopicBody"
                visible: chatTabs.currentIndex === 2
                width: parent.width
                height: createChatDialog.availableHeight - 51 - chatTabs.height - 57
                Column {
                    x: 16; y: 16; width: parent.width - 32; spacing: 12
                    Text {
                        width: parent.width; height: 20
                        text: qsTr("Select channel")
                        textFormat: Text.PlainText
                        color: root.colors["text-muted"]; font.pixelSize: 14
                        lineHeightMode: Text.FixedHeight; lineHeight: 20
                    }
                    Button {
                        id: globalTopicSelector
                        objectName: "globalTopicStreamSelector"
                        width: parent.width; height: 40
                        enabled: !root.globalTopicFrozen
                        opacity: enabled ? 1 : 0.5
                        padding: 0; horizontalPadding: 0; hoverEnabled: true
                        Accessible.name: qsTr("Select channel")
                        Accessible.role: Accessible.ComboBox
                        property int activeIndex: -1
                        property string typeahead: ""
                        function showOptions(direction) {
                            activeIndex = root.globalTopicStreams.findIndex(stream => stream.id === root.globalTopicTarget);
                            if (activeIndex < 0) activeIndex = direction < 0 ? root.globalTopicStreams.length - 1 : 0;
                            globalTopicOptions.open();
                        }
                        function choose(index) {
                            if (index < 0 || index >= root.globalTopicStreams.length) return;
                            root.selectGlobalTopic(root.globalTopicStreams[index].id);
                            globalTopicOptions.close();
                            forceActiveFocus(Qt.OtherFocusReason);
                        }
                        onClicked: globalTopicOptions.visible ? globalTopicOptions.close() : showOptions(1)
                        onEnabledChanged: if (!enabled) globalTopicOptions.close()
                        onActiveFocusChanged: if (!activeFocus) globalTopicOptions.close()
                        Keys.onPressed: event => {
                            const count = root.globalTopicStreams.length;
                            if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
                                if (!globalTopicOptions.visible) showOptions(event.key === Qt.Key_Up ? -1 : 1);
                                else activeIndex = Math.max(0, Math.min(count - 1, activeIndex + (event.key === Qt.Key_Up ? -1 : 1)));
                                event.accepted = true;
                            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                                if (globalTopicOptions.visible) choose(activeIndex); else showOptions(1);
                                event.accepted = true;
                            } else if (event.key === Qt.Key_Escape && globalTopicOptions.visible) {
                                globalTopicOptions.close(); event.accepted = true;
                            } else if ((event.key === Qt.Key_Home || event.key === Qt.Key_End) && globalTopicOptions.visible) {
                                activeIndex = event.key === Qt.Key_Home ? 0 : count - 1; event.accepted = true;
                            } else if (event.key === Qt.Key_Tab) {
                                globalTopicOptions.close();
                            } else if (event.text.length === 1 && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))) {
                                typeahead += event.text.toLocaleLowerCase();
                                topicTypeahead.restart();
                                const repeated = Array.from(typeahead).every(character => character === typeahead[0]);
                                const query = repeated ? event.text.toLocaleLowerCase() : typeahead;
                                const start = activeIndex >= 0 ? activeIndex : root.globalTopicStreams.findIndex(stream => stream.id === root.globalTopicTarget);
                                for (let offset = 1; offset <= count; ++offset) {
                                    const index = (start + offset + count) % count;
                                    if (String(root.globalTopicStreams[index].name).toLocaleLowerCase().startsWith(query)) {
                                        activeIndex = index; globalTopicOptions.open(); event.accepted = true; break;
                                    }
                                }
                            }
                            if (globalTopicOptions.visible && activeIndex >= 0) topicOptionsList.positionViewAtIndex(activeIndex, ListView.Contain);
                        }
                        Timer { id: topicTypeahead; interval: 700; onTriggered: globalTopicSelector.typeahead = "" }
                        contentItem: Item {
                            Text {
                                x: 13; width: parent.width - 50; height: parent.height
                                text: { const stream = root.currentStream(root.globalTopicTarget); return stream ? stream.name : qsTr("Select an option"); }
                                textFormat: Text.PlainText; elide: Text.ElideRight
                                verticalAlignment: Text.AlignVCenter
                                font.pixelSize: 14
                                color: root.globalTopicTarget.length ? root.colors["text-primary"] : root.colors["text-muted"]
                            }
                            NativeIconButton {
                                x: parent.width - 29; y: 12; width: 16; height: 16
                                colors: root.colors; asset: "chevron-down.svg"; glyphSize: 16
                                glyphWidth: 16; glyphHeight: 8
                                enabled: false; icon.color: root.colors["text-muted"]
                                rotation: globalTopicOptions.visible ? 180 : 0
                                focusPolicy: Qt.NoFocus; Accessible.ignored: true
                                background: Item {}
                            }
                        }
                        background: Rectangle {
                            radius: 6; color: root.colors["text-field-bg"]
                            border.color: globalTopicSelector.activeFocus ? root.colors.accent : globalTopicSelector.hovered ? root.colors["text-muted"] : root.colors["border-subtle"]
                            Rectangle {
                                x: -2; y: -2; width: parent.width + 4; height: parent.height + 4
                                radius: 8; color: "transparent"; border.width: 2
                                border.color: root.colors.accent; opacity: 0.2
                                visible: globalTopicSelector.activeFocus
                            }
                        }
                        KeyNavigation.backtab: chatTabs.itemAt(2)
                        KeyNavigation.tab: globalTopicName.enabled ? globalTopicName : channelCancel
                    }
                    Text {
                        width: parent.width; height: 20
                        text: qsTr("Topic name"); textFormat: Text.PlainText
                        color: root.colors["text-muted"]; font.pixelSize: 14
                        lineHeightMode: Text.FixedHeight; lineHeight: 20
                    }
                    ChannelField {
                        id: globalTopicName
                        objectName: "globalTopicNameInput"
                        width: parent.width
                        placeholderText: qsTr("Topic name"); Accessible.name: qsTr("Topic name")
                        enabled: !root.globalTopicFrozen
                        KeyNavigation.backtab: globalTopicSelector.enabled ? globalTopicSelector : chatTabs.itemAt(2)
                        KeyNavigation.tab: channelCancel
                    }
                    Text {
                        width: parent.width
                        visible: !root.globalTopicStreams.length
                        text: qsTr("No channels found"); textFormat: Text.PlainText
                        color: root.colors["text-muted"]; font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                    Text {
                        objectName: "globalTopicError"
                        width: parent.width
                        visible: root.globalTopicIntent && root.controller.topicCreationError.length > 0
                        text: visible ? root.controller.topicCreationError : ""; textFormat: Text.PlainText
                        color: root.colors.danger; font.pixelSize: 14; wrapMode: Text.Wrap
                        Accessible.role: Accessible.AlertMessage
                    }
                }
            }
            Flickable {
                id: archiveBody
                objectName: "archiveChatBody"
                visible: chatTabs.currentIndex === 3
                width: parent.width
                height: createChatDialog.availableHeight - 51 - chatTabs.height - 57
                contentWidth: width
                contentHeight: Math.max(height, archiveColumn.implicitHeight + 24)
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                Column {
                    id: archiveColumn
                    x: 16; y: 16; width: parent.width - 32; spacing: 12
                    ChannelField {
                        id: archiveSearch
                        objectName: "archiveChatSearch"
                        width: parent.width
                        enabled: true
                        placeholderText: qsTr("Search archived chats…")
                        Accessible.name: placeholderText
                        text: root.archiveQuery
                        onTextChanged: root.archiveQuery = text
                        onActiveFocusChanged: if (activeFocus) archiveBody.contentY = 0
                        KeyNavigation.backtab: chatTabs.itemAt(3)
                        KeyNavigation.tab: channelCancel
                        Keys.onTabPressed: event => { if (archiveChatList.count) { root.focusArchiveRow(0, false); event.accepted = true; } }
                    }
                    Rectangle {
                        objectName: "archiveRestoreAlert"
                        width: parent.width
                        height: visible ? archiveRestoreError.implicitHeight + 18 : 0
                        visible: root.archiveErrorTarget.length > 0
                        radius: 6; color: root.colors.bg; border.color: root.colors["border-subtle"]
                        Text {
                            id: archiveRestoreError
                            objectName: "archiveRestoreError"
                            x: 13; y: 9; width: parent.width - 26
                            text: qsTr("Could not unarchive the chat. Try again.")
                            textFormat: Text.PlainText; wrapMode: Text.Wrap
                            color: root.colors["notice-base"]
                            font.pixelSize: 14; lineHeight: 20; lineHeightMode: Text.FixedHeight
                            Accessible.role: Accessible.AlertMessage
                        }
                    }
                    Text {
                        objectName: "archiveCatalogError"
                        width: parent.width
                        visible: root.controller.archiveError.length > 0
                        text: qsTr("Could not load the archive.")
                        textFormat: Text.PlainText; wrapMode: Text.Wrap
                        color: root.colors["notice-base"]
                        font.pixelSize: 14; lineHeight: 20; lineHeightMode: Text.FixedHeight
                        Accessible.role: Accessible.AlertMessage
                    }
                    Rectangle {
                        id: archiveRoster
                        objectName: "archiveChatRoster"
                        width: parent.width; height: 240; radius: 6
                        color: "transparent"; border.color: root.colors["border-subtle"]
                        Accessible.role: Accessible.List
                        Accessible.name: qsTr("Archive")
                        Accessible.description: root.controller.archiveLoading ? qsTr("Loading…") : ""
                        ListView {
                            id: archiveChatList
                            objectName: "archiveChatList"
                            anchors.fill: parent; anchors.margins: 1
                            model: root.archiveVisibleChats
                            clip: true; boundsBehavior: Flickable.StopAtBounds
                            readonly property bool overflowing: contentHeight > height
                            ScrollBar.vertical: ScrollBar {
                                id: archiveScroll
                                objectName: "archiveChatScrollBar"
                                width: 10
                                topPadding: 12; bottomPadding: 12; leftPadding: 2; rightPadding: 2
                                stepSize: 40 / Math.max(40, archiveChatList.contentHeight)
                                policy: archiveChatList.overflowing ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
                                contentItem: Rectangle {
                                    implicitWidth: 6; implicitHeight: 6
                                    radius: width / 2; color: root.colors["border-subtle"]
                                }
                                background: Item {
                                    Button {
                                        objectName: "archiveScrollUp"
                                        width: parent.width; height: 12; padding: 0; horizontalPadding: 0
                                        focusPolicy: Qt.NoFocus; autoRepeat: true
                                        Accessible.ignored: true
                                        onClicked: archiveScroll.decrease()
                                        background: Item {}
                                        contentItem: Canvas {
                                            readonly property color arrowColor: root.colors["border-subtle"]
                                            onArrowColorChanged: requestPaint()
                                            onPaint: {
                                                const context = getContext("2d"); context.reset(); context.fillStyle = arrowColor;
                                                context.beginPath(); context.moveTo(5, 4); context.lineTo(8, 7); context.lineTo(2, 7); context.closePath(); context.fill();
                                            }
                                        }
                                    }
                                    Button {
                                        objectName: "archiveScrollDown"
                                        y: parent.height - height; width: parent.width; height: 12; padding: 0; horizontalPadding: 0
                                        focusPolicy: Qt.NoFocus; autoRepeat: true
                                        Accessible.ignored: true
                                        onClicked: archiveScroll.increase()
                                        background: Item {}
                                        contentItem: Canvas {
                                            readonly property color arrowColor: root.colors["border-subtle"]
                                            onArrowColorChanged: requestPaint()
                                            onPaint: {
                                                const context = getContext("2d"); context.reset(); context.fillStyle = arrowColor;
                                                context.beginPath(); context.moveTo(2, 5); context.lineTo(8, 5); context.lineTo(5, 8); context.closePath(); context.fill();
                                            }
                                        }
                                    }
                                }
                            }
                            delegate: Item {
                                id: archiveRow
                                required property var modelData
                                required property int index
                                objectName: "archive-row-" + modelData.id
                                width: archiveChatList.width - (archiveChatList.overflowing ? 10 : 0)
                                height: 51.703125
                                readonly property alias openControl: archiveOpen
                                readonly property alias restoreControl: archiveRestore
                                Rectangle { anchors.fill: parent; color: archiveOpen.hovered || archiveRestore.hovered ? root.colors["sidebar-item-hover"] : "transparent" }
                                Button {
                                    id: archiveOpen
                                    objectName: "archive-open-" + archiveRow.modelData.id
                                    width: parent.width - 40; height: parent.height
                                    padding: 0; leftPadding: 12; rightPadding: 12; topPadding: 8; bottomPadding: 8
                                    Accessible.name: qsTr("Open history: %1").arg(archiveRow.modelData.name)
                                    onClicked: root.openArchivedChat(archiveRow.modelData.id)
                                    onActiveFocusChanged: if (activeFocus) archiveChatList.positionViewAtIndex(archiveRow.index, ListView.Contain)
                                    contentItem: Column {
                                        Text { objectName: "archive-name-" + archiveRow.modelData.id; width: parent.width; height: 20; text: archiveRow.modelData.name; textFormat: Text.PlainText; font.pixelSize: 14; font.weight: Font.Medium; color: root.colors["text-primary"]; elide: Text.ElideRight }
                                        Text { objectName: "archive-kind-" + archiveRow.modelData.id; width: parent.width; height: 15.703125; text: archiveRow.modelData.isDirect ? qsTr("Direct chat") : qsTr("Channel"); textFormat: Text.PlainText; font.pixelSize: 11; color: root.colors["text-secondary"]; elide: Text.ElideRight }
                                    }
                                    background: Rectangle { color: "transparent"; border.width: archiveOpen.visualFocus ? 1 : 0; border.color: root.colors.accent }
                                    Keys.onPressed: event => {
                                        if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
                                            if (event.key === Qt.Key_Backtab || event.modifiers & Qt.ShiftModifier) root.focusArchiveRow(archiveRow.index - 1, true);
                                            else if (archiveRestore.enabled) archiveRestore.forceActiveFocus(Qt.TabFocusReason);
                                            else root.focusArchiveRow(archiveRow.index + 1, false);
                                            event.accepted = true;
                                        }
                                    }
                                }
                                NativeIconButton {
                                    id: archiveRestore
                                    objectName: "archive-restore-" + archiveRow.modelData.id
                                    x: parent.width - 40; y: (parent.height - height) / 2
                                    width: 32; height: 32; glyphSize: 16
                                    colors: root.colors; iconColor: root.colors.accent
                                    asset: archiveRow.modelData.pending ? "" : "logout.svg"
                                    enabled: Boolean(archiveRow.modelData.canUnarchive) && !archiveRow.modelData.pending
                                    opacity: enabled ? 1 : 0.5
                                    help: ""
                                    Accessible.name: (archiveRow.modelData.pending ? qsTr("Unarchiving") : qsTr("Unarchive")) + ": " + archiveRow.modelData.name
                                    Accessible.description: root.archiveErrorTarget === archiveRow.modelData.id ? archiveRestoreError.text : ""
                                    onClicked: root.restoreArchivedChat(archiveRow.modelData.id)
                                    onActiveFocusChanged: if (activeFocus) archiveChatList.positionViewAtIndex(archiveRow.index, ListView.Contain)
                                    ArchiveSpinner { objectName: "archiveRestoreSpinner-" + archiveRow.modelData.id; anchors.centerIn: parent; visible: archiveRow.modelData.pending }
                                    Keys.onPressed: event => {
                                        if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
                                            if (event.key === Qt.Key_Backtab || event.modifiers & Qt.ShiftModifier) archiveOpen.forceActiveFocus(Qt.BacktabFocusReason);
                                            else root.focusArchiveRow(archiveRow.index + 1, false);
                                            event.accepted = true;
                                        }
                                    }
                                }
                            }
                        }
                        Row {
                            objectName: "archiveLoadingStatus"
                            anchors.horizontalCenter: parent.horizontalCenter; y: 33; height: 20; spacing: 8
                            visible: root.controller.archiveLoading && !root.archiveVisibleChats.length
                            ArchiveSpinner { objectName: "archiveLoadingSpinner"; y: 3 }
                            Text { height: 20; text: qsTr("Loading…"); textFormat: Text.PlainText; font.pixelSize: 14; lineHeight: 20; lineHeightMode: Text.FixedHeight; color: root.colors["text-muted"] }
                            Accessible.role: Accessible.StaticText
                            Accessible.name: qsTr("Loading…")
                        }
                        Text {
                            objectName: "archiveEmptyStatus"
                            x: 12; y: 17; width: parent.width - 24
                            visible: !root.archiveVisibleChats.length && !root.controller.archiveLoading && !root.controller.archiveError.length
                            height: 20; text: qsTr("No archived chats found"); textFormat: Text.PlainText
                            font.pixelSize: 14; lineHeight: 20; lineHeightMode: Text.FixedHeight; color: root.colors["text-muted"]; horizontalAlignment: Text.AlignHCenter
                        }
                    }
                }
            }
            Item {
                width: parent.width
                height: 57
                Rectangle { width: parent.width; height: 1; color: root.colors["border-subtle"] }
                Row {
                    anchors.right: parent.right
                    anchors.rightMargin: 16
                    y: 13
                    height: 32
                    spacing: 8
                    Button {
                        id: channelCancel
                        objectName: "channelCancelButton"
                        text: qsTr("Cancel")
                        height: 32
                        width: contentItem.implicitWidth + 24
                        padding: 0
                        contentItem: Text { text: channelCancel.text; textFormat: Text.PlainText; font.pixelSize: 14; color: root.colors["text-muted"]; verticalAlignment: Text.AlignVCenter; horizontalAlignment: Text.AlignHCenter }
                        background: Rectangle { radius: 6; color: channelCancel.hovered ? root.colors["sidebar-item-hover"] : "transparent"; border.width: channelCancel.visualFocus ? 1 : 0; border.color: root.colors.accent }
                        onClicked: createChatDialog.close()
                        Keys.onBacktabPressed: event => { if (chatTabs.currentIndex === 3 && archiveChatList.count) { root.focusArchiveRow(archiveChatList.count - 1, true); event.accepted = true; } }
                        KeyNavigation.backtab: chatTabs.currentIndex === 3 ? archiveSearch : chatTabs.currentIndex === 0 ? directChatSearch.enabled ? directChatSearch : createChatClose : chatTabs.currentIndex === 2 ? globalTopicName.enabled ? globalTopicName : globalTopicSelector.enabled ? globalTopicSelector : createChatClose : channelMemberSearch.enabled ? channelMemberSearch : createChatClose
                        KeyNavigation.tab: chatTabs.currentIndex === 3 ? createChatClose : chatTabs.currentIndex === 0 ? directCheck.visible && directCheck.enabled ? directCheck : directRetry.visible && directRetry.enabled ? directRetry : createChatClose : chatTabs.currentIndex === 2 ? globalTopicPrimary.enabled ? globalTopicPrimary : createChatClose : channelCheck.visible && channelCheck.enabled ? channelCheck : channelPrimary.visible && channelPrimary.enabled ? channelPrimary : createChatClose
                    }
                    Button {
                        id: globalTopicPrimary
                        objectName: "globalTopicCreateButton"
                        visible: chatTabs.currentIndex === 2
                        enabled: !root.chatMutationBusy && (root.globalTopicUncertain ? root.controller.canCheckTopicCreation : root.controller.canCreateTopicIn(root.globalTopicTarget) && globalTopicName.text.trim().length > 0)
                        height: 32
                        width: contentItem.implicitWidth + 24
                        padding: 0
                        opacity: enabled ? 1 : 0.5
                        text: root.globalTopicUncertain ? qsTr("Check status") : qsTr("Create")
                        contentItem: Text { text: globalTopicPrimary.text; textFormat: Text.PlainText; font.pixelSize: 14; color: root.colors.bg; verticalAlignment: Text.AlignVCenter; horizontalAlignment: Text.AlignHCenter }
                        background: Rectangle { radius: 6; color: root.colors.accent; border.width: globalTopicPrimary.visualFocus ? 1 : 0; border.color: root.colors["text-primary"] }
                        onClicked: root.submitGlobalTopic()
                        KeyNavigation.backtab: channelCancel
                        KeyNavigation.tab: createChatClose
                    }
                    NativeButton {
                        id: directCheck
                        objectName: "directChatCheckButton"
                        visible: chatTabs.currentIndex === 0 && root.directViewIntent && root.controller.directChatUncertain
                        enabled: root.controller.canCheckDirectChat && !root.chatMutationBusy
                        colors: root.colors
                        height: 32
                        text: qsTr("Check status")
                        onClicked: root.controller.checkDirectChat()
                        KeyNavigation.backtab: channelCancel
                        KeyNavigation.tab: directRetry.visible && directRetry.enabled ? directRetry : createChatClose
                    }
                    NativeButton {
                        id: directRetry
                        objectName: "directChatRetryButton"
                        visible: chatTabs.currentIndex === 0 && root.directViewIntent && root.controller.canRetryDirectChat
                        enabled: root.controller.canRetryDirectChat && !root.chatMutationBusy
                        colors: root.colors
                        height: 32
                        text: qsTr("Retry")
                        onClicked: root.controller.retryDirectChat()
                        KeyNavigation.backtab: directCheck.visible && directCheck.enabled ? directCheck : channelCancel
                        KeyNavigation.tab: createChatClose
                    }
                    Button {
                        id: channelCheck
                        objectName: "channelCheckSecondaryButton"
                        visible: chatTabs.currentIndex === 1 && root.channelPartialMembers && root.controller.canCheckChannelCreation
                        enabled: root.controller.canCheckChannelCreation && !root.controller.channelCreationBusy
                        text: qsTr("Check status")
                        height: 32
                        width: contentItem.implicitWidth + 24
                        padding: 0
                        opacity: enabled ? 1 : 0.5
                        contentItem: Text { text: channelCheck.text; textFormat: Text.PlainText; font.pixelSize: 14; color: root.colors["text-muted"]; verticalAlignment: Text.AlignVCenter; horizontalAlignment: Text.AlignHCenter }
                        background: Rectangle { radius: 6; color: channelCheck.hovered ? root.colors["sidebar-item-hover"] : "transparent"; border.width: channelCheck.visualFocus ? 1 : 0; border.color: root.colors.accent }
                        onClicked: root.runChannelAction("check")
                        KeyNavigation.tab: channelPrimary.enabled ? channelPrimary : createChatClose
                    }
                    Button {
                        id: channelPrimary
                        objectName: root.channelPrimaryAction === "check" ? "channelCheckButton" : root.channelPrimaryAction === "add" || root.channelPrimaryAction === "retry" ? "channelRetryMembersButton" : "channelCreateButton"
                        visible: chatTabs.currentIndex === 1 && root.channelPrimaryAction !== "none"
                        enabled: !root.chatMutationBusy && (root.channelPrimaryAction === "check" ? root.controller.canCheckChannelCreation : root.channelPrimaryAction === "add" || root.channelPrimaryAction === "retry" ? root.controller.canRetryChannelMembers : root.controller.canCreateChannel && root.controller.channelMembersVerified && channelNameInput.text.trim().length > 0)
                        text: root.channelPrimaryAction === "check" ? qsTr("Check status") : root.channelPrimaryAction === "add" ? qsTr("Add members") : root.channelPrimaryAction === "retry" ? qsTr("Retry members") : qsTr("Create")
                        height: 32
                        width: contentItem.implicitWidth + 24
                        padding: 0
                        opacity: enabled ? 1 : 0.5
                        contentItem: Text { text: channelPrimary.text; textFormat: Text.PlainText; font.pixelSize: 14; color: root.colors.bg; verticalAlignment: Text.AlignVCenter; horizontalAlignment: Text.AlignHCenter }
                        background: Rectangle { radius: 6; color: root.colors.accent; border.width: channelPrimary.visualFocus ? 1 : 0; border.color: root.colors["text-primary"] }
                        onClicked: root.runChannelAction(root.channelPrimaryAction)
                        KeyNavigation.tab: createChatClose
                    }
                }
            }
        }
    }

    Popup {
        id: globalTopicOptions
        objectName: "globalTopicOptions"
        parent: Overlay.overlay
        readonly property point anchorPosition: {
            // This Item popup is translated only. Observe each parent offset,
            // including the Column polish that positions the current tab body.
            let item = globalTopicSelector;
            let x = 0, y = 0;
            while (item && item !== Overlay.overlay) {
                x += item.x; y += item.y; item = item.parent;
            }
            return Qt.point(x, y);
        }
        readonly property real desiredHeight: Math.min(320, root.globalTopicStreams.length ? root.globalTopicStreams.length * 36 + 10 : 62)
        readonly property real below: root.height - anchorPosition.y - globalTopicSelector.height - 14
        readonly property real above: anchorPosition.y - 14
        readonly property bool placeAbove: below < desiredHeight && above > below
        width: Math.min(globalTopicSelector.width, Math.max(0, root.width - 16))
        height: Math.max(0, Math.min(desiredHeight, placeAbove ? above : below))
        x: Math.max(8, Math.min(root.width - width - 8, anchorPosition.x))
        y: placeAbove ? anchorPosition.y - height - 6 : anchorPosition.y + globalTopicSelector.height + 6
        padding: 5
        focus: false; modal: false; dim: false
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onClosed: { topicTypeahead.stop(); globalTopicSelector.typeahead = ""; }
        background: Rectangle {
            objectName: "globalTopicOptionsSurface"
            radius: 12; color: root.colors["bg-elevated"]; border.color: root.colors["border-subtle"]
            readonly property bool shadowsSupported: GraphicsInfo.api !== GraphicsInfo.Software && GraphicsInfo.api !== GraphicsInfo.OpenVG && GraphicsInfo.api !== GraphicsInfo.Unknown && GraphicsInfo.api !== GraphicsInfo.Null
            layer.enabled: globalTopicOptions.visible && shadowsSupported
            layer.effect: MultiEffect {
                shadowEnabled: true; shadowColor: "black"; shadowOpacity: 0.16
                shadowVerticalOffset: 12; shadowScale: 0.99; shadowBlur: 1; blurMax: 40
            }
        }
        contentItem: ListView {
            id: topicOptionsList
            objectName: "globalTopicOptionsList"
            clip: true
            model: root.globalTopicStreams
            boundsBehavior: Flickable.StopAtBounds
            delegate: Button {
                id: topicOption
                required property var modelData
                required property int index
                objectName: "globalTopicOption-" + modelData.id
                width: ListView.view.width; height: 36; padding: 0; horizontalPadding: 0; hoverEnabled: true
                focusPolicy: Qt.NoFocus
                Accessible.role: Accessible.ListItem
                Accessible.name: String(modelData.name)
                Accessible.selected: modelData.id === root.globalTopicTarget
                onHoveredChanged: if (hovered) globalTopicSelector.activeIndex = index
                onClicked: globalTopicSelector.choose(index)
                background: Rectangle { radius: 6; color: topicOption.hovered || globalTopicSelector.activeIndex === topicOption.index ? root.colors["card-bg-active"] : "transparent" }
                contentItem: Item {
                    Text {
                        x: 10; width: parent.width - 58; height: parent.height
                        text: topicOption.modelData.name; textFormat: Text.PlainText
                        elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter
                        color: root.colors["text-primary"]; font.pixelSize: 14
                    }
                    NativeIconButton {
                        x: parent.width - 26; y: 10; width: 16; height: 16
                        visible: topicOption.modelData.id === root.globalTopicTarget
                        colors: root.colors; asset: "check.svg"; glyphSize: 16
                        glyphWidth: 16; glyphHeight: 12
                        enabled: false; icon.color: root.colors.accent
                        Accessible.ignored: true; background: Item {}
                    }
                }
            }
            Text {
                visible: !root.globalTopicStreams.length
                x: 12; y: 16; width: parent.width - 24
                text: qsTr("No options available"); textFormat: Text.PlainText
                font.pixelSize: 14; color: root.colors["text-muted"]
            }
            ScrollBar.vertical: ScrollBar {}
        }
    }

    // Issue #234 (review finding 4576938732, R9): the unresolved-encrypted-
    // creations inventory. Rows carry safe bookkeeping metadata only — the
    // attempt uuid and its created time — never key material. Check re-adopts
    // the staged key when the channel turns out to exist; Discard is the
    // informed explicit key-discard: it warns by name that a channel which
    // appears later can never be read, and only the confirmation of that
    // warning calls the controller with confirmed == true.
    Dialog {
        id: unresolvedChannelCreationsDialog
        objectName: "unresolvedChannelCreationsDialog"
        title: qsTr("Unresolved encrypted channel creations")
        header: null
        footer: null
        property string statusText: ""
        property var stageRows: []
        property var discardTarget: null
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(560, root.width - 32)
        padding: 1 // The CSS border encloses, rather than overlays, the content box.
        modal: true
        dim: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        Overlay.modal: Rectangle { color: "#80000000" }
        background: Rectangle {
            radius: 10
            color: root.colors["bg-elevated"]
            border.color: root.colors["border-subtle"]
        }
        function refresh() {
            unresolvedChannelCreationsDialog.stageRows = root.controller.unresolvedChannelCreateStages()
        }
        function createdText(rowData) {
            return qsTr("Created: %1").arg(Qt.formatDateTime(new Date(rowData.createdAt), "yyyy-MM-dd hh:mm"))
        }
        onOpened: { statusText = ""; discardTarget = null; refresh() }
        onClosed: discardTarget = null
        Connections {
            target: root.controller
            function onChannelCreateStageResolved(attempt, outcome, message) {
                unresolvedChannelCreationsDialog.statusText = message
                unresolvedChannelCreationsDialog.discardTarget = null
                unresolvedChannelCreationsDialog.refresh()
            }
        }
        contentItem: Column {
            spacing: 0
            Column {
                width: parent.width
                spacing: 4
                padding: 20
                bottomPadding: 16
                Text {
                    width: parent.width
                    text: qsTr("Unresolved encrypted channel creations")
                    textFormat: Text.PlainText
                    font.pixelSize: 16; font.weight: Font.DemiBold
                    lineHeightMode: Text.FixedHeight; lineHeight: 24
                    color: root.colors["text-primary"]
                }
                Text {
                    width: parent.width
                    text: qsTr("Check re-adopts the key if the channel landed; discard is final and may make a late channel unreadable.")
                    textFormat: Text.PlainText; wrapMode: Text.Wrap
                    color: root.colors["text-muted"]; font.pixelSize: 13
                    lineHeightMode: Text.FixedHeight; lineHeight: 18
                }
            }
            Rectangle { width: parent.width; height: 1; color: root.colors["border-subtle"] }
            Flickable {
                width: parent.width
                height: Math.min(280, Math.max(72, stageColumn.height + 32))
                contentHeight: stageColumn.height + 32
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                Column {
                    id: stageColumn
                    x: 16; y: 16; width: parent.width - 32
                    spacing: 10
                    Text {
                        objectName: "channelStageEmpty"
                        width: parent.width
                        visible: !unresolvedChannelCreationsDialog.stageRows.length
                        text: qsTr("All attempts are now resolved.")
                        textFormat: Text.PlainText
                        color: root.colors["text-muted"]; font.pixelSize: 14
                        horizontalAlignment: Text.AlignHCenter
                    }
                    Repeater {
                        model: unresolvedChannelCreationsDialog.stageRows
                        delegate: Rectangle {
                            id: channelStageRow
                            required property var modelData
                            objectName: "channelStageRow"
                            width: stageColumn.width
                            height: channelStageRowContent.height + 20
                            radius: 8
                            color: root.colors.bg
                            border.color: root.colors["border-subtle"]
                            Column {
                                id: channelStageRowContent
                                x: 12; y: 10; width: parent.width - 24
                                spacing: 4
                                Text {
                                    objectName: "channelStageAttempt-" + channelStageRow.modelData.attempt
                                    width: parent.width
                                    text: channelStageRow.modelData.attempt
                                    textFormat: Text.PlainText
                                    elide: Text.ElideMiddle
                                    color: root.colors["text-primary"]; font.pixelSize: 13
                                }
                                Text {
                                    width: parent.width
                                    text: unresolvedChannelCreationsDialog.createdText(channelStageRow.modelData)
                                    textFormat: Text.PlainText
                                    color: root.colors["text-muted"]; font.pixelSize: 12
                                }
                                Row {
                                    topPadding: 4; spacing: 8
                                    Button {
                                        objectName: "channelStageCheck-" + channelStageRow.modelData.attempt
                                        text: qsTr("Check")
                                        font.pixelSize: 13
                                        Accessible.name: text
                                        onClicked: root.controller.resolveChannelCreateStage(channelStageRow.modelData.attempt)
                                    }
                                    Button {
                                        objectName: "channelStageDiscard-" + channelStageRow.modelData.attempt
                                        text: qsTr("Discard")
                                        font.pixelSize: 13
                                        palette.buttonText: root.colors.danger
                                        Accessible.name: text
                                        onClicked: unresolvedChannelCreationsDialog.discardTarget = channelStageRow.modelData
                                    }
                                }
                            }
                        }
                    }
                }
            }
            Rectangle { width: parent.width; height: 1; color: root.colors["border-subtle"] }
            Column {
                width: parent.width
                spacing: 8
                padding: 16
                Text {
                    objectName: "channelStageStatusText"
                    width: parent.width
                    visible: unresolvedChannelCreationsDialog.statusText.length > 0
                    text: visible ? unresolvedChannelCreationsDialog.statusText : ""
                    textFormat: Text.PlainText; wrapMode: Text.Wrap
                    color: root.colors["text-secondary"]; font.pixelSize: 13
                    lineHeightMode: Text.FixedHeight; lineHeight: 18
                }
                Rectangle {
                    objectName: "channelStageDiscardConfirmPanel"
                    width: parent.width
                    visible: unresolvedChannelCreationsDialog.discardTarget !== null
                    height: visible ? discardConfirmColumn.height + 24 : 0
                    radius: 8
                    color: Qt.rgba(root.colors.danger.r, root.colors.danger.g, root.colors.danger.b, 0.08)
                    border.color: root.colors.danger
                    Column {
                        id: discardConfirmColumn
                        x: 12; y: 12; width: parent.width - 24
                        spacing: 10
                        Text {
                            objectName: "channelStageDiscardWarning"
                            width: parent.width
                            text: qsTr("This channel may still appear later. If it does, it can never be read. Discard this creation attempt and its staged key?")
                            textFormat: Text.PlainText; wrapMode: Text.Wrap
                            color: root.colors["text-primary"]; font.pixelSize: 13
                            lineHeightMode: Text.FixedHeight; lineHeight: 18
                        }
                        Row {
                            spacing: 8
                            Button {
                                objectName: "channelStageDiscardCancel"
                                text: qsTr("Cancel")
                                font.pixelSize: 13
                                Accessible.name: text
                                onClicked: unresolvedChannelCreationsDialog.discardTarget = null
                            }
                            Button {
                                objectName: "channelStageDiscardConfirm"
                                text: qsTr("Discard key")
                                font.pixelSize: 13
                                palette.buttonText: root.colors.danger
                                Accessible.name: text
                                onClicked: {
                                    const target = unresolvedChannelCreationsDialog.discardTarget
                                    if (target)
                                        root.controller.discardChannelCreateStage(target.attempt, true)
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Issue #215 create-poll dialog (docs/NATIVE_DESKTOP_POLL_CREATION.md).
    NativePollDialog {
        id: pollCreateDialog
        parent: Overlay.overlay
        colors: root.colors
        controller: root.controller
    }

    Popup {
        id: createTopicDialog
        objectName: "createTopicDialog"
        parent: Overlay.overlay
        x: Math.round((root.width - width) / 2)
        y: (root.height - height) / 2
        width: Math.min(root.width - 48, 448)
        height: topicFormContent.implicitHeight + 50
        padding: 25
        modal: true
        dim: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        Overlay.modal: Rectangle { color: "#80000000" }
        background: Rectangle {
            id: topicFormSurface
            objectName: "topicFormSurface"
            radius: 10
            color: root.colors["bg-elevated"]
            border.color: root.colors["border-subtle"]
            // Shader effects are unavailable in Qt's software/OpenVG backend.
            // Keep the complete plain card there, without an invisible layer.
            readonly property bool shadowsSupported: GraphicsInfo.api !== GraphicsInfo.Software && GraphicsInfo.api !== GraphicsInfo.OpenVG && GraphicsInfo.api !== GraphicsInfo.Unknown && GraphicsInfo.api !== GraphicsInfo.Null
            layer.enabled: createTopicDialog.visible && shadowsSupported
            layer.effect: MultiEffect {
                objectName: "topicFormShadow"
                shadowEnabled: true
                shadowColor: "black"
                shadowOpacity: 0.16
                shadowVerticalOffset: 12
                shadowScale: 0.99
                shadowBlur: 1
                blurMax: 40
            }
        }
        onOpened: topicFormClose.forceActiveFocus(root.topicFormKeyboard ? Qt.TabFocusReason : Qt.MouseFocusReason)
        onClosed: {
            topicNameInput.text = "";
            if (root.streamMenuAnchor)
                root.streamMenuAnchor.forceActiveFocus(root.topicFormKeyboard ? Qt.TabFocusReason : Qt.MouseFocusReason);
            messageList.scheduleVisibleRead();
        }
        contentItem: Column {
            id: topicFormContent
            spacing: 0
            RowLayout {
                width: parent.width
                height: 26
                spacing: 12
                Text {
                    objectName: "topicFormTitle"
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    text: qsTr("Create topic")
                    textFormat: Text.PlainText
                    color: root.colors["text-primary"]
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                    lineHeightMode: Text.FixedHeight
                    lineHeight: 24
                    elide: Text.ElideRight
                }
                NativeIconButton {
                    id: topicFormClose
                    objectName: "topicFormClose"
                    colors: root.colors
                    Layout.preferredWidth: 26
                    Layout.preferredHeight: 26
                    asset: "close.svg"
                    glyphSize: 18
                    iconColor: root.colors["text-muted"]
                    help: qsTr("Close")
                    onClicked: createTopicDialog.close()
                    KeyNavigation.tab: topicNameInput.enabled ? topicNameInput : topicFormCancel
                }
            }
            Item { width: 1; height: 16 }
            Text {
                objectName: "topicFormTarget"
                width: parent.width
                height: 20
                text: { const stream = root.currentStream(root.topicFormTarget); return stream ? stream.name : ""; }
                textFormat: Text.PlainText
                color: root.colors["text-secondary"]
                font.pixelSize: 14
                lineHeightMode: Text.FixedHeight
                lineHeight: 20
                elide: Text.ElideRight
            }
            Item { width: 1; height: 16 }
            Text {
                width: parent.width
                height: 20
                text: qsTr("Topic name")
                textFormat: Text.PlainText
                color: root.colors["text-muted"]
                font.pixelSize: 14
                lineHeightMode: Text.FixedHeight
                lineHeight: 20
            }
            Item { width: 1; height: 6 }
            TextField {
                id: topicNameInput
                objectName: "topicNameInput"
                width: parent.width
                height: 40.84375
                font.pixelSize: 16
                leftPadding: 13
                rightPadding: 13
                topPadding: 9
                bottomPadding: 9
                color: root.colors["text-primary"]
                placeholderTextColor: root.colors["text-muted"]
                selectionColor: root.colors.accent
                selectedTextColor: root.colors["on-accent"]
                placeholderText: qsTr("Topic name")
                Accessible.name: qsTr("Topic name")
                selectByMouse: true
                enabled: !root.topicFormBusy && !root.topicFormUncertain
                opacity: enabled ? 1 : 0.6
                // Core enforces Unicode code points. TextField's UTF-16
                // maximumLength would incorrectly reject astral characters.
                onAccepted: root.submitTopicForm()
                KeyNavigation.tab: topicFormCancel
                background: Rectangle {
                    radius: 6
                    color: root.colors.bg
                    border.color: root.colors["border-subtle"]
                }
            }
            Text {
                id: topicFormError
                objectName: "topicFormError"
                width: parent.width
                visible: root.topicFormIntent && root.controller.topicCreationError.length > 0
                topPadding: 12
                text: visible ? root.controller.topicCreationError : ""
                textFormat: Text.PlainText
                color: root.colors["text-secondary"]
                font.pixelSize: 14
                wrapMode: Text.Wrap
                Accessible.role: Accessible.AlertMessage
            }
            Item { width: 1; height: 16 }
            Row {
                anchors.right: parent.right
                height: 36
                spacing: 8
                Button {
                    id: topicFormCancel
                    objectName: "topicFormCancel"
                    height: 36
                    width: contentItem.implicitWidth + 32
                    horizontalPadding: 16
                    text: qsTr("Cancel")
                    font.pixelSize: 14
                    contentItem: Text {
                        text: topicFormCancel.text
                        textFormat: Text.PlainText
                        color: root.colors["text-muted"]
                        font: topicFormCancel.font
                        verticalAlignment: Text.AlignVCenter
                        horizontalAlignment: Text.AlignHCenter
                    }
                    background: Rectangle {
                        radius: 6
                        color: topicFormCancel.hovered ? root.colors["sidebar-item-hover"] : "transparent"
                        border.width: topicFormCancel.visualFocus ? 1 : 0
                        border.color: root.colors.accent
                    }
                    onClicked: createTopicDialog.close()
                    KeyNavigation.tab: topicFormSubmit.enabled ? topicFormSubmit : topicFormClose
                }
                Button {
                    id: topicFormSubmit
                    objectName: "topicFormSubmit"
                    height: 36
                    width: contentItem.implicitWidth + 32
                    horizontalPadding: 16
                    text: root.topicFormUncertain ? qsTr("Check status") : qsTr("Create")
                    opacity: enabled ? 1 : 0.5
                    enabled: !root.topicFormBusy && (root.topicFormUncertain ? root.controller.canCheckTopicCreation : root.controller.canCreateTopicIn(root.topicFormTarget) && topicNameInput.text.trim().length > 0)
                    contentItem: Row {
                        spacing: 8
                        BusyIndicator {
                            visible: root.topicFormBusy
                            running: visible
                            width: visible ? 16 : 0
                            height: 20
                            padding: 0
                            palette.dark: root.colors["on-accent"]
                        }
                        Text {
                            text: topicFormSubmit.text
                            textFormat: Text.PlainText
                            color: root.colors["on-accent"]
                            font.pixelSize: 14
                            font.weight: Font.Medium
                            height: 20
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    background: Rectangle {
                        radius: 6
                        color: root.colors.accent
                        border.width: topicFormSubmit.visualFocus ? 1 : 0
                        border.color: root.colors["text-primary"]
                    }
                    onClicked: root.submitTopicForm()
                    KeyNavigation.tab: topicFormClose
                }
            }
        }
    }

    Menu {
        id: messageMenu
        objectName: "messageMenu"
        width: 242
        focus: true
        topPadding: 5
        bottomPadding: 5
        leftPadding: 1
        rightPadding: 1
        modal: false
        dim: false
        background: Rectangle {
            objectName: "messageMenuSurface"
            radius: 6
            color: root.colors["bg-elevated"]
            border.color: root.colors["border-subtle"]
        }
        onClosed: if (reactionPicker.visible)
            reactionPicker.close()
        onAboutToShow: root.quickUsage = root.preferences.quickReactionUsage
        function focusEdge(last) {
            // Qt Menu handles Up/Down; the desktop context menu also has
            // Home/End. Its custom reaction bar is not a text action.
            for (let i = last ? count - 1 : 0; i >= 0 && i < count; i += last ? -1 : 1) {
                const entry = itemAt(i) as MessageMenuAction;
                if (entry && entry.visible && entry.enabled && entry.text && entry.text.length) {
                    currentIndex = i;
                    entry.forceActiveFocus(Qt.TabFocusReason);
                    return;
                }
            }
        }
        MenuItem {
            objectName: "reactionMenuBar"
            implicitHeight: 45
            height: visible ? 45 : 0
            leftPadding: 12
            rightPadding: 12
            topPadding: 8
            bottomPadding: 9
            background: Rectangle {
                color: "transparent"
                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 1
                    color: root.colors["border-subtle"]
                }
            }
            contentItem: RowLayout {
                spacing: 2
                Flickable {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 28
                    contentWidth: menuReactions.width
                    contentHeight: 28
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    Row {
                        id: menuReactions
                        spacing: 2
                        Repeater {
                            model: root.quickReactionChoices
                            delegate: ToolButton {
                                id: menuReactionChoice
                                required property var modelData
                                objectName: "menu-reaction-" + modelData.emojiName
                                width: 28
                                height: 28
                                padding: 0
                                enabled: root.messageMutationAllowed && !Boolean(root.currentMessage(root.menuMessageId).reactionBusy)
                                hoverEnabled: true
                                Accessible.name: qsTr("Add reaction: %1").arg(modelData.emojiName)
                                background: Rectangle {
                                    radius: 4
                                    color: menuReactionChoice.hovered || menuReactionChoice.visualFocus ? root.colors["sidebar-item-hover"] : "transparent"
                                }
                                contentItem: Text {
                                    text: menuReactionChoice.modelData.pickerUnicode
                                    textFormat: Text.PlainText
                                    font.family: fullReactionPicker.emojiFamily
                                    font.pixelSize: Math.round(fullReactionPicker.emojiPixelSize * 0.5)
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                                onClicked: root.chooseQuickReaction(root.menuMessageId, modelData.emojiName)
                            }
                        }
                    }
                }
                ToolButton {
                    id: moreReactions
                    objectName: "moreReactionsButton"
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 28
                    padding: 0
                    enabled: root.messageMutationAllowed && !Boolean(root.currentMessage(root.menuMessageId).reactionBusy)
                    Accessible.name: qsTr("More reactions")
                    display: AbstractButton.IconOnly
                    icon.source: "qrc:/workspace/icons/plus.svg"
                    icon.width: 14
                    icon.height: 14
                    icon.color: hovered ? root.colors["text-primary"] : root.colors["text-muted"]
                    background: Rectangle {
                        radius: 4
                        color: moreReactions.hovered || moreReactions.visualFocus ? root.colors["sidebar-item-hover"] : "transparent"
                    }
                    onClicked: root.openReactionPicker(root.menuMessageId, moreReactions)
                }
            }
        }
        MessageMenuAction {
            objectName: "requestMessageKeyAction"
            visible: root.menuMessageKeyMissing
            enabled: visible && root.chatEncryption.canRequest && !root.chatEncryption.busy
            asset: "chat_lock.svg"
            text: qsTr("Request key for this message")
            onTriggered: root.requestMessageKey(root.menuMessageId)
        }
        MessageMenuSeparator { visible: root.menuMessageKeyMissing }
        MessageMenuAction {
            objectName: "replyMessageAction"
            visible: root.menuMessageOrdinaryReadable && !root.aggregateConversation
            enabled: visible
            asset: "reply.svg"
            text: qsTr("Reply")
            // An open session collects another quote tab instead of replacing the
            // active quote (reference routes the reply action to the add handler
            // whenever the session has tabs, workspace-message-list.ui.tsx:1284).
            onTriggered: {
                const selection = root.menuSelectedText;
                root.controller.replyMessageId.length > 0
                    ? root.controller.addReplyTab(root.menuMessageId, selection)
                    : root.controller.beginReply(root.menuMessageId, selection);
            }
        }
        MessageMenuSeparator {
            visible: root.menuMessageOrdinaryReadable && !root.aggregateConversation
        }
        MessageMenuAction {
            objectName: "copyMessageImageAction"
            // The reference adds the item only for an image context file
            // (workspace-message-bubble-menu.ui.tsx:335), first in the
            // secondary section, before copy-link (:350-368).
            visible: root.menuMessageReadable
                && String(root.controller.mediaPreviewReference(root.menuMessageContent).kind || "") === "image"
            enabled: visible
            asset: "copy.svg"
            text: qsTr("Copy image")
            onTriggered: root.controller.copyMessageImage(root.menuMessageId)
        }
        MessageMenuAction {
            objectName: "copyMessageLinkAction"
            visible: root.menuMessageReadable
            enabled: visible
            asset: "copy.svg"
            // Reference label message.copyMessageLink
            // (workspace-message-bubble-menu.ui.tsx:247); issue #110 separates it
            // from the hyperlink "Copy link" row below.
            text: qsTr("Copy message link")
            onTriggered: root.controller.copyMessageLink(root.menuMessageId)
        }
        MessageMenuAction {
            objectName: "copyBodyLinkAction"
            // The reference adds the item only when the right-click target is an
            // http/https anchor (workspace-message-bubble-menu.ui.tsx:349-360),
            // in the secondary section before "Copy text" (issue #110).
            visible: root.menuContextLink.length > 0 && root.isSafeBodyLink(root.menuContextLink)
            enabled: visible
            asset: "copy.svg"
            text: qsTr("Copy link")
            onTriggered: root.controller.copyToClipboard(root.menuContextLink)
        }
        MessageMenuAction {
            objectName: "copyMessageAction"
            visible: root.menuMessageReadable
            enabled: visible
            asset: "copy.svg"
            text: qsTr("Copy text")
            onTriggered: root.controller.copyMessage(root.menuMessageId)
        }
        MessageMenuAction {
            objectName: "selectMessageAction"
            // The reference puts "Select" last in the bubble menu's secondary
            // section (workspace-message-bubble-menu.ui.tsx:383-395); toggling
            // the first message enters selection mode.
            visible: root.menuMessageOrdinaryReadable && root.messageSelectionSurface
                && !root.aggregateConversation
            enabled: visible
            asset: "check.svg"
            text: qsTr("Select")
            onTriggered: root.toggleMessageSelection(root.menuMessageId)
        }
        MessageMenuSeparator {
            visible: root.menuMessageOwn && root.menuMessageOrdinaryReadable
        }
        MessageMenuAction {
            objectName: "editMessageAction"
            visible: root.menuMessageOwn && root.menuMessageOrdinaryReadable
            asset: "pen.svg"
            text: qsTr("Edit")
            enabled: visible && root.messageMutationAllowed
            onTriggered: root.controller.beginEditMessage(root.menuMessageId)
        }
        MessageMenuAction {
            objectName: "deleteMessageAction"
            visible: root.menuMessageOwn && root.menuMessageOrdinaryReadable
            asset: "delete.svg"
            text: qsTr("Delete")
            enabled: visible && root.messageMutationAllowed
            onTriggered: deleteDialog.open()
        }
    }
    Shortcut {
        // Qt menus can own a popup window. Restrict the application shortcut
        // to this open menu, excluding the picker's text/navigation controls.
        sequence: "Home"
        context: Qt.ApplicationShortcut
        enabled: messageMenu.visible && !reactionPicker.visible
        onActivated: messageMenu.focusEdge(false)
    }
    Shortcut {
        sequence: "End"
        context: Qt.ApplicationShortcut
        enabled: messageMenu.visible && !reactionPicker.visible
        onActivated: messageMenu.focusEdge(true)
    }
    Dialog {
        id: deleteDialog
        anchors.centerIn: parent
        title: qsTr("Delete this message?")
        modal: true
        standardButtons: Dialog.Yes | Dialog.Cancel
        onOpened: deleteDialog.standardButton(Dialog.Yes).enabled = Qt.binding(() => root.messageMutationAllowed)
        onAccepted: {
            if (root.messageMutationAllowed)
                root.controller.deleteMessage(root.menuMessageId);
        }
    }
    // Issue #146: top overlay banners over the sidebar/chat split, mirroring
    // the reference layout-top-banner overlay (offline > rate-limit countdown >
    // reconnecting > degraded) and the bootstrap error banner whose Retry
    // re-runs the snapshot bootstrap. Anchored under the header so the split
    // is covered but header clicks keep reaching the top bar.
    Column {
        id: topBannerOverlay
        objectName: "topBannerOverlay"
        anchors.top: topBar.bottom
        anchors.topMargin: 4
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 0
        visible: root.controller.authenticated
            && (root.controller.bootstrapError || root.controller.connectionPhase !== "online")

        Rectangle {
            id: bootstrapErrorBanner
            objectName: "bootstrapErrorBanner"
            visible: root.controller.bootstrapError
            width: topBannerOverlay.width
            // Column still lays out hidden children, so a hidden strip must
            // collapse to zero height to keep the visible one flush at top.
            height: visible ? implicitHeight : 0
            readonly property color accent: root.colors["notice-base"]
            implicitHeight: bootstrapBannerContent.implicitHeight + 24
            color: Qt.rgba(accent.r, accent.g, accent.b, 0.2)
            Accessible.role: Accessible.AlertMessage
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 2
                color: Qt.rgba(bootstrapErrorBanner.accent.r, bootstrapErrorBanner.accent.g,
                    bootstrapErrorBanner.accent.b, 0.6)
            }
            RowLayout {
                id: bootstrapBannerContent
                anchors.fill: parent
                anchors.leftMargin: 16; anchors.rightMargin: 16; anchors.topMargin: 12; anchors.bottomMargin: 12
                spacing: 12
                Text {
                    objectName: "bootstrapErrorText"
                    Layout.fillWidth: true
                    text: root.controller.error.length > 0
                        ? root.controller.error : qsTr("Failed to load your messages.")
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: root.colors["text-primary"]
                    font.pixelSize: 14
                    Accessible.role: Accessible.AlertMessage
                    Accessible.name: text
                }
                NativeButton {
                    objectName: "bootstrapRetryButton"
                    colors: root.colors
                    primary: true
                    text: qsTr("Retry")
                    onClicked: root.controller.retryBootstrap()
                }
            }
        }

        Rectangle {
            id: connectionBanner
            objectName: "connectionBanner"
            visible: root.controller.connectionPhase !== "online"
            width: topBannerOverlay.width
            height: visible ? implicitHeight : 0
            readonly property color accent: root.controller.connectionSeverity === "critical"
                ? root.colors.danger : root.colors["notice-base"]
            readonly property string bannerText: root.controller.connectionPhase === "offline"
                ? qsTr("You are offline.")
                : root.controller.connectionPhase === "rate_limited"
                    ? qsTr("Too many requests. Retrying in %1 seconds.")
                        .arg(root.controller.connectionRetryInSeconds)
                    : root.controller.connectionPhase === "degraded"
                        ? qsTr("The connection is degraded.")
                        : qsTr("Reconnecting...")
            implicitHeight: connectionBannerContent.implicitHeight + 24
            color: Qt.rgba(accent.r, accent.g, accent.b, 0.2)
            Accessible.role: Accessible.AlertMessage
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 2
                color: Qt.rgba(connectionBanner.accent.r, connectionBanner.accent.g,
                    connectionBanner.accent.b, 0.6)
            }
            RowLayout {
                id: connectionBannerContent
                anchors.fill: parent
                anchors.leftMargin: 16; anchors.rightMargin: 16; anchors.topMargin: 12; anchors.bottomMargin: 12
                spacing: 12
                Text {
                    objectName: "connectionBannerText"
                    Layout.fillWidth: true
                    text: connectionBanner.bannerText
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: root.colors["text-primary"]
                    font.pixelSize: 14
                    Accessible.role: Accessible.AlertMessage
                    Accessible.name: text
                }
            }
        }
    }
    NativeMediaViewer {
        viewer: root.controller.mediaViewer
        colors: root.colors
        // Calls keep precedence over media. Suspend only the popup surface,
        // preserving the gallery and its composer/read guards until return.
        visible: viewer.open && !root.controller.incomingCall.visible
            && root.controller.callWindow.mode !== 1 // Expanded
        onRequestDownload: (uuid) => {
            root.downloadOwner = root.controller.ownerKey;
            root.downloadId = uuid;
            attachmentSaveDialog.open();
        }
    }
    // Call window shell (#162, docs/NATIVE_DESKTOP_CALL_WINDOW.md): expanded
    // mode is a scrim dialog inside the app (outside click / Escape only
    // minimize), minimized mode is the frameless PiP tool window bound to the
    // controller bounds (32 px inset / 16 px under the 640 breakpoint /
    // 320x220 default PiP 20 px from the bottom-right).
    Connections {
        target: root
        enabled: root.controller !== undefined && root.controller !== null
        function onWidthChanged() { root.controller.callWindow.setViewportSize(Qt.size(root.width, root.height)) }
        function onHeightChanged() { root.controller.callWindow.setViewportSize(Qt.size(root.width, root.height)) }
        function onActiveChanged() { root.controller.callWindow.setFocused(root.active) }
    }
    Component.onCompleted: {
        if (controller)
            controller.callWindow.setViewportSize(Qt.size(root.width, root.height))
    }
    // #162 finding 4: the shell's fullscreen control drives the real window
    // (the native counterpart of the reference browser-fullscreen on the call
    // modal), and a window-side exit (WM shortcut / Esc) syncs the controller
    // state back so the button label and glyph never go stale.
    // 0 == Window.Automatic: the enum is not resolvable in a root-Window
    // property initializer, so keep the plain value here.
    property int callRestoreVisibility: 0
    function syncCallFullscreenFromWindow() {
        if (controller && controller.callWindow.fullscreen
                && visibility !== Window.FullScreen)
            controller.callWindow.syncFullscreen(false)
    }
    onVisibilityChanged: root.syncCallFullscreenFromWindow()
    Connections {
        target: root.controller.callWindow
        enabled: root.controller !== undefined && root.controller !== null
        function onFullscreenChanged() {
            if (root.controller.callWindow.fullscreen) {
                if (root.visibility !== Window.FullScreen) {
                    root.callRestoreVisibility = root.visibility
                    root.visibility = Window.FullScreen
                }
            } else if (root.visibility === Window.FullScreen) {
                root.visibility = root.callRestoreVisibility
            }
        }
    }
    Item {
        id: callWindowOverlayItem
        objectName: "callWindowOverlay"
        anchors.fill: parent
        z: 1000
        visible: root.controller.callWindow.mode === 1 // Expanded
        Rectangle {
            // Scrim: clicking it dismisses to PiP, never ends the call
            // (handleOpenChange maps an expanded dismiss to minimize).
            objectName: "callWindowScrim"
            anchors.fill: parent
            color: Qt.alpha("#000000", 0.45)
            MouseArea {
                anchors.fill: parent
                onClicked: root.controller.callWindow.requestDismiss()
            }
        }
        Item {
            objectName: "callWindowExpandedHost"
            x: root.controller.callWindow.bounds.x
            y: root.controller.callWindow.bounds.y
            width: root.controller.callWindow.bounds.width
            height: root.controller.callWindow.bounds.height
            NativeCallWindow {
                id: callWindowExpandedShell
                objectName: "callWindowExpanded"
                anchors.fill: parent
                colors: root.colors
                controller: root.controller.callWindow
                pipMode: false
            }
        }
        Shortcut {
            sequence: "Escape"
            enabled: callWindowOverlayItem.visible
            onActivated: root.controller.callWindow.requestDismiss()
        }
    }
    Item {
        id: incomingCallOverlayItem
        objectName: "incomingCallOverlay"
        anchors.fill: parent
        // Above the call-window overlay: the store never shows an invite
        // while a call is active, so the two are never visible together.
        z: 1001
        visible: root.controller.incomingCall.visible
        NativeIncomingCallWindow {
            objectName: "incomingCallPresenter"
            anchors.fill: parent
            colors: root.colors
            invite: root.controller.calls.incomingInvite
            variant: root.controller.incomingCall.variant
            videoEnabled: root.controller.incomingCall.currentVideoEnabled
            onAcceptRequested: root.controller.incomingCall.accept()
            onDeclineRequested: root.controller.incomingCall.decline()
            onDismissRequested: root.controller.incomingCall.dismiss()
            onToggleVideoRequested: root.controller.incomingCall.toggleVideoEnabled(
                String(root.controller.calls.incomingInvite.messageId || ""))
        }
        // Escape closes the invite without a decline action (reference
        // useEscapeListener in jitsi-call-incoming-modal.tsx:70-73).
        Shortcut {
            sequence: "Escape"
            enabled: incomingCallOverlayItem.visible
            onActivated: root.controller.incomingCall.dismiss()
        }
    }
    // #162 finding 2: exactly one embed surface per call session. The Loader
    // activates as soon as the meeting URL is valid (Loading - the view's own
    // load events drive Ready/LoadFailed, finding 1) and is re-parented
    // between the expanded shell and the PiP window, so minimize/expand only
    // moves the presentation and never creates a second WebEngine session
    // (the reference React.memo mount-count contract). Dynamic source keeps
    // the QtWebEngine import lazy for every other window in the app.
    // #263: LoadFailed keeps the surface alive too. Once the real Qt load
    // status was wired (#250), a failing load deactivated the ONLY embed
    // loader, so any expand/PiP toggle afterwards held zero active loaders -
    // the one-surface contract must hold in every live-session phase, and the
    // shell overlays its retry affordance over the failed view. The retry
    // re-requests the meeting URL through the live view (see
    // NativeCallWebEngineView.qml); Idle and InvalidUrl still own no surface.
    property var callPipEmbedHost: null
    Loader {
        objectName: "callWindowEmbedLoader"
        active: root.controller.callWindow.mode !== 0
            && (root.controller.callWindow.embedState === 1 // Loading
                || root.controller.callWindow.embedState === 2 // Ready
                || root.controller.callWindow.embedState === 3) // LoadFailed
        source: "NativeCallWebEngineView.qml"
        onLoaded: if (item) {
            item.controller = root.controller.callWindow
            // The live handshake's C++ counterpart (finding 5, round 4).
            item.bridge = root.controller.embedBridge
        }
        parent: root.callPipEmbedHost !== null ? root.callPipEmbedHost
            : callWindowExpandedShell.embedHost
        anchors.fill: parent
    }
    // The PiP surface is a separate frameless tool window: Qt does not
    // instantiate Window elements nested in a Window's content, so it is
    // created when the shell minimizes and destroyed when it expands or ends.
    property var callPipInstance: null
    Connections {
        target: root.controller.callWindow
        function onModeChanged() {
            const minimized = root.controller.callWindow.mode === 2
            if (minimized && root.callPipInstance === null)
                root.callPipInstance = callPipComponent.createObject(root)
            else if (!minimized && root.callPipInstance !== null) {
                const previous = root.callPipInstance
                // Hand the single embed surface back to the expanded shell
                // before the PiP window (its former host) goes away.
                root.callPipEmbedHost = null
                root.callPipInstance = null
                previous.visible = false
                previous.destroy()
            }
        }
    }
    Component {
        id: callPipComponent
        Window {
            id: callPipWindow
            objectName: "callWindowPipWindow"
            visible: true
            // #162 finding 6: default PiP bounds are viewport-relative and
            // must be mapped through the app window's screen position (the
            // reference computes them from the browser viewport, which always
            // starts at the screen origin). User-placed bounds are already
            // screen coordinates and are used verbatim.
            x: root.controller.callWindow.pipBoundsFromUser
                ? root.controller.callWindow.bounds.x
                : root.x + root.controller.callWindow.bounds.x
            y: root.controller.callWindow.pipBoundsFromUser
                ? root.controller.callWindow.bounds.y
                : root.y + root.controller.callWindow.bounds.y
            width: root.controller.callWindow.bounds.width
            height: root.controller.callWindow.bounds.height
            minimumWidth: 280
            minimumHeight: 180
            title: root.controller.callWindow.titleText
            flags: Qt.Window | Qt.Tool | Qt.FramelessWindowHint
            color: root.colors["bg-elevated"]
            Component.onCompleted: {
                raise()
                // The single embed surface follows the presentation here.
                root.callPipEmbedHost = callPipShellRoot.embedHost
            }
            NativeCallWindow {
                id: callPipShellRoot
                objectName: "callWindowPipShell"
                anchors.fill: parent
                colors: root.colors
                controller: root.controller.callWindow
                pipMode: true
                dragWindow: callPipWindow
            }
            Item {
                // Corner resize handle, clamped to the shared 280x180 minimum.
                objectName: "callWindowResizeGrip"
                width: 16
                height: 16
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.SizeFDiagCursor
                    property real grabWidth: 0
                    property real grabHeight: 0
                    onPressed: (mouse) => {
                        grabWidth = callPipWindow.width
                        grabHeight = callPipWindow.height
                    }
                    onPositionChanged: (mouse) => {
                        if (!pressed)
                            return
                        callPipWindow.width = Math.max(280, grabWidth + mouse.x)
                        callPipWindow.height = Math.max(180, grabHeight + mouse.y)
                    }
                    onReleased: root.controller.callWindow.setPipBounds(
                        Qt.rect(callPipWindow.x, callPipWindow.y,
                            callPipWindow.width, callPipWindow.height))
                }
            }
        }
    }
}
