#pragma once

#include "native-controller.h"
#include "native-deeplink.h"
#include "native-chat-crypto.h"
#include "native-chat-key-vault.h"
#include "native-preferences.h"
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QVariantList>
#include <QDateTime>
#include <QNetworkRequest>
#include "file-client.h"
#include <QUrl>
#include <QImage>
#include "realtime-client.h"
#include <QDBusArgument>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QTimer>
#include "../mock-server.h"
#include "calendar-fixtures.h"

#include <QElapsedTimer>
#include <QWindow>
#include <QSystemTrayIcon>
#include <QDeadlineTimer>
#include <QClipboard>
#include <QMimeData>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QSignalSpy>
#include <ctime>
#include <QDBusConnection>
#include <QDBusAbstractAdaptor>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QMetaProperty>
#include <QCryptographicHash>
#include <QFile>
#include <QSemaphore>
#include <QScopeGuard>
#include <QNetworkReply>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <atomic>
#ifdef Q_OS_LINUX
#include <unistd.h>
#endif
#include <QTemporaryDir>
#include <QMap>
#include <QUuid>
#include <QTest>
#include <QUrlQuery>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QSslSocket>
#include <QSslKey>
#include <QSslConfiguration>
#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>
#include <thread>
#if defined(Q_OS_UNIX)
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include "../msvc-compat.h"

using namespace workspace;
using namespace workspace::test;

inline QJsonObject historyMessage(int number)
{
    const auto id = "88888888-8888-4888-8888-" + QString("%1").arg(number, 12, 16, QLatin1Char('0'));
    auto result = message(QString("Message %1").arg(number), id);
    result.insert("created_at", QDateTime::fromString("2026-01-02T10:00:00Z", Qt::ISODate)
                      .addSecs(number).toString(Qt::ISODateWithMs));
    return result;
}

inline Response historyResponse(const QMap<int, QJsonObject> &rows, const Request &request)
{
    const auto marker = QUrlQuery(request.target).queryItemValue("page_marker");
    const int boundary = marker.isEmpty() ? std::numeric_limits<int>::max()
                                         : marker.right(12).toInt(nullptr, 16);
    if (!marker.isEmpty() && !rows.contains(boundary))
        return Response{QJsonObject{}, 404, 0, {}};
    QJsonArray items;
    bool more = false;
    for (auto it = rows.cend(); it != rows.cbegin();) {
        --it;
        if (it.key() >= boundary)
            continue;
        if (items.size() == 50) {
            more = true;
            break;
        }
        items.append(it.value());
    }
    Response response{items, 200, 0, {}};
    if (more)
        response.headers.insert("X-Pagination-Marker", items.last().toObject().value("uuid").toString().toUtf8());
    return response;
}

