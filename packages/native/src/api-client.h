#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonValue>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QWebSocket;
class QHttpMultiPart;

namespace workspace {

struct ApiResult {
    QJsonValue data;
    int status = 0;
    QByteArray nextMarker;
    QString error;
    // Kept on canonical HTTP errors too: draft conflicts carry the current
    // snapshot and its strong revision. Never retained for cancelled requests.
    QByteArray etag{};

    bool ok() const { return status >= 200 && status < 300 && error.isEmpty(); }
};

class ApiClient final : public QObject {
    Q_OBJECT

public:
    using Callback = std::function<void(const ApiResult &)>;

    // Opaque C++-only identity for one JSON request. No reply pointer, origin or
    // credential is exposed. The default identity never cancels a request.
    class RequestId {
    public:
        RequestId() = default;
        bool isValid() const { return m_serial != 0; }
        bool operator==(const RequestId &other) const { return m_serial == other.m_serial; }
        bool operator!=(const RequestId &other) const { return !(*this == other); }
    private:
        friend class ApiClient;
        explicit RequestId(quint64 serial) : m_serial(serial) {}
        quint64 m_serial = 0;
    };

    explicit ApiClient(QObject *parent = nullptr, bool allowLoopbackHttpForTests = false);
    ~ApiClient() override;

    bool setServer(const QUrl &origin, QString *error = nullptr);
    QUrl server() const { return m_server; }
    void setAccessToken(const QByteArray &token);
    void clearSession();
    quint64 generation() const { return m_generation; }
    bool openRealtimeSocket(QWebSocket *socket, const QUrlQuery &cursor);
    // C++ transfer entrypoints keep credentials and the destination origin inside
    // this transport. Callers own streaming limits, completion and reply cleanup.
    // A successful upload reparents body to its reply; failure leaves it untouched.
    QNetworkReply *startFileUpload(QHttpMultiPart *body);
    QNetworkReply *startFileMetadata(const QString &uuid);
    QNetworkReply *startFileDownload(const QString &uuid);

    RequestId request(const QByteArray &method, const QString &path, const QJsonValue &body,
                 const QUrlQuery &query, Callback callback, bool isPublic = false,
                 const QMap<QByteArray, QByteArray> &headers = {});
    bool cancelRequest(RequestId id);

signals:
    void sessionChanged(quint64 generation);

private:
    struct Pending;
    void invalidateRequests();
    void finish(QNetworkReply *reply);
    void reject(Callback callback, const QString &message);
    QNetworkReply *startFileRequest(const QString &uuid, QHttpMultiPart *body, bool metadata);

    QNetworkAccessManager *m_network;
    QUrl m_server;
    QByteArray m_accessToken;
    quint64 m_generation = 0;
    bool m_allowLoopbackHttpForTests;
    QHash<QNetworkReply *, Pending *> m_pending;
    QHash<quint64, QNetworkReply *> m_requestReplies;
    QSet<QWebSocket *> m_sockets;
    QSet<QNetworkReply *> m_transfers;
};

} // namespace workspace
