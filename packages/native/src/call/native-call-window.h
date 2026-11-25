#pragma once

#include <QObject>
#include <QPointer>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QTimer>
#include <QStringList>
#include <QVariantMap>

#include <functional>

namespace workspace {

// Port of the reference call window shell state machine (workspace_ui
// 61dacef7 jitsi-call-modal-shell.hook.ts, jitsi-call-pip.lib.ts,
// jitsi-call-header.lib.ts, jitsi-participant-count.hook.ts and
// jitsi-call.ui.tsx state slots). All shell geometry, session-stability,
// one-time-config, permission and participant-poll rules live here so the
// offscreen suite can prove them; the WebEngine embed itself stays behind the
// injectable embed factory. See docs/NATIVE_DESKTOP_CALL_WINDOW.md.
class JitsiCallWindowController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int mode READ mode NOTIFY modeChanged)
    Q_PROPERTY(QRect bounds READ bounds NOTIFY boundsChanged)
    Q_PROPERTY(int embedState READ embedState NOTIFY embedStateChanged)
    Q_PROPERTY(int embedCreations READ embedCreations NOTIFY embedCreated)
    Q_PROPERTY(int participantCount READ participantCount NOTIFY participantCountChanged)
    Q_PROPERTY(bool fullscreen READ fullscreen NOTIFY fullscreenChanged)
    // True once the user moved/resized the PiP surface; until then the bounds
    // are viewport-relative defaults the QML maps through the window position.
    Q_PROPERTY(bool pipBoundsFromUser READ pipBoundsFromUser NOTIFY boundsChanged)
    // Localized display text for the QML shell (all follow the shell state).
    Q_PROPERTY(QString titleText READ titleText NOTIFY textsChanged)
    Q_PROPERTY(QString subtitleText READ subtitleText NOTIFY textsChanged)
    Q_PROPERTY(QString stateText READ stateText NOTIFY textsChanged)
    Q_PROPERTY(QString retryText READ retryText CONSTANT)
    Q_PROPERTY(QString fullscreenLabel READ fullscreenLabel NOTIFY textsChanged)
    Q_PROPERTY(QString minimizeLabel READ minimizeLabel NOTIFY textsChanged)
    Q_PROPERTY(QString minimizeTooltip READ minimizeTooltip NOTIFY textsChanged)
    Q_PROPERTY(QString closeLabel READ closeLabel CONSTANT)
    // #846 standalone OS call window: the call identity the reference shell
    // keys its single-window rules on (workspace_ui e0194ffb
    // jitsi-call-window.ts), and the shell visibility gate - the reference
    // creates the window hidden and shows it only after a successful load
    // (jitsi-call-window.ts:161-186), so visibility is exactly
    // "expanded and the embed finished loading".
    Q_PROPERTY(QString callId READ callId NOTIFY callIdChanged)
    Q_PROPERTY(bool windowVisible READ windowVisible NOTIFY windowVisibilityChanged)
    // #846: the shell constants the standalone QML Window binds directly, so
    // the presentation and the tests pin ONE copy of the reference numbers.
    Q_PROPERTY(int standaloneWidth READ standaloneWidth CONSTANT)
    Q_PROPERTY(int standaloneHeight READ standaloneHeight CONSTANT)
    Q_PROPERTY(int standaloneMinWidth READ standaloneMinWidth CONSTANT)
    Q_PROPERTY(int standaloneMinHeight READ standaloneMinHeight CONSTANT)
    Q_PROPERTY(QString standaloneBackground READ standaloneBackground CONSTANT)
    Q_PROPERTY(QString standaloneTitle READ standaloneTitle CONSTANT)
