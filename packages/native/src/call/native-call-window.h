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

    // jitsi-call-permissions.lib.ts:4 — set once per session, never revoked by
    // shell toggles.
    static QString allowPolicy();

    explicit JitsiCallWindowController(QObject *parent = nullptr);

    // Pure geometry replicas of the reference helpers.
    static QRect expandedBounds(const QSize &viewport);
    static QRect defaultPipBounds(const QSize &viewport);
    // Same default with the viewport rect converted to screen coordinates by
    // the hosting window's global position (issue #162 review, finding 6).
    static QRect defaultPipBounds(const QSize &viewport, const QPoint &screenOrigin);

    int mode() const { return m_mode; }
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

    // Capture-permission allowlist: true only when the requesting origin is
    // exactly the meeting origin (scheme, host and port), never by feature
    // alone (jitsi-call-permissions.lib.ts grant scope, review finding 3).
    Q_INVOKABLE bool isMeetingOrigin(const QString &origin) const;

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
    void permissionsChanged();
    void participantCountChanged();
    void fullscreenChanged();
    void closeRequested();
    // One umbrella signal so every derived localized string re-binds at once.
    void textsChanged();

private:
    void ensureEmbed();
    void syncParticipantCount();

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
    int m_pollIntervalOverrideMs = 0;
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
