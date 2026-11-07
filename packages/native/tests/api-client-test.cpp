#include "api-client.h"

#include <QEventLoop>
#include <QCoreApplication>
#include <QEvent>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QNetworkReply>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>
#include <memory>

using workspace::ApiClient;
using workspace::ApiResult;

namespace {

class HttpFixture final : public QObject {
public:
    HttpFixture()
    {
        server.listen(QHostAddress::LocalHost);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = server.nextPendingConnection()) {
                clients.append(socket);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    auto &buffer = buffers[socket];
                    buffer.append(socket->readAll());
                    const auto headerEnd = buffer.indexOf("\r\n\r\n");
                    if (headerEnd < 0)
                        return;
                    qsizetype contentLength = 0;
                    for (const auto &line : buffer.left(headerEnd).split('\n')) {
                        if (line.toLower().startsWith("content-length:"))
                            contentLength = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
                    }
                    if (buffer.size() < headerEnd + 4 + contentLength)
                        return;
                    requests.append(buffer.left(headerEnd + 4 + contentLength));
                    const auto target = buffer.left(buffer.indexOf("\r\n")).split(' ').value(1);
                    requestSockets.insert(target, socket);
                    buffers.remove(socket);
                    if (!hold)
                        respond(socket);
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QUrl origin() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()));
    }

    void respond(QTcpSocket *socket)
    {
        if (!socket || socket->state() != QAbstractSocket::ConnectedState)
            return;
        QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + " Test\r\n";
        response += "Content-Type: application/json\r\nContent-Length: "
            + QByteArray::number(payload.size()) + "\r\nConnection: close\r\n";
        response += extraHeaders + "\r\n" + payload;
        socket->write(response);
        socket->disconnectFromHost();
    }

    void release()
    {
        hold = false;
        for (const auto &socket : clients)
            respond(socket);
    }

    QTcpServer server;
    QList<QByteArray> requests;
    QHash<QByteArray, QPointer<QTcpSocket>> requestSockets;
    QList<QPointer<QTcpSocket>> clients;
    QHash<QTcpSocket *, QByteArray> buffers;
    bool hold = false;
    int status = 200;
    QByteArray payload = "{}";
    QByteArray extraHeaders;
};

ApiResult runRequest(ApiClient &client, const QByteArray &method = "GET",
                     const QString &path = QStringLiteral("/api/workspace/v1/me/"),
                     const QJsonValue &body = {}, const QUrlQuery &query = {},
                     bool isPublic = false,
                     const QMap<QByteArray, QByteArray> &headers = {})
{
    QEventLoop loop;
    ApiResult result;
    bool completed = false;
    client.request(method, path, body, query,
                   [&](const ApiResult &reply) {
                       result = reply;
                       completed = true;
                       loop.quit();
                   },
                   isPublic, headers);
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(5'000);
    if (!completed)
        loop.exec();
    if (!completed)
        result.error = QStringLiteral("Test request did not complete.");
    return result;
}

QByteArray requestHeader(const QByteArray &request, const QByteArray &name)
{
    for (const auto &line : request.left(request.indexOf("\r\n\r\n")).split('\n')) {
        if (line.left(line.indexOf(':')).toLower() == name.toLower())
            return line.mid(line.indexOf(':') + 1).trimmed();
    }
    return {};
}

} // namespace

