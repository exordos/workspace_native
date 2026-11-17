#include "realtime-client.h"
#include "native-cache-service.h"
#include "native-controller.h"
#include "native-realtime-cursor-store.h"
#include "mock-server.h"

#include <QDir>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QWebSocket>
#include <QWebSocketServer>
#include <optional>
#include <vector>

using namespace workspace;
using namespace workspace::test;

static QJsonObject realtimeEvent(quint64 epoch)
{
    return {{"schema_version", 1}, {"epoch_version", qint64(epoch)},
        {"uuid", QString("99999999-9999-4999-8999-%1").arg(epoch, 12, 16, QLatin1Char('0'))},
        {"project_id", projectId}, {"user_uuid", userId}, {"object_type", "message"},
        {"action", "updated"}, {"payload", QJsonObject{{"kind", "future.message.updated"}}},
        {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
}

static QJsonObject pruned(const QString &generation)
{
    return {{"type", "EventsCursorExpiredError"}, {"code", 410}, {"error", "epoch_pruned"},
        {"message", "Cursor expired"}, {"reason", "epoch_generation_changed"},
        {"epoch_generation", generation}, {"current_epoch_version", 30}, {"minimum_epoch_version", 1}};
}

class RealtimeFixture final : public QObject {
public:
    QTcpServer gateway;
    QWebSocketServer webSocketServer{"Realtime tests", QWebSocketServer::NonSecureMode};
    ApiClient api{nullptr, true};
    Session session{&api};
    RealtimeClient client{&api, &session};
    QVector<Request> requests;
    QVector<QPointer<QWebSocket>> sockets;
    QMap<quint64, QJsonObject> events;
    std::function<Response(const Request &)> eventsHandler;
    // Optional replacement for the mock session endpoints (projects list,
    // introspect) so a test can present multiple projects/owners. Returning
    // nullopt falls through to the stock sessionResponse.
    std::function<std::optional<Response>(const Request &)> sessionOverride;
    QString epochGeneration = "generation-a";
    quint64 baselineEpoch = 10;
    int snapshots = 0;
    bool autoSnapshot = false;
    int tokenRefreshes = 0;
    int refreshDelay = 0;
    QUrl socketRedirect;

    RealtimeFixture()
    {
        gateway.listen(QHostAddress::LocalHost);
        webSocketServer.setSupportedSubprotocols({"workspace.events.v1"});
        connect(&gateway, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = gateway.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    if (socket->property("handled").toBool())
                        return;
                    const auto bytes = socket->peek(socket->bytesAvailable());
                    const auto end = bytes.indexOf("\r\n\r\n");
                    if (end < 0)
                        return;
                    if (bytes.left(end).toLower().contains("upgrade: websocket")) {
                        socket->setProperty("handled", true);
                        if (!socketRedirect.isEmpty()) {
                            socket->readAll();
                            socket->write("HTTP/1.1 302 Found\r\nContent-Length: 0\r\nConnection: close\r\nLocation: "
                                + socketRedirect.toEncoded() + "\r\n\r\n");
                            socket->disconnectFromHost();
                            return;
                        }
                        webSocketServer.handleConnection(socket);
                        return;
                    }
                    Request request;
                    const auto lines = bytes.left(end).split('\n');
                    const auto first = lines.first().trimmed().split(' ');
                    request.method = first.value(0);
                    request.target = QUrl::fromEncoded(first.value(1));
                    for (int i = 1; i < lines.size(); ++i) {
                        const auto separator = lines.at(i).indexOf(':');
                        if (separator > 0)
                            request.headers.insert(lines.at(i).left(separator).trimmed().toLower(),
                                lines.at(i).mid(separator + 1).trimmed());
                    }
                    const auto size = request.headers.value("content-length").toInt();
                    if (bytes.size() < end + 4 + size)
                        return;
                    socket->setProperty("handled", true);
                    socket->readAll();
                    request.body = QJsonDocument::fromJson(bytes.mid(end + 4, size)).object();
                    requests.append(request);
                    const auto response = handle(request);
                    QTimer::singleShot(response.delay, socket, [socket, response] {
                        if (socket->state() != QAbstractSocket::ConnectedState)
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
                    });
                });
            }
        });
        connect(&webSocketServer, &QWebSocketServer::newConnection, this, [this] {
            auto *socket = webSocketServer.nextPendingConnection();
            sockets.append(socket);
            connect(socket, &QWebSocket::disconnected, socket, &QObject::deleteLater);
            socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
                {"type", "ready"}, {"epoch_generation", epochGeneration},
                {"epoch_version", QUrlQuery(socket->requestUrl()).queryItemValue("last_epoch_version").toLongLong()}
            }).toJson(QJsonDocument::Compact)));
        });
        connect(&client, &RealtimeClient::snapshotRequired, this, [this] {
            ++snapshots;
            if (autoSnapshot)
                client.acknowledgeSnapshot();
        });
    }

    ~RealtimeFixture() override { client.stop(); }

    QUrl origin() const { return QUrl(QString("http://127.0.0.1:%1").arg(gateway.serverPort())); }

    Response handle(const Request &request)
    {
        if (request.target.path().endsWith("/epoch/"))
            return Response{QJsonObject{{"epoch_version", qint64(baselineEpoch)},
                {"epoch_generation", epochGeneration}, {"current_epoch_version", qint64(baselineEpoch)},
                {"minimum_epoch_version", 1}}, 200, 0, {}};
        if (request.target.path().endsWith("/events/")) {
            if (eventsHandler)
                return eventsHandler(request);
            const QUrlQuery query(request.target);
            const auto after = std::max(query.queryItemValue("epoch_version>").toULongLong(),
                                       query.queryItemValue("page_marker").toULongLong());
            QJsonArray rows;
            bool more = false;
            for (auto it = events.upperBound(after); it != events.end(); ++it) {
                if (rows.size() == 2) { more = true; break; }
                rows.append(it.value());
            }
            Response response{rows, 200, 0, {}};
            if (more)
                response.headers.insert("X-Pagination-Marker", QByteArray::number(rows.last().toObject().value("epoch_version").toInteger()));
            return response;
        }
        auto response = sessionResponse(request);
        if (sessionOverride) {
            if (const auto overridden = sessionOverride(request))
                return *overridden;
        }
        if (request.body.value("grant_type") == "refresh_token") {
            auto token = response.data.toObject();
            token.insert("access_token", QString("synthetic-project-token-%1").arg(++tokenRefreshes));
            response.data = token;
            response.delay = refreshDelay;
        }
        return response;
    }

    bool login()
    {
        session.login(origin(), "synthetic-user", "synthetic-password");
        if (!QTest::qWaitFor([this] { return session.projects().size() == 1; }, 2000))
            return false;
        session.selectProject(projectId);
        return QTest::qWaitFor([this] { return session.ready(); }, 2000);
    }

    void send(const QJsonObject &frame)
    {
        if (!sockets.isEmpty() && sockets.last())
            sockets.last()->sendTextMessage(QString::fromUtf8(QJsonDocument(frame).toJson(QJsonDocument::Compact)));
    }
};

