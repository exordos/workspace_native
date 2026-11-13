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

    Q_INVOKABLE bool videoEnabled(const QString &messageId) const;
    Q_INVOKABLE void toggleVideoEnabled(const QString &messageId);
    Q_INVOKABLE void accept();
    Q_INVOKABLE void decline();

signals:
    void visibleChanged();
    void ringRequested();
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
