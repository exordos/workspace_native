#pragma once

#include <QObject>
#include <QVariantMap>

namespace workspace {

// Single active call store, a port of the reference state machine in
// workspace_ui 61dacef7 packages/web/src/features/jitsi-call/jitsi-call.model.ts.
// requestOpenCall answers "opened", "same" (same call key: the original
// active call is preserved untouched) or "blocked-active" (a different call
// is already active). Accepting an invite never replaces an active call and
// always clears the invite. See docs/NATIVE_DESKTOP_CALL_LAUNCH.md.
class JitsiCallStore final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap activeCall READ activeCall NOTIFY activeCallChanged)
    Q_PROPERTY(QVariantMap incomingInvite READ incomingInvite NOTIFY incomingInviteChanged)
public:
    explicit JitsiCallStore(QObject *parent = nullptr) : QObject(parent) {}

    QVariantMap activeCall() const { return m_activeCall; }
    QVariantMap incomingInvite() const { return m_incomingInvite; }
    bool hasActiveCall() const { return !m_activeCall.isEmpty(); }
    QString activeCallKey() const { return m_activeCall.value(QStringLiteral("callKey")).toString(); }

    // payload: meetingUrl, locationName, optional ownerKey, meetUrl,
    // displayName, startWithVideoMuted (default true).
    Q_INVOKABLE QVariantMap requestOpenCall(const QVariantMap &payload);
    Q_INVOKABLE void closeCall();
    Q_INVOKABLE void ingestIncomingInvite(const QVariantMap &invite);
    Q_INVOKABLE QVariantMap acceptIncomingInvite();
    // Reference acceptIncomingInvite(options): startWithVideoMuted from the
    // per-invite video preference (jitsi-call-shell.ui.tsx:61-63), default
    // muted when the option is absent (jitsi-call.model.ts:166).
    Q_INVOKABLE QVariantMap acceptIncomingInvite(const QVariantMap &options);
    Q_INVOKABLE void declineIncomingInvite();
    // Clears only the invite (dedupe id untouched so replays stay suppressed).
    void clearIncomingInvite();
    // Drops call state entirely (account sign-out keeps nothing in memory).
    void clear();

signals:
    void activeCallChanged();
    void incomingInviteChanged();

private:
    QVariantMap buildActiveCall(const QVariantMap &payload) const;

    QVariantMap m_activeCall;
    QVariantMap m_incomingInvite;
    QVariant m_lastIncomingMessageId;
};

} // namespace workspace