// Issue #376: a stream.created envelope carrying a minimal valid stream DTO,
// so the real NativeController applier can merge an event-sourced channel on
// top of the catalog the snapshot published.
static QJsonObject streamCreatedEvent(quint64 epoch, const QString &streamUuid, const QString &name)
{
    return {{"schema_version", 1}, {"epoch_version", qint64(epoch)},
        {"uuid", QString("99999999-9999-4999-8999-%1").arg(epoch, 12, 16, QLatin1Char('0'))},
        {"project_id", projectId}, {"user_uuid", userId}, {"object_type", "stream"},
        {"action", "created"},
        {"payload", QJsonObject{{"kind", "stream.created"}, {"uuid", streamUuid},
            {"project_id", projectId}, {"name", name}, {"encryption", false}}},
        {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
}

// Issue #376 regression harness: a mock gateway plus a "process" scope that
// binds NativeCacheService, NativeController, RealtimeCursorStore and
// RealtimeClient in exactly the order src/main.cpp does (controller first,
// so its Session::authenticated slot runs before the transport's, then
// useCache, useCursorStore and useRealtime).
class BootstrapGateway final : public QObject {
public:
    QTcpServer gateway;
    QWebSocketServer webSocketServer{"Bootstrap tests", QWebSocketServer::NonSecureMode};
    QVector<Request> requests;
    QVector<QPointer<QWebSocket>> sockets;
    QMap<quint64, QJsonObject> events;
    QJsonArray streamRows{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
        {"name", "Restored channel"}, {"encryption", false}}};
    QString epochGeneration = "generation-a";
    quint64 baselineEpoch = 10;
    int streamsGets = 0;
    int usersGets = 0;
    int epochGets = 0;
    int eventsGets = 0;
    std::function<void()> onStreamsGet;
    // Test-owned snapshot gate: while holdStreams is set, every /streams/
    // response is held until the test calls releaseStreams(), so a snapshot
    // can stay unacknowledged across a reconnect without any sleeps.
    bool holdStreams = false;
    std::vector<std::function<void()>> heldStreamSends;

    void releaseStreams()
    {
        holdStreams = false;
        const auto pending = std::exchange(heldStreamSends, {});
        for (const auto &send : pending)
            send();
    }

    BootstrapGateway()
    {
        gateway.listen(QHostAddress::LocalHost);
        webSocketServer.setSupportedSubprotocols({"workspace.events.v1"});
        connect(&gateway, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = gateway.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    if (socket->property("handled").toBool())
                        return;
                    const auto bytes = socket->peek(socket->bytesAvailable());
                    const auto end = bytes.indexOf("\r\n\r\n");
                    if (end < 0)
                        return;
                    if (bytes.left(end).toLower().contains("upgrade: websocket")) {
                        socket->setProperty("handled", true);
                        webSocketServer.handleConnection(socket);
                        return;
                    }
                    Request request;
                    const auto lines = bytes.left(end).split('\n');
                    const auto first = lines.first().trimmed().split(' ');
                    request.method = first.value(0);
                    request.target = QUrl::fromEncoded(first.value(1));
                    for (int i = 1; i < lines.size(); ++i) {
                        const auto separator = lines.at(i).indexOf(':');
                        if (separator > 0)
                            request.headers.insert(lines.at(i).left(separator).trimmed().toLower(),
                                lines.at(i).mid(separator + 1).trimmed());
                    }
                    const auto size = request.headers.value("content-length").toInt();
                    if (bytes.size() < end + 4 + size)
                        return;
                    socket->setProperty("handled", true);
                    socket->readAll();
                    request.body = QJsonDocument::fromJson(bytes.mid(end + 4, size)).object();
                    requests.append(request);
                    const auto response = handle(request);
                    const auto send = [socket, response] {
                        if (socket->state() != QAbstractSocket::ConnectedState)
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
                    if (response.defer)
                        response.defer(send);
                    else
                        QTimer::singleShot(response.delay, socket, send);
                });
            }
        });
        connect(&webSocketServer, &QWebSocketServer::newConnection, this, [this] {
            auto *socket = webSocketServer.nextPendingConnection();
            sockets.append(socket);
            connect(socket, &QWebSocket::disconnected, socket, &QObject::deleteLater);
            socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
                {"type", "ready"}, {"epoch_generation", epochGeneration},
                {"epoch_version", QUrlQuery(socket->requestUrl()).queryItemValue("last_epoch_version").toLongLong()}
            }).toJson(QJsonDocument::Compact)));
        });
    }

    QUrl origin() const { return QUrl(QString("http://127.0.0.1:%1").arg(gateway.serverPort())); }

    int indexOf(const QString &suffix) const
    {
        for (int index = 0; index < requests.size(); ++index)
            if (requests.at(index).target.path().endsWith(suffix))
                return index;
        return -1;
    }

    Response handle(const Request &request)
    {
        const auto path = request.target.path();
        if (path.endsWith("/epoch/")) {
            ++epochGets;
            return {QJsonObject{{"epoch_version", qint64(baselineEpoch)},
                {"epoch_generation", epochGeneration}, {"current_epoch_version", qint64(baselineEpoch)},
                {"minimum_epoch_version", 1}}, 200, 0, {}};
        }
        if (path.endsWith("/events/")) {
            ++eventsGets;
            const QUrlQuery query(request.target);
            const auto after = std::max(query.queryItemValue("epoch_version>").toULongLong(),
                query.queryItemValue("page_marker").toULongLong());
            QJsonArray rows;
            for (auto it = events.upperBound(after); it != events.end(); ++it)
                rows.append(it.value());
            return {rows, 200, 0, {}};
        }
        if (path.endsWith("/streams/")) {
            ++streamsGets;
            if (onStreamsGet)
                onStreamsGet();
            Response response{streamRows, 200, 0, {}};
            if (holdStreams) {
                response.defer = [this](std::function<void()> send) {
                    heldStreamSends.push_back(std::move(send));
                };
            }
            return response;
        }
        if (path.endsWith("/users/")) {
            ++usersGets;
            return {QJsonArray{}, 200, 0, {}};
        }
        if (path.endsWith("/folders/"))
            return {QJsonArray{}, 200, 0, {}};
        return sessionResponse(request);
    }
};

// One simulated application process: members are declared and wired in
// src/main.cpp order. The controller must be constructed before the client
// so its Session::authenticated slot (which drives resynchronizeRequested)
// runs before the transport's own resume slot.
class BootstrapProcess {
public:
    ApiClient api{nullptr, true};
    Session session{&api};
    NativeCacheService cache;
    NativeController controller;
    RealtimeCursorStore store;
    RealtimeClient client;

    BootstrapProcess(BootstrapGateway &mock, const QString &cacheDirectory,
        const QString &cursorDirectory, bool attachCache)
        : cache(cacheDirectory, NativeCache::Policy{}, true)
        , controller(&api, &session)
        , store(cursorDirectory)
        , client(&api, &session)
    {
        if (attachCache)
            controller.useCache(&cache);
        client.useCursorStore(&store);
        controller.useRealtime(&client);
    }

    bool login(BootstrapGateway &mock)
    {
        controller.login(mock.origin().toString(), "synthetic-user", "synthetic-password", "");
        if (!QTest::qWaitFor([this] { return controller.projects().size() == 1; }, 2000))
            return false;
        controller.openProject(projectId);
        return QTest::qWaitFor([this] { return session.ready(); }, 2000);
    }
};