class ApiClientTest final : public QObject {
    Q_OBJECT

private slots:
    void requestHandleCancellationAbortsOnlyItsHeldTcpReply()
    {
        HttpFixture fixture;
        fixture.hold = true;
        int cancelledCount = 0, otherCount = 0;
        ApiResult cancelled, other;
        ApiClient client(nullptr, true);
        QVERIFY(client.setServer(fixture.origin()));
        client.setAccessToken("synthetic-token");
        const auto generation = client.generation();
        QSignalSpy sessionChanged(&client, &ApiClient::sessionChanged);
        const auto first = client.request("GET", "/api/held-a", {}, {}, [&](const ApiResult &r) {
            ++cancelledCount; cancelled = r;
        });
        const auto second = client.request("GET", "/api/unrelated-b", {}, {}, [&](const ApiResult &r) {
            ++otherCount; other = r;
        });
        QTRY_COMPARE_WITH_TIMEOUT(fixture.requests.size(), 2, 2000);
        QVERIFY(first.isValid());
        QVERIFY(second.isValid());
        QVERIFY(first != second);
        const auto held = fixture.requestSockets.value("/api/held-a");
        const auto unrelated = fixture.requestSockets.value("/api/unrelated-b");
        QVERIFY(held && unrelated);
        QVERIFY(client.cancelRequest(first));
        QTRY_COMPARE_WITH_TIMEOUT(cancelledCount, 1, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!held || held->state() == QAbstractSocket::UnconnectedState, 2000);
        QCOMPARE(cancelled.status, 0);
        QVERIFY(!cancelled.ok());
        QVERIFY(!cancelled.error.isEmpty());
        QVERIFY(cancelled.data.isUndefined() || cancelled.data.isNull());
        QVERIFY(cancelled.nextMarker.isEmpty());
        QCOMPARE(client.generation(), generation);
        QCOMPARE(sessionChanged.size(), 0);
        QCOMPARE(otherCount, 0);
        QVERIFY(unrelated && unrelated->state() == QAbstractSocket::ConnectedState);
        fixture.respond(unrelated);
        QTRY_COMPARE_WITH_TIMEOUT(otherCount, 1, 2000);
        QVERIFY2(other.ok(), qPrintable(other.error));
        QVERIFY(!client.cancelRequest(first));
        QVERIFY(!client.cancelRequest(second));
        fixture.release();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        QCOMPARE(cancelledCount, 1);
        QCOMPARE(otherCount, 1);
        QCOMPARE(fixture.requests.size(), 2);
    }

