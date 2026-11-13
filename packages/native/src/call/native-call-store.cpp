#include "call/native-call-store.h"

#include "call/jitsi-call.h"

#include <QDateTime>

namespace workspace {

QVariantMap JitsiCallStore::buildActiveCall(const QVariantMap &payload) const
{
    const auto meetingUrl = payload.value(QStringLiteral("meetingUrl")).toString();
    QVariantMap active;
    active.insert(QStringLiteral("callKey"),
        createJitsiCallKey(payload.value(QStringLiteral("ownerKey")).toString(), meetingUrl));
    active.insert(QStringLiteral("meetingUrl"), meetingUrl);
    active.insert(QStringLiteral("locationName"), payload.value(QStringLiteral("locationName")));
    if (payload.contains(QStringLiteral("ownerKey")))
        active.insert(QStringLiteral("ownerKey"), payload.value(QStringLiteral("ownerKey")));
    if (payload.contains(QStringLiteral("meetUrl")))
        active.insert(QStringLiteral("meetUrl"), payload.value(QStringLiteral("meetUrl")));
    if (payload.contains(QStringLiteral("displayName")))
        active.insert(QStringLiteral("displayName"), payload.value(QStringLiteral("displayName")));
    active.insert(QStringLiteral("startWithVideoMuted"),
        payload.value(QStringLiteral("startWithVideoMuted"), true).toBool());
    active.insert(QStringLiteral("startedAtMs"), QDateTime::currentMSecsSinceEpoch());
    return active;
}

QVariantMap JitsiCallStore::requestOpenCall(const QVariantMap &payload)
{
    QVariantMap result;
    if (!m_activeCall.isEmpty()) {
        const auto callKey = createJitsiCallKey(
            payload.value(QStringLiteral("ownerKey")).toString(),
            payload.value(QStringLiteral("meetingUrl")).toString());
        // Same callKey re-open keeps the original active call untouched;
        // a different one is refused while the first call is active.
        result.insert(QStringLiteral("status"),
            m_activeCall.value(QStringLiteral("callKey")).toString() == callKey
                ? QStringLiteral("same") : QStringLiteral("blocked-active"));
        result.insert(QStringLiteral("activeCall"), m_activeCall);
        return result;
    }
    m_activeCall = buildActiveCall(payload);
    const auto hadInvite = !m_incomingInvite.isEmpty();
    m_incomingInvite = QVariantMap{};
    emit activeCallChanged();
    if (hadInvite)
        emit incomingInviteChanged();
    result.insert(QStringLiteral("status"), QStringLiteral("opened"));
    result.insert(QStringLiteral("activeCall"), m_activeCall);
    return result;
}

void JitsiCallStore::closeCall()
{
    if (m_activeCall.isEmpty())
        return;
    m_activeCall = QVariantMap{};
    emit activeCallChanged();
}

void JitsiCallStore::ingestIncomingInvite(const QVariantMap &invite)
{
    const auto messageId = invite.value(QStringLiteral("messageId"));
    if (messageId.isValid() && messageId == m_lastIncomingMessageId)
        return;
    m_lastIncomingMessageId = messageId;
    // An active call or an already open invite keeps the shown state; a
    // second invite never replaces it (reference ingestIncomingInvite).
    if (!m_activeCall.isEmpty() || !m_incomingInvite.isEmpty())
        return;
    m_incomingInvite = invite;
    emit incomingInviteChanged();
}

QVariantMap JitsiCallStore::acceptIncomingInvite()
{
    return acceptIncomingInvite(QVariantMap{});
}

QVariantMap JitsiCallStore::acceptIncomingInvite(const QVariantMap &options)
{
    if (m_incomingInvite.isEmpty())
        return m_activeCall;
    if (!m_activeCall.isEmpty()) {
        // Accept-while-active clears the invite without replacing the call.
        m_incomingInvite = QVariantMap{};
        emit incomingInviteChanged();
        return m_activeCall;
    }
    QVariantMap payload;
    payload.insert(QStringLiteral("meetingUrl"), m_incomingInvite.value(QStringLiteral("meetingUrl")));
    payload.insert(QStringLiteral("locationName"), m_incomingInvite.value(QStringLiteral("locationName")));
    if (m_incomingInvite.contains(QStringLiteral("ownerKey")))
        payload.insert(QStringLiteral("ownerKey"), m_incomingInvite.value(QStringLiteral("ownerKey")));
    if (m_incomingInvite.contains(QStringLiteral("meetUrl")))
        payload.insert(QStringLiteral("meetUrl"), m_incomingInvite.value(QStringLiteral("meetUrl")));
    if (m_incomingInvite.contains(QStringLiteral("displayName")))
        payload.insert(QStringLiteral("displayName"), m_incomingInvite.value(QStringLiteral("displayName")));
    // Reference buildActiveCall startWithVideoMuted default is true.
    payload.insert(QStringLiteral("startWithVideoMuted"),
        options.value(QStringLiteral("startWithVideoMuted"), true).toBool());
    m_activeCall = buildActiveCall(payload);
    m_incomingInvite = QVariantMap{};
    emit activeCallChanged();
    emit incomingInviteChanged();
    return m_activeCall;
}

void JitsiCallStore::declineIncomingInvite()
{
    if (m_incomingInvite.isEmpty())
        return;
    clearIncomingInvite();
}

void JitsiCallStore::clearIncomingInvite()
{
    if (m_incomingInvite.isEmpty())
        return;
    m_incomingInvite = QVariantMap{};
    emit incomingInviteChanged();
}

void JitsiCallStore::clear()
{
    const auto hadActive = !m_activeCall.isEmpty();
    const auto hadInvite = !m_incomingInvite.isEmpty();
    m_activeCall = QVariantMap{};
    m_incomingInvite = QVariantMap{};
    m_lastIncomingMessageId = QVariant{};
    if (hadActive)
        emit activeCallChanged();
    if (hadInvite)
        emit incomingInviteChanged();
}

} // namespace workspace