public:
    enum Mode { Closed = 0, Expanded = 1, Minimized = 2 };
    Q_ENUM(Mode)
    // Mirrors the reference apiLoadState plus the invalid-URL and not-open slots.
    enum EmbedState { Idle = 0, Loading = 1, Ready = 2, LoadFailed = 3, InvalidUrl = 4 };
    Q_ENUM(EmbedState)

    // Reference constants: jitsi-call-modal-shell.hook.ts:23-27,
    // jitsi-call-pip.lib.ts:8-10, shared/config/constants.ts:37,39.
    inline static constexpr int kDesktopInset = 32;
    inline static constexpr int kMobileInset = 16;
    inline static constexpr int kDesktopBreakpoint = 640;
    inline static constexpr int kMinWindowWidth = 280;
    inline static constexpr int kMinWindowHeight = 180;
    inline static constexpr int kDefaultPipWidth = 320;
    inline static constexpr int kDefaultPipHeight = 220;
    inline static constexpr int kPipMargin = 20;
    inline static constexpr int kParticipantsPollMs = 5000;
    inline static constexpr int kParticipantsPollUnfocusedMs = 30000;

    // #846 standalone OS window contract (workspace_ui e0194ffb
    // jitsi-call-window.ts): the expanded call presents as a top-level window
    // with exactly these shell bounds (BrowserWindow construction :238-247)
    // and load deadline (WINDOW_LOAD_TIMEOUT_MS, :11).
    inline static constexpr int kStandaloneWidth = 1000;
    inline static constexpr int kStandaloneHeight = 720;
    inline static constexpr int kStandaloneMinWidth = 400;
    inline static constexpr int kStandaloneMinHeight = 300;
    inline static constexpr int kWindowLoadTimeoutMs = 15000;
    // Payload bounds (readPayload, :40-90): callId 1..128, meetingUrl <=8192
    // http(s) without userinfo and with a non-empty path, display strings
    // truncated to 256, theme paletteId <=128 (invalid theme dropped, not a
    // rejection).
    inline static constexpr int kMaxCallIdLength = 128;
    inline static constexpr int kMaxMeetingUrlLength = 8192;
    inline static constexpr int kMaxTextLength = 256;
    inline static constexpr int kMaxPaletteIdLength = 128;

    // jitsi-call-permissions.lib.ts:4 — set once per session, never revoked by
    // shell toggles.
    static QString allowPolicy();
    // #846: the standalone shell background (jitsi-call-window.ts:244
    // backgroundColor), pinned so QML and tests share one value.
    static QString standaloneBackgroundColor();

    explicit JitsiCallWindowController(QObject *parent = nullptr);

    // Pure geometry replicas of the reference helpers.
    static QRect expandedBounds(const QSize &viewport);
    static QRect defaultPipBounds(const QSize &viewport);
    // Same default with the viewport rect converted to screen coordinates by
    // the hosting window's global position (issue #162 review, finding 6).
    static QRect defaultPipBounds(const QSize &viewport, const QPoint &screenOrigin);

    int mode() const { return m_mode; }
    // #846 call identity (see the callId property). The store's uuid callId
    // wins; direct open() payloads without one fall back to callKey and then
    // the meeting URL so the identity is never empty while a call is open.
    QString callId() const;
    bool windowVisible() const { return m_mode == Expanded && m_embedState == Ready; }
    int standaloneWidth() const { return kStandaloneWidth; }
    int standaloneHeight() const { return kStandaloneHeight; }
    int standaloneMinWidth() const { return kStandaloneMinWidth; }
    int standaloneMinHeight() const { return kStandaloneMinHeight; }
    QString standaloneBackground() const;
    // BrowserWindow title (jitsi-call-window.ts:242): the application name.
    QString standaloneTitle() const;
    QRect bounds() const;
    int embedState() const { return m_embedState; }
    int embedCreations() const { return m_embedCreations; }
    int participantCount() const { return m_participantCount; }
    bool fullscreen() const { return m_fullscreen; }
    bool pipBoundsFromUser() const { return m_pipBoundsFromUser; }
    bool permissionsGranted() const { return m_permissionsGranted; }
    int pollIntervalMs() const;

    QVariantMap sessionConfig() const;   // one-time Jitsi config overwrite
    QString displayName() const;        // trimmed source or translated fallback
    QString callName() const;           // locationName, else parsed room name
    QString meetingUrl() const;
    QString parsedDomain() const;
    QString parsedRoomName() const;

    // Localized header/state text (context JitsiCallWindowController).
    QString titleText() const;
    QString subtitleText() const;       // empty when the count is unknown
    QString stateText() const;          // loading / failed / invalid text
    QString retryText() const;
    QString fullscreenLabel() const;
    QString minimizeLabel() const;
    QString minimizeTooltip() const;
    QString closeLabel() const;

    Q_INVOKABLE void setViewportSize(const QSize &viewport);
    Q_INVOKABLE void setFocused(bool focused);
    // Remembers user move/resize of the PiP surface, clamped to min 280x180.
    Q_INVOKABLE void setPipBounds(const QRect &bounds);

    // Opens the shell in Expanded mode from a JitsiCallStore activeCall payload
    // (meetingUrl, locationName, optional meetUrl/displayName/startWithVideoMuted).
    void open(const QVariantMap &activeCall);
    // #846: the single entry point for the standalone shell, a replica of the
    // reference ipcMain.handle(open) (jitsi-call-window.ts:213-232): the
    // payload goes through the readPayload bounds first, then the
    // single-window rules apply - re-opening the same callId only announces
    // focusRequested, a different callId announces openRejected and leaves
    // the live window untouched. Returns whether the request was accepted.
    Q_INVOKABLE bool requestStandaloneOpen(const QVariantMap &activeCall);
    // Replica of readPayload (jitsi-call-window.ts:40-90) for the native
    // activeCall payload: derives and validates callId (1..128; the store
    // mints a uuid per call like the reference jitsi-call.model.ts:80),
    // enforces the meetingUrl http(s)/userinfo/path bounds and the 8192
    // cap, truncates locationName/displayName to 256 and drops an invalid
    // theme entry (a bad theme never rejects the open). An empty meetingUrl
    // is NOT rejected here: the shell's own InvalidUrl state owns unusable
    // meeting links (the reference page does the same). Returns an empty
    // map plus a reason in *rejectReason when the open must be refused.
    static QVariantMap sanitizeShellPayload(const QVariantMap &payload, QString *rejectReason);
    Q_INVOKABLE void close();
    Q_INVOKABLE void toggleMinimize();
    Q_INVOKABLE void toggleFullscreen();
    // Outside click / Escape: expands minimize, is ignored while minimized;
    // never ends a live call.
    Q_INVOKABLE void requestDismiss();
    // Jitsi readyToClose event ends the call.
    Q_INVOKABLE void readyToClose();

    // Embed lifecycle seam (the external_api loader result). These are
    // Q_INVOKABLE because the embed surface's own QML load handler
    // (NativeCallWebEngineView.qml onLoadingChanged) is the production caller
    // - a plain C++ method is not callable from QML, which would leave a
    // valid call stuck on Loading forever (review finding, round 3).
    Q_INVOKABLE void apiLoadSucceeded(); // -> Ready, creates the embed exactly once
    Q_INVOKABLE void apiLoadFailed();    // -> LoadFailed
    Q_INVOKABLE void retry();
    // Issue #728: a load that never reports success or failure must settle
    // within a finite deadline (a stalled embed used to sit on Loading
    // forever). Production deadline; the seam compresses it for tests.
    void setEmbedDeadlineMsForTest(int ms) { m_embedDeadlineMs = qMax(1, ms); }

    // Capture-permission allowlist: true only when the requesting origin is
    // exactly the meeting origin (scheme, host and port), never by feature
    // alone (jitsi-call-permissions.lib.ts grant scope, review finding 3).
    Q_INVOKABLE bool isMeetingOrigin(const QString &origin) const;

    // #87 display-capture decision point for the embed surface. Qt's
    // QWebEnginePermission carries no frame identity, so the shell models the
    // reference frame subtree (workspace_ui 61dacef7 main.ts:996-1020):
    // while this call is open and the embed is Ready, the live Jitsi meeting
    // document is the live child frame; every other requesting origin is
    // modeled as a foreign (main-frame) request, and a request explicitly
    // flagged as main-frame is denied even when its origin matches.
    // See docs/NATIVE_DESKTOP_CALL_DISPLAY_CAPTURE.md.
    Q_INVOKABLE bool isDisplayCaptureRequestAllowed(bool isMainFrame,
        const QString &requestingUrl) const;

    // #87 round 2 (findings 1/2): the frame-trust decision point the shipped
    // embed surface actually consults. Qt 6.10.2's QWebEnginePermission
    // exposes only origin and permissionType (qwebenginepermission.h:30-33,
    // 70-71) - no frame object, no main-frame flag, no frame tree - so the
    // requesting frame can never be verified as the live meeting child frame
    // the reference gate (workspace_ui main.ts:996-1020) requires. Until a
    // trustworthy frame identity exists the policy is fail closed: an
    // unverified identity denies even when the origin matches the live frame.
    // See docs/NATIVE_DESKTOP_CALL_DISPLAY_CAPTURE.md.
    Q_INVOKABLE bool isDisplayCaptureRequestTrustworthy(const QString &requestingUrl) const;

    // Window-side sync for the fullscreen transition: when the hosting window
    // leaves (or enters) fullscreen outside the shell, the controller state
    // follows (review finding 4).
    Q_INVOKABLE void syncFullscreen(bool fullscreen);

    // Test seam for the reconcile poll cadence (0 restores the focused
    // 5000 ms / unfocused 30000 ms reference intervals).
    void setPollIntervalMs(int ms);

    // Participant roster seam: events + focus-aware reconcile poll read the
    // injected provider (the live embed wires it to getNumberOfParticipants).
    void setParticipantCountProvider(std::function<int()> provider);
    void setEmbedFactory(std::function<void()> factory);
    Q_INVOKABLE void participantJoined();
    Q_INVOKABLE void participantLeft();
    Q_INVOKABLE void pollNow();
    // Ready-to-load URL for the embed host: the meeting URL plus the hash
    // form of the one-time session config (the native transport of the
    // reference configOverwrite until the external_api bridge lands).
    Q_INVOKABLE QString embedUrl() const;

