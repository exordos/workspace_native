pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Right-docked drawer shell (docs/NATIVE_DESKTOP_RIGHT_DRAWER.md).
// Reference: workspace_ui right-drawer.ui.tsx and right-drawer.model.ts at
// 61dacef78dab2f5d0be76f283bebf49fe29b84af. One app-scoped instance stays
// mounted when hidden; content layers (issue #72) mount into the body slot.
// The layer/screen stack mirrors the reference store 1:1: visibility is
// independent from navigation history, opening an existing kind promotes it,
// Back pops only internal screens, and a chat scope reset drops stale screens.
FocusScope {
    id: drawer
    required property var colors
    default property alias bodyChildren: body.data
    property string title: ""
    property bool contentFlush: false
    property bool drawerOpen: false
    property var layers: []

    width: 315
    visible: drawerOpen
    clip: true
    objectName: "nativeRightDrawer"
    z: 2
    Accessible.role: Accessible.ComplementaryContent
    Accessible.name: qsTr("Info panel")
    onDrawerOpenChanged: if (drawerOpen) drawer.forceActiveFocus();

    // Read-only projections for the shell chrome and the window CTest.
    readonly property int layerCount: drawer.layers.length
    readonly property string topLayerKind: drawer.layerCount > 0
        ? String(drawer.layers[drawer.layerCount - 1].kind) : ""
    readonly property int topScreenCount: drawer.layerCount > 0
        ? drawer.layers[drawer.layerCount - 1].screens.length : 0
    readonly property bool canGoBack: drawer.topScreenCount > 1
    readonly property var topScreen: drawer.topScreenCount > 0
        ? drawer.layers[drawer.layerCount - 1].screens[drawer.topScreenCount - 1] : null
    readonly property string currentMode: topScreen && topScreen.mode ? String(topScreen.mode) : "info"
    readonly property string currentAccountScreen: topScreen && topScreen.accountScreen
        ? String(topScreen.accountScreen) : ""
    readonly property string currentChatScreen: topScreen && topScreen.chatScreen
        ? String(topScreen.chatScreen.screen) : ""
    readonly property string currentChatKeyUuid: topScreen && topScreen.chatScreen && topScreen.chatScreen.keyUuid
        ? String(topScreen.chatScreen.keyUuid) : ""
    readonly property string currentProfileOverride: {
        if (!topScreen || !topScreen.profileOverride)
            return "";
        const override = topScreen.profileOverride;
        return String(override.kind) + ":"
            + String(override.kind === "legacy-user-id" ? override.userId : override.userUuid);
    }

    // ---- Reference store port (right-drawer.model.ts:90-408) ----

    function project(open, nextLayers) {
        drawer.drawerOpen = open;
        drawer.layers = nextLayers;
    }
    function screensFor(kind) {
        if (kind === "account")
            return [{ mode: "user-menu", accountScreen: "root" }];
        if (kind === "about")
            return [{ mode: "about" }];
        return [{ mode: "info" }];
    }
    function sameChatScope(left, right) {
        return left != null && right != null
            && left.ownerKey === right.ownerKey
            && left.runtimeGeneration === right.runtimeGeneration
            && left.streamUuid === right.streamUuid;
    }
    function sameProfileOverride(left, right) {
        if (left == null || right == null)
            return left === right;
        if (left.kind !== right.kind)
            return false;
        if (left.kind === "legacy-user-id")
            return String(left.userId) === String(right.userId);
        return left.userUuid === right.userUuid;
    }
    function sameScreen(left, right) {
        return left != null
            && left.mode === right.mode
            && left.accountScreen === right.accountScreen
            && (left.chatScreen === right.chatScreen
                || (sameChatScope(left.chatScreen, right.chatScreen)
                    && left.chatScreen != null && right.chatScreen != null
                    && left.chatScreen.screen === right.chatScreen.screen
                    && left.chatScreen.keyUuid === right.chatScreen.keyUuid))
            && sameProfileOverride(left.profileOverride, right.profileOverride);
    }
    function openUniqueLayer(requestedLayer) {
        const layers = drawer.layers;
        const existingIndex = layers.findIndex(function (layer) { return layer.kind === requestedLayer.kind; });
        if (existingIndex < 0)
            return layers.concat([requestedLayer]);
        if (existingIndex === layers.length - 1)
            return layers.slice(0, -1).concat([requestedLayer]);
        const existingLayer = layers[existingIndex];
        return layers.slice(0, existingIndex)
            .concat(layers.slice(existingIndex + 1), [existingLayer]);
    }
    function setOpen(open) {
        if (!open) {
            drawer.drawerOpen = false;
            return;
        }
        if (drawer.layers.length > 0) {
            drawer.drawerOpen = true;
            return;
        }
        project(true, [{ kind: "chat-info", screens: screensFor("chat-info") }]);
    }
    function toggleVisibility() {
        if (drawer.layers.length === 0) {
            setOpen(true);
            return;
        }
        drawer.drawerOpen = !drawer.drawerOpen;
    }
    function openLayer(kind) {
        project(true, openUniqueLayer({ kind: kind, screens: screensFor(kind) }));
    }
    function toggleChatInfo() {
        if (drawer.topLayerKind === "chat-info")
            toggleVisibility();
        else
            openLayer("chat-info");
    }
    function toggleUserMenu() {
        if (drawer.topLayerKind === "account")
            toggleVisibility();
        else
            openLayer("account");
    }
    function openScreen(screen) {
        const layers = drawer.layers.slice();
        const active = layers.length > 0 ? layers[layers.length - 1] : null;
        if (active == null)
            return;
        if (sameScreen(active.screens[active.screens.length - 1], screen)) {
            project(true, layers);
            return;
        }
        layers[layers.length - 1] = { kind: active.kind, screens: active.screens.concat([screen]) };
        project(true, layers);
    }
    function openAccountScreen(screen) {
        if (drawer.topLayerKind !== "account") {
            openLayer("account");
            if (screen !== "root")
                openScreen({ mode: "user-menu", accountScreen: screen });
            return;
        }
        openScreen({ mode: "user-menu", accountScreen: screen });
    }
    function openChatScreen(chatScreen) {
        if (chatScreen.screen === "local-key" && !chatScreen.keyUuid)
            return;
        if (drawer.topLayerKind !== "chat-info")
            openLayer("chat-info");
        const requested = { mode: "info", chatScreen: chatScreen };
        const active = drawer.layers.length > 0 ? drawer.layers[drawer.layers.length - 1] : null;
        const existingIndex = active != null
            ? active.screens.findIndex(function (screen) { return sameScreen(screen, requested); })
            : -1;
        // Returning from key details reuses the list screen instead of adding a loop to Back.
        if (active != null && existingIndex >= 0) {
            project(true, drawer.layers.slice(0, -1)
                .concat([{ kind: active.kind, screens: active.screens.slice(0, existingIndex + 1) }]));
            return;
        }
        openScreen(requested);
    }
    function resetChatScreens(scope) {
        let changed = false;
        const layers = drawer.layers.map(function (layer) {
            const screens = layer.screens.filter(function (screen) {
                return screen.chatScreen == null || sameChatScope(screen.chatScreen, scope);
            });
            if (screens.length === layer.screens.length)
                return layer;
            changed = true;
            return { kind: layer.kind, screens: screens.length > 0 ? screens : screensFor("chat-info") };
        });
        if (changed)
            project(drawer.drawerOpen, layers);
    }
    function back() {
        const layers = drawer.layers.slice();
        const active = layers.length > 0 ? layers[layers.length - 1] : null;
        if (active == null || active.screens.length <= 1)
            return;
        layers[layers.length - 1] = { kind: active.kind, screens: active.screens.slice(0, -1) };
        project(true, layers);
    }
    function closeCurrent() {
        const layers = drawer.layers.slice(0, -1);
        project(layers.length > 0, layers);
    }
    function close() {
        drawer.closeCurrent();
    }
    function clearAll() {
        project(false, []);
    }
    function openInfo() {
        drawer.openLayer("chat-info");
    }
    function openSettings() {
        drawer.openAccountScreen("settings");
    }
    function openUserMenu() {
        drawer.openLayer("account");
    }
    function openAbout() {
        drawer.openLayer("about");
    }
    function openPersonalInfo() {
        drawer.openAccountScreen("personal-info");
    }
    function openProfileOverride(override) {
        if (drawer.topLayerKind === "chat-info") {
            openScreen({ mode: "info", profileOverride: override });
            return;
        }
        openLayer("chat-info");
        openScreen({ mode: "info", profileOverride: override });
    }
    function openUserProfile(userId) {
        drawer.openProfileOverride({ kind: "legacy-user-id", userId: userId });
    }
    function openWorkspaceUserProfile(userUuid) {
        drawer.openProfileOverride({ kind: "workspace-user-uuid", userUuid: userUuid });
    }
    function clearUserProfileOverride() {
        if (topScreen && topScreen.profileOverride != null)
            drawer.back();
    }

    // Escape removes the active layer (revealing the previous one) and hides
    // the drawer with the last layer, matching the close button contract.
    Shortcut {
        sequence: "Escape"
        context: Qt.WindowShortcut
        enabled: drawer.drawerOpen
        onActivated: drawer.closeCurrent()
    }

    Rectangle {
        objectName: "nativeRightDrawerShell"
        anchors.fill: parent
        color: drawer.colors["bg-elevated"]
        radius: 6
        // The dock consumes input inside its bounds only; the rest of the
        // window stays usable (NativeAboutDrawer parity).
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onWheel: function (event) { event.accepted = true; }
        }
    }
    Item {
        id: gutter
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        anchors.topMargin: 8
        anchors.bottomMargin: 20
        Item {
            id: header
            objectName: "nativeRightDrawerHeader"
            width: gutter.width
            height: 40
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                anchors.topMargin: 4
                anchors.bottomMargin: 4
                spacing: 8
                NativeIconButton {
                    objectName: "nativeRightDrawerBack"
                    visible: drawer.canGoBack
                    colors: drawer.colors
                    Layout.preferredWidth: 32; Layout.preferredHeight: 32
                    width: 32; height: 32; glyphSize: 16
                    asset: "chevron-right.svg"
                    rotation: 180
                    help: qsTr("Back")
                    iconColor: hovered ? drawer.colors["text-primary"] : drawer.colors["text-muted"]
                    background: Rectangle {
                        radius: 6
                        color: parent.hovered ? drawer.colors["card-bg-active"] : "transparent"
                        border.width: parent.visualFocus ? 1 : 0
                        border.color: drawer.colors.accent
                    }
                    onClicked: drawer.back()
                }
                Item {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    implicitHeight: 24
                    Text {
                        objectName: "nativeRightDrawerTitle"
                        anchors.fill: parent
                        visible: drawer.title.trim().length > 0
                        text: drawer.title.trim()
                        textFormat: Text.PlainText
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        lineHeightMode: Text.FixedHeight; lineHeight: 24
                        verticalAlignment: Text.AlignVCenter
                        color: drawer.colors["text-primary"]
                        elide: Text.ElideRight
                        Accessible.role: Accessible.Heading
                    }
                }
                NativeIconButton {
                    objectName: "nativeRightDrawerClose"
                    colors: drawer.colors
                    Layout.preferredWidth: 32; Layout.preferredHeight: 32
                    width: 32; height: 32; glyphSize: 16
                    asset: "close.svg"
                    help: qsTr("Close")
                    iconColor: hovered ? drawer.colors["text-primary"] : drawer.colors["text-muted"]
                    background: Rectangle {
                        radius: 6
                        color: parent.hovered ? drawer.colors["card-bg-active"] : "transparent"
                        border.width: parent.visualFocus ? 1 : 0
                        border.color: drawer.colors.accent
                    }
                    onClicked: drawer.closeCurrent()
                }
            }
        }
        // Body slot: `flex min-h-0 flex-1 flex-col overflow-hidden`. Content
        // flush cancels the shell px-2 here, as a direct child, so a child
        // list can paint edge-to-edge (right-drawer.types.ts contentFlush).
        Item {
            id: body
            objectName: "nativeRightDrawerBody"
            anchors.top: header.bottom
            anchors.bottom: gutter.bottom
            anchors.left: gutter.left
            anchors.right: gutter.right
            anchors.leftMargin: drawer.contentFlush ? -8 : 0
            anchors.rightMargin: drawer.contentFlush ? -8 : 0
            clip: true
        }
    }
}
