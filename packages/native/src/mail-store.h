#pragma once

#include <QPointer>
#include <QVariantList>

#include "api-client.h"

namespace workspace {

// Mock-phase mail data for #879 (part of #851). Folders and message lists are
// fetched through the session ApiClient; the UI-phase server contract is
// GET /mail/folders and GET /mail/messages?folder=<id>. Live integration is
// #852; until then a deployment that does not serve those routes yet (HTTP 404)
// falls back to the sample mailbox compiled into the client and reaches the
// ready state, so opening Mail shows the promised folders instead of a dead
// error screen (exordy finding 4712325987). Non-404 failures keep the error
// state with a retry control.
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
    // Sample mode is decided ONLY by the folders response (exordy finding
    // 4712650373): the sample mailbox may surface just when the folders
    // endpoint itself was missing. If folders came from the server, a 404 on
    // the messages endpoint stays an error - never sample rows mixed under
    // server-provided folders.
    bool m_sampleMode = false;
};

} // namespace workspace
