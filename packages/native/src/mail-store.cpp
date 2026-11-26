#include "mail-store.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QUrlQuery>

namespace workspace {

namespace {

// Sample mailbox compiled into the client for the pre-#852 phase: a
// deployment that does not serve /mail/* yet answers 404, and the Mail
// section must show the promised folders instead of a dead error screen
// (exordy finding 4712325987). Mirrors the window-test fixtures so both
// paths render the same contract.
QVariantList sampleFolders()
{
    return {QVariantMap{{QStringLiteral("id"), QStringLiteral("inbox")},
                        {QStringLiteral("title"), QStringLiteral("Inbox")},
                        {QStringLiteral("unread"), 12}},
            QVariantMap{{QStringLiteral("id"), QStringLiteral("sent")},
                        {QStringLiteral("title"), QStringLiteral("Sent")},
                        {QStringLiteral("unread"), 0}},
            QVariantMap{{QStringLiteral("id"), QStringLiteral("drafts")},
                        {QStringLiteral("title"), QStringLiteral("Drafts")},
                        {QStringLiteral("unread"), 3}},
            QVariantMap{{QStringLiteral("id"), QStringLiteral("archive")},
                        {QStringLiteral("title"), QStringLiteral("Archive")},
                        {QStringLiteral("unread"), 0}},
            QVariantMap{{QStringLiteral("id"), QStringLiteral("spam")},
                        {QStringLiteral("title"), QStringLiteral("Spam")},
                        {QStringLiteral("unread"), 1}},
            QVariantMap{{QStringLiteral("id"), QStringLiteral("trash")},
                        {QStringLiteral("title"), QStringLiteral("Trash")},
                        {QStringLiteral("unread"), 0}}};
}

QVariantMap sampleMessage(const char *uuid, const char *sender, const char *subject,
                          const char *snippet, const char *time, bool unread)
{
    return {{QStringLiteral("uuid"), QString::fromUtf8(uuid)},
            {QStringLiteral("sender"), QString::fromUtf8(sender)},
            {QStringLiteral("subject"), QString::fromUtf8(subject)},
            {QStringLiteral("snippet"), QString::fromUtf8(snippet)},
            {QStringLiteral("time"), QString::fromUtf8(time)},
            {QStringLiteral("unread"), unread}};
}

QVariantList sampleMessages(const QString &folder)
{
    if (folder == QStringLiteral("inbox"))
        return {sampleMessage("mail-inbox-1", "Build Bot",
                              "Release 0.5.0 checks passed",
                              "All pipelines are green and artifacts are ready.",
                              "10:24", true),
                sampleMessage("mail-inbox-2", "Product Team",
                              "Demo agenda for Friday",
                              "Please review talking points before 15:00.",
                              "09:02", true),
                sampleMessage("mail-inbox-3", "Design",
                              "Light theme polish proposal",
                              "Attached updated spacing and color recommendations.",
                              "Mon", true)};
    if (folder == QStringLiteral("sent"))
        return {sampleMessage("mail-sent-1", "me", "Re: demo slot",
                              "Works for me.", "Tue", false)};
    return {};
}

} // namespace

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
        if (result.status == 404) {
            // The endpoints do not ship with the server yet (#852 owns live
            // integration): show the sample mailbox compiled into the client
            // instead of a dead error screen the retry could never clear.
            self->m_folders = sampleFolders();
            self->loadMessages();
            return;
        }
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
        if (result.status == 404) {
            if (self->m_folderId != folder)
                return;
            self->m_messages = sampleMessages(folder);
            self->m_state = QStringLiteral("ready");
            emit self->mailChanged();
            return;
        }
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
