#include "native-external-accounts.h"
#include "domain.h"
#include "session.h"
#include <QJsonArray>
#include <QUuid>
#include <algorithm>
#include <utility>

namespace workspace
{
namespace
{
    const QString collection = QStringLiteral("/api/workspace/v1/messenger/external_accounts/");
    QByteArray revision(const QJsonObject& account)
    {
        const auto value = account.value("revision").toInteger(-1);
        return value < 0 ? QByteArray { } : '"' + QByteArray::number(value) + '"';
    }
}

NativeExternalAccounts::NativeExternalAccounts(ApiClient* api, Session* session, QObject* parent)
    : QObject(parent)
    , m_api(api)
    , m_session(session)
{
    const auto invalidate = [this] {
        if (m_owner.isEmpty())
            return;
        close();
        emit contextLost();
    };
    connect(api, &ApiClient::sessionChanged, this, invalidate);
    connect(session, &Session::signedOut, this, invalidate);
}

NativeExternalAccounts::~NativeExternalAccounts() { cancel(); }

QString NativeExternalAccounts::owner() const
{
    if (!m_session->ready() || !isUuid(m_session->project()) || !isUuid(m_session->user()))
        return { };
    return m_api->server().toString() + '/' + m_session->project() + '/' + m_session->user();
}

bool NativeExternalAccounts::current(quint64 ticket) const
{
    return ticket == m_ticket && !m_owner.isEmpty() && m_owner == owner()
        && m_generation == m_api->generation();
}

QVariantList NativeExternalAccounts::rows() const { return current(m_ticket) ? m_rows : QVariantList { }; }

void NativeExternalAccounts::cancel()
{
    ++m_ticket;
    const auto request = std::exchange(m_request, { });
    if (request.isValid())
        m_api->cancelRequest(request);
}

void NativeExternalAccounts::close()
{
    cancel();
    m_owner.clear();
    m_attemptId.clear();
    m_attemptServer.clear();
    m_attemptEmail.clear();
    m_rows.clear();
    m_pendingRows.clear();
    m_seenRows.clear();
    m_markers.clear();
    m_busy = m_loading = m_saveFailed = m_loadFailed = false;
    emit changed();
}

void NativeExternalAccounts::open()
{
    close();
    m_owner = owner();
    m_generation = m_api->generation();
    if (m_owner.isEmpty())
        return;
    const auto ticket = m_ticket;
    m_loading = true;
    emit changed();
    if (current(ticket))
        page(ticket);
}

QVariantMap NativeExternalAccounts::project(const QJsonObject& row) const
{
    const auto id = row.value("uuid").toString();
    const auto settings = row.value("settings").toObject();
    const auto provider = row.value("provider").toString(settings.value("kind").toString());
    if (!isUuid(id) || provider != "zulip" || row.value("status").toString().isEmpty()
        || (row.contains("project_id") && row.value("project_id") != m_session->project())
        || (row.contains("user_uuid") && row.value("user_uuid") != m_session->user()))
        return { };
    return { { "id", id }, { "server", settings.value("server_url").toString() },
        { "email", settings.value("email").toString() }, { "status", row.value("status").toString() },
        { "live", row.value("live_ready").toBool() },
        { "safeError", row.value("safe_error").toString(row.value("error_description").toString()) },
        { "etag", revision(row) } };
}

void NativeExternalAccounts::page(quint64 ticket, const QByteArray& marker)
{
    QUrlQuery query;
    query.addQueryItem("page_limit", "100");
    if (!marker.isEmpty())
        query.addQueryItem("page_marker", QString::fromUtf8(marker));
    m_request = m_api->request("GET", collection, { }, query, [this, ticket](const ApiResult& result) {
        if (!current(ticket))
            return;
        bool valid = result.ok() && result.data.isArray();
        for (const auto& value : result.data.toArray()) {
            const auto row = project(value.toObject());
            const auto id = row.value("id").toString();
            if (id.isEmpty() || m_seenRows.contains(id) || m_seenRows.size() >= 10000) {
                valid = false;
                break;
            }
            m_seenRows.insert(id);
            m_pendingRows.append(row);
        }
        if (valid && !result.nextMarker.isEmpty() && !m_markers.contains(result.nextMarker)) {
            m_markers.insert(result.nextMarker);
            page(ticket, result.nextMarker);
            return;
        }
        m_loadFailed = !valid || !result.nextMarker.isEmpty();
        m_rows = m_loadFailed ? QVariantList { } : std::move(m_pendingRows);
        m_pendingRows.clear();
        m_seenRows.clear();
        m_markers.clear();
        m_loading = false;
        m_request = { };
        emit changed();
    });
}

void NativeExternalAccounts::cancelForm()
{
    if (m_busy) {
        // A server may already have accepted a cancelled write.
        open();
        return;
    }
    m_saveFailed = false;
    m_attemptId.clear();
    m_attemptServer.clear();
    m_attemptEmail.clear();
    emit changed();
}

void NativeExternalAccounts::finishWrite(
    quint64 ticket, const QString& id, const QVariantMap& row, bool removing, bool success)
{
    if (!current(ticket))
        return;
    m_busy = false;
    m_request = { };
    m_saveFailed = !success;
    if (success) {
        m_attemptId.clear();
        m_attemptEmail.clear();
        m_attemptServer.clear();
        auto existing = std::find_if(m_rows.begin(), m_rows.end(),
            [&](const QVariant& value) { return value.toMap().value("id") == id; });
        if (removing) {
            if (existing != m_rows.end())
                m_rows.erase(existing);
        } else if (existing == m_rows.end()) {
            m_rows.append(row);
        } else {
            *existing = row;
        }
    }
    emit changed();
    if (success && current(ticket))
        emit saved();
}

void NativeExternalAccounts::write(quint64 ticket, const QString& id, const QByteArray& method,
    const QString& path, const QJsonObject& body, const Headers& headers)
{
    const bool creating = method == "POST" && path == collection;
    const bool removing = method == "DELETE";
    m_request = m_api->request(
        method, path, body, { },
        [this, ticket, id, creating, removing](const ApiResult& result) {
            if (!current(ticket))
                return;
            auto row = project(result.data.toObject());
            const auto returnedId = row.value("id").toString();
            if (creating && result.ok() && !returnedId.isEmpty() && returnedId != id) {
                // Binding an imported Zulip account keeps its original canonical UUID.
                // Read it in the current actor's scope before presenting it as theirs.
                m_request = m_api->request("GET", collection + returnedId, { }, { },
                    [this, ticket, returnedId](const ApiResult& read) {
                        if (!current(ticket))
                            return;
                        auto canonical = project(read.data.toObject());
                        const auto server
                            = QUrl(canonical.value("server").toString()).adjusted(QUrl::StripTrailingSlash);
                        const bool matches = read.ok() && canonical.value("id") == returnedId
                            && server == QUrl(m_attemptServer)
                            && canonical.value("email").toString().compare(m_attemptEmail, Qt::CaseInsensitive)
                                == 0;
                        if (!read.etag.isEmpty())
                            canonical["etag"] = read.etag;
                        finishWrite(ticket, returnedId, canonical, false, matches);
                    });
                return;
            }
            if (!result.etag.isEmpty())
                row["etag"] = result.etag;
            finishWrite(ticket, id, row, removing, result.ok() && (removing || returnedId == id));
        },
        false, headers);
}

void NativeExternalAccounts::save(const QString& mode, const QString& id, const QString& server,
    const QString& email, const QString& credential)
{
    if (!current(m_ticket) || m_busy || m_loading || m_loadFailed)
        return;
    const auto url = QUrl(server.trimmed(), QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty() || url.scheme() != "https" || !url.userInfo().isEmpty()
        || url.hasFragment() || url.hasQuery() || email.trimmed().isEmpty()
        || (mode != "edit" && credential.trimmed().isEmpty())) {
        m_saveFailed = true;
        emit changed();
        return;
    }
    const auto existing = std::find_if(
        m_rows.cbegin(), m_rows.cend(), [&](const QVariant& v) { return v.toMap().value("id") == id; });
    if (mode == "create" ? !m_rows.isEmpty() : existing == m_rows.cend())
        return;
    if (mode != "create" && mode != "edit" && mode != "reconnect")
        return;
    const auto old = existing == m_rows.cend() ? QVariantMap { } : existing->toMap();
    if (mode == "reconnect" && old.value("status") != "auth_required" && old.value("status") != "degraded")
        return;
    const auto origin = url.adjusted(QUrl::StripTrailingSlash).toString();
    const auto address = email.trimmed();
    QJsonObject settings { { "kind", "zulip" }, { "server_url", origin }, { "email", address } };
    if (!credential.trimmed().isEmpty())
        settings["api_key"] = credential.trimmed();
    const auto ticket = ++m_ticket;
    m_busy = true;
    m_saveFailed = false;
    emit changed();
    if (!current(ticket))
        return;
    if (mode == "create") {
        if (m_attemptId.isEmpty() || m_attemptServer != origin || m_attemptEmail != address) {
            m_attemptId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_attemptServer = origin;
            m_attemptEmail = address;
        }
        settings["selection_mode"] = "all";
        settings["history_depth"] = "all";
        settings["default_project_id"] = m_session->project();
        write(ticket, m_attemptId, "POST", collection,
            { { "uuid", m_attemptId }, { "provider", "zulip" }, { "settings", settings } }, { });
        return;
    }
    if (mode == "reconnect") {
        Headers headers;
        const auto etag = old.value("etag").toByteArray();
        if (!etag.isEmpty())
            headers.insert("If-Match", etag);
        write(ticket, id, "POST", collection + id + "/actions/reconnect/invoke", { { "settings", settings } },
            headers);
        return;
    }
    m_request = m_api->request(
        "GET", collection + id, { }, { }, [this, ticket, id, settings, old](const ApiResult& result) {
            if (!current(ticket))
                return;
            const auto raw = result.data.toObject();
            if (!result.ok() || project(raw).value("id") != id) {
                m_request = { };
                m_busy = false;
                m_saveFailed = true;
                emit changed();
                return;
            }
            auto replacement = raw.value("settings").toObject();
            replacement.remove("api_key");
            if (settings.value("server_url").toString() != old.value("server").toString())
                replacement["server_url"] = settings.value("server_url");
            if (settings.value("email").toString() != old.value("email").toString())
                replacement["email"] = settings.value("email");
            if (settings.contains("api_key"))
                replacement["api_key"] = settings.value("api_key");
            Headers headers;
            const auto etag = result.etag.isEmpty() ? revision(raw) : result.etag;
            if (!etag.isEmpty())
                headers.insert("If-Match", etag);
            write(ticket, id, "PUT", collection + id, { { "settings", replacement } }, headers);
        });
}

void NativeExternalAccounts::remove(const QString& id)
{
    if (!current(m_ticket) || m_busy || m_loading || m_loadFailed || !isUuid(id))
        return;
    const auto listed = std::any_of(
        m_rows.cbegin(), m_rows.cend(), [&](const QVariant& row) { return row.toMap().value("id") == id; });
    if (!listed)
        return;
    const auto ticket = ++m_ticket;
    m_busy = true;
    m_saveFailed = false;
    emit changed();
    if (current(ticket))
        write(ticket, id, "DELETE", collection + id, { }, { });
}
} // namespace workspace
