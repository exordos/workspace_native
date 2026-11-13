#include "call/incoming-call-lifecycle.h"

#include "call/incoming-call.h"
#include "call/native-call-store.h"

namespace workspace {

IncomingCallLifecycle::IncomingCallLifecycle(QObject *parent)
    : QObject(parent)
{
#ifdef WORKSPACE_NATIVE_CALL_INCOMING_VARIANT
    m_variant = resolveIncomingCallVariant(
        QStringLiteral(WORKSPACE_NATIVE_CALL_INCOMING_VARIANT));
#endif
    m_ringTimer.setInterval(m_ringIntervalMs);
    connect(&m_ringTimer, &QTimer::timeout, this, &IncomingCallLifecycle::playRing);
    // PR #257 finding 5: Qt's default timer type may coarsen a 45 s deadline
    // by up to 5% (firing the auto-decline up to 2.25 s early). The decline
    // deadline is a user-visible contract, so it runs Precise.
    m_declineTimer.setTimerType(Qt::PreciseTimer);
    m_declineTimer.setSingleShot(true);
    m_declineTimer.setInterval(m_autoDeclineMs);
    connect(&m_declineTimer, &QTimer::timeout, this, [this] {
        const auto messageId = currentInviteMessageId();
        handleDecline(messageId);
        emit autoDeclined(messageId);
    });
}

void IncomingCallLifecycle::attachStore(JitsiCallStore *store)
{
    m_store = store;
    connect(m_store, &JitsiCallStore::incomingInviteChanged, this,
        &IncomingCallLifecycle::onStoreChanged);
    connect(m_store, &JitsiCallStore::activeCallChanged, this,
        &IncomingCallLifecycle::onStoreChanged);
}

void IncomingCallLifecycle::setRingIntervalMs(int ms)
{
    if (m_ringIntervalMs == ms)
        return;
    m_ringIntervalMs = ms;
    if (m_ringTimer.isActive())
        m_ringTimer.start(ms);
    emit ringIntervalMsChanged();
}

void IncomingCallLifecycle::setAutoDeclineMs(int ms)
{
    if (m_autoDeclineMs == ms)
        return;
    m_autoDeclineMs = ms;
    if (m_declineTimer.isActive())
        m_declineTimer.start(ms);
    emit autoDeclineMsChanged();
}

void IncomingCallLifecycle::setNotificationSound(const QString &value)
{
    if (m_notificationSound == value)
        return;
    m_notificationSound = value;
    emit notificationSoundChanged();
}

void IncomingCallLifecycle::setVariant(const QString &value)
{
    const auto resolved = resolveIncomingCallVariant(value);
    if (resolved == m_variant)
        return;
    m_variant = resolved;
    emit variantChanged();
}

bool IncomingCallLifecycle::videoEnabled(const QString &messageId) const
{
    return m_videoEnabledByInvite.value(messageId, false);
}

void IncomingCallLifecycle::toggleVideoEnabled(const QString &messageId)
{
    // jitsi-call-shell.ui.tsx:68-74: per-invite flip, default false.
    if (messageId.isEmpty())
        return;
    m_videoEnabledByInvite.insert(messageId, !videoEnabled(messageId));
    emit videoPreferenceChanged();
}

void IncomingCallLifecycle::accept()
{
    // jitsi-call-shell.ui.tsx:61-65: accept with the per-invite video
    // preference and clear it.
    if (!m_store || m_store->incomingInvite().isEmpty())
        return;
    const auto messageId = currentInviteMessageId();
    QVariantMap options;
    options.insert(QStringLiteral("startWithVideoMuted"), !videoEnabled(messageId));
    m_store->acceptIncomingInvite(options);
    clearVideoPreference(messageId);
}

void IncomingCallLifecycle::decline()
{
    if (!m_store)
        return;
    handleDecline(currentInviteMessageId());
}

void IncomingCallLifecycle::dismiss()
{
    if (!m_store || m_store->incomingInvite().isEmpty())
        return;
    const auto messageId = currentInviteMessageId();
    clearVideoPreference(messageId);
    // No decline, no call: the invite and its notification disappear
    // (jitsi-call.model.ts clearIncomingInvite path).
    m_store->clearIncomingInvite();
    emit dismissRequested(messageId);
}

void IncomingCallLifecycle::onStoreChanged()
{
    // PR #257 finding 6: the video preference belongs to the invite that is
    // currently shown. When the current invite changes (replaced or cleared
    // outside the accept/decline/dismiss handlers, e.g. an expired invite),
    // the discarded invite's preference is dropped so a replay starts at the
    // default, and videoPreferenceChanged fires so the QML binding picks up
    // the new invite's value instead of keeping the stale one.
    const auto inviteId = currentInviteMessageId();
    if (inviteId != m_trackedInviteId) {
        m_videoEnabledByInvite.remove(m_trackedInviteId);
        m_trackedInviteId = inviteId;
        emit videoPreferenceChanged();
    }
    // lifecycle.hook.ts:21: visible = invite present, no active call.
    const auto now = m_store && !m_store->incomingInvite().isEmpty()
        && !m_store->hasActiveCall();
    if (now == m_visible)
        return;
    m_visible = now;
    if (m_visible)
        armTimers();
    else
        cancelTimers();
    emit visibleChanged();
}

void IncomingCallLifecycle::armTimers()
{
    // lifecycle.hook.ts:28-58: single-shot auto-decline at autoDeclineMs
    // (45000 by default) and the ring playing immediately then every
    // ringIntervalMs (1500 by default) while visible.
    playRing();
    if (m_ringIntervalMs > 0)
        m_ringTimer.start(m_ringIntervalMs);
    m_declineTimer.start(m_autoDeclineMs);
}

void IncomingCallLifecycle::cancelTimers()
{
    m_ringTimer.stop();
    m_declineTimer.stop();
}

void IncomingCallLifecycle::handleDecline(const QString &messageId)
{
    // The shell decline handler clears the per-invite preference first, then
    // the store decline clears the invite (jitsi-call-shell.ui.tsx:66-70).
    clearVideoPreference(messageId);
    if (m_store)
        m_store->declineIncomingInvite();
}

void IncomingCallLifecycle::playRing()
{
    // lifecycle.hook.ts:43-46: the "none" preset suppresses every ring.
    if (m_notificationSound == QLatin1String("none"))
        return;
    emit ringRequested();
}

void IncomingCallLifecycle::clearVideoPreference(const QString &messageId)
{
    if (!messageId.isEmpty() && m_videoEnabledByInvite.remove(messageId) > 0)
        emit videoPreferenceChanged();
}

QString IncomingCallLifecycle::currentInviteMessageId() const
{
    return m_store
        ? m_store->incomingInvite().value(QStringLiteral("messageId")).toString()
        : QString();
}

} // namespace workspace