inline QJsonObject controllerEvent(const QString &kind, QJsonObject payload, int epoch = 12)
{
    payload.insert("kind", kind);
    return {{"schema_version", 1}, {"epoch_version", epoch},
        {"uuid", QString("99999999-9999-4999-8999-%1").arg(epoch, 12, 16, QLatin1Char('0'))},
        {"project_id", projectId}, {"user_uuid", userId}, {"object_type", kind.section('.', 0, 0)},
        {"action", kind.section('.', 1, 1)}, {"payload", payload},
        {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
}

inline QVariantMap modelMessage(const NativeController &controller, const QString &id)
{
    for (const auto &row : controller.messages())
        if (row.toMap().value("id").toString() == id) return row.toMap();
    return {};
}

inline bool flushCache(NativeCacheService &cache)
{
    struct Completion { bool done = false; bool okay = false; };
    const auto state = std::make_shared<Completion>();
    QElapsedTimer elapsed;
    elapsed.start();
    const auto generation = cache.lease().generation;
    cache.flush(&cache, [state](bool okay) { state->okay = okay; state->done = true; });
    // Bug #694: flush is a FIFO barrier, so it completes only after the worker
    // has drained what was queued ahead of it. On a saturated runner (CI disk
    // throttling, co-runner CPU starvation) an in-flight SQLite commit or
    // budget pass can outlast a fixed 2 s wait; both CI occurrences showed a
    // healthy service with the callback simply still pending. The contract is
    // eventual completion: keep the 2 s band as a FLUSH_SLOW telemetry probe
    // and grant a documented 12 s total budget before declaring a failure.
    bool completed = QTest::qWaitFor([state] { return state->done; }, 2000);
    if (!completed)
    {
        qInfo() << "FLUSH_SLOW" << "elapsed" << elapsed.elapsed()
                << "available" << cache.available() << "maintenanceBusy" << cache.maintenanceBusy();
        completed = QTest::qWaitFor([state] { return state->done; }, 10000);
        if (completed)
            qInfo() << "FLUSH_SLOW_COMPLETED" << "elapsed" << elapsed.elapsed();
    }
    if (!completed || !state->okay)
        qInfo() << "FLUSH_DIAGNOSTIC" << "completed" << completed << "done" << state->done
                << "okay" << state->okay << "elapsed" << elapsed.elapsed()
                << "beforeGeneration" << generation << "afterGeneration" << cache.lease().generation
                << "available" << cache.available() << "error" << cache.error()
                << "maintenanceBusy" << cache.maintenanceBusy();
    return completed && state->okay;
}

inline std::optional<NativeCache::Entry> cacheEntry(NativeCacheService &cache,
    const NativeCache::Lease &lease, const NativeCache::Key &key)
{
    if (!flushCache(cache)) return {};
    struct Completion { bool done = false; std::optional<NativeCache::Entry> entry; };
    const auto state = std::make_shared<Completion>();
    cache.get(lease, key, &cache, [state](auto entry) { state->entry = std::move(entry); state->done = true; });
    if (!QTest::qWaitFor([state] { return state->done; }, 2000)) return {};
    return state->entry;
}

// Login wizard organization discovery payload (issue #58), shaped after the
// public messenger server_settings contract realm fields.
inline QJsonObject organizationSettingsDto(const QString &realmName = QStringLiteral("Acme Realm"),
                                           const QString &icon = QStringLiteral("/avatars/realm.png"))
{
    return {{QStringLiteral("result"), QStringLiteral("success")}, {QStringLiteral("msg"), QString{}},
            {QStringLiteral("realm_name"), realmName}, {QStringLiteral("realm_icon"), icon},
            {QStringLiteral("registration_url"), QStringLiteral("https://reg.example/signup")}};
}

// Loopback TLS grant for the durable key vault (same synthetic CA pattern as
// native-e2ee-read-test.cpp): the vault account identity requires an https
// origin (SessionVault::accountId), so the key-management fixture proxies its
// plain-HTTP mock through a local TLS gateway.
inline QString KeyCaCertificatePath, KeyCaPrivateKeyPath;

class LoopbackKeyTls final : public QTcpServer {
public:
    QSslCertificate certificate;
    QSslKey key;
    quint16 upstream;
    explicit LoopbackKeyTls(quint16 port) : upstream(port) {
        QFile cert(KeyCaCertificatePath);
        QFile secret(KeyCaPrivateKeyPath);
        if (cert.open(QIODevice::ReadOnly)) certificate = QSslCertificate(cert.readAll());
        if (secret.open(QIODevice::ReadOnly)) key = QSslKey(secret.readAll(), QSsl::Rsa);
        listen(QHostAddress::LocalHost);
    }
    QUrl origin() const { return QUrl(QString("https://127.0.0.1:%1").arg(serverPort())); }
protected:
    void incomingConnection(qintptr descriptor) override {
        auto *client = new QSslSocket(this);
        if (!client->setSocketDescriptor(descriptor)) { delete client; return; }
        client->setLocalCertificate(certificate); client->setPrivateKey(key);
        connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
        connect(client, &QSslSocket::encrypted, this, [this, client] {
            auto *server = new QTcpSocket(client);
            connect(server, &QTcpSocket::connected, client, [server, client] { server->write(client->readAll()); });
            connect(client, &QTcpSocket::readyRead, server, [server, client] {
                if (server->state() == QAbstractSocket::ConnectedState) server->write(client->readAll());
            });
            connect(server, &QTcpSocket::readyRead, client, [server, client] { client->write(server->readAll()); });
            connect(server, &QTcpSocket::disconnected, client, &QTcpSocket::disconnectFromHost);
            server->connectToHost(QHostAddress::LocalHost, upstream);
        });
        client->startServerEncryption();
    }
};

class Fixture {
public:
    MockServer server;
    ApiClient api{nullptr, true};
    Session session{&api};
    NativeController controller{&api, &session};
    std::function<Response(const Request &)> messages;
    std::function<Response(const Request &)> mentions;
    std::function<Response(const Request &)> users;
    std::function<Response(const Request &)> topics;
    std::function<Response(const Request &)> topicActions;
    std::function<Response(const Request &)> realtime;
    std::function<Response(const Request &)> authentication;
    std::function<Response(const Request &)> files;
    std::function<Response(const Request &)> counters;
    std::function<Response(const Request &)> streams;
    std::function<Response(const Request &)> bindings;
    std::function<Response(const Request &)> reactions;
    std::function<Response(const Request &)> serverSettings;
    std::function<Response(const Request &)> folders;
    std::function<Response(const Request &)> calendar;
    bool extraTopic = false;
    bool encrypted = false;
    bool aggregateMessages = false;
    int aggregateHistoryGets = 0;
    int topicHistoryGets = 0;
    int totalHistoryGets() const { return aggregateHistoryGets + topicHistoryGets; }

    Fixture()
    {
        // Issue #718 slice 2: the background topic-preload sweep would add
        // unbounded GET /messages/ traffic to the exact-request pins that
        // every controller fixture relies on. Product default is ON; the
        // harness keeps fixtures deterministic, and the two sidebar sweep
        // cases opt back in explicitly via setTopicPreloadEnabledForTest.
        controller.setTopicPreloadEnabledForTest(false);
        server.handler = [this](const Request &request) {
            const auto path = request.target.path();
            if (path.contains("/message_reactions/") && reactions)
                return reactions(request);
            if (path.contains("/files/") && files)
                return files(request);
            if (path.endsWith("/actions/toggle_done/invoke") && topicActions)
                return topicActions(request);
            if (path == "/api/workspace/v1/messenger/streams/" + streamId)
                return counters ? counters(request) : Response{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Native channel"}, {"encryption", encrypted}, {"unread_count", 0},
                    {"active_unread_count", 0}, {"passive_unread_count", 0}, {"last_message_uuid", QJsonValue::Null}}};
            if (path == "/api/workspace/v1/messenger/stream_topics/" + topicId || path == "/api/workspace/v1/messenger/stream_topics/" + secondTopicId)
                return counters ? counters(request) : Response{QJsonObject{{"uuid", path.endsWith(secondTopicId) ? secondTopicId : topicId}, {"project_id", projectId},
                    {"stream_uuid", streamId}, {"name", "Native topic"}, {"unread_count", 0},
                    {"active_unread_count", 0}, {"passive_unread_count", 0}, {"last_message_uuid", QJsonValue::Null}}};
            if (path.endsWith("/streams/"))
                return streams ? streams(request) : Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Native channel"}, {"encryption", encrypted}}}};
            if (path.endsWith("/users/"))
                return users ? users(request) : Response{QJsonArray{}};
            if (path.endsWith("/folders/"))
                return folders ? folders(request) : Response{QJsonArray{}};
            if (path == "/api/workspace/v1/calendar/calendars/")
                return Response{calendarList()};
            if (path == "/api/workspace/v1/calendar/events/")
                return calendar ? calendar(request) : Response{QJsonArray{}};
            if (path.endsWith("/stream_bindings/") && bindings)
                return bindings(request);
            if (path.endsWith("/stream_topics/")) {
                if (topics)
                    return topics(request);
                QJsonArray topics{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                    {"stream_uuid", streamId}, {"name", "Native topic"}}};
                if (extraTopic)
                    topics.append(QJsonObject{{"uuid", secondTopicId}, {"project_id", projectId},
                        {"stream_uuid", streamId}, {"name", "Second topic"}});
                return Response{topics};
            }
            if (path.contains("/messages/")) {
                if (request.method == "GET" && path.endsWith("/messages/")) {
                    if (QUrlQuery(request.target).hasQueryItem("mentioned"))
                        return mentions ? mentions(request) : Response{QJsonArray{}};
                    if (QUrlQuery(request.target).hasQueryItem("topic_uuid")) ++topicHistoryGets;
                    else ++aggregateHistoryGets;
                }
                // Existing fixtures describe a topic endpoint. Aggregate cases
                // opt into their separate mixed-topic collection explicitly;
                // legacy history counters measure topic GETs, not stream GETs.
                if (!aggregateMessages && request.method == "GET" && path.endsWith("/messages/")
                    && !QUrlQuery(request.target).hasQueryItem("topic_uuid")) return Response{QJsonArray{}};
                return messages ? messages(request) : Response{QJsonArray{}};
            }
            if (realtime && (path.endsWith("/epoch/") || path.endsWith("/events/")))
                return realtime(request);
            if (path.endsWith("/server_settings"))
                return serverSettings ? serverSettings(request) : Response{organizationSettingsDto()};
            return authentication ? authentication(request) : sessionResponse(request);
        };
    }

    bool start(const QUrl &origin = {}, int expectedStreams = 1)
    {
        controller.login((origin.isEmpty() ? server.origin() : origin).toString(), "synthetic-user", "synthetic-password", "");
        if (!QTest::qWaitFor([this] { return controller.projects().size() == 1; }, 2000))
            return false;
        controller.openProject(projectId);
        if (!QTest::qWaitFor([this, expectedStreams] { return controller.streams().size() == expectedStreams; }, 2000))
            return false;
        controller.selectStream(streamId);
        if (!QTest::qWaitFor([this] { return !controller.topics().isEmpty(); }, 2000))
            return false;
        controller.selectTopic(topicId);
        return QTest::qWaitFor([this] { return !controller.busy(); }, 2000);
    }
};

