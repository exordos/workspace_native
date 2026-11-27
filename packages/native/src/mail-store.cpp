#include "mail-store.h"

#include "workspace-reference.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QLocale>
#include <QTextDocument>
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
                          const char *snippet, const char *time, bool unread, const char *body)
{
    return {{QStringLiteral("uuid"), QString::fromUtf8(uuid)},
            {QStringLiteral("sender"), QString::fromUtf8(sender)},
            {QStringLiteral("subject"), QString::fromUtf8(subject)},
            {QStringLiteral("snippet"), QString::fromUtf8(snippet)},
            {QStringLiteral("time"), QString::fromUtf8(time)},
            {QStringLiteral("recipients"), QStringList{QStringLiteral("team@example.test")}},
            {QStringLiteral("body"), QString::fromUtf8(body)},
            {QStringLiteral("unread"), unread}};
}

QVariantList sampleMessages(const QString &folder)
{
    if (folder == QStringLiteral("inbox"))
        return {sampleMessage("mail-inbox-1", "Build Bot",
                              "Release 0.5.0 checks passed",
                              "All pipelines are green and artifacts are ready.",
                              "10:24", true, "Hello team,\n\nCI has completed successfully for all targets."),
                sampleMessage("mail-inbox-2", "Product Team",
                              "Demo agenda for Friday",
                              "Please review talking points before 15:00.",
                              "09:02", true, "Please review the talking points before our Friday demo."),
                sampleMessage("mail-inbox-3", "Design",
                              "Light theme polish proposal",
                              "Attached updated spacing and color recommendations.",
                              "Mon", true, "Here are the proposed spacing and color updates for the light theme.")};
    if (folder == QStringLiteral("sent"))
        return {sampleMessage("mail-sent-1", "me", "Re: demo slot",
                              "Works for me.", "Tue", false, "The proposed demo slot works for me.")};
    return {};
}

QVariantMap workspaceMessage(const QJsonObject &object)
{
    QStringList text;
    QStringList html;
    for (const auto &entry : object.value("payload").toObject().value("parts").toArray()) {
        const auto part = entry.toObject();
        const auto kind = part.value("kind").toString();
        if (kind == QStringLiteral("text"))
            text.append(part.value("content").toString());
        else if (kind == QStringLiteral("html"))
            html.append(part.value("content").toString());
    }
    if (text.isEmpty() && !html.isEmpty()) {
        QTextDocument document;
        document.setResourceProvider([](const QUrl &) { return QVariant{}; });
        document.setHtml(html.join(QStringLiteral("<br>")));
        text.append(document.toPlainText());
    }
    const auto body = text.join(QLatin1Char('\n'));
    auto stamp = object.value("sent_at").toString();
    if (stamp.isEmpty())
        stamp = object.value("created_at").toString();
    const auto local = QDateTime::fromString(stamp, Qt::ISODateWithMs).toLocalTime();
    const auto date = local.isValid() ? QLocale().toString(local, QLocale::ShortFormat) : QString();
    QStringList attachments;
    for (const auto &entry : object.value("attachment_urns").toArray()) {
        const auto reference = parseWorkspaceReference(entry.toString());
        if (reference && reference->kind == WorkspaceReferenceKind::File)
            attachments.append(QStringLiteral("urn:file:") + reference->id);
    }
    return {{"uuid", object.value("uuid").toString()},
            {"sender", object.value("from_address").toString()},
            {"recipients", object.value("to_addresses").toVariant().toStringList()},
            {"subject", object.value("subject").toString()},
            {"date", date}, {"time", date}, {"body", body}, {"snippet", body.left(250).simplified()},
            {"attachments", attachments}, {"unread", !object.value("read").toBool()}};
}

} // namespace

MailStore::MailStore(ApiClient *api, QObject *parent)
    : QObject(parent), m_api(api)
{
    connect(api, &ApiClient::sessionChanged, this, [this](quint64, bool sameOwnerRenewal) {
        if (!sameOwnerRenewal)
            reset();
    });
}