signals:
    void modeChanged();
    void boundsChanged();
    void embedStateChanged();
    void embedCreated();
    // #792: retry() asks the shell to re-instantiate the embed Loader. The
    // state flip alone (LoadFailed->Loading) keeps the Loader active with
    // status Loader.Error, so nothing re-attempts the import and Retry can
    // never succeed - the shell must drive a real Loader reload.
    void embedRetryRequested();
    void permissionsChanged();
    void participantCountChanged();
    void fullscreenChanged();
    void closeRequested();
    // #846 standalone shell announcements. windowVisibilityChanged fires when
    // the hidden-until-loaded gate flips (the QML top-level Window binds its
    // visible to it); focusRequested is the same-callId re-open (reference
    // focus path); openRejected carries the refusal reason; standaloneClosed
    // announces a closed call window to the main window (tray tooltip and
    // display-sleep inhibit reset, reference ':closed' at :271-286) and fires
    // ONLY when the window had actually loaded.
    void windowVisibilityChanged();
    void callIdChanged();
    void focusRequested();
    void openRejected(const QString &reason);
    void standaloneClosed(const QString &callId);
    // One umbrella signal so every derived localized string re-binds at once.
    void textsChanged();

private:
    void ensureEmbed();
    void syncParticipantCount();
    // Issue #728: fires when a call has been on Loading for m_embedDeadlineMs.
    void embedDeadlineElapsed();

    Mode m_mode = Closed;
    EmbedState m_embedState = Idle;
    QSize m_viewport{1280, 800};
    QRect m_pipBounds;
    bool m_fullscreen = false;
    int m_embedCreations = 0;
    bool m_permissionsGranted = false;
    int m_participantCount = -1;
    bool m_focused = true;
    bool m_pipBoundsFromUser = false;
    QTimer m_pollTimer;
    QTimer m_embedDeadlineTimer;
    // #846: the reference window load timeout (jitsi-call-window.ts:11) - a
    // load that never reports success or failure fails the shell within this
    // window, so the standalone surface is never shown for a stalled load.
    int m_embedDeadlineMs = kWindowLoadTimeoutMs;
    int m_pollIntervalOverrideMs = 0;
    // #846: true once the embed finished loading for the current call; the
    // reference announces ':closed' to the main window only when the window
    // had actually loaded (jitsi-call-window.ts:271-286).
    bool m_windowShown = false;
    QVariantMap m_call;
    QString m_domain;
    QString m_roomName;
    std::function<int()> m_countProvider;
    std::function<void()> m_embedFactory;
};

