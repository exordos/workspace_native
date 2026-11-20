#pragma once

#include "api-client.h"
#include <QJsonObject>
#include <QSet>
#include <QVariantList>

namespace workspace
{
class Session;

class NativeExternalAccounts final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool loadFailed READ loadFailed NOTIFY changed)
    Q_PROPERTY(bool saveFailed READ saveFailed NOTIFY changed)
public:
    NativeExternalAccounts(ApiClient* api, Session* session, QObject* parent);
    ~NativeExternalAccounts() override;
    QVariantList rows() const;
    bool loading() const { return m_loading; }
    bool busy() const { return m_busy; }
    bool loadFailed() const { return m_loadFailed; }
    bool saveFailed() const { return m_saveFailed; }
    Q_INVOKABLE void open();
    Q_INVOKABLE void close();
    Q_INVOKABLE void cancelForm();
    Q_INVOKABLE void save(const QString& mode, const QString& id, const QString& server, const QString& email,
        const QString& credential);
    Q_INVOKABLE void remove(const QString& id);
signals:
    void changed();
    void saved();
    void contextLost();

private:
    using Headers = QMap<QByteArray, QByteArray>;
    QString owner() const;
    bool current(quint64 ticket) const;
    void cancel();
    void reset();
    void page(quint64 ticket, const QByteArray& marker = { });
    void write(quint64 ticket, const QString& id, const QByteArray& method, const QString& path,
        const QJsonObject& body, const Headers& headers);
    void finishWrite(quint64 ticket, const QString& id, const QVariantMap& row, bool removing, bool success);
    QVariantMap project(const QJsonObject& row) const;
    ApiClient* m_api;
    Session* m_session;
    QString m_owner, m_attemptId, m_attemptServer, m_attemptEmail;
    quint64 m_generation = 0, m_ticket = 0;
    bool m_loading = false, m_busy = false, m_loadFailed = false, m_saveFailed = false;
    bool m_listOpen = false, m_writeSent = false, m_refreshAfterWrite = false;
    ApiClient::RequestId m_request;
    QVariantList m_rows, m_pendingRows;
    QSet<QString> m_seenRows;
    QSet<QByteArray> m_markers;
};
} // namespace workspace
