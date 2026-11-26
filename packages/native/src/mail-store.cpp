#include "mail-store.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QUrlQuery>

namespace workspace {

MailStore::MailStore(ApiClient *api, QObject *parent)
    : QObject(parent), m_api(api)
{
}

void MailStore::refresh()
{
    m_state = QStringLiteral("loading");
    emit mailChanged();
    QPointer<MailStore> self(this);
    const quint64 token = ++m_requestToken;
    m_api->request("GET", "/mail/folders", QJsonValue(), QUrlQuery(),
                   [self, token](const ApiResult &result) {
        if (!self || token != self->m_requestToken)
            return;
        if (result.status != 200 || !result.data.isObject()) {
            self->m_state = QStringLiteral("error");
            emit self->mailChanged();
            return;
        }
        QVariantList folders;
        const auto array = result.data.toObject().value(QStringLiteral("folders")).toArray();
        for (const auto &value : array) {
            const auto object = value.toObject();
            folders.append(QVariantMap{
                {QStringLiteral("id"), object.value(QStringLiteral("id")).toString()},
                {QStringLiteral("title"), object.value(QStringLiteral("title")).toString()},
                {QStringLiteral("unread"), object.value(QStringLiteral("unread")).toInt()}});
        }
        self->m_folders = folders;
        self->loadMessages();
    });
}

void MailStore::selectFolder(const QString &folder)
{
    m_folderId = folder;
    loadMessages();
}

void MailStore::selectMessage(const QString &uuid)
{
    if (m_selectedMessage == uuid)
        return;
    m_selectedMessage = uuid;
    emit mailChanged();
}

void MailStore::loadMessages()
{
    m_state = QStringLiteral("loading");
    m_selectedMessage.clear();
    emit mailChanged();
    QPointer<MailStore> self(this);
    const quint64 token = ++m_requestToken;
    const QString folder = m_folderId;
    m_api->request("GET", "/mail/messages", QJsonValue(),
                   QUrlQuery{{QStringLiteral("folder"), folder}},
                   [self, token, folder](const ApiResult &result) {
        if (!self || token != self->m_requestToken)
            return;
        if (result.status != 200 || !result.data.isObject()) {
            self->m_messages.clear();
            self->m_state = QStringLiteral("error");
            emit self->mailChanged();
            return;
        }
        QVariantList messages;
        const auto array = result.data.toObject().value(QStringLiteral("messages")).toArray();
        for (const auto &value : array) {
            const auto object = value.toObject();
            messages.append(QVariantMap{
                {QStringLiteral("uuid"), object.value(QStringLiteral("uuid")).toString()},
                {QStringLiteral("sender"), object.value(QStringLiteral("sender")).toString()},
                {QStringLiteral("subject"), object.value(QStringLiteral("subject")).toString()},
                {QStringLiteral("snippet"), object.value(QStringLiteral("snippet")).toString()},
                {QStringLiteral("time"), object.value(QStringLiteral("time")).toString()},
                {QStringLiteral("unread"), object.value(QStringLiteral("unread")).toBool()}});
        }
        // A reply that raced a folder switch would list the wrong folder's
        // rows; re-checking the folder keeps the list consistent with the
        // sidebar selection even when the token check passed.
        if (self->m_folderId != folder)
            return;
        self->m_messages = messages;
        self->m_state = QStringLiteral("ready");
        emit self->mailChanged();
    });
}

} // namespace workspace