class RealtimeClientTest final : public QObject {
    Q_OBJECT
private slots:
    void baselineWaitsForSnapshotAndSocketKeepsBearerOutOfUrl()
    {
        RealtimeFixture fixture;
        QVERIFY(fixture.login());
        fixture.client.start();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.snapshots, 1, 1000);
        QCOMPARE(fixture.sockets.size(), 0);
        QVERIFY(std::none_of(fixture.requests.begin(), fixture.requests.end(), [](const Request &r) {
            return r.target.path().endsWith("/events/");
        }));
        fixture.client.acknowledgeSnapshot();
        QTRY_COMPARE(fixture.sockets.size(), 1);
        const auto socket = fixture.sockets.first();
        QVERIFY(socket);
        const auto protocols = socket->request().rawHeader("Sec-WebSocket-Protocol");
        QVERIFY(protocols.contains("workspace.events.v1"));
        QVERIFY(protocols.contains("bearer.synthetic-project-token-1"));
        QVERIFY(!socket->requestUrl().toString().contains("token"));
        QCOMPARE(socket->subprotocol(), QString("workspace.events.v1"));
        QCOMPARE(fixture.client.acknowledgedEpoch(), quint64(10));
    }

    void paginatedSparseEpochsAdvanceOnlyAfterExactAcknowledgement()
    {
        RealtimeFixture fixture;
        fixture.autoSnapshot = true;
        for (quint64 epoch : {12, 20, 21})
            fixture.events.insert(epoch, realtimeEvent(epoch));
        QSignalSpy received(&fixture.client, &RealtimeClient::eventReceived);
        QVERIFY(fixture.login());
        fixture.client.start();
        QTRY_COMPARE(received.size(), 1);
        QCOMPARE(fixture.client.acknowledgedEpoch(), quint64(10));
        QVERIFY(!fixture.client.acknowledge("not-the-pending-event"));
        for (int index = 0; index < 3; ++index) {
            QTRY_COMPARE(received.size(), index + 1);
            const auto event = received.at(index).at(0).toJsonObject();
            QVERIFY(fixture.client.acknowledge(event.value("uuid").toString()));
        }
        QTRY_COMPARE(fixture.sockets.size(), 1);
        QCOMPARE(fixture.client.acknowledgedEpoch(), quint64(21));
        QCOMPARE(received.at(0).at(0).toJsonObject().value("epoch_version").toInt(), 12);
        QCOMPARE(received.at(1).at(0).toJsonObject().value("epoch_version").toInt(), 20);
    }

    void websocketWakeupsBridgeMissingAndDuplicateNotifications()
    {
        RealtimeFixture fixture;
        fixture.autoSnapshot = true;
        QVector<quint64> received;
        connect(&fixture.client, &RealtimeClient::eventReceived, &fixture.client, [&](const QJsonObject &event) {
            received.append(event.value("epoch_version").toInteger());
            fixture.client.acknowledge(event.value("uuid").toString());
        });
        QVERIFY(fixture.login());
        fixture.client.start();
        QTRY_COMPARE(fixture.sockets.size(), 1);
        fixture.events.insert(12, realtimeEvent(12));
        fixture.events.insert(20, realtimeEvent(20));
        fixture.send(realtimeEvent(20));
        fixture.send(realtimeEvent(20));
        fixture.send(realtimeEvent(12));
        QTRY_COMPARE(received, QVector<quint64>({12, 20}));
        fixture.events.insert(29, realtimeEvent(29));
        fixture.client.nudge();
        QTRY_COMPARE(received, QVector<quint64>({12, 20, 29}));
        QCOMPARE(fixture.client.acknowledgedEpoch(), quint64(29));
    }

    void reconnectResumesAcknowledgedCursor()
    {
        RealtimeFixture fixture;
        fixture.autoSnapshot = true;
        connect(&fixture.client, &RealtimeClient::eventReceived, &fixture.client, [&](const QJsonObject &event) {
            fixture.client.acknowledge(event.value("uuid").toString());
        });
        QVERIFY(fixture.login());
        fixture.client.start();
        QTRY_COMPARE(fixture.sockets.size(), 1);
        fixture.events.insert(12, realtimeEvent(12));
        fixture.send(realtimeEvent(12));
        QTRY_COMPARE(fixture.client.acknowledgedEpoch(), quint64(12));
        fixture.sockets.first()->close();
        QTRY_COMPARE(fixture.sockets.size(), 2);
        QCOMPARE(QUrlQuery(fixture.sockets.last()->requestUrl()).queryItemValue("last_epoch_version"), "12");
        QCOMPARE(fixture.snapshots, 1);
    }

    void generationChangeRejectsOldAcknowledgementAndLogoutStopsTransport()
    {
        RealtimeFixture fixture;
        fixture.autoSnapshot = true;
        fixture.events.insert(12, realtimeEvent(12));
        QSignalSpy received(&fixture.client, &RealtimeClient::eventReceived);
        QVERIFY(fixture.login());
        fixture.client.start();
        QTRY_COMPARE(received.size(), 1);
        const auto oldUuid = received.first().first().toJsonObject().value("uuid").toString();
        fixture.events.remove(12);
        fixture.session.requestRefresh();
        QTRY_COMPARE(fixture.tokenRefreshes, 2);
        QTRY_COMPARE(fixture.sockets.size(), 1);
        QVERIFY(!fixture.client.acknowledge(oldUuid));
        QCOMPARE(fixture.client.acknowledgedEpoch(), quint64(10));
        fixture.session.logout();
        QTest::qWait(100);
        const auto requests = fixture.requests.size();
        fixture.client.nudge();
        QTest::qWait(100);
        QCOMPARE(fixture.requests.size(), requests);
        QVERIFY(!fixture.client.acknowledge(oldUuid));
    }

    void prunedRestCursorAndSocketFrameRequireFreshSnapshots()
    {
        RealtimeFixture fixture;
        fixture.autoSnapshot = true;
        bool expireOnce = true;
        fixture.eventsHandler = [&](const Request &) {
            if (expireOnce) {
                expireOnce = false;
                fixture.epochGeneration = "generation-b";
                fixture.baselineEpoch = 30;
                return Response{pruned("generation-b"), 410, 0, {}};
            }
            return Response{QJsonArray{}, 200, 0, {}};
        };
        QVERIFY(fixture.login());
        fixture.client.start();
        QTRY_COMPARE(fixture.snapshots, 2);
        QTRY_COMPARE(fixture.sockets.size(), 1);
        QCOMPARE(fixture.client.epochGeneration(), "generation-b");
        fixture.autoSnapshot = false;
        fixture.epochGeneration = "generation-c";
        fixture.baselineEpoch = 40;
        fixture.send(pruned("generation-c"));
        QTRY_COMPARE(fixture.snapshots, 3);
        QCOMPARE(fixture.client.acknowledgedEpoch(), quint64(40));
        fixture.client.retrySnapshot();
        QCOMPARE(fixture.snapshots, 4);
    }

    void manualResynchronizationPausesDispatchUntilSnapshotAcknowledged()
    {
        RealtimeFixture fixture;
        fixture.autoSnapshot = true;
        QSignalSpy received(&fixture.client, &RealtimeClient::eventReceived);
        QVERIFY(fixture.login());
        fixture.client.start();
        QTRY_COMPARE(fixture.sockets.size(), 1);
        fixture.autoSnapshot = false;
        fixture.baselineEpoch = 20;
        fixture.client.resynchronize();
        QTRY_COMPARE(fixture.snapshots, 2);
        fixture.events.insert(21, realtimeEvent(21));
        fixture.client.nudge();
        QTest::qWait(100);
        QCOMPARE(received.size(), 0);
        fixture.client.acknowledgeSnapshot();
        QTRY_COMPARE(received.size(), 1);
        QCOMPARE(received.first().first().toJsonObject().value("epoch_version").toInt(), 21);
    }

    void foreignOwnerAndMalformedFramesNeverAdvanceCursor()
    {
        RealtimeFixture fixture;
        fixture.autoSnapshot = true;
        QSignalSpy received(&fixture.client, &RealtimeClient::eventReceived);
        QVERIFY(fixture.login());
        fixture.client.start();
        QTRY_COMPARE(fixture.sockets.size(), 1);
        auto foreign = realtimeEvent(12);
        foreign.insert("user_uuid", streamId);
        fixture.send(foreign);
        QTest::qWait(100);
        QCOMPARE(received.size(), 0);
        QCOMPARE(fixture.client.acknowledgedEpoch(), quint64(10));
    }

    void projectTransitionRejectsAnOutstandingAcknowledgement()
    {
        RealtimeFixture fixture;
        fixture.autoSnapshot = true;
        fixture.events.insert(12, realtimeEvent(12));
        QSignalSpy received(&fixture.client, &RealtimeClient::eventReceived);
        QVERIFY(fixture.login());
        fixture.client.start();
        QTRY_COMPARE(received.size(), 1);
        const auto uuid = received.first().first().toJsonObject().value("uuid").toString();
        fixture.refreshDelay = 150;
        fixture.session.selectProject(projectId);
        QVERIFY(!fixture.session.ready());
        QVERIFY(!fixture.client.acknowledge(uuid));
        QCOMPARE(fixture.client.acknowledgedEpoch(), quint64(10));
    }

    void websocketRedirectNeverForwardsBearerToAnotherOrigin()
    {
        RealtimeFixture fixture;
        RealtimeFixture destination;
        fixture.autoSnapshot = true;
        fixture.socketRedirect = destination.origin();
        fixture.socketRedirect.setScheme("ws");
        fixture.socketRedirect.setPath("/api/workspace/v1/events/ws");
        QSignalSpy states(&fixture.client, &RealtimeClient::connectionChanged);
        QVERIFY(fixture.login());
        fixture.client.start();
        QTRY_VERIFY(std::any_of(states.begin(), states.end(), [](const QList<QVariant> &state) {
            return state.first() == "retrying";
        }));
        QTest::qWait(100);
        QCOMPARE(destination.sockets.size(), 0);
        QCOMPARE(destination.requests.size(), 0);
    }

    void stopFencesDelayedRestAndCancelsReconnect()
    {
        RealtimeFixture fixture;
        fixture.autoSnapshot = true;
        fixture.eventsHandler = [](const Request &) {
            return Response{QJsonArray{realtimeEvent(12)}, 200, 200, {}};
        };
        QSignalSpy received(&fixture.client, &RealtimeClient::eventReceived);
        QVERIFY(fixture.login());
        fixture.client.start();
        QTRY_VERIFY(std::any_of(fixture.requests.begin(), fixture.requests.end(), [](const Request &r) {
            return r.target.path().endsWith("/events/");
        }));
        fixture.client.stop();
        const auto count = fixture.requests.size();
        QTest::qWait(300);
        QCOMPARE(received.size(), 0);
        QCOMPARE(fixture.requests.size(), count);
        QCOMPARE(fixture.sockets.size(), 0);
        QVERIFY(!fixture.client.acknowledge(realtimeEvent(12).value("uuid").toString()));
    }

    void forbiddenRestUsesBackoffWithoutRepeatedTokenRenewal()
    {
        RealtimeFixture fixture;
        fixture.autoSnapshot = true;
        fixture.eventsHandler = [](const Request &) { return Response{QJsonObject{}, 403, 0, {}}; };
        QSignalSpy states(&fixture.client, &RealtimeClient::connectionChanged);
        QVERIFY(fixture.login());
        fixture.client.start();
        QTRY_VERIFY(std::any_of(states.begin(), states.end(), [](const QList<QVariant> &state) {
            return state.first() == "retrying";
        }));
        QTest::qWait(100);
        QCOMPARE(fixture.tokenRefreshes, 1);
        QCOMPARE(std::count_if(fixture.requests.begin(), fixture.requests.end(), [](const Request &r) {
            return r.target.path().endsWith("/events/");
        }), 1);
    }

    void malformedRestEnvelopeNeverDispatchesOrAdvances()
    {
        RealtimeFixture fixture;
        fixture.autoSnapshot = true;
        fixture.eventsHandler = [](const Request &) {
            auto event = realtimeEvent(12);
            event.insert("project_id", streamId);
            return Response{QJsonArray{event}, 200, 0, {}};
        };
        QSignalSpy received(&fixture.client, &RealtimeClient::eventReceived);
        QSignalSpy states(&fixture.client, &RealtimeClient::connectionChanged);
        QVERIFY(fixture.login());
        fixture.client.start();
        QTRY_VERIFY(std::any_of(states.begin(), states.end(), [](const QList<QVariant> &state) {
            return state.first() == "invalid_data";
        }));
        QCOMPARE(received.size(), 0);
        QCOMPARE(fixture.client.acknowledgedEpoch(), quint64(10));
    }

    void socketAuthenticationCloseRenewsAndResumesSameCursor()
    {
        RealtimeFixture fixture;
        fixture.autoSnapshot = true;
        QVERIFY(fixture.login());
        fixture.client.start();
        QTRY_COMPARE(fixture.sockets.size(), 1);
        fixture.sockets.first()->close(QWebSocketProtocol::CloseCode(4401));
        QTRY_COMPARE(fixture.tokenRefreshes, 2);
        QTRY_COMPARE(fixture.sockets.size(), 2);
        QCOMPARE(fixture.client.acknowledgedEpoch(), quint64(10));
        QCOMPARE(fixture.snapshots, 1);
    }

    void unsuccessfulHandshakesUseIncreasingBackoff()
    {
        RealtimeFixture fixture;
        fixture.autoSnapshot = true;
        fixture.socketRedirect = QUrl("ws://127.0.0.1:1/refused");
        QElapsedTimer clock;
        clock.start();
        QVector<qint64> attempts;
        connect(&fixture.client, &RealtimeClient::connectionChanged, &fixture.client, [&](const QString &state) {
            if (state == "connecting")
                attempts.append(clock.elapsed());
        });
        QVERIFY(fixture.login());
        fixture.client.start();
        QTRY_COMPARE_WITH_TIMEOUT(attempts.size(), 3, 6000);
        QVERIFY(attempts.at(1) - attempts.at(0) >= 800);
        QVERIFY(attempts.at(2) - attempts.at(1) >= 1700);
    }

    // Issue #15: durable realtime cursors (docs/NATIVE_DESKTOP_DURABLE_REALTIME_CURSORS.md)

    void persistedCursorAdvancesAndFreshClientResumesFromIt()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        RealtimeCursorStore store(directory.path());
        RealtimeFixture fixture;
        fixture.autoSnapshot = true;
        fixture.client.useCursorStore(&store);
        connect(&fixture.client, &RealtimeClient::eventReceived, &fixture.client, [&](const QJsonObject &event) {
            fixture.client.acknowledge(event.value("uuid").toString());
        });
        QVERIFY(fixture.login());
        fixture.client.start();
        QTRY_COMPARE(fixture.sockets.size(), 1);
        fixture.events.insert(12, realtimeEvent(12));
        fixture.send(realtimeEvent(12));
        QTRY_COMPARE(fixture.client.acknowledgedEpoch(), quint64(12));
        const auto key = RealtimeCursorStore::ownerKey(fixture.api.server(), projectId, userId);
        // The coalesced write lands within the throttle window after the ack.
        QTRY_VERIFY_WITH_TIMEOUT(store.read(key).has_value(), 2000);
        const auto persisted = store.read(key);
        QVERIFY(persisted.has_value());
        QCOMPARE(persisted->generation, QString("generation-a"));
        QCOMPARE(persisted->epoch, quint64(12));
        // A fresh instance (the process-restart case) restores the durable
        // cursor, and since issue #376 the restore requires one authorized
        // snapshot before the transport delivers anything; the socket then
        // still continues at the persisted epoch.
        fixture.client.stop();
        RealtimeClient resumed(&fixture.api, &fixture.session);
        resumed.useCursorStore(&store);
        QSignalSpy restoredSnapshots(&resumed, &RealtimeClient::snapshotRequired);
        resumed.start();
        QTRY_COMPARE_WITH_TIMEOUT(restoredSnapshots.size(), 1, 3000);
        resumed.acknowledgeSnapshot();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.sockets.size(), 2, 3000);
        const auto socket = fixture.sockets.last();
        QVERIFY(socket);
        const QUrlQuery cursor(socket->requestUrl());
        QCOMPARE(cursor.queryItemValue("last_epoch_version"), QString("12"));
        QCOMPARE(cursor.queryItemValue("epoch_generation"), QString("generation-a"));
        QCOMPARE(resumed.acknowledgedEpoch(), quint64(12));
        resumed.stop();
    }

    // Issue #376 supersedes the original "no snapshot on persisted restore"
    // premise: the durable row proves the transport position, never model
    // hydration. A fresh-process restore must skip the epoch endpoint
    // round-trip while still requiring exactly one authorized snapshot before
    // catch-up and the socket continue from the persisted position.
    void persistedRestoreRequiresSnapshotWithoutEpochRoundTrip()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        RealtimeCursorStore store(directory.path());
        RealtimeFixture fixture;
        fixture.autoSnapshot = true;
        fixture.client.useCursorStore(&store);
        connect(&fixture.client, &RealtimeClient::eventReceived, &fixture.client, [&](const QJsonObject &event) {
            fixture.client.acknowledge(event.value("uuid").toString());
        });
        QVERIFY(fixture.login());
        fixture.client.start();
        QTRY_COMPARE(fixture.sockets.size(), 1);
        fixture.events.insert(12, realtimeEvent(12));
        fixture.send(realtimeEvent(12));
        QTRY_COMPARE(fixture.client.acknowledgedEpoch(), quint64(12));
        const auto key = RealtimeCursorStore::ownerKey(fixture.api.server(), projectId, userId);
        QTRY_VERIFY_WITH_TIMEOUT(store.read(key).has_value(), 2000);
        fixture.client.stop();
        const auto epochRequestsBefore = std::count_if(fixture.requests.cbegin(), fixture.requests.cend(),
            [](const Request &request) { return request.target.path().endsWith("/epoch/"); });
        const auto eventsRequestsBefore = std::count_if(fixture.requests.cbegin(), fixture.requests.cend(),
            [](const Request &request) { return request.target.path().endsWith("/events/"); });
        RealtimeClient resumed(&fixture.api, &fixture.session);
        resumed.useCursorStore(&store);
        QSignalSpy snapshots(&resumed, &RealtimeClient::snapshotRequired);
        resumed.start();
        QTRY_COMPARE_WITH_TIMEOUT(snapshots.size(), 1, 3000);
        // No epoch endpoint round-trip: the restored position replaces it, and
        // the snapshot gate held catch-up back until the acknowledgement.
        QCOMPARE(std::count_if(fixture.requests.cbegin(), fixture.requests.cend(),
                   [](const Request &request) { return request.target.path().endsWith("/epoch/"); }),
                 epochRequestsBefore);
        QCOMPARE(std::count_if(fixture.requests.cbegin(), fixture.requests.cend(),
                   [](const Request &request) { return request.target.path().endsWith("/events/"); }),
                 eventsRequestsBefore);
        resumed.acknowledgeSnapshot();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.sockets.size(), 2, 3000);
        const auto lastEvents = std::find_if(fixture.requests.crbegin(), fixture.requests.crend(),
            [](const Request &request) { return request.target.path().endsWith("/events/"); });
        QVERIFY(lastEvents != fixture.requests.crend());
        QCOMPARE(QUrlQuery(lastEvents->target).queryItemValue("epoch_version>"), QString("12"));
        const QUrlQuery cursor(fixture.sockets.last()->requestUrl());
        QCOMPARE(cursor.queryItemValue("last_epoch_version"), QString("12"));
        resumed.stop();
    }

    void cursorExpiredAfterRestartResynchronizesAndReplacesPersistedCursor()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        RealtimeCursorStore store(directory.path());
        RealtimeFixture fixture;
        fixture.autoSnapshot = true;
        fixture.client.useCursorStore(&store);
        connect(&fixture.client, &RealtimeClient::eventReceived, &fixture.client, [&](const QJsonObject &event) {
            fixture.client.acknowledge(event.value("uuid").toString());
        });
        QVERIFY(fixture.login());
        fixture.client.start();
        QTRY_COMPARE(fixture.sockets.size(), 1);
        fixture.events.insert(12, realtimeEvent(12));
        fixture.send(realtimeEvent(12));
        QTRY_COMPARE(fixture.client.acknowledgedEpoch(), quint64(12));
        const auto key = RealtimeCursorStore::ownerKey(fixture.api.server(), projectId, userId);
        QTRY_VERIFY_WITH_TIMEOUT(store.read(key).has_value(), 2000);
        fixture.client.stop();
        // The server pruned generation-a while the client was away: the stale
        // restored cursor is rejected through the existing resynchronize path.
        fixture.epochGeneration = "generation-b";
        fixture.baselineEpoch = 30;
        fixture.events.clear();
        fixture.eventsHandler = [&](const Request &request) {
            if (QUrlQuery(request.target).queryItemValue("epoch_generation") != fixture.epochGeneration)
                return Response{pruned(fixture.epochGeneration), 410, 0, {}};
            return Response{QJsonArray{}, 200, 0, {}};
        };
        RealtimeClient resumed(&fixture.api, &fixture.session);
        resumed.useCursorStore(&store);
        QSignalSpy snapshots(&resumed, &RealtimeClient::snapshotRequired);
        resumed.start();
        // Issue #376: the restore first asks for one snapshot, and the pruned
        // generation only surfaces when catch-up runs above the restored
        // position.
        QTRY_COMPARE_WITH_TIMEOUT(snapshots.size(), 1, 3000);
        resumed.acknowledgeSnapshot();
        // The catch-up answered 410 and resynchronized: the pruned row was
        // deleted before the fresh baseline asked for the second snapshot, so
        // a restart in between now finds no cursor instead of resending the
        // pruned one.
        QTRY_COMPARE_WITH_TIMEOUT(snapshots.size(), 2, 3000);
        QVERIFY(!store.read(key).has_value());
        resumed.acknowledgeSnapshot();
        // The post-resync cursor replaces the persisted one after the fresh
        // snapshot is acknowledged.
        QTRY_VERIFY_WITH_TIMEOUT(store.read(key).has_value(), 2000);
        const auto replaced = store.read(key);
        QVERIFY(replaced.has_value());
        QCOMPARE(replaced->generation, QString("generation-b"));
        QCOMPARE(replaced->epoch, quint64(30));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.sockets.size(), 2, 3000);
        const QUrlQuery cursor(fixture.sockets.last()->requestUrl());
        QCOMPARE(cursor.queryItemValue("last_epoch_version"), QString("30"));
        QCOMPARE(cursor.queryItemValue("epoch_generation"), QString("generation-b"));
        resumed.stop();
    }

    void signOutClearsPersistedCursorAndNextConnectStartsClean()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        RealtimeCursorStore store(directory.path());
        RealtimeFixture fixture;
        fixture.autoSnapshot = true;
        fixture.client.useCursorStore(&store);
        connect(&fixture.client, &RealtimeClient::eventReceived, &fixture.client, [&](const QJsonObject &event) {
            fixture.client.acknowledge(event.value("uuid").toString());
        });
        QVERIFY(fixture.login());
        fixture.client.start();
        QTRY_COMPARE(fixture.sockets.size(), 1);
        fixture.events.insert(12, realtimeEvent(12));
        fixture.send(realtimeEvent(12));
        QTRY_COMPARE(fixture.client.acknowledgedEpoch(), quint64(12));
        const auto key = RealtimeCursorStore::ownerKey(fixture.api.server(), projectId, userId);
        QTRY_VERIFY_WITH_TIMEOUT(store.read(key).has_value(), 2000);
        fixture.events.clear();
        fixture.session.logout();
        // The signed-out owner's position must not survive anywhere.
        QVERIFY(!store.read(key).has_value());
        // The next connect cannot resume it: fresh baseline plus snapshot,
        // and the socket carries the new baseline position, never 12.
        QVERIFY(fixture.login());
        QTRY_COMPARE(fixture.snapshots, 2);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.sockets.size(), 2, 3000);
        QCOMPARE(fixture.client.acknowledgedEpoch(), quint64(10));
        const QUrlQuery cursor(fixture.sockets.last()->requestUrl());
        QCOMPARE(cursor.queryItemValue("last_epoch_version"), QString("10"));
    }

    void durableCursorStoreIsOwnerScopedMonotonicAndValidating()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        RealtimeCursorStore store(directory.path());
        const auto ownerA = RealtimeCursorStore::ownerKey(QUrl("https://owner-a.test"), projectId, userId);
        const auto ownerB = RealtimeCursorStore::ownerKey(QUrl("https://owner-b.test"), projectId, userId);
        QVERIFY(ownerA != ownerB);
        QVERIFY(store.save(ownerA, {QStringLiteral("generation-a"), 7}));
        QVERIFY(store.save(ownerB, {QStringLiteral("generation-b"), 9}));
        const auto fromA = store.read(ownerA);
        QVERIFY(fromA.has_value());
        QCOMPARE(fromA->generation, QString("generation-a"));
        QCOMPARE(fromA->epoch, quint64(7));
        // Same-generation writes only ever advance the cursor.
        QVERIFY(store.save(ownerA, {QStringLiteral("generation-a"), 5}));
        QCOMPARE(store.read(ownerA)->epoch, quint64(7));
        QVERIFY(store.save(ownerA, {QStringLiteral("generation-a"), 11}));
        QCOMPARE(store.read(ownerA)->epoch, quint64(11));
        // A replacement generation (post-resync) is accepted after a prune.
        QVERIFY(store.save(ownerA, {QStringLiteral("generation-c"), 2}));
        QCOMPARE(store.read(ownerA)->generation, QString("generation-c"));
        // Wipes stay owner-scoped: clearing A never touches B.
        QVERIFY(store.clear(ownerA));
        QVERIFY(!store.read(ownerA).has_value());
        const auto fromB = store.read(ownerB);
        QVERIFY(fromB.has_value());
        QCOMPARE(fromB->generation, QString("generation-b"));
        // Malformed keys and values never round-trip.
        QVERIFY(!store.read(QStringLiteral("not-an-owner-hash")).has_value());
        QVERIFY(!store.save(QStringLiteral("not-an-owner-hash"), {QStringLiteral("generation-a"), 1}));
        QVERIFY(!store.save(ownerA, {QString(), 1}));
        QVERIFY(!store.save(ownerA, {QString(2000, QLatin1Char('g')), 1}));
        QVERIFY(!store.save(ownerA, {QStringLiteral("generation-d"), 9007199254740992ULL}));
        QVERIFY(!store.read(ownerA).has_value());
    }

    void missingOrCorruptDurableCursorFallsBackToCleanConnect()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto databasePath = QDir(directory.path()).filePath(QStringLiteral("realtime_cursors.sqlite3"));
        const auto owner = RealtimeCursorStore::ownerKey(QUrl("https://owner.test"), projectId, userId);
        {
            RealtimeCursorStore empty(directory.path());
            QVERIFY(!empty.read(owner).has_value());
        }
        // A row tampered into an invalid shape reads back as no cursor and is
        // discarded, so a later connect can re-persist cleanly.
        {
            RealtimeCursorStore store(directory.path());
            QVERIFY(store.save(owner, {QStringLiteral("generation-a"), 12}));
        }
        {
            auto tamper = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("realtime-cursor-tamper"));
            tamper.setDatabaseName(databasePath);
            QVERIFY(tamper.open());
            QSqlQuery query(tamper);
            QVERIFY(query.exec(QStringLiteral("UPDATE realtime_cursors SET epoch_version = CAST('x' AS TEXT)")));
            tamper.close();
            QSqlDatabase::removeDatabase(QStringLiteral("realtime-cursor-tamper"));
        }
        {
            RealtimeCursorStore store(directory.path());
            QVERIFY(!store.read(owner).has_value());
            QVERIFY(store.save(owner, {QStringLiteral("generation-a"), 13}));
            const auto rewritten = store.read(owner);
            QVERIFY(rewritten.has_value());
            QCOMPARE(rewritten->epoch, quint64(13));
        }
        // An unreadable database file never blocks the socket: the client
        // takes the untouched baseline path with no regression.
        QFile::remove(databasePath);
        QFile garbage(databasePath);
        QVERIFY(garbage.open(QIODevice::WriteOnly));
        garbage.write(QByteArray(4096, 'x'));
        garbage.close();
        RealtimeCursorStore corrupted(directory.path());
        QVERIFY(!corrupted.available());
        QVERIFY(!corrupted.read(owner).has_value());
        RealtimeFixture fixture;
        fixture.autoSnapshot = true;
        fixture.client.useCursorStore(&corrupted);
        QVERIFY(fixture.login());
        fixture.client.start();
        QTRY_COMPARE(fixture.snapshots, 1);
        fixture.client.acknowledgeSnapshot();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.sockets.size(), 1, 3000);
        QVERIFY(fixture.sockets.first()->requestUrl().toString().contains("last_epoch_version=10"));
        QCOMPARE(fixture.client.acknowledgedEpoch(), quint64(10));
    }

    // Review #359 findings 4584028530/4584028572: a durable store that stays
    // readable but rejects writes must not trap expiry recovery: the pruned
    // owner stays fenced from durable reads until the delete actually lands,
    // and the delete is retried once storage recovers.

    void failedExpiryClearFencesDurableReadAndRetriesUntilStorageRecovers()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        RealtimeCursorStore store(directory.path());
        RealtimeFixture fixture;
        fixture.autoSnapshot = true;
        fixture.client.useCursorStore(&store);
        connect(&fixture.client, &RealtimeClient::eventReceived, &fixture.client, [&](const QJsonObject &event) {
            fixture.client.acknowledge(event.value("uuid").toString());
        });
        QVERIFY(fixture.login());
        fixture.client.start();
        QTRY_COMPARE(fixture.sockets.size(), 1);
        fixture.events.insert(12, realtimeEvent(12));
        fixture.send(realtimeEvent(12));
        QTRY_COMPARE(fixture.client.acknowledgedEpoch(), quint64(12));
        const auto key = RealtimeCursorStore::ownerKey(fixture.api.server(), projectId, userId);
        QTRY_VERIFY_WITH_TIMEOUT(store.read(key).has_value(), 2000);
        fixture.client.stop();
        // The server pruned generation-a while the client was away, and the
        // store now rejects every write and delete (readable-but-write-
        // rejecting database).
        store.setFaultsForTest(true, true);
        fixture.epochGeneration = "generation-b";
        fixture.baselineEpoch = 30;
        fixture.events.clear();
        fixture.eventsHandler = [&](const Request &request) {
            if (QUrlQuery(request.target).queryItemValue("epoch_generation") != fixture.epochGeneration)
                return Response{pruned(fixture.epochGeneration), 410, 0, {}};
            return Response{QJsonArray{}, 200, 0, {}};
        };
        fixture.client.start();
        // Before the fix: resume rereads the still-present pruned row on
        // every cycle and the client spins on 410 forever, so a fresh
        // baseline snapshot never happens. With the fence the client stops
        // trusting the fenced row and re-baselines exactly once.
        QTRY_COMPARE_WITH_TIMEOUT(fixture.snapshots, 2, 3000);
        // The invalidated row stays fenced while its delete keeps failing.
        QTest::qWait(600);
        QCOMPARE(fixture.snapshots, 2);
        const auto fenced = store.read(key);
        QVERIFY(fenced.has_value());
        QCOMPARE(fenced->generation, QString("generation-a"));
        // Storage recovers: the retried delete and the post-snapshot write
        // replace the pruned row with the fresh baseline cursor.
        store.setFaultsForTest(false, false);
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            const auto current = store.read(key);
            return current.has_value() && current->generation == QString("generation-b");
        })(), 3000);
        fixture.client.stop();
    }

    void coldStartWithUnclearedPrunedRowRebaselinesInsteadOfResurrectingIt()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        RealtimeCursorStore store(directory.path());
        RealtimeFixture fixture;
        fixture.autoSnapshot = true;
        fixture.client.useCursorStore(&store);
        connect(&fixture.client, &RealtimeClient::eventReceived, &fixture.client, [&](const QJsonObject &event) {
            fixture.client.acknowledge(event.value("uuid").toString());
        });
        QVERIFY(fixture.login());
        fixture.client.start();
        QTRY_COMPARE(fixture.sockets.size(), 1);
        fixture.events.insert(12, realtimeEvent(12));
        fixture.send(realtimeEvent(12));
        QTRY_COMPARE(fixture.client.acknowledgedEpoch(), quint64(12));
        const auto key = RealtimeCursorStore::ownerKey(fixture.api.server(), projectId, userId);
        QTRY_VERIFY_WITH_TIMEOUT(store.read(key).has_value(), 2000);
        fixture.client.stop();
        // Restart with a pruned generation and a write-rejecting store: a
        // fresh client must resynchronize to a fresh baseline instead of
        // restoring the invalidated row it intends to discard but cannot.
        store.setFaultsForTest(true, true);
        fixture.epochGeneration = "generation-b";
        fixture.baselineEpoch = 30;
        fixture.events.clear();
        fixture.eventsHandler = [&](const Request &request) {
            if (QUrlQuery(request.target).queryItemValue("epoch_generation") != fixture.epochGeneration)
                return Response{pruned(fixture.epochGeneration), 410, 0, {}};
            return Response{QJsonArray{}, 200, 0, {}};
        };
        RealtimeClient resumed(&fixture.api, &fixture.session);
        resumed.useCursorStore(&store);
        QSignalSpy snapshots(&resumed, &RealtimeClient::snapshotRequired);
        resumed.start();
        QTRY_COMPARE_WITH_TIMEOUT(snapshots.size(), 1, 3000);
        resumed.acknowledgeSnapshot();
        // Issue #376: catch-up over the restored row now hits the 410, and
        // the fenced baseline requests its own second snapshot before catch-up
        // may continue.
        QTRY_COMPARE_WITH_TIMEOUT(snapshots.size(), 2, 3000);
        resumed.acknowledgeSnapshot();
        // Catch-up continues from the fresh baseline only: the request stream
        // settles on generation-b and stops, instead of repeating 410 rounds
        // against the pruned generation-a position forever.
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            const auto lastEvents = std::find_if(fixture.requests.crbegin(), fixture.requests.crend(),
                [](const Request &request) { return request.target.path().endsWith("/events/"); });
            return lastEvents != fixture.requests.crend()
                && QUrlQuery(lastEvents->target).queryItemValue("epoch_generation") == QString("generation-b");
        })(), 3000);
        const auto settledRequests = std::count_if(fixture.requests.cbegin(), fixture.requests.cend(),
            [](const Request &request) { return request.target.path().endsWith("/events/"); });
        QTest::qWait(600);
        QCOMPARE(std::count_if(fixture.requests.cbegin(), fixture.requests.cend(),
                   [](const Request &request) { return request.target.path().endsWith("/events/"); }),
                 settledRequests);
        // A second restart with storage still rejecting writes must also
        // not resurrect the row: the fence is per resume, not per process.
        resumed.stop();
        RealtimeClient again(&fixture.api, &fixture.session);
        again.useCursorStore(&store);
        QSignalSpy againSnapshots(&again, &RealtimeClient::snapshotRequired);
        again.start();
        QTRY_COMPARE_WITH_TIMEOUT(againSnapshots.size(), 1, 3000);
        again.stop();
        // Storage recovers: the retry clears the pruned row for good.
        store.setFaultsForTest(false, false);
        fixture.client.start();
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            const auto current = store.read(key);
            return !current.has_value() || current->generation != QString("generation-a");
        })(), 3000);
        fixture.client.stop();
    }

    // Review #359 finding 4584028540: a transient durable save failure must
    // keep the dirty cursor and retry while the owner stays current, instead
    // of silently dropping the pending cursor until the next event.

    void failedCursorSaveIsRetainedAndRetriedWhileOwnerStaysCurrent()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        RealtimeCursorStore store(directory.path());
        RealtimeFixture fixture;
        fixture.autoSnapshot = true;
        fixture.client.useCursorStore(&store);
        connect(&fixture.client, &RealtimeClient::eventReceived, &fixture.client, [&](const QJsonObject &event) {
            fixture.client.acknowledge(event.value("uuid").toString());
        });
        QVERIFY(fixture.login());
        // The owner key needs the live server URL, so compute it after login.
        const auto key = RealtimeCursorStore::ownerKey(fixture.api.server(), projectId, userId);
        // Storage rejects writes from the first persist onward (deterministic).
        store.setFaultsForTest(true, false);
        fixture.client.start();
        QTRY_COMPARE(fixture.sockets.size(), 1);
        fixture.events.insert(12, realtimeEvent(12));
        fixture.send(realtimeEvent(12));
        QTRY_COMPARE(fixture.client.acknowledgedEpoch(), quint64(12));
        // The stream keeps serving while storage is broken, and nothing lands.
        QTest::qWait(900);
        QVERIFY(!store.read(key).has_value());
        // Storage recovers while the stream is idle: the retained cursor is
        // retried without any new event arriving.
        store.setFaultsForTest(false, false);
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            const auto current = store.read(key);
            return current.has_value() && current->generation == QString("generation-a")
                && current->epoch == quint64(12);
        })(), 3000);
        fixture.client.stop();
    }

    // Review #359 finding 4584028556: the owner-switch wipe must remove the
    // departed owner's in-memory entry along with its durable row, so
    // switching away and back re-baselines instead of resuming from memory.

    void ownerSwitchWipeAlsoRemovesInMemoryCursorAndSwitchBackRebaselines()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        RealtimeCursorStore store(directory.path());
        RealtimeFixture fixture;
        fixture.autoSnapshot = true;
        fixture.client.useCursorStore(&store);
        connect(&fixture.client, &RealtimeClient::eventReceived, &fixture.client, [&](const QJsonObject &event) {
            fixture.client.acknowledge(event.value("uuid").toString());
        });
        const QString secondProjectId = "88888888-8888-4888-8888-888888888888";
        auto currentProject = QString(projectId);
        fixture.sessionOverride = [&](const Request &request) -> std::optional<Response> {
            if (request.target.path() == "/api/core/v1/iam/projects/")
                return Response{QJsonArray{
                    QJsonObject{{"uuid", projectId}, {"name", "Synthetic project"}, {"status", "ACTIVE"}},
                    QJsonObject{{"uuid", secondProjectId}, {"name", "Second project"}, {"status", "ACTIVE"}}},
                    200, 0, {}};
            if (request.target.path().endsWith("/actions/introspect"))
                return Response{QJsonObject{{"user_info", QJsonObject{{"uuid", userId}}},
                    {"project_id", currentProject}, {"otp_verified", true}, {"permissions", QJsonArray{}}},
                    200, 0, {}};
            return std::nullopt;
        };
        fixture.session.login(fixture.origin(), "synthetic-user", "synthetic-password");
        QVERIFY(QTest::qWaitFor([&] { return fixture.session.projects().size() == 2; }, 2000));
        fixture.session.selectProject(projectId);
        QVERIFY(QTest::qWaitFor([&] { return fixture.session.ready(); }, 2000));
        fixture.client.start();
        QTRY_COMPARE(fixture.sockets.size(), 1);
        fixture.events.insert(12, realtimeEvent(12));
        fixture.send(realtimeEvent(12));
        QTRY_COMPARE(fixture.client.acknowledgedEpoch(), quint64(12));
        const auto keyA = RealtimeCursorStore::ownerKey(fixture.api.server(), projectId, userId);
        QTRY_VERIFY_WITH_TIMEOUT(store.read(keyA).has_value(), 2000);
        const auto snapshotsInOwnerA = fixture.snapshots;
        // Switch away to the second owner: the first owner's durable row is
        // wiped and the second owner re-baselines cleanly.
        currentProject = secondProjectId;
        fixture.events.clear();
        fixture.session.selectProject(secondProjectId);
        QVERIFY(QTest::qWaitFor([&] { return fixture.session.ready(); }, 2000));
        QTRY_VERIFY_WITH_TIMEOUT(!store.read(keyA).has_value(), 2000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.snapshots, snapshotsInOwnerA + 1, 3000);
        // Switch back: the wiped owner must be gone from memory too, so the
        // return takes the fresh baseline path instead of resuming the wiped
        // cursor at epoch 12 out of thin air.
        currentProject = projectId;
        fixture.session.selectProject(projectId);
        QVERIFY(QTest::qWaitFor([&] { return fixture.session.ready(); }, 2000));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.snapshots, snapshotsInOwnerA + 2, 3000);
        QCOMPARE(fixture.client.acknowledgedEpoch(), quint64(10));
        QTRY_VERIFY_WITH_TIMEOUT(fixture.sockets.size() >= 2 && fixture.sockets.last()
            && QUrlQuery(fixture.sockets.last()->requestUrl()).queryItemValue("last_epoch_version")
                == QString("10"), 3000);
        fixture.client.stop();
    }

    // Review #359 finding 4584028540 (reopened): a persist that failed while
    // the owner was current must survive a suspend/resume with no further
    // realtime events. The suspended coalescing timer is inert by design, so
    // resume has to re-arm it for the retained dirty cursor.

    void failedCursorSaveIsRetainedAndRetriedAfterSuspendResume()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        RealtimeCursorStore store(directory.path());
        RealtimeFixture fixture;
        fixture.autoSnapshot = true;
        fixture.client.useCursorStore(&store);
        connect(&fixture.client, &RealtimeClient::eventReceived, &fixture.client, [&](const QJsonObject &event) {
            fixture.client.acknowledge(event.value("uuid").toString());
        });
        QVERIFY(fixture.login());
        const auto key = RealtimeCursorStore::ownerKey(fixture.api.server(), projectId, userId);
        store.setFaultsForTest(true, false);
        fixture.client.start();
        QTRY_COMPARE(fixture.sockets.size(), 1);
        fixture.events.insert(12, realtimeEvent(12));
        fixture.send(realtimeEvent(12));
        QTRY_COMPARE(fixture.client.acknowledgedEpoch(), quint64(12));
        // The coalescing flush keeps failing while the owner stays current.
        QTest::qWait(900);
        QVERIFY(!store.read(key).has_value());
        // The client suspends: the teardown flush fails and the re-armed
        // timer fires inertly while suspended. Resuming brings the client
        // back with no further realtime events at all.
        fixture.client.stop();
        QTest::qWait(900);
        QVERIFY(!store.read(key).has_value());
        fixture.client.start();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.sockets.size(), 2, 3000);
        // The resume kept using the retained cursor position.
        QCOMPARE(QUrlQuery(fixture.sockets.last()->requestUrl()).queryItemValue("last_epoch_version"),
                 QString("12"));
        // Storage recovers while the stream stays idle: the retained cursor
        // must be persisted after the resume without any new event.
        store.setFaultsForTest(false, false);
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            const auto current = store.read(key);
            return current.has_value() && current->generation == QString("generation-a")
                && current->epoch == quint64(12);
        })(), 3000);
        fixture.client.stop();
    }

    // Review #359 finding 4585327330: save() also returns true when a
    // higher-epoch row of the same generation survives as a monotonic no-op,
    // so a successful save must not release the invalidation fence until the
    // key verifiably holds the replacement cursor.

    void noopSaveKeepsInvalidationFenceAndStaleRowNeverRestored()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        RealtimeCursorStore store(directory.path());
        RealtimeFixture fixture;
        fixture.autoSnapshot = true;
        fixture.client.useCursorStore(&store);
        connect(&fixture.client, &RealtimeClient::eventReceived, &fixture.client, [&](const QJsonObject &event) {
            fixture.client.acknowledge(event.value("uuid").toString());
        });
        QVERIFY(fixture.login());
        fixture.client.start();
        QTRY_COMPARE(fixture.sockets.size(), 1);
        fixture.events.insert(12, realtimeEvent(12));
        fixture.send(realtimeEvent(12));
        QTRY_COMPARE(fixture.client.acknowledgedEpoch(), quint64(12));
        const auto key = RealtimeCursorStore::ownerKey(fixture.api.server(), projectId, userId);
        QTRY_VERIFY_WITH_TIMEOUT(store.read(key).has_value(), 2000);
        // The server prunes the position inside the same generation (the
        // epoch endpoint answers with generation-a at the lower baseline
        // epoch 10) and every delete fails: the durable row gen-a/12 stays
        // as the invalidated row the client fences out of restore.
        fixture.events.clear();
        store.setFaultsForTest(false, true);
        fixture.client.resynchronize();
        QTRY_COMPARE(fixture.snapshots, 2);
        QCOMPARE(fixture.client.acknowledgedEpoch(), quint64(10));
        // The post-resync save (gen-a/10) is a no-op against the surviving
        // gen-a/12 row: it reports success without replacing anything.
        QTest::qWait(900);
        const auto stale = store.read(key);
        QVERIFY(stale.has_value());
        QCOMPARE(stale->generation, QString("generation-a"));
        QCOMPARE(stale->epoch, quint64(12));
        // Storage recovers. With the fence correctly held the owed delete
        // keeps retrying and lands, so the invalidated future row is gone;
        // releasing the fence on the no-op save dropped the delete instead.
        store.setFaultsForTest(false, false);
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            const auto current = store.read(key);
            return !current.has_value() || current->epoch < quint64(12);
        })(), 3000);
        fixture.client.stop();
        // The next connect can never restore the future-epoch row: the
        // owed delete removed gen-a/12, and the held-dirty replacement
        // landed in its place, so the fresh client resumes the valid
        // gen-a/10 position instead of restoring the pruned one.
        RealtimeClient resumed(&fixture.api, &fixture.session);
        resumed.useCursorStore(&store);
        QSignalSpy snapshots(&resumed, &RealtimeClient::snapshotRequired);
        resumed.start();
        // Issue #376: the fresh process takes over the valid gen-a/10
        // position but still requires one authorized snapshot before catch-up
        // and the socket start from it.
        QTRY_COMPARE_WITH_TIMEOUT(snapshots.size(), 1, 3000);
        resumed.acknowledgeSnapshot();
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            const auto current = store.read(key);
            return current.has_value() && current->generation == QString("generation-a")
                && current->epoch == quint64(10);
        })(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.sockets.size() >= 2 && fixture.sockets.last()
            && QUrlQuery(fixture.sockets.last()->requestUrl()).queryItemValue("last_epoch_version")
                == QString("10"), 3000);
        resumed.stop();
    }

    // Review #359 finding 4585693719: the held fence is right, but a no-op
    // save must not drop the dirty replacement cursor. Once the owed delete
    // recovers and removes the stale row, the replacement has to be written
    // to the now-empty key without any new realtime event arriving; a save
    // that only cleared the dirty flag leaves the replacement unpersisted
    // forever.

    void noopSaveReschedulesReplacementAfterDeleteRecovers()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        RealtimeCursorStore store(directory.path());
        RealtimeFixture fixture;
        fixture.autoSnapshot = true;
        fixture.client.useCursorStore(&store);
        connect(&fixture.client, &RealtimeClient::eventReceived, &fixture.client, [&](const QJsonObject &event) {
            fixture.client.acknowledge(event.value("uuid").toString());
        });
        QVERIFY(fixture.login());
        fixture.client.start();
        QTRY_COMPARE(fixture.sockets.size(), 1);
        fixture.events.insert(12, realtimeEvent(12));
        fixture.send(realtimeEvent(12));
        QTRY_COMPARE(fixture.client.acknowledgedEpoch(), quint64(12));
        const auto key = RealtimeCursorStore::ownerKey(fixture.api.server(), projectId, userId);
        QTRY_VERIFY_WITH_TIMEOUT(store.read(key).has_value(), 2000);
        // Invalidation whose delete fails, and whose replacement cursor
        // (gen-a/10, lower epoch inside the same generation) can only be a
        // monotonic no-op save against the surviving gen-a/12 row.
        fixture.events.clear();
        store.setFaultsForTest(false, true);
        fixture.client.resynchronize();
        QTRY_COMPARE(fixture.snapshots, 2);
        // The fence is held: repeated no-op saves never touch the stale row.
        QTest::qWait(900);
        const auto stale = store.read(key);
        QVERIFY(stale.has_value());
        QCOMPARE(stale->generation, QString("generation-a"));
        QCOMPARE(stale->epoch, quint64(12));
        // Storage recovers: the delete lands, and the replacement cursor
        // must then be re-marked and persisted on the cleared key - no new
        // realtime event is sent for the rest of this test.
        store.setFaultsForTest(false, false);
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            const auto current = store.read(key);
            return current.has_value() && current->generation == QString("generation-a")
                && current->epoch == quint64(10);
        })(), 3000);
        fixture.client.stop();
    }

    // Review #359 finding 4585327340: a second failed owner clear must not
    // overwrite the first owner's pending deletion. Every failed delete
    // retries independently until it lands.

    void secondFailedOwnerClearKeepsFirstOwnerClearPending()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        RealtimeCursorStore store(directory.path());
        RealtimeFixture fixture;
        fixture.autoSnapshot = true;
        fixture.client.useCursorStore(&store);
        connect(&fixture.client, &RealtimeClient::eventReceived, &fixture.client, [&](const QJsonObject &event) {
            fixture.client.acknowledge(event.value("uuid").toString());
        });
        const QString projectB = "88888888-8888-4888-8888-888888888888";
        const QString projectC = "77777777-7777-4777-8777-777777777777";
        auto currentProject = QString(projectId);
        fixture.sessionOverride = [&](const Request &request) -> std::optional<Response> {
            if (request.target.path() == "/api/core/v1/iam/projects/")
                return Response{QJsonArray{
                    QJsonObject{{"uuid", projectId}, {"name", "Project A"}, {"status", "ACTIVE"}},
                    QJsonObject{{"uuid", projectB}, {"name", "Project B"}, {"status", "ACTIVE"}},
                    QJsonObject{{"uuid", projectC}, {"name", "Project C"}, {"status", "ACTIVE"}}},
                    200, 0, {}};
            if (request.target.path().endsWith("/actions/introspect"))
                return Response{QJsonObject{{"user_info", QJsonObject{{"uuid", userId}}},
                    {"project_id", currentProject}, {"otp_verified", true}, {"permissions", QJsonArray{}}},
                    200, 0, {}};
            return std::nullopt;
        };
        fixture.session.login(fixture.origin(), "synthetic-user", "synthetic-password");
        QVERIFY(QTest::qWaitFor([&] { return fixture.session.projects().size() == 3; }, 2000));
        fixture.session.selectProject(projectId);
        QVERIFY(QTest::qWaitFor([&] { return fixture.session.ready(); }, 2000));
        fixture.client.start();
        QTRY_COMPARE(fixture.sockets.size(), 1);
        fixture.events.insert(12, realtimeEvent(12));
        fixture.send(realtimeEvent(12));
        QTRY_COMPARE(fixture.client.acknowledgedEpoch(), quint64(12));
        const auto keyA = RealtimeCursorStore::ownerKey(fixture.api.server(), projectId, userId);
        QTRY_VERIFY_WITH_TIMEOUT(store.read(keyA).has_value(), 2000);
        // Every owner clear now fails while the client walks A -> B -> C: the
        // A delete must stay pending while the B delete also fails.
        fixture.events.clear();
        store.setFaultsForTest(false, true);
        currentProject = projectB;
        fixture.session.selectProject(projectB);
        QVERIFY(QTest::qWaitFor([&] { return fixture.session.ready(); }, 2000));
        QTRY_COMPARE(fixture.snapshots, 2);
        currentProject = projectC;
        fixture.session.selectProject(projectC);
        QVERIFY(QTest::qWaitFor([&] { return fixture.session.ready(); }, 2000));
        QTRY_COMPARE(fixture.snapshots, 3);
        // Storage recovers: both owed deletes land independently, A's was
        // not lost when B's clear failed afterwards.
        store.setFaultsForTest(false, false);
        QTRY_VERIFY_WITH_TIMEOUT(!store.read(keyA).has_value(), 3000);
        // Returning to A therefore re-baselines instead of restoring the
        // row that was supposed to be wiped.
        const auto snapshotsBeforeReturn = fixture.snapshots;
        currentProject = projectId;
        fixture.session.selectProject(projectId);
        QVERIFY(QTest::qWaitFor([&] { return fixture.session.ready(); }, 2000));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.snapshots, snapshotsBeforeReturn + 1, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.sockets.size() >= 4 && fixture.sockets.last()
            && QUrlQuery(fixture.sockets.last()->requestUrl()).queryItemValue("last_epoch_version")
                == QString("10"), 3000);
        fixture.client.stop();
    }

    // Issue #376: restoring a durable cursor must not skip the fresh
    // controller bootstrap. These cases bind the controller, cache, cursor
    // store and transport in src/main.cpp order and require the catalogs to
    // load without a manual refresh, with the history cache empty, disabled
    // or evicted, while catch-up around the initialization is reconciled
    // and never lost.

    void restoredDurableCursorBootstrapsFreshControllerModels()
    {
        QTemporaryDir cursorDirectory;
        QVERIFY(cursorDirectory.isValid());
        BootstrapGateway mock;
        const auto key = RealtimeCursorStore::ownerKey(mock.origin(), projectId, userId);
        {
            RealtimeCursorStore seed(cursorDirectory.path());
            QVERIFY(seed.save(key, {QStringLiteral("generation-a"), 10}));
        }
        QTemporaryDir cacheDirectory;
        QVERIFY(cacheDirectory.isValid());
        BootstrapProcess app(mock, cacheDirectory.path(), cursorDirectory.path(), true);
        QVERIFY(app.login(mock));
        // A fresh controller must load the catalogs with no user
        // intervention: pre-fix the restored cursor went straight to
        // catch-up and /streams/ was never requested.
        QVERIFY(QTest::qWaitFor([&] { return app.controller.streams().size() == 1; }, 4000));
        QVERIFY(mock.streamsGets >= 1);
        QVERIFY(mock.usersGets >= 1);
        // The restored transport position replaces the epoch endpoint
        // round-trip, and catch-up only starts after the snapshot was
        // requested: events can never overtake the catalogs.
        QCOMPARE(mock.epochGets, 0);
        QVERIFY(mock.indexOf("/streams/") >= 0);
        // The socket resumes from the persisted position.
        QVERIFY(QTest::qWaitFor([&] {
            if (mock.sockets.isEmpty() || !mock.sockets.last())
                return false;
            const QUrlQuery cursor(mock.sockets.last()->requestUrl());
            return cursor.queryItemValue("last_epoch_version") == QString("10")
                && cursor.queryItemValue("epoch_generation") == QString("generation-a");
        }, 4000));
        // Catch-up started only after the snapshot was requested: no event
        // can overtake the catalogs it has to be applied on top of.
        QVERIFY(mock.indexOf("/events/") >= 0);
        QVERIFY(mock.indexOf("/streams/") < mock.indexOf("/events/"));
    }

    void restoredDurableCursorReconcilesEventsArrivingDuringInitialLoad()
    {
        QTemporaryDir cursorDirectory;
        QVERIFY(cursorDirectory.isValid());
        BootstrapGateway mock;
        const auto key = RealtimeCursorStore::ownerKey(mock.origin(), projectId, userId);
        {
            RealtimeCursorStore seed(cursorDirectory.path());
            QVERIFY(seed.save(key, {QStringLiteral("generation-a"), 10}));
        }
        // One channel was created after the cursor was persisted, and one
        // more arrives on the server while the snapshot catalog is in
        // flight. Neither may be lost, and the catalog still comes from the
        // snapshot.
        mock.events.insert(11, streamCreatedEvent(11,
            QStringLiteral("99999999-9999-4999-8999-000000000011"), "Channel from catch-up"));
        mock.onStreamsGet = [&] {
            mock.events.insert(12, streamCreatedEvent(12,
                QStringLiteral("99999999-9999-4999-8999-000000000012"), "Channel during load"));
        };
        QTemporaryDir cacheDirectory;
        QVERIFY(cacheDirectory.isValid());
        BootstrapProcess app(mock, cacheDirectory.path(), cursorDirectory.path(), true);
        QVERIFY(app.login(mock));
        QVERIFY(QTest::qWaitFor([&] { return app.controller.streams().size() == 3; }, 4000));
        QVERIFY(mock.streamsGets >= 1);
        QCOMPARE(mock.epochGets, 0);
        QVERIFY(mock.indexOf("/streams/") < mock.indexOf("/events/"));
        // Both catch-up events were acknowledged, so the durable cursor
        // advanced: a restart right here would not replay them again.
        QVERIFY(QTest::qWaitFor([&] {
            const auto current = app.store.read(key);
            return current.has_value() && current->generation == QString("generation-a")
                && current->epoch == quint64(12);
        }, 4000));
    }

    // Findings 4599009714/4599009723: the snapshot requirement of a restored
    // durable cursor is application-model hydration state, not transport
    // state: a suspend()/resume() cycle while the restore-time snapshot is
    // still unacknowledged must keep catch-up blocked until the catalog
    // bootstrap completes, and catch-up must then resume above the
    // restored epoch exactly as without the interruption.
    void reconnectDuringUnacknowledgedRestoreSnapshotKeepsHydrationGate()
    {
        QTemporaryDir cursorDirectory;
        QVERIFY(cursorDirectory.isValid());
        BootstrapGateway mock;
        const auto key = RealtimeCursorStore::ownerKey(mock.origin(), projectId, userId);
        {
            RealtimeCursorStore seed(cursorDirectory.path());
            QVERIFY(seed.save(key, {QStringLiteral("generation-a"), 10}));
        }
        // One channel was created after the cursor was persisted; it may
        // only be applied on top of the snapshot catalog.
        mock.events.insert(11, streamCreatedEvent(11,
            QStringLiteral("99999999-9999-4999-8999-000000000011"), "Channel after reconnect"));
        // Hold every catalog response: the restore-time snapshot stays
        // unacknowledged until the test releases it. No sleeps in this race.
        mock.holdStreams = true;
        QTemporaryDir cacheDirectory;
        QVERIFY(cacheDirectory.isValid());
        BootstrapProcess app(mock, cacheDirectory.path(), cursorDirectory.path(), true);
        QSignalSpy snapshots(&app.client, &RealtimeClient::snapshotRequired);
        QVERIFY(app.login(mock));
        // The durable restore asked for one snapshot and the catalog request
        // is in flight (held), so nothing has been acknowledged yet.
        QVERIFY(QTest::qWaitFor([&] { return mock.streamsGets >= 1 && snapshots.size() >= 1; }, 4000));
        QCOMPARE(mock.epochGets, 0);
        QCOMPARE(mock.eventsGets, 0);
        // A reconnect lands while the snapshot response is still held. The
        // transport must keep the hydration requirement across it.
        app.client.reconnectNow("power_resume");
        QVERIFY2(QTest::qWaitFor([&] { return snapshots.size() >= 2 && mock.streamsGets >= 2; }, 4000),
            "reconnect during the unacknowledged restore snapshot did not keep the snapshot requirement");
        // The reconnect must not have started catch-up ahead of the
        // acknowledgement: no first-sync request and no resumed socket.
        QVERIFY2(mock.eventsGets == 0,
            "catch-up issued a first-sync request before the restore snapshot was acknowledged");
        QVERIFY(mock.sockets.isEmpty());
        // Release the held snapshot: the catalog completes, the controller
        // acknowledges, and catch-up runs above the restored epoch.
        mock.releaseStreams();
        QVERIFY(QTest::qWaitFor([&] { return app.controller.streams().size() == 2; }, 4000));
        const auto lastEvents = std::find_if(mock.requests.crbegin(), mock.requests.crend(),
            [](const Request &request) { return request.target.path().endsWith("/events/"); });
        QVERIFY(lastEvents != mock.requests.crend());
        QCOMPARE(QUrlQuery(lastEvents->target).queryItemValue("epoch_version>"), QString("10"));
        QCOMPARE(QUrlQuery(lastEvents->target).queryItemValue("epoch_generation"), QString("generation-a"));
        QVERIFY(mock.indexOf("/streams/") < mock.indexOf("/events/"));
        // The acknowledged catch-up advanced the durable cursor.
        QVERIFY(QTest::qWaitFor([&] {
            const auto current = app.store.read(key);
            return current.has_value() && current->generation == QString("generation-a")
                && current->epoch == quint64(11);
        }, 4000));
    }

    void restoredDurableCursorBootstrapsWithCacheDisabled()
    {
        QTemporaryDir cursorDirectory;
        QVERIFY(cursorDirectory.isValid());
        BootstrapGateway mock;
        const auto key = RealtimeCursorStore::ownerKey(mock.origin(), projectId, userId);
        {
            RealtimeCursorStore seed(cursorDirectory.path());
            QVERIFY(seed.save(key, {QStringLiteral("generation-a"), 10}));
        }
        QTemporaryDir cacheDirectory;
        QVERIFY(cacheDirectory.isValid());
        // No history cache is attached at all (cache disabled): the
        // snapshot path is pure network and must still run.
        BootstrapProcess app(mock, cacheDirectory.path(), cursorDirectory.path(), false);
        QVERIFY(app.login(mock));
        QVERIFY(QTest::qWaitFor([&] { return app.controller.streams().size() == 1; }, 4000));
        QVERIFY(mock.streamsGets >= 1);
        QCOMPARE(mock.epochGets, 0);
    }

    void restoredDurableCursorBootstrapsAfterCacheEviction()
    {
        QTemporaryDir cursorDirectory;
        QTemporaryDir cacheDirectory;
        QVERIFY(cursorDirectory.isValid());
        QVERIFY(cacheDirectory.isValid());
        BootstrapGateway mock;
        const auto key = RealtimeCursorStore::ownerKey(mock.origin(), projectId, userId);
        {
            BootstrapProcess first(mock, cacheDirectory.path(), cursorDirectory.path(), true);
            QVERIFY(first.login(mock));
            // Baseline run: epoch round-trip plus snapshot, and the
            // acknowledged snapshot persists the cursor.
            QVERIFY(QTest::qWaitFor([&] { return first.controller.streams().size() == 1; }, 4000));
            QCOMPARE(mock.epochGets, 1);
            QVERIFY(QTest::qWaitFor([&] { return first.store.read(key).has_value(); }, 2000));
        }
        // The operating system evicted the whole history cache directory.
        // The cursor database is a separate file and survives - the exact
        // user scenario of this bug: a restart where no replayed events
        // reconstruct the existing catalog.
        const auto priorEpochGets = mock.epochGets;
        for (const auto &entry : QDir(cacheDirectory.path())
                 .entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot)) {
            if (entry.isDir())
                QVERIFY(QDir(entry.absoluteFilePath()).removeRecursively());
            else
                QVERIFY(QFile::remove(entry.absoluteFilePath()));
        }
        {
            BootstrapProcess second(mock, cacheDirectory.path(), cursorDirectory.path(), true);
            QVERIFY(second.login(mock));
            QVERIFY(QTest::qWaitFor([&] { return second.controller.streams().size() == 1; }, 4000));
            // The second process resumed the persisted position (no new
            // epoch round-trip) and still loaded the catalogs.
            QCOMPARE(mock.epochGets, priorEpochGets);
            QVERIFY(mock.streamsGets >= 2);
        }
    }

    void sameProcessStopStartKeepsInMemoryCursorWithoutSnapshot()
    {
        // The counter-guard for #376: the snapshot gate applies to
        // restores from the durable store - the fresh-process connect. A
        // same-process suspend/resume has an already-hydrated consumer and
        // must keep resuming from the in-memory cursor without a snapshot.
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        RealtimeCursorStore store(directory.path());
        RealtimeFixture fixture;
        fixture.autoSnapshot = true;
        fixture.client.useCursorStore(&store);
        connect(&fixture.client, &RealtimeClient::eventReceived, &fixture.client, [&](const QJsonObject &event) {
            fixture.client.acknowledge(event.value("uuid").toString());
        });
        QVERIFY(fixture.login());
        fixture.client.start();
        QTRY_COMPARE(fixture.sockets.size(), 1);
        fixture.events.insert(12, realtimeEvent(12));
        fixture.send(realtimeEvent(12));
        QTRY_COMPARE(fixture.client.acknowledgedEpoch(), quint64(12));
        const auto snapshotsBeforeRestart = fixture.snapshots;
        fixture.client.stop();
        fixture.client.start();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.sockets.size(), 2, 3000);
        QCOMPARE(fixture.snapshots, snapshotsBeforeRestart);
        QCOMPARE(QUrlQuery(fixture.sockets.last()->requestUrl()).queryItemValue("last_epoch_version"),
                 QString("12"));
        fixture.client.stop();
    }
};


QTEST_GUILESS_MAIN(RealtimeClientTest)
#include "realtime-client-test.moc"
