#include "realtime-client.h"
#include "mock-server.h"

#include <QJsonDocument>
#include <QSignalSpy>
#include <QTest>
#include <QWebSocket>
#include <QWebSocketServer>

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
};

QTEST_GUILESS_MAIN(RealtimeClientTest)
#include "realtime-client-test.moc"