class NativeControllerTestHarness : public QObject {
    Q_OBJECT
public:
    static Response callMeetSettings(const Request & = {})
    {
        return Response{QJsonObject{{QStringLiteral("result"), QStringLiteral("success")},
            {QStringLiteral("meet_url"), QStringLiteral("https://meet.example.com/x/")}}};
    }

    QTemporaryDir certificateDirectory;
    QSslConfiguration previousSsl{QSslConfiguration::defaultConfiguration()};
private slots:
    void initTestCase()
    {
        QVERIFY(certificateDirectory.isValid());
        KeyCaCertificatePath = certificateDirectory.filePath("key-ca.pem");
        KeyCaPrivateKeyPath = certificateDirectory.filePath("key-ca-key.pem");
        QProcess generate;
        generate.start(QStringLiteral(WORKSPACE_NATIVE_E2EE_OPENSSL_EXECUTABLE), {"req", "-x509", "-newkey", "rsa:2048",
            "-nodes", "-days", "2", "-subj", "/CN=cassi-native-key-loopback",
            "-addext", "subjectAltName=IP:127.0.0.1,DNS:localhost",
            "-addext", "basicConstraints=critical,CA:TRUE", "-keyout", KeyCaPrivateKeyPath, "-out", KeyCaCertificatePath});
        QVERIFY(generate.waitForFinished(15000));
        QCOMPARE(generate.exitCode(), 0);
        QVERIFY(QFile::setPermissions(KeyCaPrivateKeyPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        QFile certificate(KeyCaCertificatePath); QVERIFY(certificate.open(QIODevice::ReadOnly));
        auto trusted = previousSsl;
        auto authorities = trusted.caCertificates();
        authorities.append(QSslCertificate(certificate.readAll()));
        trusted.setCaCertificates(authorities);
        QSslConfiguration::setDefaultConfiguration(trusted);
    }
    void cleanupTestCase() { QSslConfiguration::setDefaultConfiguration(previousSsl); }
};
