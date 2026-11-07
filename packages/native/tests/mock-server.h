#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <functional>
#include <memory>

namespace workspace::test {

inline const QString projectId = "11111111-1111-4111-8111-111111111111";
inline const QString userId = "22222222-2222-4222-8222-222222222222";
inline const QString streamId = "33333333-3333-4333-8333-333333333333";
inline const QString topicId = "44444444-4444-4444-8444-444444444444";
inline const QString secondTopicId = "55555555-5555-4555-8555-555555555555";
inline const QString messageId = "66666666-6666-4666-8666-666666666666";
inline const QString returnedId = "77777777-7777-4777-8777-777777777777";

struct Request {
    QByteArray method;
    QUrl target;
    QHash<QByteArray, QByteArray> headers;
    QJsonObject body;
};

struct Response {
    QJsonValue data = QJsonObject{};
    int status = 200;
    int delay = 0;
    QMap<QByteArray, QByteArray> headers;
    std::function<void(std::function<void()>)> defer; // optional test-owned delivery gate
};

class MockServer final : public QTcpServer {
public:
    std::function<Response(const Request &)> handler;
    QVector<Request> requests;

    MockServer()
    {
        listen(QHostAddress::LocalHost, 0);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
                    if (socket->property("handled").toBool())
                        return;
                    buffer->append(socket->readAll());
                    const auto end = buffer->indexOf("\r\n\r\n");
                    if (end < 0)
                        return;
                    const auto lines = buffer->left(end).split('\n');
                    const auto first = lines.first().trimmed().split(' ');
                    if (first.size() < 2)
                        return;
                    Request request;
                    request.method = first.at(0);
                    request.target = QUrl::fromEncoded(first.at(1));
                    for (int i = 1; i < lines.size(); ++i) {
                        const auto separator = lines.at(i).indexOf(':');
                        if (separator > 0)
                            request.headers.insert(lines.at(i).left(separator).trimmed().toLower(),
                                                   lines.at(i).mid(separator + 1).trimmed());
                    }
                    const auto size = request.headers.value("content-length").toInt();
                    if (buffer->size() < end + 4 + size)
                        return;
                    request.body = QJsonDocument::fromJson(buffer->mid(end + 4, size)).object();
                    socket->setProperty("handled", true);
                    requests.append(request);
                    const auto response = handler ? handler(request) : Response{};
                    const auto send = [guarded = QPointer<QTcpSocket>(socket), response] {
                        auto *socket = guarded.data();
                        if (!socket || socket->state() != QAbstractSocket::ConnectedState)
                            return;
                        const auto body = response.data.isArray()
                            ? QJsonDocument(response.data.toArray()).toJson(QJsonDocument::Compact)
                            : QJsonDocument(response.data.toObject()).toJson(QJsonDocument::Compact);
                        QByteArray headers = "HTTP/1.1 " + QByteArray::number(response.status)
                            + " Test\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: "
                            + QByteArray::number(body.size()) + "\r\n";
                        for (auto it = response.headers.cbegin(); it != response.headers.cend(); ++it)
                            headers += it.key() + ": " + it.value() + "\r\n";
                        socket->write(headers + "\r\n" + body);
                        socket->disconnectFromHost();
                    };
                    if (response.defer) response.defer(send);
                    else QTimer::singleShot(response.delay, socket, send);
                });
            }
        });
    }

    QUrl origin() const { return QUrl(QString("http://127.0.0.1:%1").arg(serverPort())); }
};

inline QJsonObject message(const QString &content = "Hello from a synthetic account",
                           const QString &uuid = messageId, const QString &topic = topicId)
{
    return {{"uuid", uuid}, {"project_id", projectId}, {"stream_uuid", streamId},
            {"topic_uuid", topic}, {"author_uuid", userId}, {"is_own", true},
            {"read", true}, {"starred", false}, {"created_at", "2026-01-02T10:00:00.000Z"},
            {"payload", QJsonObject{{"kind", "markdown"}, {"content", content}}}};
}

inline Response sessionResponse(const Request &request)
{
    if (request.target.path().endsWith("/actions/get_token/invoke"))
        return {QJsonObject{{"access_token", request.body.value("grant_type") == "refresh_token"
            ? "synthetic-project-token" : "synthetic-base-token"},
            {"refresh_token", "synthetic-refresh-token"}, {"expires_in", 3600}}};
    if (request.target.path() == "/api/core/v1/iam/projects/")
        return {QJsonArray{QJsonObject{{"uuid", projectId}, {"name", "Synthetic project"},
                                     {"status", "ACTIVE"}}}};
    if (request.target.path().endsWith("/actions/introspect"))
        return {QJsonObject{{"user_info", QJsonObject{{"uuid", userId}}},
                            {"project_id", projectId}, {"otp_verified", true},
                            {"permissions", QJsonArray{}}}};
    return {QJsonObject{}, 404};
}

}
