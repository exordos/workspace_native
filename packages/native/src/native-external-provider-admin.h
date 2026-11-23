#pragma once

#include "api-client.h"
#include <QJsonObject>
#include <QVariantMap>

namespace workspace
{
class Session;

// Issue #633: admin-only provider policy model (design
// docs/NATIVE_DESKTOP_EXTERNAL_PROVIDER_ADMIN.md). Mirrors the reference
// manage-external-provider hook: access probe, draft/save with etag conflicts
// and three-way rebase, custom-CA save block, health, emergency actions.
class NativeExternalProviderAdmin final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString accessStatus READ accessStatus NOTIFY changed)
    Q_PROPERTY(bool dialogOpen READ dialogOpen NOTIFY changed)
    Q_PROPERTY(QString policyStatus READ policyStatus NOTIFY changed)
    Q_PROPERTY(QVariantMap policy READ policy NOTIFY changed)
    Q_PROPERTY(QVariantMap draft READ draft NOTIFY changed)
    Q_PROPERTY(bool dirty READ dirty NOTIFY changed)
    Q_PROPERTY(bool fieldsValid READ fieldsValid NOTIFY changed)
    Q_PROPERTY(bool caBlocked READ caBlocked NOTIFY changed)
    Q_PROPERTY(bool canSave READ canSave NOTIFY changed)
    Q_PROPERTY(bool saveBusy READ saveBusy NOTIFY changed)
    Q_PROPERTY(bool saveFailed READ saveFailed NOTIFY changed)
    Q_PROPERTY(bool conflict READ conflict NOTIFY changed)
    Q_PROPERTY(QString actionStatus READ actionStatus NOTIFY changed)
    Q_PROPERTY(bool actionFailed READ actionFailed NOTIFY changed)
    Q_PROPERTY(QString healthStatus READ healthStatus NOTIFY changed)
    Q_PROPERTY(QVariantMap health READ health NOTIFY changed)
public:
    NativeExternalProviderAdmin(ApiClient* api, Session* session, QObject* parent);
    ~NativeExternalProviderAdmin() override;

    QString accessStatus() const { return m_accessStatus; }
    bool dialogOpen() const { return m_dialogOpen; }
    QString policyStatus() const { return m_policyStatus; }
    QVariantMap policy() const { return m_policy; }
    QVariantMap draft() const { return m_draft; }
    bool dirty() const;
    bool fieldsValid() const;
    bool caBlocked() const;
    bool canSave() const;
    bool saveBusy() const { return m_saveBusy; }
    bool saveFailed() const { return m_saveFailed; }
    bool conflict() const { return m_conflict; }
    QString actionStatus() const { return m_actionStatus; }
    bool actionFailed() const { return m_actionFailed; }
    QString healthStatus() const { return m_healthStatus; }
    QVariantMap health() const { return m_health; }

    Q_INVOKABLE void probe();
    Q_INVOKABLE void close();
    Q_INVOKABLE void openDialog();
    Q_INVOKABLE void closeDialog();
    Q_INVOKABLE void refreshPolicy();
    Q_INVOKABLE void refreshHealth();
    Q_INVOKABLE void save();
    Q_INVOKABLE void suspend();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void resetDraft();
    Q_INVOKABLE void keepDraft();
    Q_INVOKABLE void setEnabled(bool enabled);
    Q_INVOKABLE void setLimit(const QString& key, double value);
    QString policyEtag() const { return m_etag; }

signals:
    void changed();

private:
    using Headers = QMap<QByteArray, QByteArray>;
    QString owner() const;
    bool current(quint64 ticket) const;
    void cancelAll();
    void reset();
    bool parsePolicy(const QJsonObject& dto, const QByteArray& etag);
    // Reissues a refresh that the in-flight-mutation guard dropped, when the
    // mutation failed without adopting a fresh policy (#735 review round).
    void adoptDeferredRefresh();
    QVariantMap snapshot(const QVariantMap& policy) const;
    void rebaseDraft(const QVariantMap& previousBase, const QVariantMap& incomingBase);
    void loadHealth(bool initial);
    void forbidAccess();
    ApiClient* m_api;
    Session* m_session;
    QString m_owner, m_accessStatus = QStringLiteral("unknown"), m_policyStatus = QStringLiteral("idle");
    QString m_healthStatus = QStringLiteral("idle"), m_actionStatus = QStringLiteral("none");
    QVariantMap m_policy, m_draft, m_health;
    QByteArray m_etag;
    quint64 m_generation = 0, m_ticket = 0;
    bool m_dialogOpen = false, m_conflict = false, m_saveBusy = false, m_saveFailed = false;
    bool m_actionFailed = false, m_mutationBusy = false;
    // A policy refresh reply that was dropped because a mutation was in
    // flight; reissued if that mutation fails without adopting a policy.
    bool m_refreshDeferred = false;
    quint64 m_policySerial = 0, m_healthSerial = 0, m_saveSerial = 0, m_actionSerial = 0;
    // Issue #740: bumped on every CONFIRMED policy mutation (saved PUT, suspend,
    // resume, adopted conflict snapshot). A policy refresh captures it at dispatch
    // and discards its reply when a confirmed mutation landed in the meantime -
    // such a reply's snapshot predates our write and must not roll back
    // revision/ETag/values.
    quint64 m_policyGeneration = 0;
    ApiClient::RequestId m_policyRequest, m_healthRequest, m_saveRequest, m_actionRequest;
};
} // namespace workspace