    void requestHandleNeverCrossesTransportOrCompletedRequest()
    {
        HttpFixture fixture;
        fixture.hold = true;
        int firstCount = 0, secondCount = 0;
        ApiClient firstClient(nullptr, true), secondClient(nullptr, true);
        QVERIFY(firstClient.setServer(fixture.origin()));
        QVERIFY(secondClient.setServer(fixture.origin()));
        firstClient.setAccessToken("first-synthetic-token");
        secondClient.setAccessToken("second-synthetic-token");
        const auto first = firstClient.request("GET", "/api/first", {}, {}, [&](const ApiResult &r) {
            QVERIFY(r.ok()); ++firstCount;
        });
        const auto second = secondClient.request("GET", "/api/second", {}, {}, [&](const ApiResult &r) {
            QVERIFY(r.ok()); ++secondCount;
        });
        QTRY_COMPARE_WITH_TIMEOUT(fixture.requests.size(), 2, 2000);
        QVERIFY(first.isValid() && second.isValid());
        QVERIFY(first != second);
        QVERIFY(!firstClient.cancelRequest(ApiClient::RequestId{}));
        QVERIFY(!firstClient.cancelRequest(second));
        QVERIFY(!secondClient.cancelRequest(first));
        QCOMPARE(firstCount, 0);
        QCOMPARE(secondCount, 0);
        fixture.release();
        QTRY_COMPARE_WITH_TIMEOUT(firstCount, 1, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(secondCount, 1, 2000);
        QVERIFY(!firstClient.cancelRequest(first));
        QVERIFY(!secondClient.cancelRequest(second));
    }

    void cancelledCallbackMayStartAnotherRequest()
    {
        HttpFixture fixture;
        fixture.hold = true;
        int cancelledCount = 0, replacementCount = 0;
        ApiClient::RequestId first, replacement;
        ApiClient client(nullptr, true);
        QVERIFY(client.setServer(fixture.origin()));
        client.setAccessToken("synthetic-token");
        first = client.request("GET", "/api/first", {}, {}, [&](const ApiResult &r) {
            ++cancelledCount;
            QVERIFY(!r.ok());
            QVERIFY(!client.cancelRequest(first));
            fixture.hold = false;
            replacement = client.request("GET", "/api/reentrant", {}, {}, [&](const ApiResult &next) {
                QVERIFY(next.ok()); ++replacementCount;
            });
        });
        QTRY_COMPARE_WITH_TIMEOUT(fixture.requests.size(), 1, 2000);
        QVERIFY(client.cancelRequest(first));
        QTRY_COMPARE_WITH_TIMEOUT(replacementCount, 1, 2000);
        QCOMPARE(cancelledCount, 1);
        QVERIFY(replacement.isValid() && replacement != first);
        QCOMPARE(fixture.requests.size(), 2);
    }

    void targetedCancelCallbackMayDestroyTransport()
    {
        HttpFixture fixture;
        fixture.hold = true;
        int callbacks = 0;
        QPointer<ApiClient> client = new ApiClient(nullptr, true);
        const auto cleanup = qScopeGuard([&] { delete client.data(); });
        QVERIFY(client->setServer(fixture.origin()));
        client->setAccessToken("synthetic-token");
        const auto id = client->request("GET", "/api/destroy", {}, {}, [&](const ApiResult &r) {
            ++callbacks;
            QVERIFY(!r.ok());
            delete client.data();
        });
        QTRY_COMPARE_WITH_TIMEOUT(fixture.requests.size(), 1, 2000);
        QVERIFY(client->cancelRequest(id));
        QTRY_VERIFY_WITH_TIMEOUT(client.isNull(), 2000);
        QCOMPARE(callbacks, 1);
    }

    void callerCancellationWinsOverBufferedSuccessfulBody()
    {
        HttpFixture fixture;
        fixture.payload = "{\"must_not_publish\":true}";
        fixture.extraHeaders = "X-Pagination-Marker: must-not-publish\r\n";
        int callbacks = 0;
        bool accepted = false;
        ApiResult result;
        ApiClient client(nullptr, true);
        QVERIFY(client.setServer(fixture.origin()));
        client.setAccessToken("synthetic-token");
        const auto id = client.request("GET", "/api/buffered", {}, {}, [&](const ApiResult &r) {
            ++callbacks; result = r;
        });
        const auto replies = client.findChildren<QNetworkReply *>();
        QCOMPARE(replies.size(), 1);
        connect(replies.first(), &QNetworkReply::readyRead, &client, [&] {
            accepted = client.cancelRequest(id);
        });
        QTRY_COMPARE_WITH_TIMEOUT(callbacks, 1, 2000);
        QVERIFY(accepted);
        QCOMPARE(result.status, 0);
        QVERIFY(!result.ok());
        QVERIFY(result.data.isUndefined() || result.data.isNull());
        QVERIFY(result.nextMarker.isEmpty());
        QVERIFY(!client.cancelRequest(id));
    }

    void admissionRejectionHasNoHandleAndKeepsQueuedCallback()
    {
        HttpFixture fixture;
        int callbacks = 0;
        ApiResult result;
        ApiClient client(nullptr, true);
        QVERIFY(client.setServer(fixture.origin()));
        client.setAccessToken("synthetic-token");
        const auto id = client.request("GET", "https://other.example/api", {}, {}, [&](const ApiResult &r) {
            ++callbacks; result = r;
        });
        QVERIFY(!id.isValid());
        QVERIFY(!client.cancelRequest(id));
        QCOMPARE(callbacks, 0);
        QTRY_COMPARE_WITH_TIMEOUT(callbacks, 1, 2000);
        QVERIFY(!result.ok());
        QCOMPARE(result.status, 0);
        QCOMPARE(fixture.requests.size(), 0);
    }

    void tokenRotationRetiresOnlyTheOldGenerationHandle()
    {
        HttpFixture fixture;
        fixture.hold = true;
        int oldCount = 0, freshCount = 0;
        ApiResult oldResult;
        ApiClient client(nullptr, true);
        QVERIFY(client.setServer(fixture.origin()));
        client.setAccessToken("synthetic-old-token");
        const auto old = client.request("GET", "/api/old", {}, {}, [&](const ApiResult &r) {
            ++oldCount; oldResult = r;
        });
        QTRY_COMPARE_WITH_TIMEOUT(fixture.requests.size(), 1, 2000);
        const auto generation = client.generation();
        client.setAccessToken("synthetic-fresh-token");
        QTRY_COMPARE_WITH_TIMEOUT(oldCount, 1, 2000);
        QVERIFY(!oldResult.ok());
        QCOMPARE(oldResult.status, 0);
        QVERIFY(client.generation() > generation);
        QVERIFY(!client.cancelRequest(old));
        const auto fresh = client.request("GET", "/api/fresh", {}, {}, [&](const ApiResult &r) {
            QVERIFY(r.ok()); ++freshCount;
        });
        QTRY_COMPARE_WITH_TIMEOUT(fixture.requests.size(), 2, 2000);
        QVERIFY(old.isValid() && fresh.isValid() && old != fresh);
        QVERIFY(!client.cancelRequest(old));
        fixture.release();
        QTRY_COMPARE_WITH_TIMEOUT(freshCount, 1, 2000);
        QCOMPARE(requestHeader(fixture.requests.last(), "Authorization"), QByteArray("Bearer synthetic-fresh-token"));
    }


    void destroyedTransportHandleCannotCancelItsReplacement()
    {
        HttpFixture fixture;
        fixture.hold = true;
        int oldCallbacks = 0, newCallbacks = 0;
        auto first = std::make_unique<ApiClient>(nullptr, true);
        QVERIFY(first->setServer(fixture.origin()));
        first->setAccessToken("synthetic-first-token");
        const auto old = first->request("GET", "/api/destroyed-client", {}, {},
            [&](const ApiResult &) { ++oldCallbacks; });
        QTRY_COMPARE_WITH_TIMEOUT(fixture.requests.size(), 1, 2000);
        const auto oldSocket = fixture.requestSockets.value("/api/destroyed-client");
        QVERIFY(oldSocket);
        first.reset();
        QTRY_VERIFY_WITH_TIMEOUT(!oldSocket || oldSocket->state() == QAbstractSocket::UnconnectedState, 2000);
        QCOMPARE(oldCallbacks, 0); // Transport destruction deliberately drops its callbacks.
        ApiClient replacement(nullptr, true);
        QVERIFY(replacement.setServer(fixture.origin()));
        replacement.setAccessToken("synthetic-replacement-token");
        const auto fresh = replacement.request("GET", "/api/replacement-client", {}, {},
            [&](const ApiResult &result) { QVERIFY(result.ok()); ++newCallbacks; });
        QTRY_COMPARE_WITH_TIMEOUT(fixture.requests.size(), 2, 2000);
        QVERIFY(old.isValid() && fresh.isValid() && old != fresh);
        QVERIFY(!replacement.cancelRequest(old));
        QCOMPARE(newCallbacks, 0);
        fixture.respond(fixture.requestSockets.value("/api/replacement-client"));
        QTRY_COMPARE_WITH_TIMEOUT(newCallbacks, 1, 2000);
        QVERIFY(!replacement.cancelRequest(fresh));
        QCOMPARE(oldCallbacks, 0);
        QCOMPARE(requestHeader(fixture.requests.last(), "Authorization"), QByteArray("Bearer synthetic-replacement-token"));
    }

    void productionRequiresHttpsOrigin()
    {
        ApiClient production;
        QString error;
        QVERIFY(production.setServer(QUrl(QStringLiteral("https://workspace.example.com/")), &error));
        QVERIFY(error.isEmpty());
        QCOMPARE(production.server().toString(), QStringLiteral("https://workspace.example.com"));
        const auto generation = production.generation();
        const QStringList invalid = {
            QStringLiteral("http://127.0.0.1:8080"), QStringLiteral("https://name@example.com"),
            QStringLiteral("https://example.com/api"), QStringLiteral("https://example.com?"),
            QStringLiteral("https://example.com#"), QStringLiteral("file:///etc/passwd"),
            QStringLiteral("//example.com"), QStringLiteral("https://example.com/?token=secret")};
        for (const auto &origin : invalid) {
            QVERIFY2(!production.setServer(QUrl(origin), &error), qPrintable(origin));
            QVERIFY(!error.isEmpty());
            QCOMPARE(production.generation(), generation);
        }
        ApiClient testClient(nullptr, true);
        QVERIFY(testClient.setServer(QUrl(QStringLiteral("http://127.0.0.1:8080"))));
        QVERIFY(!testClient.setServer(QUrl(QStringLiteral("http://example.com"))));
    }

    void sendsRealBearerJsonHeadersAndCursor()
    {
        HttpFixture fixture;
        QVERIFY(fixture.server.isListening());
        fixture.payload = "{\"uuid\":\"returned-message\"}";
        fixture.extraHeaders = "X-Pagination-Marker: next-page\r\n";
        ApiClient client(nullptr, true);
        QVERIFY(client.setServer(fixture.origin()));
        client.setAccessToken("test-token");
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("epoch_version>"), QStringLiteral("42"));
        query.addQueryItem(QStringLiteral("epoch_generation"), QStringLiteral("generation-a"));
        const QJsonObject body{{QStringLiteral("payload"),
                                QJsonObject{{QStringLiteral("kind"), QStringLiteral("markdown")},
                                            {QStringLiteral("content"), QStringLiteral("Hello")}}}};
        const auto result = runRequest(client, "POST", QStringLiteral("/api/workspace/v1/messenger/messages/"),
                                       body, query, false, {{"If-Match", "\"3\""}});
        QVERIFY2(result.ok(), qPrintable(result.error));
        QCOMPARE(result.status, 200);
        QCOMPARE(result.data.toObject().value(QStringLiteral("uuid")).toString(),
                 QStringLiteral("returned-message"));
        QCOMPARE(result.nextMarker, QByteArray("next-page"));
        QCOMPARE(fixture.requests.size(), 1);
        const auto request = fixture.requests.first();
        QVERIFY(request.startsWith("POST /api/workspace/v1/messenger/messages/?"));
        QVERIFY(request.contains("epoch_version%3E=42"));
        QCOMPARE(requestHeader(request, "Authorization"), QByteArray("Bearer test-token"));
        QCOMPARE(requestHeader(request, "Content-Type"), QByteArray("application/json"));
        QCOMPARE(requestHeader(request, "If-Match"), QByteArray("\"3\""));
        QCOMPARE(QJsonDocument::fromJson(request.mid(request.indexOf("\r\n\r\n") + 4)).object(), body);
    }

