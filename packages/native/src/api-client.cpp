#include "api-client.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QHttpMultiPart>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QRegularExpression>
#include <QWebSocket>
#include <QWebSocketHandshakeOptions>

#include <atomic>
#include <limits>

namespace workspace {
namespace {

constexpr qsizetype MaxResponseBytes = 16 * 1024 * 1024;
constexpr int RequestTimeoutMs = 30'000;

quint64 nextRequestSerial()
{
    // Handles cannot alias another transport or a destroyed/recreated instance.
    static std::atomic<quint64> next{1};
    auto value = next.load(std::memory_order_relaxed);
    while (value != std::numeric_limits<quint64>::max()) {
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
            return value;
    }
    return 0; // Saturate instead of ever reusing a request identity.
}

bool isLoopback(const QString &host)
{
    return host == QStringLiteral("127.0.0.1") || host == QStringLiteral("::1")
        || host.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0;
}

bool validPath(const QString &path)
{
    if (!path.startsWith('/') || path.startsWith(QStringLiteral("//")))
        return false;
    QString decoded = path;
    // Reject traversal even after repeated proxy percent-decoding. Encoded path
    // separators are never required for the API's UUID resource identifiers.
    for (int pass = 0; pass < 8; ++pass) {
        if (decoded.contains('\\') || decoded.contains('?') || decoded.contains('#')
            || decoded.contains(QStringLiteral("//")))
            return false;
        for (const QChar character : decoded) {
            if (character.isSpace() || character.unicode() < 0x20 || character.unicode() == 0x7f)
                return false;
        }
        const auto segments = decoded.split('/');
        for (const auto &segment : segments) {
            if (segment == QStringLiteral(".") || segment == QStringLiteral(".."))
                return false;
        }
        const QString next = QUrl::fromPercentEncoding(decoded.toUtf8());
        if (next == decoded)
            return !decoded.contains('%');
        if (next.count('/') != decoded.count('/') || next.contains('\\'))
            return false;
        decoded = next;
    }
    return false;
}

bool validExtraHeader(const QByteArray &name, const QByteArray &value)
{
    const QByteArray lower = name.toLower();
    if (lower.isEmpty() || lower == "authorization" || lower == "host" || lower == "cookie"
        || lower == "proxy-authorization" || lower == "content-length"
        || lower == "transfer-encoding" || lower == "connection" || lower == "content-type"
        || lower == "accept" || lower == "origin" || lower == "referer")
        return false;
    for (const auto character : name) {
        if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
              || (character >= '0' && character <= '9') || character == '-'))
            return false;
    }
    for (const auto character : value) {
        if (static_cast<unsigned char>(character) < 0x20 || character == 0x7f)
            return false;
    }
    return true;
}

QString statusError(int status)
{
    if (status == 401)
        return ApiClient::tr("Authentication failed (HTTP 401).");
    if (status == 403)
        return ApiClient::tr("Access denied (HTTP 403).");
    return ApiClient::tr("Request failed (HTTP %1).").arg(status);
}

} // namespace

struct ApiClient::Pending {
    Callback callback;
    QByteArray response;
    quint64 generation = 0;
    quint64 serial = 0;
    bool callerCancelled = false;
    bool cancelled = false;
    bool tooLarge = false;
    bool timedOut = false;
    bool expectsJson = true;
};

ApiClient::ApiClient(QObject *parent, bool allowLoopbackHttpForTests)
    : QObject(parent), m_network(new QNetworkAccessManager(this)),
      m_allowLoopbackHttpForTests(allowLoopbackHttpForTests)
{
}

ApiClient::~ApiClient()
{
    ++m_generation;
    m_requestReplies.clear();
    const auto transfers = m_transfers.values();
    for (auto *reply : transfers) {
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
    }
    const auto sockets = m_sockets.values();
    for (auto *socket : sockets) {
        disconnect(socket, nullptr, this, nullptr);
        socket->abort();
    }
    const auto replies = m_pending.keys();
    for (auto *reply : replies) {
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        delete m_pending.take(reply);
    }
}

