#include "native-call-bridge.h"

namespace workspace {

namespace {
QVariantMap normalizedDescriptor(const QVariantMap &descriptor)
{
    QVariantMap out;
    out.insert(QStringLiteral("ownerKey"), descriptor.value(QStringLiteral("ownerKey")).toString());
    out.insert(QStringLiteral("runtimeGeneration"),
        qulonglong(descriptor.value(QStringLiteral("runtimeGeneration")).toULongLong()));
    out.insert(QStringLiteral("streamUuid"), descriptor.value(QStringLiteral("streamUuid")).toString());
    return out;
}
} // namespace

// Port of chat-dm-call-bridge.model.ts:29-59 (issue #164). The reference logs
// each store write via logStoreAction; the native client has no store logger,
// so writes only mutate state (recorded deviation).

void DmCallBridge::setPendingPartnerUserUuid(const QString &uuid)
{
    if (m_pendingPartnerUuid == uuid) return; // zustand set of the same primitive is inert
    m_pendingPartnerUuid = uuid;
    emit pendingPartnerUuidChanged();
}

void DmCallBridge::clearPendingPartner()
{
    // Reference clearPendingDmCallPartner: early return when already empty.
    if (m_pendingPartnerUuid.isEmpty()) return;
    m_pendingPartnerUuid.clear();
    emit pendingPartnerUuidChanged();
}

QString DmCallBridge::consumePendingPartner()
{
    if (m_pendingPartnerUuid.isEmpty()) return {};
    const auto pending = m_pendingPartnerUuid;
    m_pendingPartnerUuid.clear();
    emit pendingPartnerUuidChanged();
    return pending;
}

void DmCallBridge::setActiveCallDescriptor(const QVariantMap &descriptor)
{
    if (descriptor.isEmpty()) { clearActiveCallDescriptor(); return; }
    const auto next = normalizedDescriptor(descriptor);
    if (next == m_activeCallDescriptor) return;
    m_activeCallDescriptor = next;
    emit activeCallDescriptorChanged();
    refreshSurfaceEnabled();
}

void DmCallBridge::clearActiveCallDescriptor()
{
    if (m_activeCallDescriptor.isEmpty()) return;
    m_activeCallDescriptor.clear();
    emit activeCallDescriptorChanged();
    refreshSurfaceEnabled();
}

void DmCallBridge::setViewedChat(const QString &ownerKey, quint64 runtimeGeneration,
    const QString &streamUuid, bool direct)
{
    const bool directChanged = m_viewedChatIsDirect != direct;
    m_viewedOwnerKey = ownerKey;
    m_viewedRuntimeGeneration = runtimeGeneration;
    m_viewedStreamUuid = streamUuid;
    m_viewedChatIsDirect = direct;
    if (directChanged) emit viewedChatIsDirectChanged();
    refreshSurfaceEnabled();
}

void DmCallBridge::refreshSurfaceEnabled()
{
    // right-panel-workspace-info.ui.tsx:841-849: invoke is handed to the call
    // surface only when ownerKey, runtimeGeneration and streamUuid all match.
    const bool next = !m_activeCallDescriptor.isEmpty() && !m_viewedOwnerKey.isEmpty()
        && m_activeCallDescriptor.value(QStringLiteral("ownerKey")).toString() == m_viewedOwnerKey
        && m_activeCallDescriptor.value(QStringLiteral("runtimeGeneration")).toULongLong()
            == m_viewedRuntimeGeneration
        && m_activeCallDescriptor.value(QStringLiteral("streamUuid")).toString() == m_viewedStreamUuid;
    if (next == m_callSurfaceEnabled) return;
    m_callSurfaceEnabled = next;
    emit callSurfaceEnabledChanged();
}

void DmCallBridge::invokeCallSurface()
{
    // A mismatched descriptor means the reference selector returned undefined:
    // there is simply no invoke to call.
    if (!m_callSurfaceEnabled) return;
    emit callSurfaceInvokeRequested();
}

} // namespace workspace