    void publicLoginNeverSendsBearerAndSupportsOtp()
    {
        HttpFixture fixture;
        ApiClient client(nullptr, true);
        QVERIFY(client.setServer(fixture.origin()));
        client.setAccessToken("previous-account-token");
        const auto result = runRequest(client, "POST",
                                       QStringLiteral("/api/core/v1/iam/clients/default/actions/get_token/invoke"),
                                       QJsonObject{{QStringLiteral("grant_type"), QStringLiteral("login+password")}},
                                       {}, true, {{"X-OTP", "123456"}});
        QVERIFY2(result.ok(), qPrintable(result.error));
        QCOMPARE(fixture.requests.size(), 1);
        QCOMPARE(requestHeader(fixture.requests.first(), "Authorization"), QByteArray());
        QCOMPARE(requestHeader(fixture.requests.first(), "X-OTP"), QByteArray("123456"));
    }

    void rejectsUnsafePathsAndHeaderOverrides()
    {
        HttpFixture fixture;
        ApiClient client(nullptr, true);
        QVERIFY(client.setServer(fixture.origin()));
        client.setAccessToken("test-token");
        const QStringList paths = {
            QStringLiteral("https://attacker.example/api"), QStringLiteral("//attacker.example/api"),
            QStringLiteral("/api/../credentials"), QStringLiteral("/api/./users"),
            QStringLiteral("/api/%2e%2e/credentials"), QStringLiteral("/api/%252e%252e/credentials"),
            QStringLiteral("/api/%2f%2fattacker.example"), QStringLiteral("/api/\\attacker.example"),
            QStringLiteral("/api/users?access_token=value"), QStringLiteral("/api/users#fragment"),
            QStringLiteral("/api/\r\nInjected: value")};
        for (const auto &path : paths) {
            const auto result = runRequest(client, "GET", path);
            QVERIFY2(!result.ok(), qPrintable(path));
            QCOMPARE(result.status, 0);
        }
        for (const auto &name : {QByteArray("Authorization"), QByteArray("authorization"),
                                 QByteArray("Host"), QByteArray("Cookie"), QByteArray("Content-Length")}) {
            const auto result = runRequest(client, "GET", QStringLiteral("/api/test"), {}, {}, false,
                                           {{name, "override"}});
            QVERIFY(!result.ok());
        }
        QVERIFY(!runRequest(client, "GET", QStringLiteral("/api/test"), {}, {}, false,
                            {{"X-OTP", "123\r\nInjected: value"}}).ok());
        QCOMPARE(fixture.requests.size(), 0);
    }