bool ApiClient::setServer(const QUrl &origin, QString *error)
{
    const bool schemeAllowed = origin.scheme() == QStringLiteral("https")
        || (m_allowLoopbackHttpForTests && origin.scheme() == QStringLiteral("http")
            && isLoopback(origin.host()));
    if (!origin.isValid() || origin.isRelative() || origin.host().isEmpty() || !schemeAllowed
        || origin.authority().contains('@') || origin.hasQuery() || origin.hasFragment()
        || (!origin.path().isEmpty() && origin.path() != QStringLiteral("/"))) {
        if (error)
            *error = ApiClient::tr("Enter an HTTPS server origin without a path, credentials, query, or fragment.");
        return false;
    }
    QUrl normalized = origin;
    normalized.setPath({});
    if (normalized != m_server) {
        m_server = normalized;
        m_accessToken.clear();
        invalidateRequests();
    }
    if (error)
        error->clear();
    return true;
}

void ApiClient::setAccessToken(const QByteArray &token)
{
    if (m_accessToken == token)
        return;
    m_accessToken = token;
    invalidateRequests();
}

void ApiClient::clearSession()
{
    m_accessToken.clear();
    invalidateRequests();
}

void ApiClient::invalidateRequests()
{
    ++m_generation;
    m_requestReplies.clear();
    const auto transfers = m_transfers.values();
    for (auto *reply : transfers)
        reply->abort();
    const auto replies = m_pending.keys();
    for (auto *reply : replies) {
        auto *pending = m_pending.value(reply, nullptr);
        if (pending) {
            pending->cancelled = true;
            reply->abort();
        }
    }
    const auto sockets = m_sockets.values();
    for (auto *socket : sockets)
        socket->abort();
    emit sessionChanged(m_generation);
}

QNetworkReply *ApiClient::startFileUpload(QHttpMultiPart *body)
{
    return body ? startFileRequest({}, body, false) : nullptr;
}

QNetworkReply *ApiClient::startFileMetadata(const QString &uuid)
{
    return startFileRequest(uuid, nullptr, true);
}

QNetworkReply *ApiClient::startFileDownload(const QString &uuid)
{
    return startFileRequest(uuid, nullptr, false);
}

QNetworkReply *ApiClient::startFileRequest(const QString &uuid, QHttpMultiPart *body, bool metadata)
{
    static const QRegularExpression uuidPattern(
        QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
    if (m_server.isEmpty() || m_accessToken.isEmpty() || (!body && !uuidPattern.match(uuid).hasMatch()))
        return nullptr;
    for (const auto character : m_accessToken)
        if (static_cast<unsigned char>(character) <= 0x20 || character == 0x7f)
            return nullptr;
    QUrl url = m_server;
    url.setPath(QStringLiteral("/api/workspace/v1/messenger/files/")
                + (body ? QString() : uuid.toLower() + (metadata ? QString() : QStringLiteral("/actions/download"))));
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setRawHeader("Accept", body || metadata ? "application/json" : "application/octet-stream");
    request.setRawHeader("Accept-Encoding", "identity");
    request.setRawHeader("Authorization", "Bearer " + m_accessToken);
    auto *reply = body ? m_network->post(request, body) : m_network->get(request);
    if (body)
        body->setParent(reply);
    m_transfers.insert(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply] { m_transfers.remove(reply); });
    connect(reply, &QObject::destroyed, this, [this, reply] { m_transfers.remove(reply); });
    return reply;
}