void MailStore::reset()
{
    emit ownerRetired();
    if (m_state == QStringLiteral("idle"))
        return;
    ++m_requestToken;
    m_folders.clear();
    m_workspaceMailbox = false;
    m_messages.clear();
    m_folderId = QStringLiteral("inbox");
    m_selectedMessage.clear();
    m_sampleMode = false;
    m_sampleMessages = false;
    m_state = QStringLiteral("idle");
    emit mailChanged();
}

void MailStore::refresh()
{
    const quint64 token = ++m_requestToken;
    m_selectedMessage.clear();
    m_state = QStringLiteral("loading");
    emit mailChanged();
    if (token == m_requestToken)
        loadWorkspacePage(true, token);
}

void MailStore::loadWorkspacePage(bool folders, quint64 token, QJsonArray rows,
                                  QSet<QByteArray> markers, const QByteArray &marker)
{
    const QString path = folders ? QStringLiteral("/api/workspace/v1/mail/folders/")
                                 : QStringLiteral("/api/workspace/v1/mail/messages/");
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("page_limit"), QStringLiteral("100"));
    if (!folders)
        query.addQueryItem(QStringLiteral("folder_uuid"), m_folderId);
    if (!marker.isEmpty())
        query.addQueryItem(QStringLiteral("page_marker"), QString::fromUtf8(marker));

    QPointer<MailStore> self(this);
    m_api->request("GET", path, {}, query, [self, folders, token, rows = std::move(rows),
                   markers = std::move(markers), marker](const ApiResult &result) mutable {
        if (!self || self->m_requestToken != token)
            return;
        if (folders && marker.isEmpty() && result.status == 404) {
            if (self->m_workspaceMailbox || self->m_folderId.isEmpty())
                self->m_folderId = QStringLiteral("inbox");
            self->m_workspaceMailbox = false;
            self->loadPreviewFolders(token);
            return;
        }
        if (!result.ok() || !result.data.isArray() || markers.size() >= 500
            || (!result.nextMarker.isEmpty() && markers.contains(result.nextMarker))) {
            self->m_messages.clear();
            self->m_sampleMessages = false;
            if (folders) {
                self->m_folders.clear();
                self->m_folderId.clear();
                self->m_sampleMode = false;
            }
            self->m_state = QStringLiteral("error");
            emit self->mailChanged();
            return;
        }
        for (const auto &row : result.data.toArray())
            rows.append(row);
        if (!result.nextMarker.isEmpty()) {
            markers.insert(result.nextMarker);
            self->loadWorkspacePage(folders, token, std::move(rows), std::move(markers), result.nextMarker);
            return;
        }

        QVariantList items;
        QString preferred;
        QSet<QString> seen;
        for (const auto &value : rows) {
            const auto object = value.toObject();
            const auto id = object.value(QStringLiteral("uuid")).toString();
            if (id.isEmpty() || seen.contains(id))
                continue;
            seen.insert(id);
            if (folders) {
                if (object.value(QStringLiteral("kind")).toString() == QStringLiteral("inbox") && preferred.isEmpty())
                    preferred = id;
                items.append(QVariantMap{{"id", id}, {"title", object.value("name").toString()},
                                         {"unread", object.value("unread_count").toInt()}});
            } else if (object.value(QStringLiteral("folder_uuid")).toString() == self->m_folderId) {
                items.append(workspaceMessage(object));
            }
        }
        self->m_workspaceMailbox = true;
        self->m_sampleMode = false;
        self->m_sampleMessages = false;
        if (folders) {
            self->m_folders = items;
            if (!seen.contains(self->m_folderId)) {
                self->m_folderId = !preferred.isEmpty() ? preferred
                    : items.isEmpty() ? QString() : items.first().toMap().value("id").toString();
            }
            if (!self->m_folderId.isEmpty()) {
                self->loadMessages();
                return;
            }
            self->m_messages.clear();
        } else {
            self->m_messages = items;
        }
        self->m_state = QStringLiteral("ready");
        emit self->mailChanged();
    });
}

