#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>

namespace workspace {

class JitsiCallStore;

// Port of useIncomingCallLifecycle (workspace_ui 61dacef7
// packages/web/src/features/jitsi-call/jitsi-call-incoming-lifecycle.hook.ts)
// plus the per-invite video preference held by jitsi-call-shell.ui.tsx:35-106.
// GUI-free: the audible ringtone is the ringRequested signal (testable seam;
// no native audio sink exists on this host), the ring schedule and the
// auto-decline run on QTimers whose intervals are injectable so tests drive
// the cadence with shortened values while the production defaults stay at the
// reference constants (INCOMING_INVITE_RING_INTERVAL_MS = 1500,
// INCOMING_INVITE_AUTO_DECLINE_MS = 45000). See
// docs/NATIVE_DESKTOP_INCOMING_CALL_UI.md.
class IncomingCallLifecycle final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool visible READ visible NOTIFY visibleChanged)
    Q_PROPERTY(int ringIntervalMs READ ringIntervalMs WRITE setRingIntervalMs NOTIFY ringIntervalMsChanged)
    Q_PROPERTY(int autoDeclineMs READ autoDeclineMs WRITE setAutoDeclineMs NOTIFY autoDeclineMsChanged)
    Q_PROPERTY(QString notificationSound READ notificationSound WRITE setNotificationSound NOTIFY notificationSoundChanged)
    Q_PROPERTY(QString variant READ variant WRITE setVariant NOTIFY variantChanged)
    Q_PROPERTY(bool currentVideoEnabled READ currentVideoEnabled NOTIFY videoPreferenceChanged)
public:
    explicit IncomingCallLifecycle(QObject *parent = nullptr);

    void attachStore(JitsiCallStore *store);

    bool visible() const { return m_visible; }
    int ringIntervalMs() const { return m_ringIntervalMs; }
    void setRingIntervalMs(int ms);
    int autoDeclineMs() const { return m_autoDeclineMs; }
    void setAutoDeclineMs(int ms);
    QString notificationSound() const { return m_notificationSound; }
    void setNotificationSound(const QString &value);
    QString variant() const { return m_variant; }
    void setVariant(const QString &value);
    // The video preference of the currently visible invite (QML binding
    // target; false when no invite is present).
    bool currentVideoEnabled() const { return videoEnabled(currentInviteMessageId()); }

    // Observable surface for the auto-decline timer contract (PR #257
    // finding 5): the 45 s deadline must not drift with Qt's default coarse
    // timer, so the decline timer must run Precise and its armed interval
    // mirrors autoDeclineMs.
    Qt::TimerType declineTimerType() const { return m_declineTimer.timerType(); }
    int declineTimerInterval() const { return m_declineTimer.interval(); }

    Q_INVOKABLE bool videoEnabled(const QString &messageId) const;
    Q_INVOKABLE void toggleVideoEnabled(const QString &messageId);
    Q_INVOKABLE void accept();
    Q_INVOKABLE void decline();
    // Escape/backdrop close: clears the invite without a decline action
    // (jitsi-call-incoming-modal.tsx:59-73 "incoming call dismissed without
    // invite").
    Q_INVOKABLE void dismiss();

signals:
    void visibleChanged();
    void ringRequested();
    void dismissRequested(const QString &messageId);
    void autoDeclined(const QString &messageId);
    void ringIntervalMsChanged();
    void autoDeclineMsChanged();
    void notificationSoundChanged();
    void variantChanged();
    void videoPreferenceChanged();

private:
    void onStoreChanged();
    void armTimers();
    void cancelTimers();
    void handleDecline(const QString &messageId);
    void playRing();
    void clearVideoPreference(const QString &messageId);
    QString currentInviteMessageId() const;

    JitsiCallStore *m_store = nullptr;
    bool m_visible = false;
    int m_ringIntervalMs = 1500;
    int m_autoDeclineMs = 45000;
    QString m_notificationSound = QStringLiteral("default");
    QString m_variant = QStringLiteral("large");
    QHash<QString, bool> m_videoEnabledByInvite;
    QTimer m_ringTimer;
    QTimer m_declineTimer;
};

} // namespace workspace