bool ApiClient::openRealtimeSocket(QWebSocket *socket, const QUrlQuery &cursor)
{
    if (!socket || m_server.isEmpty() || m_accessToken.isEmpty())
        return false;
    for (const auto character : m_accessToken)
        if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
              || (character >= '0' && character <= '9') || character == '.'
              || character == '_' || character == '-'))
            return false;
    QSet<QString> keys;
    for (const auto &item : cursor.queryItems()) {
        if ((item.first != "last_epoch_version" && item.first != "epoch_generation")
            || keys.contains(item.first) || item.second.isEmpty())
            return false;
        keys.insert(item.first);
    }
    bool validEpoch = false;
    const auto epoch = cursor.queryItemValue("last_epoch_version").toULongLong(&validEpoch);
    if (!validEpoch || epoch > 9007199254740991ULL || !keys.contains("epoch_generation"))
        return false;
    QUrl url = m_server;
    url.setScheme(url.scheme() == "https" ? "wss" : "ws");
    url.setPath("/api/workspace/v1/events/ws");
    url.setQuery(cursor);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    QWebSocketHandshakeOptions options;
    options.setSubprotocols({"workspace.events.v1", QString::fromLatin1("bearer." + m_accessToken)});
    if (!m_sockets.contains(socket)) {
        m_sockets.insert(socket);
        connect(socket, &QObject::destroyed, this, [this, socket] { m_sockets.remove(socket); });
    }
    socket->open(request, options);
    return true;
}

void ApiClient::reject(Callback callback, const QString &message)
{
    QTimer::singleShot(0, this, [callback = std::move(callback), message] {
        if (callback) {
            ApiResult result;
            result.error = message;
            callback(result);
        }
    });
}

ApiClient::RequestId ApiClient::request(const QByteArray &method, const QString &path, const QJsonValue &body,
                        const QUrlQuery &query, Callback callback, bool isPublic,
                        const QMap<QByteArray, QByteArray> &headers)
{
    if (m_server.isEmpty() || !validPath(path)) {
        reject(std::move(callback), ApiClient::tr("Invalid API server or path."));
        return {};
    }
    if (method != "GET" && method != "HEAD" && method != "POST" && method != "PUT"
        && method != "PATCH" && method != "DELETE") {
        reject(std::move(callback), ApiClient::tr("Unsupported API method."));
        return {};
    }
    if (!body.isNull() && !body.isUndefined() && !body.isObject() && !body.isArray()) {
        reject(std::move(callback), ApiClient::tr("API request body must be a JSON object or array."));
        return {};
    }
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        if (!validExtraHeader(it.key(), it.value())) {
            reject(std::move(callback), ApiClient::tr("Invalid API request header."));
            return {};
        }
    }
    if (!isPublic && m_accessToken.isEmpty()) {
        reject(std::move(callback), ApiClient::tr("Sign in to continue."));
        return {};
    }
    for (const auto character : m_accessToken) {
        if (!isPublic && (static_cast<unsigned char>(character) <= 0x20 || character == 0x7f)) {
            reject(std::move(callback), ApiClient::tr("Invalid session credential."));
            return {};
        }
    }

    const auto serial = nextRequestSerial();
    if (serial == 0) {
        reject(std::move(callback), ApiClient::tr("No request identifiers are available."));
        return {};
    }

    QUrl url = m_server;
    url.setPath(path);
    url.setQuery(query);
    QNetworkRequest networkRequest(url);
    networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                QNetworkRequest::ManualRedirectPolicy);
    networkRequest.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
                                QNetworkRequest::Manual);
    networkRequest.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
                                QNetworkRequest::Manual);
    networkRequest.setAttribute(QNetworkRequest::AuthenticationReuseAttribute,
                                QNetworkRequest::Manual);
    networkRequest.setRawHeader("Accept", "application/json");
    if (!isPublic)
        networkRequest.setRawHeader("Authorization", "Bearer " + m_accessToken);
    for (auto it = headers.cbegin(); it != headers.cend(); ++it)
        networkRequest.setRawHeader(it.key(), it.value());
    QByteArray payload;
    if (body.isObject() || body.isArray()) {
        payload = body.isObject() ? QJsonDocument(body.toObject()).toJson(QJsonDocument::Compact)
                                  : QJsonDocument(body.toArray()).toJson(QJsonDocument::Compact);
        networkRequest.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    }

    auto *reply = m_network->sendCustomRequest(networkRequest, method, payload);
    reply->setReadBufferSize(MaxResponseBytes + 1);
    auto *pending = new Pending;
    pending->callback = std::move(callback);
    pending->generation = m_generation;
    pending->serial = serial;
    pending->expectsJson = method != "HEAD" && method != "DELETE";
    m_pending.insert(reply, pending);
    m_requestReplies.insert(serial, reply);
    auto *timer = new QTimer(reply);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, reply] {
        if (auto *state = m_pending.value(reply, nullptr)) {
            state->timedOut = true;
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        auto *state = m_pending.value(reply, nullptr);
        if (!state)
            return;
        const auto remaining = MaxResponseBytes - state->response.size();
        state->response.append(reply->read(remaining + 1));
        if (state->response.size() > MaxResponseBytes) {
            state->tooLarge = true;
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] { finish(reply); });
    timer->start(RequestTimeoutMs);
    return RequestId(serial);
}

