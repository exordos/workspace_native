#pragma once

#include <QObject>
#include <QSettings>
#include <QPointer>
#include <QRect>
#include <QTimer>
#include <QDate>
#include <QJsonObject>
#include <QVariantList>
#include <QStringList>
#include <functional>
#include <optional>

class QWindow;
class QDBusVariant;

namespace workspace {

class NativePreferences final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString paletteId READ paletteId WRITE setPaletteId NOTIFY paletteIdChanged)
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(QString locale READ locale WRITE setLocale NOTIFY localeChanged)
    Q_PROPERTY(bool dark READ dark NOTIFY darkChanged)
    Q_PROPERTY(bool prefersReducedMotion READ prefersReducedMotion NOTIFY prefersReducedMotionChanged)
    Q_PROPERTY(qreal sidebarPreferredWidth READ sidebarPreferredWidth WRITE setSidebarPreferredWidth NOTIFY sidebarPreferredWidthChanged)
    Q_PROPERTY(bool composerToolbarExpanded READ composerToolbarExpanded WRITE setComposerToolbarExpanded NOTIFY composerToolbarExpandedChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(int cacheLimitGb READ cacheLimitGb WRITE setCacheLimitGb NOTIFY cacheLimitGbChanged)
    Q_PROPERTY(QString cacheError READ cacheError NOTIFY cacheErrorChanged)
    Q_PROPERTY(bool cacheBusy READ cacheBusy NOTIFY cacheStatusChanged)
    Q_PROPERTY(bool cacheAvailable READ cacheAvailable NOTIFY cacheStatusChanged)
    Q_PROPERTY(qint64 cacheAppliedLimitBytes READ cacheAppliedLimitBytes NOTIFY cacheStatusChanged)
    Q_PROPERTY(qint64 cacheUsageBytes READ cacheUsageBytes NOTIFY cacheStatusChanged)
    Q_PROPERTY(QVariantList suggestedReactions READ suggestedReactions NOTIFY reactionHistoryChanged)
    Q_PROPERTY(QVariantMap quickReactionUsage READ quickReactionUsage NOTIFY reactionHistoryChanged)
    Q_PROPERTY(QStringList organizationHistory READ organizationHistory NOTIFY organizationHistoryChanged)
    Q_PROPERTY(QString authIdleTimeout READ authIdleTimeout WRITE setAuthIdleTimeout NOTIFY authIdleTimeoutChanged)
    // Issue #102 (docs/NATIVE_DESKTOP_CHAT_LIST_DENSITY.md): the reference chat list
    // density and click action (settings.model.ts chatListDensity/chatClickAction).
    // Persisted globally like the reference settings blob, not per owner.
    Q_PROPERTY(QString chatListDensity READ chatListDensity WRITE setChatListDensity NOTIFY chatListDensityChanged)
    Q_PROPERTY(QString chatClickAction READ chatClickAction WRITE setChatClickAction NOTIFY chatClickActionChanged)
public:
    explicit NativePreferences(QString settingsFile = {}, QObject *parent = nullptr,
                               std::function<QDate()> utcToday = {});
    ~NativePreferences() override;
    QString paletteId() const { return m_palette; }
    QString mode() const { return m_mode; }
    QString locale() const { return m_locale; }
    bool dark() const;
    // Issue #107: the platform reduced-motion preference behind the web's
    // prefers-reduced-motion media query. The XDG Settings portal value
    // (org.freedesktop.appearance/reduced-motion) unless a test seam pinned it.
    bool prefersReducedMotion() const { return m_reducedMotionOverride.value_or(m_portalReducedMotion); }
    // Test seam for the portal probe (like setNowProvider for the clock): pins
    // the exposed flag to the given value as a portal announcing that key would.
    void setPrefersReducedMotionOverride(bool reduced);
    qreal sidebarPreferredWidth() const { return m_sidebarWidth; }
    bool composerToolbarExpanded() const { return m_composerToolbarExpanded; }
    QString error() const { return m_error; }
    int cacheLimitGb() const { return m_cacheLimitGb; }
    QString cacheError() const { return m_cacheError; }
    bool cacheBusy() const { return m_cacheBusy; }
    bool cacheAvailable() const { return m_cacheAvailable; }
    qint64 cacheAppliedLimitBytes() const { return m_cacheAppliedLimitBytes; }
    qint64 cacheUsageBytes() const { return m_cacheUsageBytes; }
    QVariantList suggestedReactions() const;
    QVariantMap quickReactionUsage() const;
    void setOwner(const QString &owner);
    // Explicit settings I/O actions; never called from a QML/read-only getter.
    bool loadOwnStatusAway(const QString &owner);
    void recordOwnStatusAway(const QString &owner, bool away);
    // Activity panel open/closed state (#101, sidebar-config.model.ts
    // activityOpen): per-workspace-session, default closed like the reference
    // DEFAULT_CONFIG. Same per-owner storage pattern as manualAway.
    bool loadActivityOpen(const QString &owner);
    void recordActivityOpen(const QString &owner, bool open);
    QStringList loadLastConversation(const QString &owner);
    void recordLastConversation(const QString &owner, const QString &stream, const QString &topic);
    QStringList loadExpandedStreams(const QString &owner);
    void recordExpandedStreams(const QString &owner, const QStringList &streams);
    QStringList organizationHistory() const { return m_organizationHistory; }
    // Issue #165: the reference authIdleTimeout setting (enforced by
    // NativeAuthGuard; the settings row itself is issue #135). Absent or
    // invalid values resolve to "3d" like settings.model.ts:75,175-178.
    QString authIdleTimeout() const { return m_authIdleTimeout; }
    void setAuthIdleTimeout(const QString &preset);
    // Issue #102: strict reference resolvers - only the single non-default token
    // survives (settings.model.ts:124,173), anything else is the default value.
    QString chatListDensity() const { return m_chatListDensity; }
    QString chatClickAction() const { return m_chatClickAction; }
    void setChatListDensity(const QString &density);
    void setChatClickAction(const QString &action);
    // Mention frecency (mention-frecency.lib.ts): decayed pick counts per user,
    // partitioned by the opaque caller scope (the controller's creation owner,
    // which stays valid on loopback fixtures where the HTTPS vault owner is empty).
    QVariantMap mentionFrecencyFor(const QString &owner);
    Q_INVOKABLE void recordMentionPick(const QString &userUuid, const QString &owner);
    // Test seam for the decay clock; production uses wall-clock milliseconds.
    void setNowProvider(std::function<qint64()> nowMs);
    // Origins of successfully logged-in organizations (deduped, newest first),
    // mirroring workspace-organization-history.lib.ts.
    Q_INVOKABLE void recordOrganizationOrigin(const QString &origin);
    void setPaletteId(const QString &palette);
    void setMode(const QString &mode);
    void setLocale(const QString &locale);
    void setCacheLimitGb(int gigabytes);
    void reportCacheError(const QString &error);
    void reportCacheStatus(bool busy, bool available, qint64 appliedLimitBytes, qint64 usageBytes);
    void setSidebarPreferredWidth(qreal width);
    void setComposerToolbarExpanded(bool expanded);
    Q_INVOKABLE void toggleMode();
    Q_INVOKABLE void recordSuggestedReaction(const QString &name, const QString &expectedOwner);
    Q_INVOKABLE void recordQuickReactionUse(const QString &name, const QString &expectedOwner);
    void attachWindow(QWindow *window);
    bool windowMaximized() const { return m_windowMaximized; }
    void activateWindow();

signals:
    void paletteIdChanged();
    void organizationHistoryChanged();
    void modeChanged();
    void localeChanged();
    void darkChanged();
    void prefersReducedMotionChanged();
    void sidebarPreferredWidthChanged();
    void composerToolbarExpandedChanged();
    void errorChanged();
    void cacheLimitGbChanged();
    void cacheErrorChanged();
    void cacheStatusChanged();
    void reactionHistoryChanged();
    void mentionFrecencyChanged();
    void authIdleTimeoutChanged();
    void chatListDensityChanged();
    void chatClickActionChanged();

private slots:
    // XDG Settings portal SettingChanged (org.freedesktop.appearance
    // reduced-motion): keeps the exposed flag live when the user flips the
    // system reduce-motion toggle.
    void onPortalSettingChanged(const QString &ns, const QString &key, const QDBusVariant &value);

protected:
    // #62: F11 fullscreen toggle on the attached window (the reference keeps
    // the View menu's togglefullscreen role while the menu bar is hidden).
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void toggleWindowFullscreen();
    // #62 round 2: the gate fed by every window signal while a fullscreen
    // transition runs, and the release that re-captures the live state.
    void noteFullscreenGateSignal();
    void finishFullscreenGate();
    // #62 round 2: reads the window's current geometry/maximized state;
    // shared by the gate release and immediate saves during an open gate.
    void recaptureLiveWindowState();
    QString appearanceGroup() const;
    QString settingsGroup() const;
    void loadAuthIdleTimeout();
    void loadChatLayout();
    void probePortalReducedMotion();
    void updatePortalReducedMotion(bool reduced);
    std::optional<bool> m_reducedMotionOverride;
    bool m_portalReducedMotion = false;
    void loadAppearance();
    void persist();
    void setError(const QString &error);
    void loadReactionHistory();
    void persistReactionHistory();
    void ensureMentionStore(const QString &owner);
    void persistMentionFrecency(const QString &owner);
    qint64 nowMs() const;
    QJsonObject recentReactionUsage(const QJsonObject &days) const;
    void saveWindow();
    void saveLastConversation();
    QSettings m_settings;
    QString m_owner;
    QString m_palette = QStringLiteral("blue-cold");
    QString m_mode = QStringLiteral("system");
    QString m_locale = QStringLiteral("en");
    QString m_error;
    QString m_cacheError;
    bool m_cacheBusy = false;
    bool m_cacheAvailable = false;
    qint64 m_cacheAppliedLimitBytes = 0;
    qint64 m_cacheUsageBytes = 0;
    int m_cacheLimitGb = 10;
    QVariantList m_suggested;
    QJsonObject m_quickUsage;
    QJsonObject m_mentionStore;
    QString m_mentionStoreOwner;
    bool m_mentionStoreLoaded = false;
    std::function<qint64()> m_nowMs;
    std::function<QDate()> m_utcToday;
    qreal m_sidebarWidth = 299;
    // Global composer toolbar expansion, mirroring settings.model.ts
    // composerToolbarExpanded (default false).
    bool m_composerToolbarExpanded = false;
    bool m_systemDark = false;
    QPointer<QWindow> m_window;
    QTimer m_windowSaveTimer;
    QTimer m_conversationSaveTimer;
    QString m_pendingConversationOwner;
    QStringList m_pendingConversation;
    QStringList m_organizationHistory;
    QString m_authIdleTimeout = QStringLiteral("3d");
    QString m_chatListDensity = QStringLiteral("general");
    QString m_chatClickAction = QStringLiteral("stream");
    QRect m_normalGeometry;
    // #62 round 3: the bounds the window had while maximized, recorded from
    // live (ungated) geometry signals. The WM's unmaximize hop lands on
    // exactly these bounds as WindowNoState, so matching them marks a
    // transition rather than a settled user change (recaptureLiveWindowState).
    QRect m_maximizedGeometry;
    // #62 round 4: the fullscreen shape the window wears, plus the in-flight
    // flag for the restore sequence a release starts (F11 release, or the
    // first FullScreen observation of an enter - round 6). A screen-filling
    // WindowNoState counts as the WM hop only while that restore is in flight
    // and the frame still equals this snapshot; a user-settled normal window
    // may legitimately fill the screen, and its moves differ in shape and
    // recapture normally (recaptureLiveWindowState).
    QRect m_fullscreenSnapshot;
    bool m_fullscreenRestoreInFlight = false;
    // #62 round 5 (review cid 4611449708): the enter-side mirror of the
    // release flag above. Set while F11 is taking a window INTO fullscreen and
    // cleared when the FullScreen state observation arrives (round 6:
    // recorded in noteFullscreenGateSignal the moment the signal lands, not
    // at gate end - the cap must not retire it early, review cid 4611449708).
    // A WM may deliver the fullscreen configure before the fullscreen state;
    // while the enter is in flight, any WindowNoState re-capture lacks
    // that atom, so it is still the transition frame, not settled normal
    // bounds, and must not move the saved geometry or clear the maximized
    // flag (recaptureLiveWindowState).
    bool m_fullscreenEnterInFlight = false;
    bool m_windowMaximized = false;
    // #62 round 2: true while a fullscreen transition is in flight. The gate
    // covers the WM's transient unmaximize/restore hop so it never reaches
    // the persisted normal geometry or maximized flag (the reference
    // getNormalBounds rule), but it ends as soon as the window settles into
    // the state the transition asked for, and the release re-captures the
    // live window state so legitimate user changes during the transition
    // are persisted, not dropped.
    bool m_fullscreenPending = false;
    bool m_fullscreenAwaitingExit = false;
    bool m_fullscreenExitMaximized = false;
    QTimer m_fullscreenGateSettle;
    QTimer m_fullscreenGateCap;
};

} // namespace workspace
