#pragma once

#include <QPointer>
#include <QVariantList>

#include "api-client.h"

namespace workspace {

// Mock-phase mail data for #879 (part of #851). Folders and message lists are
// fetched through the session ApiClient; the UI-phase server contract is
// GET /mail/folders and GET /mail/messages?folder=<id>. Live integration is
// #852; until then only the window-test mock server serves these routes and a
// real deployment lands in the adapter error state with a retry control.
class MailStore final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList folders READ folders NOTIFY mailChanged)
    Q_PROPERTY(QString folderId READ folderId NOTIFY mailChanged)
    Q_PROPERTY(QVariantList messages READ messages NOTIFY mailChanged)
    Q_PROPERTY(QString state READ state NOTIFY mailChanged)
    Q_PROPERTY(QString selectedMessageUuid READ selectedMessageUuid NOTIFY mailChanged)

public:
    explicit MailStore(ApiClient *api, QObject *parent = nullptr);

    QVariantList folders() const { return m_folders; }
    QString folderId() const { return m_folderId; }
    QVariantList messages() const { return m_messages; }
    QString state() const { return m_state; }
    QString selectedMessageUuid() const { return m_selectedMessage; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void selectFolder(const QString &folder);
    Q_INVOKABLE void selectMessage(const QString &uuid);

signals:
    void mailChanged();

private:
    void loadMessages();

    ApiClient *m_api;
    QVariantList m_folders;
    QVariantList m_messages;
    QString m_folderId = QStringLiteral("inbox");
    QString m_state = QStringLiteral("idle");
    QString m_selectedMessage;
    // Replies older than the newest selection/refresh are dropped, never
    // allowed to overwrite fresh state.
    quint64 m_requestToken = 0;
};

} // namespace workspace