    void redirectsNeverForwardCredentials()
    {
        HttpFixture source;
        HttpFixture destination;
        source.status = 302;
        source.extraHeaders = "Location: " + destination.origin().toEncoded() + "/api/stolen\r\n";
        ApiClient client(nullptr, true);
        QVERIFY(client.setServer(source.origin()));
        client.setAccessToken("test-token");
        const auto result = runRequest(client, "POST", QStringLiteral("/api/login"),
                                       QJsonObject{{QStringLiteral("login"), QStringLiteral("tester")}}, {}, true);
        QVERIFY(!result.ok());
        QCOMPARE(result.status, 302);
        QCOMPARE(result.error, QStringLiteral("Redirects are not allowed."));
        QTest::qWait(30);
        QCOMPARE(source.requests.size(), 1);
        QCOMPARE(destination.requests.size(), 0);
    }

    void tokenChangeAbortsAndFencesDelayedSuccess()
    {
        HttpFixture fixture;
        fixture.hold = true;
        ApiClient client(nullptr, true);
        QVERIFY(client.setServer(fixture.origin()));
        client.setAccessToken("account-one-token");
        int callbackCount = 0;
        ApiResult result;
        client.request("GET", QStringLiteral("/api/workspace/v1/me/"), {}, {},
                       [&](const ApiResult &reply) { ++callbackCount; result = reply; });
        QTRY_COMPARE(fixture.requests.size(), 1);
        const auto previousGeneration = client.generation();
        client.setAccessToken("account-two-token");
        QVERIFY(client.generation() > previousGeneration);
        QTRY_COMPARE(callbackCount, 1);
        QVERIFY(!result.ok());
        QCOMPARE(result.status, 0);
        QVERIFY(result.data.isNull() || result.data.isUndefined());
        fixture.release();
        QTest::qWait(30);
        QCOMPARE(callbackCount, 1);
    }

