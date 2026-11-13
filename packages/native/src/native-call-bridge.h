#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

namespace workspace {

// DM call initiation bridge (issue #164, docs/NATIVE_DESKTOP_DM_CALL_BRIDGE.md).
// Port of workspace_ui 61dacef7 packages/web/src/features/chat-dm-call-bridge/
// chat-dm-call-bridge.model.ts:29-59. Owns the pending DM call partner queue and
// the active-call descriptor {ownerKey, runtimeGeneration, streamUuid}; the call
// surface (sidebar Call button) is enabled only while the descriptor matches the
// viewed chat on all three fields. Clearing an empty pending is a no-op; a
// consumed pending clears exactly once and never re-fires while the same DM
// stays open.
class DmCallBridge final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString pendingPartnerUuid READ pendingPartnerUuid NOTIFY pendingPartnerUuidChanged)
    Q_PROPERTY(QVariantMap activeCallDescriptor READ activeCallDescriptor NOTIFY activeCallDescriptorChanged)
    Q_PROPERTY(bool callSurfaceEnabled READ callSurfaceEnabled NOTIFY callSurfaceEnabledChanged)
    Q_PROPERTY(bool viewedChatIsDirect READ viewedChatIsDirect NOTIFY viewedChatIsDirectChanged)
public:
    explicit DmCallBridge(QObject *parent = nullptr) : QObject(parent) {}

    QString pendingPartnerUuid() const { return m_pendingPartnerUuid; }
    QVariantMap activeCallDescriptor() const { return m_activeCallDescriptor; }
    bool callSurfaceEnabled() const { return m_callSurfaceEnabled; }
    bool viewedChatIsDirect() const { return m_viewedChatIsDirect; }

    Q_INVOKABLE void setPendingPartnerUserUuid(const QString &uuid);
    Q_INVOKABLE void clearPendingPartner();
    // Returns the pending uuid and clears it in one step; empty string (no write,
    // no notify) when nothing is pending.
    Q_INVOKABLE QString consumePendingPartner();
    Q_INVOKABLE void setActiveCallDescriptor(const QVariantMap &descriptor);
    Q_INVOKABLE void clearActiveCallDescriptor();
    void setViewedChat(const QString &ownerKey, quint64 runtimeGeneration,
        const QString &streamUuid, bool direct);
    Q_INVOKABLE void invokeCallSurface();

signals:
    void pendingPartnerUuidChanged();
    void activeCallDescriptorChanged();
    void callSurfaceEnabledChanged();
    void viewedChatIsDirectChanged();
    void callSurfaceInvokeRequested();

private:
    void refreshSurfaceEnabled();

    QString m_pendingPartnerUuid;
    QVariantMap m_activeCallDescriptor;
    QString m_viewedOwnerKey;
    quint64 m_viewedRuntimeGeneration = 0;
    QString m_viewedStreamUuid;
    bool m_viewedChatIsDirect = false;
    bool m_callSurfaceEnabled = false;
};

} // namespace workspace
