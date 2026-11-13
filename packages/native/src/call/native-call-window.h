#pragma once

#include <QObject>
#include <QRect>
#include <QSize>
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

    int mode() const { return m_mode; }
    QRect bounds() const;
    int embedState() const { return m_embedState; }
    int embedCreations() const { return m_embedCreations; }
    int participantCount() const { return m_participantCount; }
    bool fullscreen() const { return m_fullscreen; }
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

    // Embed lifecycle seam (the external_api loader result):
    void apiLoadSucceeded(); // -> Ready, creates the embed exactly once
    void apiLoadFailed();    // -> LoadFailed
    Q_INVOKABLE void retry();

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
    QVariantMap m_call;
    QString m_domain;
    QString m_roomName;
    std::function<int()> m_countProvider;
    std::function<void()> m_embedFactory;
};

} // namespace workspace