bool ApiClient::cancelRequest(RequestId id)
{
    auto *reply = m_requestReplies.take(id.m_serial);
    auto *pending = reply ? m_pending.value(reply, nullptr) : nullptr;
    if (!pending)
        return false;
    // Retire the handle before abort: finished/callback can be synchronous,
    // reenter the transport, or destroy it. Do not access members after abort.
    pending->callerCancelled = true;
    reply->abort();
    return true;
}

void ApiClient::finish(QNetworkReply *reply)
{
    auto *pending = m_pending.take(reply);
    if (!pending)
        return;
    m_requestReplies.remove(pending->serial);
    ApiResult result;
    result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (pending->callerCancelled) {
        result.status = 0;
        result.error = ApiClient::tr("Request cancelled.");
    } else if (pending->cancelled || pending->generation != m_generation) {
        result.status = 0;
        result.error = ApiClient::tr("Request cancelled because the session changed.");
    } else if (pending->tooLarge) {
        result.error = ApiClient::tr("Server response exceeds the allowed size.");
    } else if (pending->timedOut) {
        result.error = ApiClient::tr("Request timed out.");
    } else if (result.status >= 300 && result.status < 400) {
        result.error = ApiClient::tr("Redirects are not allowed.");
    } else {
        const auto remaining = MaxResponseBytes - pending->response.size();
        pending->response.append(reply->read(remaining + 1));
        if (pending->response.size() > MaxResponseBytes) {
            result.error = ApiClient::tr("Server response exceeds the allowed size.");
        } else if (result.status == 0 || (reply->error() != QNetworkReply::NoError
                                         && result.status >= 200 && result.status < 300)) {
            result.error = ApiClient::tr("Network request failed.");
        } else {
            const auto response = pending->response.trimmed();
            if (!response.isEmpty()) {
                QJsonParseError parseError;
                const auto document = QJsonDocument::fromJson(response, &parseError);
                if (parseError.error != QJsonParseError::NoError || document.isNull()) {
                    result.error = ApiClient::tr("Server returned invalid JSON.");
                } else {
                    result.data = document.isArray() ? QJsonValue(document.array())
                                                     : QJsonValue(document.object());
                    result.etag = reply->rawHeader("ETag");
                }
            } else if (result.status != 204 && pending->expectsJson) {
                result.error = ApiClient::tr("Server returned an empty response.");
            }
            if (result.status < 200 || result.status >= 300)
                result.error = statusError(result.status);
            if (result.error.isEmpty())
                result.nextMarker = reply->rawHeader("X-Pagination-Marker");
        }
    }
    const auto callback = std::move(pending->callback);
    delete pending;
    reply->deleteLater();
    if (callback)
        callback(result);
}

} // namespace workspace
