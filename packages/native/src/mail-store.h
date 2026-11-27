#pragma once

#include <QJsonArray>
#include <QPointer>
#include <QVariantList>

#include "api-client.h"

namespace workspace {

// Reads the project-scoped Workspace mailbox, with a labelled preview for older servers.
class MailStore final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList folders READ folders NOTIFY mailChanged)
    Q_PROPERTY(QString folderId READ folderId NOTIFY mailChanged)
    Q_PROPERTY(QVariantList messages READ messages NOTIFY mailChanged)
    Q_PROPERTY(bool sampleFolders READ hasSampleFolders NOTIFY mailChanged)
    Q_PROPERTY(bool sampleMessages READ hasSampleMessages NOTIFY mailChanged)
    Q_PROPERTY(QString state READ state NOTIFY mailChanged)
    Q_PROPERTY(QString selectedMessageUuid READ selectedMessageUuid NOTIFY mailChanged)

public:
    explicit MailStore(ApiClient *api, QObject *parent = nullptr);

    QVariantList folders() const { return m_folders; }
    QString folderId() const { return m_folderId; }
    QVariantList messages() const { return m_messages; }
    bool hasSampleFolders() const { return m_sampleMode; }
    bool hasSampleMessages() const { return m_sampleMessages; }
    QString state() const { return m_state; }
    QString selectedMessageUuid() const { return m_selectedMessage; }

    void reset();
    Q_INVOKABLE QVariantList describeAttachments(const QList<QUrl> &urls) const;
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void selectFolder(const QString &folder);
    Q_INVOKABLE void selectMessage(const QString &uuid);

signals:
    void mailChanged();
    void ownerRetired();

private:
    void loadMessages();
    void loadPreviewFolders(quint64 token);
    void loadWorkspacePage(bool folders, quint64 token, QJsonArray rows = {},
                           QSet<QByteArray> markers = {}, const QByteArray &marker = {});
    bool m_workspaceMailbox = false;

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
    bool m_sampleMessages = false;
};

} // namespace workspace