void MailStore::loadPreviewFolders(quint64 token)
{
    QPointer<MailStore> self(this);
    m_api->request("GET", "/mail/folders", QJsonValue(), QUrlQuery(),
                   [self, token](const ApiResult &result) {
        if (!self || token != self->m_requestToken)
            return;
        if (result.status == 404) {
            // The endpoints do not ship with the server yet (#852 owns live
            // integration): show the sample mailbox compiled into the client
            // instead of a dead error screen the retry could never clear.
            // This is also what arms sample mode for message loads.
            self->m_sampleMode = true;
            self->m_folders = sampleFolders();
            self->loadMessages();
            return;
        }
        if (result.status != 200 || !result.data.isObject()) {
            self->m_state = QStringLiteral("error");
            emit self->mailChanged();
            return;
        }
        // Server-provided folders: message loads must never fall back to the
        // sample mailbox under these ids.
        self->m_sampleMode = false;
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

QVariantList MailStore::describeAttachments(const QList<QUrl> &urls) const
{
    QVariantList rows;
    for (const auto &url : urls) {
        if (!url.isLocalFile())
            continue;
        const QFileInfo file(url.toLocalFile());
        if (file.isFile())
            rows.append(QVariantMap{{QStringLiteral("name"), file.fileName()},
                                    {QStringLiteral("displaySize"), QLocale().formattedDataSize(file.size())}});
    }
    return rows;
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
    const quint64 token = ++m_requestToken;
    m_state = QStringLiteral("loading");
    m_selectedMessage.clear();
    emit mailChanged();
    if (token != m_requestToken)
        return;
    if (m_workspaceMailbox) {
        loadWorkspacePage(false, token);
        return;
    }
    QPointer<MailStore> self(this);
    const QString folder = m_folderId;
    m_api->request("GET", "/mail/messages", QJsonValue(),
                   QUrlQuery{{QStringLiteral("folder"), folder}},
                   [self, token, folder](const ApiResult &result) {
        if (!self || token != self->m_requestToken)
            return;
        if (result.status == 404 && self->m_sampleMode) {
            // Only a deployment whose folders endpoint was missing too may
            // show sample messages; mixing sample rows under server folders
            // would fabricate mailbox content (finding 4712650373).
            if (self->m_folderId != folder)
                return;
            self->m_messages = sampleMessages(folder);
            self->m_sampleMessages = true;
            self->m_state = QStringLiteral("ready");
            emit self->mailChanged();
            return;
        }
        if (result.status != 200 || !result.data.isObject()) {
            self->m_messages.clear();
            self->m_sampleMessages = false;
            self->m_state = QStringLiteral("error");
            emit self->mailChanged();
            return;
        }
        QVariantList messages;
        const auto array = result.data.toObject().value(QStringLiteral("messages")).toArray();
        for (const auto &value : array) {
            const auto object = value.toObject();
            QStringList recipients;
            const auto addresses = object.value(QStringLiteral("recipients")).toArray();
            for (const auto &address : addresses) {
                if (address.isString())
                    recipients.append(address.toString());
            }
            QVariantMap row{
                {QStringLiteral("uuid"), object.value(QStringLiteral("uuid")).toString()},
                {QStringLiteral("sender"), object.value(QStringLiteral("sender")).toString()},
                {QStringLiteral("subject"), object.value(QStringLiteral("subject")).toString()},
                {QStringLiteral("recipients"), recipients},
                {QStringLiteral("date"), object.value(QStringLiteral("date")).toString()},
                {QStringLiteral("snippet"), object.value(QStringLiteral("snippet")).toString()},
                {QStringLiteral("time"), object.value(QStringLiteral("time")).toString()},
                {QStringLiteral("unread"), object.value(QStringLiteral("unread")).toBool()}};
            const auto body = object.value(QStringLiteral("body"));
            if (body.isString())
                row.insert(QStringLiteral("body"), body.toString());
            messages.append(row);
        }
        // A reply that raced a folder switch would list the wrong folder's
        // rows; re-checking the folder keeps the list consistent with the
        // sidebar selection even when the token check passed.
        if (self->m_folderId != folder)
            return;
        self->m_messages = messages;
        self->m_sampleMessages = false;
        self->m_state = QStringLiteral("ready");
        emit self->mailChanged();
    });
}

} // namespace workspace