    void changingServerClearsCredentialAndLogoutFencesRequests()
    {
        HttpFixture first;
        HttpFixture second;
        first.hold = true;
        ApiClient client(nullptr, true);
        QVERIFY(client.setServer(first.origin()));
        client.setAccessToken("account-one-token");
        int callbackCount = 0;
        ApiResult delayed;
        client.request("GET", QStringLiteral("/api/test"), {}, {},
                       [&](const ApiResult &reply) { ++callbackCount; delayed = reply; });
        QTRY_COMPARE(first.requests.size(), 1);
        QVERIFY(client.setServer(second.origin()));
        QTRY_COMPARE(callbackCount, 1);
        QVERIFY(!delayed.ok());
        const auto missingCredential = runRequest(client);
        QVERIFY(!missingCredential.ok());
        QCOMPARE(second.requests.size(), 0);
        client.setAccessToken("account-two-token");
        second.hold = true;
        callbackCount = 0;
        client.request("GET", QStringLiteral("/api/test"), {}, {},
                       [&](const ApiResult &reply) { ++callbackCount; delayed = reply; });
        QTRY_COMPARE(second.requests.size(), 1);
        client.clearSession();
        QTRY_COMPARE(callbackCount, 1);
        QCOMPARE(delayed.status, 0);
        QVERIFY(!delayed.ok());
    }

    void handlesInvalidJsonSafeErrorsAndNoMutationRetry()
    {
        HttpFixture fixture;
        ApiClient client(nullptr, true);
        QVERIFY(client.setServer(fixture.origin()));
        client.setAccessToken("test-token");
        fixture.payload = "<html>not JSON</html>";
        auto result = runRequest(client);
        QCOMPARE(result.status, 200);
        QVERIFY(!result.ok());
        QCOMPARE(result.error, QStringLiteral("Server returned invalid JSON."));
        fixture.status = 401;
        fixture.payload = "{\"message\":\"secret-server-echo\"}";
        result = runRequest(client);
        QCOMPARE(result.status, 401);
        QVERIFY(!result.ok());
        QVERIFY(!result.error.contains(QStringLiteral("secret-server-echo")));
        fixture.status = 503;
        const auto count = fixture.requests.size();
        result = runRequest(client, "POST", QStringLiteral("/api/messages/"), QJsonObject{});
        QCOMPARE(result.status, 503);
        QVERIFY(!result.ok());
        QTest::qWait(30);
        QCOMPARE(fixture.requests.size(), count + 1);
        fixture.status = 204;
        fixture.payload.clear();
        result = runRequest(client, "DELETE", QStringLiteral("/api/messages/one"));
        QVERIFY2(result.ok(), qPrintable(result.error));
    }

    void capsResponseBuffer()
    {
        HttpFixture fixture;
        fixture.payload = QByteArray(16 * 1024 * 1024 + 1, 'x');
        ApiClient client(nullptr, true);
        QVERIFY(client.setServer(fixture.origin()));
        client.setAccessToken("test-token");
        const auto result = runRequest(client);
        QVERIFY(!result.ok());
        QCOMPARE(result.error, QStringLiteral("Server response exceeds the allowed size."));
        QVERIFY(result.data.isNull() || result.data.isUndefined());
    }
};

QTEST_GUILESS_MAIN(ApiClientTest)
#include "api-client-test.moc"