// Production live source for the participant count and shared roster
// (review finding 5, round 3). This is the native counterpart of the exact
// external_api surface the reference subscribes to in
// jitsi-participant-count.hook.ts:19-33 and jitsi-call-modal-shell.hook.ts:
// 172-210: the getNumberOfParticipants()/getParticipantsInfo() reads arrive
// through the report* slots (the native transport until the #160
// external_api.js handshake can deliver them), and the participantsJoined/
// participantsLeft listeners fire the handleParticipants* slots. Once
// attached it IS the window controller's count provider in production, so
// the controller's focus-aware timer only ever reconciles against this
// bridge instead of owning the data.
class JitsiEmbedBridge final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int liveParticipantCount READ liveParticipantCount NOTIFY liveParticipantCountChanged)
    Q_PROPERTY(QStringList liveRoster READ liveRoster NOTIFY liveRosterChanged)
public:
    explicit JitsiEmbedBridge(QObject *parent = nullptr);
    int liveParticipantCount() const { return m_liveCount; }
    QStringList liveRoster() const { return m_roster; }
    // Native transport of the two external_api reads against the embed page.
    Q_INVOKABLE void reportParticipantCount(int count);
    Q_INVOKABLE void reportRoster(const QStringList &displayNames);
    // The reference's two external_api listeners (jitsi-participant-count.
    // hook.ts:24-29): each re-syncs the shell count from this bridge.
    Q_INVOKABLE void handleParticipantsJoined();
    Q_INVOKABLE void handleParticipantsLeft();
    // Production wiring, done once by NativeController: the controller's
    // count provider becomes this bridge's live count, and closing the call
    // resets the live session exactly like the reference hook.
    void attachTo(JitsiCallWindowController *controller);
signals:
    void liveParticipantCountChanged();
    void liveRosterChanged();
private:
    int m_liveCount = -1;
    QStringList m_roster;
    QPointer<JitsiCallWindowController> m_controller;
};

} // namespace workspace
