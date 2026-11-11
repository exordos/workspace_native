#include "native-controller.h"
#include "native-preferences.h"
#include <QNetworkRequest>
#include "file-client.h"
#include "media-viewer.h"
#include <QUrl>
#include "native-markdown.h"
#include "realtime-client.h"
#include "mock-server.h"

#include <QElapsedTimer>
#include <QClipboard>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QDBusConnection>
#include <QDBusAbstractAdaptor>
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
#include <QTemporaryDir>
#include <QMap>
#include <QTest>
#include <QUrlQuery>
#include <QWebSocket>
#include <QWebSocketServer>
#include <algorithm>
#include <limits>
#include <memory>

using namespace workspace;
using namespace workspace::test;

static QJsonObject historyMessage(int number)
{
    const auto id = "88888888-8888-4888-8888-" + QString("%1").arg(number, 12, 16, QLatin1Char('0'));
    auto result = message(QString("Message %1").arg(number), id);
    result.insert("created_at", QDateTime::fromString("2026-01-02T10:00:00Z", Qt::ISODate)
                      .addSecs(number).toString(Qt::ISODateWithMs));
    return result;
}

static Response historyResponse(const QMap<int, QJsonObject> &rows, const Request &request)
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

static QJsonObject controllerEvent(const QString &kind, QJsonObject payload, int epoch = 12)
{
    payload.insert("kind", kind);
    return {{"schema_version", 1}, {"epoch_version", epoch},
        {"uuid", QString("99999999-9999-4999-8999-%1").arg(epoch, 12, 16, QLatin1Char('0'))},
        {"project_id", projectId}, {"user_uuid", userId}, {"object_type", kind.section('.', 0, 0)},
        {"action", kind.section('.', 1, 1)}, {"payload", payload},
        {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
}

static QVariantMap modelMessage(const NativeController &controller, const QString &id)
{
    for (const auto &row : controller.messages())
        if (row.toMap().value("id").toString() == id) return row.toMap();
    return {};
}

static bool flushCache(NativeCacheService &cache)
{
    struct Completion { bool done = false; bool okay = false; };
    const auto state = std::make_shared<Completion>();
    QElapsedTimer elapsed;
    elapsed.start();
    const auto generation = cache.lease().generation;
    cache.flush(&cache, [state](bool okay) { state->okay = okay; state->done = true; });
    const bool completed = QTest::qWaitFor([state] { return state->done; }, 2000);
    if (!completed || !state->okay)
        qInfo() << "FLUSH_DIAGNOSTIC" << "completed" << completed << "done" << state->done
                << "okay" << state->okay << "elapsed" << elapsed.elapsed()
                << "beforeGeneration" << generation << "afterGeneration" << cache.lease().generation
                << "available" << cache.available() << "error" << cache.error()
                << "maintenanceBusy" << cache.maintenanceBusy();
    return completed && state->okay;
}

static std::optional<NativeCache::Entry> cacheEntry(NativeCacheService &cache,
    const NativeCache::Lease &lease, const NativeCache::Key &key)
{
    if (!flushCache(cache)) return {};
    struct Completion { bool done = false; std::optional<NativeCache::Entry> entry; };
    const auto state = std::make_shared<Completion>();
    cache.get(lease, key, &cache, [state](auto entry) { state->entry = std::move(entry); state->done = true; });
    if (!QTest::qWaitFor([state] { return state->done; }, 2000)) return {};
    return state->entry;
}

class ControllerMemorySecrets final : public SessionSecretStore {
public:
    QMap<QString, QByteArray> values;
    void write(const QString &key, const QByteArray &secret, Completion done) override {
        values.insert(key, secret); QTimer::singleShot(0, this, [done] { done(Error::None); });
    }
    void read(const QString &key, Loaded done) override {
        const auto value = values.value(key);
        QTimer::singleShot(0, this, [done, value] { done(value, value.isEmpty() ? Error::NotFound : Error::None); });
    }
    void remove(const QString &key, Completion done) override {
        values.remove(key); QTimer::singleShot(0, this, [done] { done(Error::None); });
    }
};

class RealtimeGateway final : public QTcpServer {
public:
    QWebSocketServer websocket{"Controller integration", QWebSocketServer::NonSecureMode};
    QVector<QPointer<QWebSocket>> sockets;
    explicit RealtimeGateway(quint16 httpPort)
    {
        listen(QHostAddress::LocalHost);
        websocket.setSupportedSubprotocols({"workspace.events.v1"});
        connect(this, &QTcpServer::newConnection, this, [this, httpPort] {
            while (auto *client = nextPendingConnection()) {
                connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
                connect(client, &QTcpSocket::readyRead, this, [this, httpPort, client] {
                    if (client->property("routed").toBool())
                        return;
                    const auto headers = client->peek(client->bytesAvailable());
                    if (!headers.contains("\r\n\r\n"))
                        return;
                    client->setProperty("routed", true);
                    if (headers.toLower().contains("upgrade: websocket")) {
                        websocket.handleConnection(client);
                        return;
                    }
                    auto *upstream = new QTcpSocket(client);
                    connect(upstream, &QTcpSocket::connected, client, [client, upstream] { upstream->write(client->readAll()); });
                    connect(client, &QTcpSocket::readyRead, upstream, [client, upstream] {
                        if (upstream->state() == QAbstractSocket::ConnectedState)
                            upstream->write(client->readAll());
                    });
                    connect(upstream, &QTcpSocket::readyRead, client, [client, upstream] { client->write(upstream->readAll()); });
                    connect(upstream, &QTcpSocket::disconnected, client, &QTcpSocket::disconnectFromHost);
                    upstream->connectToHost(QHostAddress::LocalHost, httpPort);
                });
            }
        });
        connect(&websocket, &QWebSocketServer::newConnection, this, [this] {
            auto *socket = websocket.nextPendingConnection();
            sockets.append(socket);
            connect(socket, &QWebSocket::disconnected, socket, &QObject::deleteLater);
            socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"type", "ready"},
                {"epoch_generation", "controller-generation"},
                {"epoch_version", QUrlQuery(socket->requestUrl()).queryItemValue("last_epoch_version").toLongLong()}
            }).toJson(QJsonDocument::Compact)));
        });
    }
    QUrl origin() const { return QUrl(QString("http://127.0.0.1:%1").arg(serverPort())); }
};

// Login wizard organization discovery payload (issue #58), shaped after the
// public messenger server_settings contract realm fields.
static QJsonObject organizationSettingsDto(const QString &realmName = QStringLiteral("Acme Realm"),
                                           const QString &icon = QStringLiteral("/avatars/realm.png"))
{
    return {{QStringLiteral("result"), QStringLiteral("success")}, {QStringLiteral("msg"), QString{}},
            {QStringLiteral("realm_name"), realmName}, {QStringLiteral("realm_icon"), icon},
            {QStringLiteral("registration_url"), QStringLiteral("https://reg.example/signup")}};
}

class Fixture {
public:
    MockServer server;
    ApiClient api{nullptr, true};
    Session session{&api};
    NativeController controller{&api, &session};
    std::function<Response(const Request &)> messages;
    std::function<Response(const Request &)> users;
    std::function<Response(const Request &)> topics;
    std::function<Response(const Request &)> topicActions;
    std::function<Response(const Request &)> realtime;
    std::function<Response(const Request &)> authentication;
    std::function<Response(const Request &)> files;
    std::function<Response(const Request &)> counters;
    std::function<Response(const Request &)> streams;
    std::function<Response(const Request &)> reactions;
    std::function<Response(const Request &)> serverSettings;
    bool extraTopic = false;
    bool encrypted = false;
    bool aggregateMessages = false;
    int aggregateHistoryGets = 0;
    int topicHistoryGets = 0;
    int totalHistoryGets() const { return aggregateHistoryGets + topicHistoryGets; }

    Fixture()
    {
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

    bool start(const QUrl &origin = {})
    {
        controller.login((origin.isEmpty() ? server.origin() : origin).toString(), "synthetic-user", "synthetic-password", "");
        if (!QTest::qWaitFor([this] { return controller.projects().size() == 1; }, 2000))
            return false;
        controller.openProject(projectId);
        if (!QTest::qWaitFor([this] { return controller.streams().size() == 1; }, 2000))
            return false;
        controller.selectStream(streamId);
        if (!QTest::qWaitFor([this] { return !controller.topics().isEmpty(); }, 2000))
            return false;
        controller.selectTopic(topicId);
        return QTest::qWaitFor([this] { return !controller.busy(); }, 2000);
    }
};

static QJsonObject creationTopic(const QString &id, const QString &stream, const QString &name)
{
    return {{"uuid", id}, {"project_id", projectId}, {"stream_uuid", stream}, {"user_uuid", userId},
        {"name", name}, {"is_done", false}, {"is_default", false}, {"unread_count", 0},
        {"active_unread_count", 0}, {"passive_unread_count", 0}, {"last_message_uuid", QJsonValue::Null}};
}

class TopicCreationFixture {
public:
    Fixture fixture;
    int posts = 0, confirmations = 0, catalogs = 0;
    int postStatus = 201, getStatus = 200, catalogStatus = 200, postDelay = 0, getDelay = 0, catalogDelay = 0;
    QJsonObject created;
    std::function<void(QJsonObject &)> alterPost, alterGet;
    bool includeCreated = true;
    explicit TopicCreationFixture()
    {
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [this, fallback](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path == "/api/workspace/v1/messenger/stream_topics/" && request.method == "POST") {
                ++posts;
                created = creationTopic(request.body.value("uuid").toString(),
                    request.body.value("stream_uuid").toString(), request.body.value("name").toString());
                auto result = created;
                if (alterPost) alterPost(result);
                return {result, postStatus, postDelay, {}};
            }
            if (!created.isEmpty() && path == "/api/workspace/v1/messenger/stream_topics/" + created.value("uuid").toString()) {
                ++confirmations;
                auto result = created;
                if (alterGet) alterGet(result);
                return {result, getStatus, getDelay, {}};
            }
            if (posts && path == "/api/workspace/v1/messenger/stream_topics/" && request.method == "GET") {
                ++catalogs;
                QJsonArray rows;
                const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
                if (stream == streamId) rows.append(creationTopic(topicId, streamId, "Native topic"));
                if (includeCreated) rows.append(created);
                return {rows, catalogStatus, catalogDelay, {}};
            }
            return fallback(request);
        };
    }
};

static const QString channelMemberA = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
static const QString channelMemberB = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
static const QString channelDefault = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";

static QJsonObject creationChannel(const QString &id, const QString &name = "Created channel")
{
    return {{"uuid", id}, {"project_id", projectId}, {"user_uuid", userId}, {"owner", userId},
        {"name", name}, {"description", ""}, {"role", "owner"}, {"notification_mode", "all_messages"},
        {"source_name", "native"}, {"provider_uuid", QJsonValue::Null}, {"provider_identity", QJsonValue::Null},
        {"invite_only", false}, {"announce", false}, {"private", false}, {"is_archived", false},
        {"encryption", false}, {"current_encryption_key_uuid", QJsonValue::Null}, {"direct_user_uuid", QJsonValue::Null},
        {"default_topic_uuid", channelDefault}, {"unread_count", 0}, {"active_unread_count", 0},
        {"passive_unread_count", 0}, {"last_message_uuid", QJsonValue::Null},
        {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
}

static QJsonObject creationBinding(const QString &user, int number)
{
    return {{"uuid", historyMessage(number).value("uuid")}, {"project_id", projectId},
        {"user_uuid", user}, {"who_uuid", userId}, {"role", "member"}, {"notification_mode", "all_messages"},
        {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
}

class ChannelCreationFixture {
public:
    Fixture fixture;
    QJsonObject created, defaultTopic;
    QJsonArray bindings;
    QVector<QJsonObject> streamBodies, memberBodies;
    int posts = 0, gets = 0, topicGets = 0, streamGets = 0, memberPosts = 0, bindingGets = 0, usersGets = 0;
    int postStatus = 201, getStatus = 200, addStatus = 200, bindingStatus = 200, topicStatus = 200, catalogStatus = 200;
    int postDelay = 0, getDelay = 0, addDelay = 0, bindingDelay = 0, topicDelay = 0, catalogDelay = 0;
    bool includeCreated = true, includeDefault = true, applyMembers = true;
    std::function<void(QJsonObject &)> alterPost, alterGet;
    std::function<void(QJsonArray &)> alterBindings;
    ChannelCreationFixture()
    {
        fixture.users = [](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", userId}, {"username", "self"}},
            QJsonObject{{"uuid", channelMemberA}, {"username", "alice"}, {"first_name", "Alice"},
                {"last_name", "Adams"}, {"email", "alice@example.invalid"}},
            QJsonObject{{"uuid", channelMemberB}, {"username", "bob"}, {"first_name", QJsonValue::Null},
                {"email", QJsonValue::Null}}}}; };
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [this, fallback](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path == "/api/workspace/v1/users/") ++usersGets;
            if (path == "/api/workspace/v1/messenger/streams/" && request.method == "POST") {
                ++posts; streamBodies.append(request.body);
                created = creationChannel(request.body.value("uuid").toString(), request.body.value("name").toString());
                for (const auto *field : {"description", "invite_only", "announce"}) created[field] = request.body.value(field);
                defaultTopic = creationTopic(channelDefault, created.value("uuid").toString(), "General Topic");
                defaultTopic["is_default"] = true; defaultTopic["notification_mode"] = "default";
                defaultTopic["created_at"] = "2026-01-02T10:00:00Z";
                defaultTopic["updated_at"] = "2026-01-02T10:00:00Z";
                auto result = created; if (alterPost) alterPost(result);
                return {result, postStatus, postDelay};
            }
            if (!created.isEmpty() && path == "/api/workspace/v1/messenger/streams/" + created.value("uuid").toString()) {
                ++gets; auto result = created; if (alterGet) alterGet(result); return {result, getStatus, getDelay};
            }
            if (path == "/api/workspace/v1/messenger/streams/" && request.method == "GET") {
                ++streamGets; QJsonArray result{creationChannel(streamId, "Existing channel")};
                if (includeCreated && !created.isEmpty()) result.append(created);
                return {result, catalogStatus, catalogDelay};
            }
            if (!created.isEmpty() && path == "/api/workspace/v1/messenger/stream_topics/"
                && QUrlQuery(request.target).queryItemValue("stream_uuid") == created.value("uuid").toString()) {
                ++topicGets; return {includeDefault ? QJsonArray{defaultTopic} : QJsonArray{}, topicStatus, topicDelay};
            }
            if (!created.isEmpty() && path == "/api/workspace/v1/messenger/streams/"
                + created.value("uuid").toString() + "/actions/add_users/invoke") {
                ++memberPosts; memberBodies.append(request.body);
                QJsonArray result; int n = 100;
                for (const auto &value : request.body.value("member").toArray()) {
                    auto row = creationBinding(value.toString(), n++); row["stream_uuid"] = created.value("uuid"); result.append(row);
                }
                if (applyMembers) bindings = result;
                if (alterBindings) alterBindings(result);
                return {result, addStatus, addDelay};
            }
            if (path == "/api/workspace/v1/messenger/stream_bindings/") {
                ++bindingGets; return {bindings, bindingStatus, bindingDelay};
            }
            return fallback(request);
        };
    }
};

static const QString directId = "8c471e8b-a67b-5167-aeb7-b5ea70d13133";

static QString expectedDirectId(const QString &peer, const QString &viewer = userId, const QString &project = projectId)
{
    QStringList pair{peer.toLower(), viewer.toLower()}; pair.sort();
    return QUuid::createUuidV5(QUuid("26f42131-4503-5f59-8aa6-32b20f551751"),
        (project.toLower() + ":" + pair.join(':')).toUtf8()).toString(QUuid::WithoutBraces);
}

static QJsonObject creationDirect(const QString &peer = channelMemberA, const QString &id = {})
{
    auto row = creationChannel(id.isEmpty() ? expectedDirectId(peer) : id, "Direct");
    row["description"] = "Private workspace"; row["private"] = true; row["direct_user_uuid"] = peer;
    return row;
}

class DirectChatFixture {
public:
    Fixture fixture;
    QJsonObject created, defaultTopic;
    int posts = 0, gets = 0, topicGets = 0, streamGets = 0, usersGets = 0;
    int postStatus = 201, lookupStatus = 0, topicStatus = 200, catalogStatus = 200;
    int postDelay = 0, getDelay = 0, topicDelay = 0, catalogDelay = 0;
    bool applyPost = true, includeDefault = true, includeCreated = true;
    QVector<QJsonObject> bodies;
    std::function<void(QJsonObject &)> alterPost, alterGet;
    DirectChatFixture()
    {
        fixture.users = [](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", userId}, {"username", "self"}},
            QJsonObject{{"uuid", channelMemberA}, {"username", "alice"}, {"first_name", "Alice"},
                {"last_name", "Adams"}, {"email", "alice@example.invalid"}},
            QJsonObject{{"uuid", channelMemberB}, {"username", "bob"}}}}; };
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [this, fallback](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path == "/api/workspace/v1/users/") ++usersGets;
            if (path == "/api/workspace/v1/messenger/streams/" && request.method == "POST") {
                ++posts; bodies.append(request.body);
                auto row = creationDirect(request.body.value("direct_user_uuid").toString());
                if (applyPost) seed(row);
                if (alterPost) alterPost(row);
                return {row, postStatus, postDelay};
            }
            if (request.method == "GET" && (path == "/api/workspace/v1/messenger/streams/" + directId
                || path == "/api/workspace/v1/messenger/streams/" + expectedDirectId(channelMemberB))) {
                ++gets; auto row = created; if (alterGet) alterGet(row);
                return {row, lookupStatus ? lookupStatus : created.isEmpty() ? 404 : 200, getDelay};
            }
            if (path == "/api/workspace/v1/messenger/streams/" && request.method == "GET") {
                ++streamGets; QJsonArray rows{creationChannel(streamId, "Existing channel")};
                if (includeCreated && !created.isEmpty() && (gets || posts)) rows.append(created);
                return {rows, catalogStatus, catalogDelay};
            }
            if (path == "/api/workspace/v1/messenger/stream_topics/"
                && QUrlQuery(request.target).queryItemValue("stream_uuid") == created.value("uuid").toString()) {
                ++topicGets; return {includeDefault ? QJsonArray{defaultTopic} : QJsonArray{}, topicStatus, topicDelay};
            }
            return fallback(request);
        };
    }
    void seed(QJsonObject row = creationDirect())
    {
        created = row;
        defaultTopic = creationTopic(channelDefault, row.value("uuid").toString(), "General Topic");
        defaultTopic["is_default"] = true; defaultTopic["notification_mode"] = "default";
        defaultTopic["created_at"] = "2026-01-02T10:00:00Z";
        defaultTopic["updated_at"] = "2026-01-02T10:00:00Z";
    }
};

static const QString archiveId = "90909090-9090-4090-8090-909090909090";
static const QString otherArchiveId = "91919191-9191-4191-8191-919191919191";
class ArchiveFixture {
public:
    Fixture fixture;
    QJsonObject archived = creationChannel(archiveId, "Archived channel"), other = creationChannel(otherArchiveId, "Other archived channel");
    int posts = 0, gets = 0, postStatus = 200, getStatus = 200, postDelay = 0, getDelay = 0, catalogDelay = 0;
    bool applyPost = true, includeOther = false, closed = false;
    std::function<Response(const Request &)> history;
    std::function<void(QJsonObject &)> postDto, getDto;
    QMap<QString,QJsonObject> additional;
    ArchiveFixture() {
        archived["is_archived"] = true; other["is_archived"] = true;
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [this, fallback](const Request &request) -> Response {
            const auto path = request.target.path(), prefix = QString("/api/workspace/v1/messenger/streams/");
            const auto target=path.mid(prefix.size()).section('/',0,0);
            const bool known=target==archiveId || target==otherArchiveId || additional.contains(target);
            if (path.startsWith(prefix) && known && path.endsWith("/actions/unarchive/invoke")) {
                ++posts; auto &row = target==archiveId ? archived : target==otherArchiveId ? other : additional[target];
                if (applyPost) { row["is_archived"] = false; row["updated_at"] = "2026-01-02T10:01:00Z"; }
                auto result = row; if (postDto) postDto(result); return {result, postStatus, postDelay, {}};
            }
            if (request.method == "GET" && known && path == prefix+target) {
                ++gets; auto row = target==archiveId ? archived : target==otherArchiveId ? other : additional.value(target);
                if (getDto) getDto(row); return {row, getStatus, getDelay, {}};
            }
            if (path == prefix && request.method == "GET") {
                QJsonArray rows{creationChannel(streamId, "Active channel"), archived}; if (includeOther) rows.append(other); for(const auto &row:additional) rows.append(row); return {rows,200,catalogDelay,{}};
            }
            const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
            if (path.endsWith("/stream_topics/") && (stream == archiveId || stream == otherArchiveId)) {
                auto topic = creationTopic(topicId, stream, "Archived topic"); topic["is_done"] = closed; return {QJsonArray{topic}};
            }
            if (path == "/api/workspace/v1/messenger/stream_topics/" + topicId && fixture.controller.selectedStream() == archiveId) {
                auto topic = creationTopic(topicId, archiveId, "Archived topic"); topic["is_done"] = closed; return {topic};
            }
            if (path.endsWith("/messages/") && (stream == archiveId || stream == otherArchiveId)) {
                if (history) return history(request);
                auto row = message(); row["stream_uuid"] = stream; return {QJsonArray{row}};
            }
            return fallback(request);
        };
    }
    bool start() { return fixture.start(); }
};

static QVariantMap archivedChat(const NativeController &controller, const QString &id = archiveId)
{
    for (const auto &row : controller.archivedChats()) if (row.toMap().value("id").toString() == id) return row.toMap();
    return {};
}

static const QString openingStreamId = "abababab-abab-4bab-8bab-abababababab";
static const QString openingRequestedId = "cdcdcdcd-cdcd-4dcd-8dcd-cdcdcdcdcdcd";
static const QString openingCanonicalId = "efefefef-efef-4fef-8fef-efefefefefef";

class TopicOpeningFixture {
public:
    Fixture fixture;
    QJsonObject resolved = creationTopic(openingRequestedId, openingStreamId, "Canonical topic");
    QJsonObject destination = creationChannel(openingStreamId, "Destination channel");
    QJsonArray history;
    bool includeDestination = false, canonicalSeen = false, recordRequests = false;
    int lookupStatus = 200, lookupDelay = 0, detailStatus = 200, detailDelay = 0;
    int topicsStatus = 200, topicsDelay = 0;
    int lookups = 0, details = 0, catalogs = 0, users = 0, mutations = 0;
    int aggregateHistory = 0, topicHistory = 0, targetTopicLists = 0;
    QVector<QUrl> historyTargets;
    QStringList lookupTargets, unexpectedRequests;
    QMap<QString, Response> additionalLookups;
    bool omitResolvedFromList = false;
    std::function<void(QJsonObject &)> alterLookup;

    TopicOpeningFixture()
    {
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [this, fallback](const Request &request) -> Response {
            const auto path = request.target.path();
            if (recordRequests) {
                const bool allowedGet = request.method == "GET"
                    && (path == "/api/workspace/v1/users/"
                        || path == "/api/workspace/v1/messenger/streams/"
                        || path == "/api/workspace/v1/messenger/streams/" + openingStreamId
                        || path == "/api/workspace/v1/messenger/stream_topics/"
                        || path == "/api/workspace/v1/messenger/stream_topics/" + openingRequestedId
                        || additionalLookups.contains(path)
                        || path == "/api/workspace/v1/messenger/messages/");
                if (!allowedGet) unexpectedRequests.append(QString::fromUtf8(request.method)
                    + " " + request.target.toString(QUrl::FullyEncoded));
            }
            if (request.method != "GET") {
                if (path.startsWith("/api/workspace/v1/messenger/")) ++mutations;
                return fallback(request);
            }
            if (additionalLookups.contains(path)) {
                ++lookups;
                lookupTargets.append(path);
                canonicalSeen = true;
                return additionalLookups.value(path);
            }
            if (path == "/api/workspace/v1/messenger/stream_topics/" + openingRequestedId) {
                ++lookups;
                lookupTargets.append(path);
                canonicalSeen = true;
                auto row = resolved;
                if (alterLookup) alterLookup(row);
                return {row, lookupStatus, lookupDelay, {}};
            }
            if (path == "/api/workspace/v1/messenger/streams/" + openingStreamId) {
                ++details;
                return {destination, detailStatus, detailDelay, {}};
            }
            if (path == "/api/workspace/v1/messenger/streams/") {
                ++catalogs;
                QJsonArray rows{creationChannel(streamId, "Origin channel")};
                if (includeDestination) rows.append(destination);
                return {rows};
            }
            if (path == "/api/workspace/v1/users/") ++users;
            if (path == "/api/workspace/v1/messenger/stream_topics/") {
                const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
                QJsonArray rows;
                if (stream == streamId) rows = {creationTopic(topicId, streamId, "Origin topic"),
                    creationTopic(secondTopicId, streamId, "Other known topic")};
                if (canonicalSeen && stream == resolved.value("stream_uuid").toString()) {
                    ++targetTopicLists;
                    for (qsizetype i = rows.size(); i > 0; --i)
                        if (rows.at(i - 1).toObject().value("uuid") == resolved.value("uuid")) rows.removeAt(i - 1);
                    if (!omitResolvedFromList) rows.append(resolved);
                    return {rows, topicsStatus, topicsDelay, {}};
                }
                return {rows};
            }
            if (path == "/api/workspace/v1/messenger/messages/") {
                const QUrlQuery query(request.target);
                historyTargets.append(request.target);
                if (query.hasQueryItem("topic_uuid")) ++topicHistory; else ++aggregateHistory;
                if (canonicalSeen && query.queryItemValue("stream_uuid") == resolved.value("stream_uuid").toString()
                    && query.queryItemValue("topic_uuid") == resolved.value("uuid").toString()) return {history};
                return {QJsonArray{}};
            }
            return fallback(request);
        };
    }

    bool start()
    {
        auto &controller = fixture.controller;
        controller.login(fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        if (!QTest::qWaitFor([&] { return controller.projects().size() == 1; }, 2000)) return false;
        controller.openProject(projectId);
        const int expected = includeDestination && !destination.value("is_archived").toBool() ? 2 : 1;
        if (!QTest::qWaitFor([&] { return controller.streams().size() == expected; }, 2000)) return false;
        controller.selectStream(streamId);
        if (!QTest::qWaitFor([&] { return controller.topics().size() == 2; }, 2000)) return false;
        controller.selectTopic(topicId);
        if (!QTest::qWaitFor([&] { return !controller.busy(); }, 2000)) return false;
        if (!QTest::qWaitFor([&] { return controller.channelMembersVerified(); }, 2000)) return false;
        lookups = details = catalogs = users = mutations = aggregateHistory = topicHistory = targetTopicLists = 0;
        historyTargets.clear();
        lookupTargets.clear();
        unexpectedRequests.clear();
        recordRequests = true;
        return true;
    }

    QPointer<QNetworkReply> openingReply(bool detail = false) const
    {
        const auto path = detail ? "/api/workspace/v1/messenger/streams/" + openingStreamId
                                 : "/api/workspace/v1/messenger/stream_topics/" + openingRequestedId;
        for (auto *reply : fixture.api.findChildren<QNetworkReply *>())
            if (reply->url().path() == path) return reply;
        return {};
    }
};

// Media viewer file slots used by the gallery tests (issue #8).
static const QString mvFileA{"aaaaaaaa-0000-4000-8000-00000000aa01"};
static const QString mvFileB{"bbbbbbbb-0000-4000-8000-00000000bb02"};
static const QString mvFileC{"cccccccc-0000-4000-8000-00000000cc03"};

static QVariantMap mediaViewerRow(const QString &id, const QString &fileUuid,
    const QString &kind = QStringLiteral("markdown"))
{
    const auto body = QStringLiteral("Head\n\n![%1.png](urn:image:%2?name=%1.png&content_type=image%2Fpng&size=3)\n\nTail")
        .arg(id, fileUuid);
    return QVariantMap{{"id", id}, {"kind", kind}, {"content", body}, {"displayContent", body}};
}

// Private additive test fixture only. Actual existing Users HTTP collection and
// canonical existing-direct GET flow; no controller/QML directory injection.
static const QString avatarReferenceA = "urn:image:aaaaaaaa-0000-4000-8000-000000000001";
static const QString avatarReferenceB = "urn:image:bbbbbbbb-0000-4000-8000-000000000002";

static QJsonObject avatarReferencePerson(const QString &id, const QString &username,
                                        const QJsonValue &avatar = QJsonValue(QJsonValue::Undefined))
{
    QJsonObject row{{"uuid", id}, {"project_id", projectId}, {"username", username},
        {"source", "iam"}, {"status", "offline"}, {"first_name", QJsonValue::Null},
        {"last_name", QJsonValue::Null}, {"email", username + "@example.invalid"},
        {"last_ping_at", "2026-01-02T10:00:00Z"}, {"created_at", "2026-01-02T10:00:00Z"},
        {"updated_at", "2026-01-02T10:00:00Z"}};
    if (!avatar.isUndefined()) row.insert("avatar", avatar);
    return row;
}

class AvatarReferenceFixture {
public:
    // Captured data outlives DirectChatFixture's server/controller callbacks.
    QJsonArray directory;
    DirectChatFixture direct;
    explicit AvatarReferenceFixture(const QJsonValue &avatar)
    {
        directory = {avatarReferencePerson(userId, "avatar-self", avatar),
            avatarReferencePerson(channelMemberA, "avatar-peer", avatar),
            avatarReferencePerson(channelMemberB, "other-peer")};
        direct.seed();
        direct.fixture.authentication = [](const Request &request) {
            auto response = sessionResponse(request);
            if (request.target.path().endsWith("/actions/introspect")) {
                auto raw = response.data.toObject(); auto profile = raw.value("user_info").toObject();
                profile["avatar"] = avatarReferenceB; raw["user_info"] = profile; response.data = raw;
            }
            return response;
        }; // Decoy IAM field cannot replace the actual Workspace Users source.
        direct.fixture.users = [this](const Request &) { return Response{directory}; };
        direct.fixture.messages = [](const Request &request) {
            const QUrlQuery query(request.target);
            if (query.queryItemValue("stream_uuid") != directId
                || query.queryItemValue("topic_uuid") != channelDefault) return Response{QJsonArray{}};
            auto own = message("Actual own image-reference witness", messageId, channelDefault);
            own["stream_uuid"] = directId;
            auto peer = message("Actual peer image-reference witness", returnedId, channelDefault);
            peer["stream_uuid"] = directId; peer["author_uuid"] = channelMemberA; peer["is_own"] = false;
            return Response{QJsonArray{own, peer}};
        };
    }
    bool start()
    {
        auto &c = direct.fixture.controller;
        if (!direct.fixture.start()
            || !QTest::qWaitFor([&] { return c.directChatDirectoryVerified(); }, 2000)) return false;
        c.startDirectChat(channelMemberA);
        if (!QTest::qWaitFor([&] { return c.directChatPhase() == "ready"
            && c.selectedStream() == directId; }, 2000)) return false;
        c.selectTopic(channelDefault);
        return QTest::qWaitFor([&] { return !c.busy() && c.messages().size() == 2; }, 2000);
    }
    QVariantMap sidebar() const
    {
        for (const auto &row : direct.fixture.controller.streams())
            if (row.toMap().value("id").toString() == directId) return row.toMap();
        return {};
    }
};

// Additive presentation fixture only: the existing real Users/direct/history HTTP paths.
static QJsonObject presencePerson(const QString &id, const QString &username, const QJsonValue &status,
    const QString &updated = "2026-01-02T10:00:00Z", const QJsonValue &text = QJsonValue::Null,
    const QJsonValue &emoji = QJsonValue::Null, const QJsonValue &avatar = QJsonValue(avatarReferenceA))
{
    auto row = avatarReferencePerson(id, username, avatar);
    if (status.isUndefined()) row.remove("status"); else row["status"] = status;
    row["updated_at"] = updated;
    row["status_text"] = text; row["status_emoji"] = emoji;
    return row;
}

class OwnStatusFixture {
public:
    QTemporaryDir storage;
    NativePreferences preferences{storage.filePath("own-status.ini")};
    QJsonObject own = avatarReferencePerson(userId, "cassi", avatarReferenceA);
    QJsonObject posted, returned;
    int posts = 0, usersGets = 0, responseStatus = 200;
    bool held = false, differentOwner = false;
    std::function<void()> release;
    Fixture fixture; // All captured transport/settings facts outlive Controller/server.
    OwnStatusFixture()
    {
        own["status"] = "active"; own["status_text"] = "Before"; own["status_emoji"] = "☕";
        fixture.users = [this](const Request &) { ++usersGets; return Response{QJsonArray{own}}; };
        fixture.authentication = [this](const Request &request) {
            auto response = sessionResponse(request);
            if (differentOwner && request.target.path().endsWith("/actions/introspect")) {
                auto raw = response.data.toObject(), user = raw.value("user_info").toObject();
                user["uuid"] = returnedId; raw["user_info"] = user; response.data = raw;
            }
            return response;
        };
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [this, fallback](const Request &request) {
            if (request.target.path() != "/api/workspace/v1/users/" + userId + "/actions/presence/invoke")
                return fallback(request);
            ++posts; posted = request.body;
            auto row = returned.isEmpty() ? own : returned;
            if (returned.isEmpty()) {
                row["status"] = posted.value("status"); row["status_text"] = posted.value("text");
                row["status_emoji"] = posted.value("emoji"); row["updated_at"] = "2026-01-02T10:01:00Z";
                row["last_ping_at"] = "2026-01-02T10:01:00Z";
            }
            Response response{row, responseStatus};
            if (held) response.defer = [this](std::function<void()> send) { release = std::move(send); };
            return response;
        };
        fixture.controller.usePreferences(&preferences);
    }
    bool start()
    {
        return storage.isValid() && fixture.start()
            && QTest::qWaitFor([this] { return fixture.api.findChildren<QNetworkReply *>().isEmpty(); }, 2000);
    }
    QPointer<QNetworkReply> pending() const
    {
        for (auto *reply : fixture.api.findChildren<QNetworkReply *>())
            if (!reply->isFinished() && reply->url().path() == "/api/workspace/v1/users/" + userId + "/actions/presence/invoke"
                && reply->request().attribute(QNetworkRequest::CustomVerbAttribute).toByteArray() == "POST") return reply;
        return {};
    }
    ~OwnStatusFixture() { if (release) release(); }
};

static QString conversationPreferenceOwner(const QUrl &origin, const QString &user = userId,
                                           const QString &project = projectId)
{
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(QJsonArray{
        origin.toString(QUrl::FullyEncoded), project, user}).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
}

// Recording fake for the desktop-notification transport (issue #3). It tracks
// "one active notification per tag" the same way the Electron shell does: a
// new show for an active tag replaces the previous holder.
class RecordingNotifications final : public workspace::NotificationTransport {
public:
    struct Show { QString tag, title, body; };
    QList<Show> shows;
    QStringList closed;
    int attentionRequests = 0;
    int attentionClears = 0;

    bool show(const QString &tag, const QString &title, const QString &body) override
    {
        shows.push_back({tag, title, body});
        return true;
    }
    void closeByTag(const QString &tag) override { closed << tag; }
    void requestAttention() override { ++attentionRequests; }
    void clearAttention() override { ++attentionClears; }
    QString lastTag() const { return shows.isEmpty() ? QString{} : shows.last().tag; }
};

// Minimal session-bus stand-in for org.freedesktop.Notifications so the wire
// signature of Notify/CloseNotification is verifiable without a real daemon.
class FakeFreedesktopNotifications final : public QObject
{
    Q_OBJECT
public:
    struct NotifyCall
    {
        QString app;
        uint replacesId = 0;
        QString icon;
        QString summary;
        QString body;
        QStringList actions;
        QVariantMap hints;
        int timeout = 0;
    };
    QList<NotifyCall> notifyCalls;
    QList<uint> closedIds;
};

class FakeFreedesktopNotificationsAdaptor final : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
public:
    explicit FakeFreedesktopNotificationsAdaptor(FakeFreedesktopNotifications *owner)
        : QDBusAbstractAdaptor(owner), m_owner(owner) {}

public slots:
    uint Notify(const QString &app, uint replacesId, const QString &icon,
        const QString &summary, const QString &body, const QStringList &actions,
        const QVariantMap &hints, int timeout)
    {
        m_owner->notifyCalls.push_back(
            {app, replacesId, icon, summary, body, actions, hints, timeout});
        return 100u + quint32(m_owner->notifyCalls.size());
    }
    void CloseNotification(uint id) { m_owner->closedIds.append(id); }

private:
    FakeFreedesktopNotifications *m_owner;
};

static QJsonObject notificationStreamDto(const QString &mode = QStringLiteral("all_messages"))
{
    return {{QStringLiteral("uuid"), streamId}, {QStringLiteral("project_id"), projectId},
            {QStringLiteral("name"), QStringLiteral("Native channel")},
            {QStringLiteral("encryption"), false}, {QStringLiteral("notification_mode"), mode}};
}

static QJsonObject notificationTopicDto(const QString &id = topicId,
                                        const QString &name = QStringLiteral("Native topic"),
                                        const QString &mode = QStringLiteral("default"))
{
    return {{QStringLiteral("uuid"), id}, {QStringLiteral("project_id"), projectId},
            {QStringLiteral("stream_uuid"), streamId}, {QStringLiteral("name"), name},
            {QStringLiteral("notification_mode"), mode}};
}

// Marks a history DTO as a foreign unread arrival (notification candidate).
static QJsonObject incomingDto(QJsonObject row, const QString &author)
{
    row[QStringLiteral("read")] = false;
    row[QStringLiteral("is_own")] = false;
    row[QStringLiteral("author_uuid")] = author;
    return row;
}

static const QString notificationAuthor = QStringLiteral("55555555-5555-4555-8555-555555555555");

class NativeControllerTest final : public QObject {
    Q_OBJECT
private slots:
    void lastConversationStartupUsesCanonicalOwnedRoute_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto *mode : {"stream", "topic", "moved-topic"}) QTest::newRow(mode) << QString(mode);
    }
    void lastConversationStartupUsesCanonicalOwnedRoute()
    {
        QFETCH(QString, mode);
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.ini");
        TopicOpeningFixture test;
        const auto owner = conversationPreferenceOwner(test.fixture.server.origin());
        if (mode != "moved-topic") test.resolved["stream_uuid"] = streamId;
        else test.resolved["uuid"] = openingCanonicalId;
        {
            NativePreferences initial(path);
            initial.recordLastConversation(owner, streamId, mode == "stream" ? QString{} : openingRequestedId);
        }
        NativePreferences preferences(path);
        auto &controller = test.fixture.controller;
        controller.usePreferences(&preferences);
        controller.login(test.fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(controller.projects().size(), 1);
        controller.openProject(projectId);
        const auto expectedStream = mode == "moved-topic" ? openingStreamId : streamId;
        const auto expectedTopic = mode == "stream" ? QString{} : test.resolved.value("uuid").toString();
        QTRY_COMPARE_WITH_TIMEOUT(controller.selectedStream(), expectedStream, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(controller.selectedTopic(), expectedTopic, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy() && !controller.topicResolving(), 2000);
        QVERIFY2(controller.error().isEmpty(), qPrintable(controller.error()));
        QCOMPARE(test.lookups, mode == "stream" ? 0 : 1);
        QCOMPARE(test.mutations, 0);
        QCOMPARE(preferences.loadLastConversation(owner), (QStringList{expectedStream, expectedTopic}));
        for (const auto &target : test.historyTargets) {
            const QUrlQuery query(target);
            QCOMPARE(query.queryItemValue("stream_uuid"), expectedStream);
            QCOMPARE(query.queryItemValue("topic_uuid"), expectedTopic);
        }
    }

    void manualNavigationWinsBeforeAutomaticConversationDispatch_data()
    {
        QTest::addColumn<bool>("root");
        QTest::newRow("manual-stream") << false;
        QTest::newRow("manual-messenger-root") << true;
    }
    void manualNavigationWinsBeforeAutomaticConversationDispatch()
    {
        QFETCH(bool, root);
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        TopicOpeningFixture test;
        const auto owner = conversationPreferenceOwner(test.fixture.server.origin());
        preferences.recordLastConversation(owner, streamId, openingRequestedId);
        auto &controller = test.fixture.controller;
        controller.usePreferences(&preferences);
        bool chosen = false;
        connect(&controller, &NativeController::streamsChanged, &controller, [&] {
            if (chosen || !controller.authenticated() || controller.streams().isEmpty()) return;
            chosen = true;
            controller.selectStream(root ? QString{} : streamId);
        });
        controller.login(test.fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(controller.projects().size(), 1);
        controller.openProject(projectId);
        QTRY_VERIFY(chosen);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 2000);
        QCOMPARE(controller.selectedStream(), root ? QString{} : streamId);
        QVERIFY(controller.selectedTopic().isEmpty());
        QCOMPARE(test.lookups, 0);
        QCOMPARE(test.mutations, 0);
        QCOMPARE(preferences.loadLastConversation(owner), (QStringList{root ? QString{} : streamId, QString{}}));
    }

    void lastConversationDoesNotCrossTheVerifiedOwner()
    {
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        TopicOpeningFixture test;
        preferences.recordLastConversation(conversationPreferenceOwner(test.fixture.server.origin(), returnedId), streamId, openingRequestedId);
        auto &controller = test.fixture.controller;
        controller.usePreferences(&preferences);
        controller.login(test.fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(controller.projects().size(), 1);
        controller.openProject(projectId);
        QTRY_COMPARE(controller.streams().size(), 1);
        QTRY_VERIFY(!controller.busy());
        QVERIFY(controller.selectedStream().isEmpty());
        QVERIFY(controller.selectedTopic().isEmpty());
        QCOMPARE(test.lookups, 0);
        QCOMPARE(test.mutations, 0);
    }

    void unavailableSavedStreamDoesNotChooseAnotherChat()
    {
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        TopicOpeningFixture test;
        preferences.recordLastConversation(conversationPreferenceOwner(test.fixture.server.origin()), openingStreamId, {});
        auto &controller = test.fixture.controller;
        controller.usePreferences(&preferences);
        controller.login(test.fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(controller.projects().size(), 1);
        controller.openProject(projectId);
        QTRY_VERIFY(!controller.error().isEmpty());
        QVERIFY(controller.selectedStream().isEmpty());
        QVERIFY(controller.selectedTopic().isEmpty());
        QCOMPARE(test.lookups, 0);
        QCOMPARE(test.mutations, 0);
    }
    void usersHttpNamesMatchSourceAndPreserveLegacyAbsentFields_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::addColumn<QString>("expectedAuthor");
        QTest::addColumn<QString>("expectedDirectory");
        QTest::newRow("canonical-first-last") << QString("canonical-first-last")
            << QString("Alice Synthetic") << QString("Alice Synthetic");
        QTest::newRow("canonical-present-null") << QString("canonical-present-null")
            << QString("alice") << QString("alice");
        QTest::newRow("canonical-present-whitespace") << QString("canonical-present-whitespace")
            << QString("alice") << QString("alice");
        QTest::newRow("legacy-name-when-both-absent") << QString("legacy-name")
            << QString("Legacy name") << QString("alice");
        QTest::newRow("legacy-display-name-when-both-absent") << QString("legacy-display-name")
            << QString("Legacy display name") << QString("alice");
    }

    void usersHttpNamesMatchSourceAndPreserveLegacyAbsentFields()
    {
        QFETCH(QString, scenario); QFETCH(QString, expectedAuthor); QFETCH(QString, expectedDirectory);
        AvatarReferenceFixture test{QJsonValue(avatarReferenceA)}; auto &c = test.direct.fixture.controller;
        auto peer = test.directory[1].toObject();
        peer["username"] = "alice"; peer["email"] = "alice@example.invalid";
        peer["name"] = "Legacy name"; peer["display_name"] = "Legacy display name";
        if (scenario == "canonical-first-last") {
            peer["first_name"] = " Alice "; peer["last_name"] = " Synthetic ";
        } else if (scenario == "canonical-present-whitespace") {
            peer["first_name"] = " \t"; peer["last_name"] = "\n ";
        } else if (scenario.startsWith("legacy-")) {
            peer.remove("first_name"); peer.remove("last_name");
            if (scenario == "legacy-display-name") peer.remove("name");
        } // The canonical null row retains the actual nullable Users fields.
        test.directory[1] = peer; // Before the actual Users HTTP collection is served.
        QVERIFY(test.start());
        QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(test.direct.usersGets, 1); QCOMPARE(test.direct.gets, 1); QCOMPARE(test.direct.posts, 0);
        QVERIFY(c.directChatDirectoryVerified()); QVERIFY(c.isDirectConversation());
        QCOMPARE(c.selectedStream(), directId); QCOMPARE(c.selectedTopic(), channelDefault);
        QCOMPARE(c.directPeer().value("displayName").toString(), expectedDirectory);
        QCOMPARE(test.sidebar().value("displayName").toString(), expectedDirectory);
        const auto displayed = modelMessage(c, returnedId);
        QCOMPARE(displayed.value("authorId").toString(), channelMemberA);
        QCOMPARE(displayed.value("content").toString(), QString("Actual peer image-reference witness"));
        QVERIFY(!displayed.value("own").toBool());
        QCOMPARE(displayed.value("authorAvatar").toString(), avatarReferenceA);
        QCOMPARE(c.directPeer().value("avatar").toString(), avatarReferenceA);
        QCOMPARE(c.viewerDisplayName(), QString("avatar-self"));
        QCOMPARE(modelMessage(c, messageId).value("author").toString(), QString("avatar-self"));
        // First intended behavioral RED: current HTTP projection ignores canonical name parts.
        QCOMPARE(displayed.value("author").toString(), expectedAuthor);
        for (const auto &request : test.direct.fixture.server.requests)
            QVERIFY(!request.target.path().contains("/files/"));
    }

    void ownStatusContextTracksOnlyAuthenticatedOwnerAndTransportGeneration()
    {
        OwnStatusFixture test; auto &f = test.fixture; auto &c = f.controller;
        QVERIFY(c.metaObject()->indexOfProperty("ownStatusContextKey") >= 0);
        const auto context = [&] { return c.property("ownStatusContextKey").toString(); };
        QVERIFY(context().isEmpty());
        QVERIFY(test.start());
        QVERIFY(c.authenticated()); QVERIFY(c.ownerKey().isEmpty()); // Real loopback, no HTTPS Vault owner.
        const auto original = context();
        QCOMPARE(original.size(), 64);
        QCOMPARE(QByteArray::fromHex(original.toLatin1()).toHex(), original.toLatin1());
        const auto generation = f.api.generation();

        // Actual public route changes do not own this account-level editor.
        c.selectTopic({}); QCOMPARE(c.selectedTopic(), QString{}); QCOMPARE(context(), original);
        c.selectTopic(topicId); QTRY_VERIFY(!c.busy()); QCOMPARE(context(), original);
        c.refresh(); QTRY_COMPARE(test.usersGets, 2);
        QTRY_VERIFY(!c.busy() && f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(f.api.generation(), generation); QCOMPARE(context(), original);

        // Actual Users/presence admission publishes state without replacing the editor context.
        auto live = test.own;
        live["first_name"] = "Live"; live["email"] = "live@example.invalid";
        live["status"] = "do_not_disturb"; live["status_text"] = "Live metadata";
        live["updated_at"] = "2026-01-02T10:02:00Z";
        QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated", live, 286)));
        QCOMPARE(c.ownStatus().value("text").toString(), QString("Live metadata"));
        QCOMPARE(c.account().value("displayEmail").toString(), QString("live@example.invalid"));
        QCOMPARE(context(), original); QCOMPARE(f.api.generation(), generation);

        QSignalSpy changed(&c, &NativeController::stateChanged); QVERIFY(changed.isValid());
        f.api.setAccessToken("synthetic-status-context-generation");
        QVERIFY(f.api.generation() > generation); QVERIFY(c.authenticated());
        QVERIFY(!context().isEmpty()); QVERIFY(context() != original); QVERIFY(changed.count() > 0);
        const auto replacement = context();
        const auto replacementGeneration = f.api.generation();
        f.api.setAccessToken("synthetic-status-context-generation");
        QCOMPARE(f.api.generation(), replacementGeneration); QCOMPARE(context(), replacement);
        c.logout(); QVERIFY(!c.authenticated()); QVERIFY(context().isEmpty());

        test.differentOwner = true; test.own["uuid"] = returnedId;
        QVERIFY(test.start()); QVERIFY(c.authenticated()); QCOMPARE(f.session.user(), returnedId);
        QVERIFY(!context().isEmpty()); QVERIFY(context() != original); QVERIFY(context() != replacement);
        QCOMPARE(test.posts, 0);
    }

    void ownStatusDraftDistinguishesMeasuredIdleFromDnd_data()
    {
        QTest::addColumn<QString>("status"); QTest::addColumn<bool>("away");
        QTest::newRow("measured-idle-is-not-deliberate-away") << QString("idle") << false;
        QTest::newRow("received-dnd-seeds-away") << QString("do_not_disturb") << true;
    }
    void ownStatusDraftDistinguishesMeasuredIdleFromDnd()
    {
        QFETCH(QString, status); QFETCH(bool, away);
        OwnStatusFixture test; test.own["status"] = status;
        test.own["status_text"] = " Focus "; test.own["status_emoji"] = "☕";
        QVERIFY(test.start()); auto &c = test.fixture.controller;
        QCOMPARE(test.usersGets, 1); QCOMPARE(c.account().value("presence").toString(), QString("idle"));
        const auto identity = test.fixture.session.publicProfile();
        QCOMPARE(c.ownStatus().value("text").toString(), QString("Focus"));
        QCOMPARE(c.ownStatus().value("emoji").toString(), QString("☕"));
        QCOMPARE(c.ownStatus().value("away").toBool(), away);
        for (int i = 0; i < 20; ++i) QVERIFY(!c.ownStatus().value("pending").toBool());
        QCOMPARE(test.posts, 0); QCOMPARE(test.usersGets, 1);
        QCOMPARE(test.fixture.session.publicProfile(), identity);
    }

    void ownStatusPublishesOnlyTheCanonicalResponseAndRemembersTheChoice_data()
    {
        QTest::addColumn<bool>("away");
        QTest::newRow("save-deliberate-away") << true;
        QTest::newRow("save-cleared-active") << false;
    }
    void ownStatusPublishesOnlyTheCanonicalResponseAndRemembersTheChoice()
    {
        QFETCH(bool, away); OwnStatusFixture test; QVERIFY(test.start()); test.held = true;
        auto &f = test.fixture; auto &c = f.controller;
        const auto identity = f.session.publicProfile(), before = c.account();
        const auto beforeDraft = c.ownStatus();
        c.saveOwnStatus(away ? "  Focus \t" : " \n", away ? " ☕ " : " ", away);
        QTRY_COMPARE(test.posts, 1); QTRY_VERIFY(bool(test.release)); QTRY_VERIFY(test.pending());
        QCOMPARE(test.posted.value("status").toString(), away ? QString("idle") : QString("active"));
        if (away) {
            QCOMPARE(test.posted.value("text").toString(), QString("Focus"));
            QCOMPARE(test.posted.value("emoji").toString(), QString("☕"));
        } else { QVERIFY(test.posted.value("text").isNull()); QVERIFY(test.posted.value("emoji").isNull()); }
        QVERIFY(c.ownStatus().value("pending").toBool());
        QCOMPARE(c.ownStatus().value("text"), beforeDraft.value("text"));
        QCOMPARE(c.account(), before); // No optimistic display/presence, identity or avatar mutation.
        c.saveOwnStatus("Forbidden duplicate", "", !away); QCOMPARE(test.posts, 1);
        auto send = std::move(test.release); test.release = {}; send();
        QTRY_VERIFY(!c.ownStatus().value("pending").toBool());
        QVERIFY(c.ownStatus().value("error").toString().isEmpty());
        QCOMPARE(c.ownStatus().value("text").toString(), away ? QString("Focus") : QString{});
        QCOMPARE(c.ownStatus().value("away").toBool(), away);
        QCOMPARE(c.account().value("presence").toString(), away ? QString("idle") : QString("active"));
        QCOMPARE(c.account().value("email"), before.value("email"));
        QCOMPARE(c.account().value("avatar"), before.value("avatar"));
        QCOMPARE(f.session.publicProfile(), identity); QCOMPARE(test.posts, 1); QCOMPARE(test.usersGets, 1);
        // New preferences object + new Controller, same process/session: persisted manual witness,
        // not a claimed OS/process restart. No model or private setting is injected.
        NativePreferences reopened(test.storage.filePath("own-status.ini"));
        NativeController restored(&f.api, &f.session); restored.usePreferences(&reopened);
        QCOMPARE(restored.ownStatus().value("away").toBool(), away);
    }

    void ownStatusFailureCannotPublishOrRememberAnUnconfirmedChoice_data()
    {
        QTest::addColumn<bool>("wrongUser");
        QTest::newRow("transport-503") << false;
        QTest::newRow("successful-other-user-dto") << true;
    }
    void ownStatusFailureCannotPublishOrRememberAnUnconfirmedChoice()
    {
        QFETCH(bool, wrongUser); OwnStatusFixture test; QVERIFY(test.start());
        auto &c = test.fixture.controller;
        if (wrongUser) { test.returned = test.own; test.returned["uuid"] = returnedId; }
        else test.responseStatus = 503;
        const auto before = c.account();
        c.saveOwnStatus("Unconfirmed", "☕", true);
        QTRY_COMPARE(test.posts, 1); QTRY_VERIFY(!c.ownStatus().value("pending").toBool());
        QVERIFY(!c.ownStatus().value("error").toString().isEmpty());
        QCOMPARE(c.ownStatus().value("text").toString(), QString("Before"));
        QVERIFY(!c.ownStatus().value("away").toBool()); QCOMPARE(c.account(), before);
        NativePreferences reopened(test.storage.filePath("own-status.ini"));
        NativeController restored(&test.fixture.api, &test.fixture.session); restored.usePreferences(&reopened);
        QVERIFY(!restored.ownStatus().value("away").toBool()); QCOMPARE(test.posts, 1);
    }

    void ownStatusAcknowledgedReplyIsRetiredByCancelApiOrOwnerReplacement_data()
    {
        QTest::addColumn<QString>("change");
        for (const auto *s : {"cancel", "api-generation", "same-project-reauth", "logout-new-owner"})
            QTest::newRow(s) << QString::fromLatin1(s);
    }
    void ownStatusAcknowledgedReplyIsRetiredByCancelApiOrOwnerReplacement()
    {
        QFETCH(QString, change); OwnStatusFixture test; QVERIFY(test.start()); test.held = true;
        auto &f = test.fixture; auto &c = f.controller;
        c.saveOwnStatus("Retired", "☕", true);
        QTRY_COMPARE(test.posts, 1); QTRY_VERIFY(bool(test.release)); QTRY_VERIFY(test.pending());
        const auto pending = test.pending(); QVERIFY(pending && !pending->isFinished());
        if (change == "cancel") c.cancelOwnStatusUpdate();
        else if (change == "api-generation") f.api.setAccessToken("synthetic-own-status-replacement");
        else if (change == "same-project-reauth") f.session.selectProject(projectId);
        else {
            c.logout(); QVERIFY(c.account().isEmpty());
            test.differentOwner = true; test.own["uuid"] = returnedId;
            QVERIFY(f.start());
        }
        QTRY_VERIFY(pending.isNull() || pending->isFinished());
        QVERIFY(!c.ownStatus().value("pending").toBool());
        auto send = std::move(test.release); test.release = {}; send();
        QVERIFY(!c.ownStatus().value("pending").toBool());
        QVERIFY(c.ownStatus().value("text").toString() != "Retired");
        QVERIFY(!c.ownStatus().value("away").toBool()); QCOMPARE(test.posts, 1);
    }

    void ownStatusAdmissionCanBeCanceledReentrantlyBeforeAnyPost()
    {
        OwnStatusFixture test; QVERIFY(test.start()); auto &c = test.fixture.controller;
        bool armed = true; int interrupted = 0;
        const auto connection = connect(&c, &NativeController::stateChanged, this, [&] {
            if (armed && c.ownStatus().value("pending").toBool()) {
                armed = false; ++interrupted; c.cancelOwnStatusUpdate();
            }
        });
        auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
        c.saveOwnStatus("Not admitted", "☕", true);
        QCOMPARE(interrupted, 1); QCOMPARE(test.posts, 0);
        QVERIFY(!c.ownStatus().value("pending").toBool());
        QCOMPARE(c.ownStatus().value("text").toString(), QString("Before"));
        QVERIFY(!c.ownStatus().value("away").toBool());
    }

    void ownStatusHttpResponseKeepsNewerRawStatusAndExistingNameAvatarOrdering()
    {
        OwnStatusFixture test; QVERIFY(test.start()); test.held = true;
        auto &c = test.fixture.controller;
        c.saveOwnStatus("Older HTTP", "☕", false);
        QTRY_COMPARE(test.posts, 1); QTRY_VERIFY(bool(test.release)); QTRY_VERIFY(test.pending());
        auto live = test.own;
        live["first_name"] = "Live"; live["last_name"] = "Viewer"; live["avatar"] = avatarReferenceB;
        live["status"] = "do_not_disturb"; live["status_text"] = "Newer live"; live["status_emoji"] = "🌙";
        live["updated_at"] = "2026-01-02T10:02:00Z";
        QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated", live, 280)));
        QCOMPARE(c.ownStatus().value("text").toString(), QString("Newer live"));
        QCOMPARE(c.account().value("avatar").toString(), avatarReferenceB);
        auto send = std::move(test.release); test.release = {}; send();
        QTRY_VERIFY(!c.ownStatus().value("pending").toBool());
        QCOMPARE(c.ownStatus().value("text").toString(), QString("Newer live"));
        QCOMPARE(c.ownStatus().value("emoji").toString(), QString("🌙"));
        QVERIFY(c.ownStatus().value("away").toBool()); // Received DND, not inferred idle.
        QCOMPARE(c.account().value("presence").toString(), QString("idle"));
        QCOMPARE(c.account().value("avatar").toString(), avatarReferenceA); // Existing unversioned profile replacement stays independent.
        QCOMPARE(c.viewerDisplayName(), QString("cassi"));
        QCOMPARE(test.posts, 1); QCOMPARE(test.usersGets, 1);
    }

    void accountDisplayEmailUsesOwnedUsersBeforeIamWithoutChangingIdentity_data()
    {
        QTest::addColumn<QJsonValue>("usersEmail");
        QTest::addColumn<QString>("iamEmail");
        QTest::addColumn<QString>("expected");
        const auto canonical = QString(QChar(0xfeff)) + "  cassi@example.invalid \t" + QChar(0xfeff);
        QTest::newRow("owned-users-over-iam") << QJsonValue(canonical) << QString("iam@example.invalid")
            << QString("cassi@example.invalid");
        QTest::newRow("owned-users-with-empty-iam") << QJsonValue(canonical) << QString{}
            << QString("cassi@example.invalid");
        QTest::newRow("blank-users-falls-back-to-iam") << QJsonValue(" \t\n")
            << QString("iam@example.invalid") << QString("iam@example.invalid");
        QTest::newRow("null-users-falls-back-to-iam") << QJsonValue(QJsonValue::Null)
            << QString("iam@example.invalid") << QString("iam@example.invalid");
        QTest::newRow("missing-users-email-falls-back-to-iam") << QJsonValue(QJsonValue::Undefined)
            << QString("iam@example.invalid") << QString("iam@example.invalid");
    }

    void accountDisplayEmailUsesOwnedUsersBeforeIamWithoutChangingIdentity()
    {
        QFETCH(QJsonValue, usersEmail); QFETCH(QString, iamEmail); QFETCH(QString, expected);
        AvatarReferenceFixture test{QJsonValue(avatarReferenceA)};
        auto &f = test.direct.fixture; auto &c = f.controller;
        auto own = test.directory[0].toObject();
        if (usersEmail.isUndefined()) own.remove("email"); else own["email"] = usersEmail;
        test.directory[0] = own;
        const auto authentication = f.authentication;
        f.authentication = [authentication, iamEmail](const Request &request) {
            auto response = authentication(request);
            if (request.target.path().endsWith("/actions/introspect")) {
                auto raw = response.data.toObject(), user = raw.value("user_info").toObject();
                user["email"] = iamEmail; raw["user_info"] = user; response.data = raw;
            }
            return response;
        };
        QVERIFY(test.start()); QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(test.direct.usersGets, 1); QCOMPARE(test.direct.posts, 0);
        const auto identity = f.session.publicProfile();
        QCOMPARE(identity.value("email").toString(), iamEmail);
        QVERIFY(!identity.contains("displayEmail"));
        QCOMPARE(c.account().value("uuid").toString(), userId);
        QCOMPARE(c.account().value("avatar").toString(), avatarReferenceA);
        // Actual Users HTTP/owned profile is ready before the new display-only oracle.
        QCOMPARE(c.account().value("displayEmail").toString(), expected);
        QCOMPARE(c.account().value("email").toString(), iamEmail);
        QCOMPARE(f.session.publicProfile(), identity);
    }

    void ownRealtimeEmailNotifiesAndForeignEmailNeverReplacesTheViewer()
    {
        AvatarReferenceFixture test{QJsonValue(avatarReferenceA)};
        auto &f = test.direct.fixture; auto &c = f.controller;
        QVERIFY(test.start()); QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        const auto identity = f.session.publicProfile(); const auto accounts = c.savedAccounts();
        const int usersBefore = test.direct.usersGets;
        const auto nameBefore = c.viewerDisplayName();
        const auto avatarBefore = c.account().value("avatar");
        auto own = test.directory[0].toObject(); own["email"] = " \tupdated@example.invalid\n";
        own["updated_at"] = "2026-01-02T10:01:00Z";
        QSignalSpy changed(&c, &NativeController::stateChanged); QVERIFY(changed.isValid());
        QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated", own, 200)));
        QVERIFY(!changed.isEmpty());
        QCOMPARE(c.account().value("displayEmail").toString(), QString("updated@example.invalid"));
        auto foreign = test.directory[1].toObject(); foreign["email"] = "foreign@example.invalid";
        QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated", foreign, 201)));
        QCOMPARE(c.account().value("displayEmail").toString(), QString("updated@example.invalid"));
        own["email"] = QJsonValue::Null; own["updated_at"] = "2026-01-02T10:02:00Z";
        QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated", own, 202)));
        QCOMPARE(c.account().value("displayEmail").toString(), identity.value("email").toString());
        QCOMPARE(c.account().value("email"), identity.value("email"));
        QCOMPARE(f.session.publicProfile(), identity); QCOMPARE(c.savedAccounts(), accounts);
        QCOMPARE(c.viewerDisplayName(), nameBefore); QCOMPARE(c.account().value("avatar"), avatarBefore);
        QCOMPARE(test.direct.usersGets, usersBefore); QCOMPARE(test.direct.posts, 0);
    }

    void ownedCachedEmailRemainsDisplayMetadataUntilTheActualUsersReply()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        bool hold = false; std::function<void()> release;
        NativeCacheService initial(directory.path(), {}, true);
        AvatarReferenceFixture test{QJsonValue(avatarReferenceA)};
        auto &f = test.direct.fixture; auto &c = f.controller;
        auto own = test.directory[0].toObject(); own["email"] = " cached@example.invalid ";
        test.directory[0] = own;
        f.users = [&](const Request &) {
            Response response{test.directory};
            if (hold) response.defer = [&](std::function<void()> send) { release = std::move(send); };
            return response;
        };
        c.useCache(&initial); QVERIFY(test.start()); QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        const auto identity = f.session.publicProfile();
        QVERIFY(flushCache(initial));
        const auto saved = cacheEntry(initial, initial.lease(), {NativeCache::Kind::Users, {}, {}, {}});
        QVERIFY(saved);
        bool ownedEmailPersisted = false;
        for (const auto &value : saved->rows) {
            const auto row = value.toObject();
            if (row.value("uuid").toString() == userId)
                ownedEmailPersisted = row.value("email").toString() == " cached@example.invalid ";
        }
        QVERIFY(ownedEmailPersisted);
        c.setRealtimeMode(true);
        c.useCache(nullptr); initial.shutdown(); QTRY_VERIFY(initial.stopped());
        // Reopen the actual SQLite profile entry, with a current Users reply held.
        own["email"] = QJsonValue::Null; test.directory[0] = own;
        hold = true; const int before = test.direct.usersGets;
        NativeCacheService reopened(directory.path(), {}, true);
        NativeController restored(&f.api, &f.session);
        auto releaseOnExit = qScopeGuard([&] { if (release) release(); });
        restored.useCache(&reopened);
        QTRY_COMPARE(test.direct.usersGets, before + 1); QTRY_VERIFY(bool(release));
        QTRY_COMPARE(restored.account().value("avatar").toString(), avatarReferenceA);
        QTRY_VERIFY(restored.cacheStale());
        QPointer<QNetworkReply> pending;
        for (auto *reply : f.api.findChildren<QNetworkReply *>())
            if (!reply->isFinished() && reply->url().path() == "/api/workspace/v1/users/") pending = reply;
        QVERIFY(pending && !pending->isFinished());
        QCOMPARE(restored.account().value("displayEmail").toString(), QString("cached@example.invalid"));
        QCOMPARE(restored.account().value("email"), identity.value("email"));
        auto send = std::move(release); release = {}; hold = false; send();
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QTRY_COMPARE(restored.account().value("displayEmail").toString(), identity.value("email").toString());
        QCOMPARE(f.session.publicProfile(), identity);
        QCOMPARE(test.direct.usersGets, before + 1); QCOMPARE(test.direct.posts, 0);
    }

    void oldOwnerEmailCannotReturnAfterLogoutNewOwnerOrALateUsersReply()
    {
        bool newOwner = false, holdNext = false; int usersGets = 0;
        std::function<void()> releaseOld;
        Fixture f;
        f.users = [&](const Request &) {
            ++usersGets;
            auto oldUser = avatarReferencePerson(userId, "old-viewer");
            oldUser["email"] = "old-users@example.invalid";
            Response response{QJsonArray{oldUser}}; // Deliberately no Users row for the new owner.
            if (holdNext) { holdNext = false; response.defer = [&](std::function<void()> send) { releaseOld = std::move(send); }; }
            return response;
        };
        f.authentication = [&](const Request &request) {
            auto response = sessionResponse(request);
            if (request.target.path().endsWith("/actions/introspect")) {
                auto raw = response.data.toObject(), user = raw.value("user_info").toObject();
                user["uuid"] = newOwner ? returnedId : userId;
                user["email"] = newOwner ? "new-iam@example.invalid" : "old-iam@example.invalid";
                raw["user_info"] = user; response.data = raw;
            }
            return response;
        };
        auto releaseOnExit = qScopeGuard([&] { if (releaseOld) releaseOld(); });
        QVERIFY(f.start()); QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(f.controller.account().value("displayEmail").toString(), QString("old-users@example.invalid"));
        holdNext = true; f.controller.refresh(); QTRY_COMPARE(usersGets, 2); QTRY_VERIFY(bool(releaseOld));
        QPointer<QNetworkReply> pending;
        for (auto *reply : f.api.findChildren<QNetworkReply *>())
            if (!reply->isFinished() && reply->url().path() == "/api/workspace/v1/users/") pending = reply;
        QVERIFY(pending && !pending->isFinished());
        const auto generation = f.api.generation(); f.controller.logout();
        QVERIFY(f.controller.account().isEmpty()); QVERIFY(f.session.publicProfile().isEmpty());
        QTRY_VERIFY(pending.isNull() || pending->isFinished());
        newOwner = true; QVERIFY(f.start()); QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(f.api.generation() > generation); QCOMPARE(usersGets, 3);
        QCOMPARE(f.controller.account().value("uuid").toString(), returnedId);
        QCOMPARE(f.controller.account().value("displayEmail").toString(), QString("new-iam@example.invalid"));
        QCOMPARE(f.controller.account().value("email").toString(), QString("new-iam@example.invalid"));
        auto send = std::move(releaseOld); releaseOld = {}; send();
        QCOMPARE(f.controller.account().value("displayEmail").toString(), QString("new-iam@example.invalid"));
        auto oldEvent = controllerEvent("user.updated", avatarReferencePerson(userId, "old-viewer"), 240);
        QVERIFY(!f.controller.applyRealtimeEvent(oldEvent));
        QCOMPARE(f.controller.account().value("displayEmail").toString(), QString("new-iam@example.invalid"));
        f.controller.logout(); QVERIFY(f.controller.account().isEmpty());
    }

    void presenceUsesActualUsersAndPreservesIndependentPresentation_data()
    {
        QTest::addColumn<QJsonValue>("wireStatus"); QTest::addColumn<QString>("expected");
        QTest::newRow("active") << QJsonValue("active") << QString("active");
        QTest::newRow("idle") << QJsonValue("idle") << QString("idle");
        QTest::newRow("do-not-disturb") << QJsonValue("do_not_disturb") << QString("idle");
        QTest::newRow("offline") << QJsonValue("offline") << QString("offline");
        QTest::newRow("absent") << QJsonValue(QJsonValue::Undefined) << QString{};
        QTest::newRow("null") << QJsonValue(QJsonValue::Null) << QString{};
        QTest::newRow("unknown-string") << QJsonValue("online") << QString{};
        QTest::newRow("invalid-object") << QJsonValue(QJsonObject{{"active",true}}) << QString{};
    }
    void presenceUsesActualUsersAndPreservesIndependentPresentation()
    {
        QFETCH(QJsonValue, wireStatus); QFETCH(QString, expected);
        AvatarReferenceFixture test(avatarReferenceA); auto &c = test.direct.fixture.controller;
        test.directory = {presencePerson(userId,"avatar-self",wireStatus),
            presencePerson(channelMemberA,"avatar-peer",wireStatus),
            avatarReferencePerson(channelMemberB,"other-peer")};
        QVERIFY2(test.start(),"Actual Users/direct/topic history must settle before presence assertions");
        QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(test.direct.usersGets,1); QCOMPARE(test.direct.gets,1); QCOMPARE(test.direct.posts,0);
        QCOMPARE(c.account().value("uuid").toString(),userId);
        QCOMPARE(c.viewerDisplayName(),QString("avatar-self"));
        QCOMPARE(c.directPeer().value("displayName").toString(),QString("avatar-peer"));
        QCOMPARE(modelMessage(c,returnedId).value("author").toString(),QString("avatar-peer"));
        QCOMPARE(c.account().value("avatar").toString(),avatarReferenceA);
        QCOMPARE(c.directPeer().value("avatar").toString(),avatarReferenceA);
        QCOMPARE(c.selectedStream(),directId); QCOMPARE(c.selectedTopic(),channelDefault);
        QVERIFY(c.directChatDirectoryVerified());
        QCOMPARE(c.channelMemberChoices().first().toMap(),QVariantMap({{"id",channelMemberA},
            {"displayName","avatar-peer"},{"username","avatar-peer"},{"email","avatar-peer@example.invalid"}}));
        const auto sidebar = test.sidebar(), peer = modelMessage(c,returnedId), own = modelMessage(c,messageId);
        QVERIFY(sidebar.contains("id") && peer.contains("id") && own.contains("id"));
        QVERIFY2(c.account().contains("presence"),"Actual Workspace Users presence is absent from account projection");
        QVERIFY(c.directPeer().contains("presence")); QVERIFY(sidebar.contains("presence"));
        QVERIFY(peer.contains("authorPresence")); QVERIFY(own.contains("authorPresence"));
        QCOMPARE(c.account().value("presence").toString(),expected);
        QCOMPARE(c.directPeer().value("presence").toString(),expected);
        QCOMPARE(sidebar.value("presence").toString(),expected);
        QCOMPARE(peer.value("authorPresence").toString(),expected);
        QCOMPARE(own.value("authorPresence").toString(),expected);
        for (const auto &request : test.direct.fixture.server.requests)
            QVERIFY(!request.target.path().contains("/files/")
                && !(request.method!="GET" && request.target.path().startsWith("/api/workspace/")));
    }

    void ownStatusLabelUsesSourceTrimAndEmojiRules_data()
    {
        QTest::addColumn<QJsonValue>("text"); QTest::addColumn<QJsonValue>("emoji");
        QTest::addColumn<QString>("expected");
        QTest::newRow("empty") << QJsonValue(" \t") << QJsonValue(" ") << QString{};
        QTest::newRow("unicode") << QJsonValue(" Focus ") << QJsonValue(QString::fromUtf8("☕")) << QString::fromUtf8("☕ Focus");
        const auto feff = QString(QChar(0xfeff));
        QTest::newRow("source-feff-trim") << QJsonValue(feff+"Focus"+feff) << QJsonValue(feff+"house"+feff) << QString::fromUtf8("🏠 Focus");
        QTest::newRow("unknown-shortcode") << QJsonValue(" Ship ") << QJsonValue("party_parrot") << QString("Ship");
        QTest::newRow("legacy-emoji-only") << QJsonValue(QJsonValue::Null) << QJsonValue("speech_balloon") << QString::fromUtf8("💬");
        QTest::newRow("kelvin-not-ascii-shortcode") << QJsonValue(QJsonValue::Null) << QJsonValue(QString::fromUtf8("K")) << QString::fromUtf8("K");
        QTest::newRow("long-s-not-ascii-shortcode") << QJsonValue(QJsonValue::Null) << QJsonValue(QString::fromUtf8("ſ")) << QString::fromUtf8("ſ");
    }
    void ownStatusLabelUsesSourceTrimAndEmojiRules()
    {
        QFETCH(QJsonValue,text); QFETCH(QJsonValue,emoji); QFETCH(QString,expected);
        AvatarReferenceFixture test(avatarReferenceA); auto &c = test.direct.fixture.controller;
        test.directory.replace(0,presencePerson(userId,"avatar-self","active","2026-01-02T10:00:00Z",text,emoji));
        QVERIFY(test.start()); QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(c.viewerDisplayName(),QString("avatar-self")); QCOMPARE(c.account().value("avatar").toString(),avatarReferenceA);
        QVERIFY2(c.account().contains("statusLabel"),"Source own custom-status projection is absent");
        QCOMPARE(c.account().value("statusLabel").toString(),expected);
        QCOMPARE(c.account().value("presence").toString(),QString("active"));
        QCOMPARE(test.direct.usersGets,1); QCOMPARE(test.direct.posts,0);
    }

    void presenceOnlyRealtimeNotifiesExistingSurfacesWithoutChangingIdentity()
    {
        AvatarReferenceFixture test(avatarReferenceA); auto &c = test.direct.fixture.controller;
        QVERIFY(test.start()); QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        const auto choices = c.channelMemberChoices(); const auto context = c.directChatContextKey();
        const auto account = c.account();
        QSignalSpy state(&c,&NativeController::stateChanged), streams(&c,&NativeController::streamsChanged),
            messages(&c,&NativeController::messagesChanged);
        QVERIFY(state.isValid() && streams.isValid() && messages.isValid());
        const QStringList statuses{"active","idle","do_not_disturb","offline"};
        const QStringList visuals{"active","idle","idle","offline"};
        QString previous = "offline"; int epoch = 80;
        for (qsizetype i=0;i<statuses.size();++i) {
            state.clear(); streams.clear(); messages.clear();
            const auto stamp = QString("2026-01-02T10:0%1:00Z").arg(i+1);
            QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated",
                presencePerson(channelMemberA,"avatar-peer",statuses[i],stamp),epoch++)));
            QCOMPARE(c.directPeer().value("displayName").toString(),QString("avatar-peer"));
            QCOMPARE(c.directPeer().value("avatar").toString(),avatarReferenceA);
            QCOMPARE(c.directPeer().value("presence").toString(),visuals[i]);
            QCOMPARE(test.sidebar().value("presence").toString(),visuals[i]);
            QCOMPARE(modelMessage(c,returnedId).value("authorPresence").toString(),visuals[i]);
            if (previous!=visuals[i]) QVERIFY2(!streams.isEmpty(),"A pure presence transition must notify the real Direct sidebar model");
            QVERIFY(!messages.isEmpty()); QVERIFY(!state.isEmpty()); previous=visuals[i];
            state.clear();
            const auto label = QString("Own public status %1").arg(i);
            QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated",
                presencePerson(userId,"avatar-self",statuses[i],stamp,label),epoch++)));
            QCOMPARE(c.account().value("presence").toString(),visuals[i]);
            QCOMPARE(c.account().value("statusLabel").toString(),label); QVERIFY(!state.isEmpty());
            QCOMPARE(c.account().value("displayName"),account.value("displayName"));
            QCOMPARE(c.account().value("avatar").toString(),avatarReferenceA);
            QCOMPARE(c.channelMemberChoices(),choices); QCOMPARE(c.directChatContextKey(),context);
            QCOMPARE(c.selectedStream(),directId); QCOMPARE(c.selectedTopic(),channelDefault);
        }
        QCOMPARE(test.direct.usersGets,1); QCOMPARE(test.direct.posts,0);
        for (const auto &request : test.direct.fixture.server.requests) QVERIFY(!request.target.path().contains("/files/"));
    }

    void olderPresenceKeepsNewFactsWithoutChangingLegacyReplacementOrder_data()
    {
        QTest::addColumn<bool>("throughUsers"); QTest::addColumn<QJsonValue>("wireUpdatedAt");
        QTest::newRow("admitted-realtime") << false << QJsonValue("2026-01-02T10:04:00Z");
        QTest::newRow("deferred-users-http") << true << QJsonValue("2026-01-02T10:04:00Z");
        QTest::newRow("missing-time-admitted-realtime") << false << QJsonValue(QJsonValue::Undefined);
        QTest::newRow("invalid-time-admitted-realtime") << false << QJsonValue("not-a-timestamp");
    }
    void olderPresenceKeepsNewFactsWithoutChangingLegacyReplacementOrder()
    {
        QFETCH(bool,throughUsers); QFETCH(QJsonValue,wireUpdatedAt);
        bool hold=false; std::function<void()> release;
        AvatarReferenceFixture test(avatarReferenceA); auto &c = test.direct.fixture.controller;
        test.directory = {presencePerson(userId,"avatar-self","active","2026-01-02T10:05:00Z","Current"),
            presencePerson(channelMemberA,"avatar-peer","active","2026-01-02T10:05:00Z"),
            avatarReferencePerson(channelMemberB,"other-peer")};
        test.direct.fixture.users = [&](const Request &) {
            Response response{test.directory};
            if(hold) { hold=false; response.defer=[&](std::function<void()> send){release=std::move(send);}; }
            return response;
        };
        auto releaseOnExit=qScopeGuard([&] { if(release) release(); });
        QVERIFY(test.start()); QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        auto oldSelf=presencePerson(userId,"older-self","idle","2026-01-02T10:04:00Z","Older",QJsonValue::Null,avatarReferenceB);
        auto oldPeer=presencePerson(channelMemberA,"older-peer","idle","2026-01-02T10:04:00Z",QJsonValue::Null,QJsonValue::Null,avatarReferenceB);
        if(wireUpdatedAt.isUndefined()) { oldSelf.remove("updated_at"); oldPeer.remove("updated_at"); }
        else { oldSelf["updated_at"]=wireUpdatedAt; oldPeer["updated_at"]=wireUpdatedAt; }
        if(throughUsers) {
            test.directory = {oldSelf,oldPeer,avatarReferencePerson(channelMemberB,"other-peer")};
            hold=true; c.refresh(); QTRY_COMPARE(test.direct.usersGets,2); QTRY_VERIFY(bool(release));
            QPointer<QNetworkReply> pending;
            for(auto *reply:test.direct.fixture.api.findChildren<QNetworkReply *>())
                if(!reply->isFinished()&&reply->url().path()=="/api/workspace/v1/users/") pending=reply;
            QVERIFY(pending && !pending->isFinished());
            QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated",
                presencePerson(channelMemberA,"avatar-peer","active","2026-01-02T10:06:00Z"),100)));
            QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated",
                presencePerson(userId,"avatar-self","active","2026-01-02T10:06:00Z","Current"),101)));
            auto send=std::move(release); release={}; send();
            QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        } else {
            QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated",oldPeer,100)));
            QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated",oldSelf,101)));
        }
        // The old name/avatar replacement remains observable; only new status facts are ordered.
        QCOMPARE(c.directPeer().value("displayName").toString(),QString("older-peer"));
        QCOMPARE(c.viewerDisplayName(),QString("older-self"));
        QCOMPARE(c.directPeer().value("avatar").toString(),avatarReferenceB);
        QCOMPARE(c.account().value("avatar").toString(),avatarReferenceB);
        QCOMPARE(c.account().value("presence").toString(),QString("active"));
        QCOMPARE(c.account().value("statusLabel").toString(),QString("Current"));
        QCOMPARE(c.directPeer().value("presence").toString(),QString("active"));
        QCOMPARE(test.sidebar().value("presence").toString(),QString("active"));
        QCOMPARE(modelMessage(c,returnedId).value("authorPresence").toString(),QString("active"));
        QCOMPARE(test.direct.posts,0);
    }

    void reopenedUsersProfilesAreOfflineUntilTheActualUsersReply()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        bool hold=false; std::function<void()> release;
        NativeCacheService initial(directory.path(),{},true);
        AvatarReferenceFixture test(avatarReferenceA); auto &c=test.direct.fixture.controller;
        test.directory = {presencePerson(userId,"avatar-self","active","2026-01-02T10:05:00Z","Current"),
            presencePerson(channelMemberA,"avatar-peer","active","2026-01-02T10:05:00Z"),
            avatarReferencePerson(channelMemberB,"other-peer")};
        test.direct.fixture.users=[&](const Request &) {
            Response response{test.directory};
            if(hold) response.defer=[&](std::function<void()> send){release=std::move(send);};
            return response;
        };
        c.useCache(&initial); QVERIFY(test.start()); QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(flushCache(initial)); auto saved=cacheEntry(initial,initial.lease(),{NativeCache::Kind::Users,{},{},{}});
        QVERIFY(saved); QCOMPARE(saved->rows.size(),3);
        for(const auto &value:saved->rows) {
            const auto row=value.toObject(); QVERIFY(row.contains("username"));
            QVERIFY(!row.contains("status")); QVERIFY(!row.contains("status_text"));
            QVERIFY(!row.contains("status_emoji")); QVERIFY(!row.contains("last_ping_at"));
        }
        c.setRealtimeMode(true); // Existing public mode stops its poll; no fake event or store injection.
        QSignalSpy shutdownNotice(&initial,&NativeCacheService::shutdownFinished); QVERIFY(shutdownNotice.isValid());
        c.useCache(nullptr); initial.shutdown(); QTRY_VERIFY(initial.stopped());
        QCOMPARE(shutdownNotice.size(),1); // Real worker SQL close acknowledged before reopening.
        hold=true; const int before=test.direct.usersGets;
        NativeCacheService reopened(directory.path(),{},true);
        NativeController restored(&test.direct.fixture.api,&test.direct.fixture.session);
        auto releaseOnExit=qScopeGuard([&] { if(release) release(); });
        restored.useCache(&reopened);
        QTRY_COMPARE(test.direct.usersGets,before+1); QTRY_VERIFY(bool(release));
        QTRY_COMPARE(restored.account().value("avatar").toString(),avatarReferenceA);
        QTRY_VERIFY(restored.cacheStale());
        QCOMPARE(restored.account().value("presence").toString(),QString("offline"));
        QVERIFY(restored.account().contains("statusLabel"));
        QVERIFY(restored.account().value("statusLabel").toString().isEmpty());
        QPointer<QNetworkReply> pending;
        for(auto *reply:test.direct.fixture.api.findChildren<QNetworkReply *>())
            if(!reply->isFinished()&&reply->url().path()=="/api/workspace/v1/users/") pending=reply;
        QVERIFY(pending && !pending->isFinished());
        auto send=std::move(release); release={}; hold=false; send();
        QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QTRY_COMPARE(restored.account().value("presence").toString(),QString("active"));
        QCOMPARE(restored.account().value("statusLabel").toString(),QString("Current"));
        QCOMPARE(test.direct.usersGets,before+1); QCOMPARE(test.direct.posts,0);
    }

    void oldOwnerUsersCannotRestorePresenceAfterReplacementOrReset()
    {
        bool newOwner=false,holdNext=false; int usersGets=0; std::function<void()> releaseOld;
        Fixture f;
        f.users=[&](const Request &) {
            ++usersGets;
            Response response{QJsonArray{presencePerson(newOwner?returnedId:userId,newOwner?"new-viewer":"old-viewer",newOwner?"idle":"active"),
                presencePerson(channelMemberA,newOwner?"new-peer":"old-peer",newOwner?"idle":"active")}};
            if(holdNext) { holdNext=false; response.defer=[&](std::function<void()> send){releaseOld=std::move(send);}; }
            return response;
        };
        f.authentication=[&](const Request &request) {
            auto response=sessionResponse(request);
            if(newOwner&&request.target.path().endsWith("/actions/introspect")) {
                auto raw=response.data.toObject(), user=raw.value("user_info").toObject();
                user["uuid"]=returnedId; raw["user_info"]=user; response.data=raw;
            }
            return response;
        };
        f.messages=[](const Request &) {
            auto row=message("Actual owner-scoped presence history"); row["author_uuid"]=channelMemberA; row["is_own"]=false;
            return Response{QJsonArray{row}};
        };
        auto releaseOnExit=qScopeGuard([&] { if(releaseOld) releaseOld(); });
        QVERIFY(f.start()); QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(f.controller.viewerDisplayName(),QString("old-viewer")); QCOMPARE(usersGets,1);
        holdNext=true; f.controller.refresh(); QTRY_COMPARE(usersGets,2); QTRY_VERIFY(bool(releaseOld));
        QPointer<QNetworkReply> oldReply;
        for(auto *reply:f.api.findChildren<QNetworkReply *>())
            if(!reply->isFinished()&&reply->url().path()=="/api/workspace/v1/users/") oldReply=reply;
        QVERIFY(oldReply && !oldReply->isFinished());
        const auto generation=f.api.generation(); f.controller.logout();
        QVERIFY(f.controller.account().isEmpty()); QVERIFY(f.controller.messages().isEmpty()); QVERIFY(f.controller.streams().isEmpty());
        QTRY_VERIFY(oldReply.isNull()||oldReply->isFinished());
        newOwner=true; QVERIFY(f.start()); QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(f.controller.account().value("uuid").toString(),returnedId); QCOMPARE(usersGets,3);
        QCOMPARE(f.controller.viewerDisplayName(),QString("new-viewer")); QVERIFY(f.api.generation()>generation);
        QCOMPARE(f.controller.account().value("presence").toString(),QString("idle"));
        QCOMPARE(modelMessage(f.controller,messageId).value("authorPresence").toString(),QString("idle"));
        auto send=std::move(releaseOld); releaseOld={}; send();
        QCOMPARE(f.controller.account().value("presence").toString(),QString("idle"));
        auto oldEvent=controllerEvent("user.updated",presencePerson(channelMemberA,"old-peer","active"),180);
        QVERIFY(!f.controller.applyRealtimeEvent(oldEvent));
        QCOMPARE(modelMessage(f.controller,messageId).value("authorPresence").toString(),QString("idle"));
        f.controller.logout(); QVERIFY(f.controller.account().isEmpty()); QVERIFY(f.controller.messages().isEmpty());
    }

    void presenceOnlyUsersReplyNotifiesAfterTheOtherCatalogRepliesHaveSettled()
    {
        bool hold=false,observedCurrent=false; std::function<void()> release;
        AvatarReferenceFixture test(avatarReferenceA); auto &c=test.direct.fixture.controller;
        test.direct.fixture.users=[&](const Request &) {
            Response response{test.directory};
            if(hold) { hold=false; response.defer=[&](std::function<void()> send){release=std::move(send);}; }
            return response;
        };
        auto releaseOnExit=qScopeGuard([&] { if(release) release(); });
        QVERIFY(test.start()); QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        const auto choices=c.channelMemberChoices(); const auto names=c.directPeer().value("displayName");
        test.directory.replace(1,presencePerson(channelMemberA,"avatar-peer","idle","2026-01-02T10:01:00Z"));
        hold=true; c.refresh(); QTRY_COMPARE(test.direct.usersGets,2); QTRY_VERIFY(bool(release));
        // Only the acknowledged, deliberately held Users request remains; earlier
        // Streams signals cannot satisfy the post-Users-publication witness.
        QTRY_COMPARE(test.direct.fixture.api.findChildren<QNetworkReply *>().size(),1);
        QPointer<QNetworkReply> pending=test.direct.fixture.api.findChildren<QNetworkReply *>().first();
        QVERIFY(pending&&!pending->isFinished()); QCOMPARE(pending->url().path(),QString("/api/workspace/v1/users/"));
        const auto observer=connect(&c,&NativeController::streamsChanged,&c,[&] {
            if(test.sidebar().value("presence").toString()=="idle") observedCurrent=true;
        });
        auto disconnectObserver=qScopeGuard([&] { disconnect(observer); });
        auto send=std::move(release); release={}; send();
        QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QTRY_VERIFY(!c.busy());
        QCOMPARE(c.directPeer().value("displayName"),names);
        QCOMPARE(c.directPeer().value("avatar").toString(),avatarReferenceA);
        QCOMPARE(c.directPeer().value("presence").toString(),QString("idle"));
        QVERIFY2(observedCurrent,"The actual Users-only presence publication must notify the Direct sidebar");
        QCOMPARE(c.channelMemberChoices(),choices); QCOMPARE(test.direct.posts,0);
    }

    void aPresenceOnlySubscriberCanSignOutBeforeAnyOldOwnerRefill()
    {
        bool armed=false; int observations=0;
        AvatarReferenceFixture test(avatarReferenceA); auto &c=test.direct.fixture.controller;
        QVERIFY(test.start()); QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        const auto observer=connect(&c,&NativeController::streamsChanged,&c,[&] {
            if(!armed) return;
            armed=false; ++observations; c.logout();
        });
        auto disconnectObserver=qScopeGuard([&] { disconnect(observer); });
        armed=true;
        QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated",
            presencePerson(channelMemberA,"avatar-peer","active","2026-01-02T10:01:00Z"),190)));
        QCOMPARE(observations,1); QVERIFY(!c.authenticated());
        QVERIFY(c.account().isEmpty()); QVERIFY(c.directPeer().isEmpty());
        QVERIFY(c.streams().isEmpty()); QVERIFY(c.messages().isEmpty());
    }

    void anotherProjectDoesNotInheritTheSameUsersPresenceTimestamp()
    {
        const QString otherProject="bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
        bool secondProject=false; int usersGets=0;
        Fixture f;
        f.authentication=[&](const Request &request) {
            auto response=sessionResponse(request);
            if(request.target.path()=="/api/core/v1/iam/projects/")
                response.data=QJsonArray{QJsonObject{{"uuid",projectId},{"name","First project"},{"status","ACTIVE"}},
                    QJsonObject{{"uuid",otherProject},{"name","Second project"},{"status","ACTIVE"}}};
            if(secondProject&&request.target.path().endsWith("/actions/introspect")) {
                auto raw=response.data.toObject(); raw["project_id"]=otherProject; response.data=raw;
            }
            return response;
        };
        f.streams=[&](const Request &) {
            return Response{secondProject?QJsonArray{}:QJsonArray{creationChannel(streamId,"First project channel")}};
        };
        f.topics=[&](const Request &) {
            return Response{secondProject?QJsonArray{}:QJsonArray{creationTopic(topicId,streamId,"First project topic")}};
        };
        f.users=[&](const Request &) {
            ++usersGets;
            auto row=presencePerson(userId,secondProject?"second-viewer":"first-viewer",secondProject?"idle":"active",
                secondProject?"2026-01-02T10:00:00Z":"2026-01-02T10:05:00Z");
            row["project_id"]=secondProject?otherProject:projectId;
            return Response{QJsonArray{row}};
        };
        f.controller.login(f.server.origin().toString(),"synthetic-user","synthetic-password","");
        QTRY_COMPARE(f.controller.projects().size(),2); f.controller.openProject(projectId);
        QTRY_COMPARE(f.controller.viewerDisplayName(),QString("first-viewer"));
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty()); QCOMPARE(usersGets,1);
        secondProject=true;
        // Actual Session public boundary, without controller.reset or a fake row:
        // the new owner may reuse the global user UUID but not its old timestamp.
        f.session.selectProject(otherProject);
        QCOMPARE(f.session.project(),otherProject); QVERIFY(!f.session.ready());
        QVERIFY(f.controller.account().isEmpty());
        QTRY_COMPARE(f.controller.viewerDisplayName(),QString("second-viewer"));
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty()); QCOMPARE(usersGets,2);
        QCOMPARE(f.controller.account().value("uuid").toString(),userId);
        QCOMPARE(f.controller.account().value("presence").toString(),QString("idle"));
    }

    void avatarImageReferencesUseTheActualUsersCollectionAcrossPublicProjections_data()
    {
        QTest::addColumn<QJsonValue>("wireAvatar");
        QTest::addColumn<QString>("expected");
        QTest::newRow("image") << QJsonValue(avatarReferenceA) << avatarReferenceA;
        QTest::newRow("feff-trim") << QJsonValue(QString(QChar(0xfeff)) + avatarReferenceA + QChar(0xfeff)) << avatarReferenceA;
        QTest::newRow("nel-not-trimmed") << QJsonValue(QString(QChar(0x85)) + avatarReferenceA + QChar(0x85)) << QString{};
        QTest::newRow("nonstring-object") << QJsonValue(QJsonObject{{"image", avatarReferenceA}}) << QString{};
        QTest::newRow("extra-suffix") << QJsonValue(avatarReferenceA + "/extra") << QString{};
        QTest::newRow("nil-uuid-reference") << QJsonValue("urn:image:00000000-0000-0000-0000-000000000000")
            << QString("urn:image:00000000-0000-0000-0000-000000000000");
        const QString upperUuid = "urn:image:" + avatarReferenceA.mid(10).toUpper();
        QTest::newRow("uppercase-uuid") << QJsonValue(upperUuid) << upperUuid;
        QTest::newRow("outer-whitespace") << QJsonValue("  " + avatarReferenceA + "\n") << avatarReferenceA;
        QTest::newRow("absent") << QJsonValue(QJsonValue::Undefined) << QString{};
        QTest::newRow("null") << QJsonValue(QJsonValue::Null) << QString{};
        QTest::newRow("invalid-image-id") << QJsonValue("urn:image:not-a-uuid") << QString{};
        QTest::newRow("other-source-urn") << QJsonValue("urn:url:https://images.example.invalid/avatar.png") << QString{};
    }
    void avatarImageReferencesUseTheActualUsersCollectionAcrossPublicProjections()
    {
        QFETCH(QJsonValue, wireAvatar); QFETCH(QString, expected);
        AvatarReferenceFixture test(wireAvatar); auto &c = test.direct.fixture.controller;
        QVERIFY2(test.start(), "Actual Users/direct GET/topic history fixture must settle before avatar oracles");
        QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(test.direct.usersGets, 1); QCOMPARE(test.direct.gets, 1); QCOMPARE(test.direct.posts, 0);
        QVERIFY(c.directChatDirectoryVerified()); QVERIFY(c.isDirectConversation());
        QCOMPARE(c.selectedStream(), directId); QCOMPARE(c.selectedTopic(), channelDefault);
        QCOMPARE(c.directPeer().value("id").toString(), channelMemberA);
        QCOMPARE(c.directPeer().value("displayName").toString(), QString("avatar-peer"));
        QCOMPARE(c.viewerDisplayName(), QString("avatar-self"));
        const auto sidebar = test.sidebar(); QVERIFY(!sidebar.isEmpty());
        QCOMPARE(sidebar.value("peerId").toString(), channelMemberA);
        QCOMPARE(sidebar.value("displayName").toString(), QString("avatar-peer"));
        QCOMPARE(c.channelMemberChoices().first().toMap(), QVariantMap({{"id", channelMemberA},
            {"displayName", "avatar-peer"}, {"username", "avatar-peer"}, {"email", "avatar-peer@example.invalid"}}));
        const auto peer = modelMessage(c, returnedId), own = modelMessage(c, messageId);
        QCOMPARE(peer.value("authorId").toString(), channelMemberA);
        QCOMPARE(peer.value("author").toString(), QString("avatar-peer"));
        QVERIFY(!peer.value("own").toBool()); QVERIFY(own.value("own").toBool());
        QCOMPARE(peer.value("content").toString(), QString("Actual peer image-reference witness"));
        QCOMPARE(c.account().value("uuid").toString(), userId);
        // Proposed additive public display contract; no loader invocation.
        QCOMPARE(c.account().value("avatar").toString(), expected);
        QCOMPARE(c.directPeer().value("avatar").toString(), expected);
        QCOMPARE(sidebar.value("avatar").toString(), expected);
        QCOMPARE(peer.value("authorAvatar").toString(), expected);
        QCOMPARE(own.value("authorAvatar").toString(), expected);
        for (const auto &request : test.direct.fixture.server.requests)
            QVERIFY(!request.target.path().contains("/files/"));
    }

    void avatarOnlyRealtimeUpdatesNotifyEveryAffectedProjectionAndClearOldReferences()
    {
        AvatarReferenceFixture test(QJsonValue::Null); auto &c = test.direct.fixture.controller;
        QVERIFY(test.start()); QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        const auto accountBefore = c.account();
        const auto context = c.directChatContextKey();
        const auto names = qMakePair(c.directPeer().value("displayName"), modelMessage(c, returnedId).value("author"));
        QSignalSpy state(&c, &NativeController::stateChanged), streams(&c, &NativeController::streamsChanged),
            messages(&c, &NativeController::messagesChanged);
        QVERIFY(state.isValid() && streams.isValid() && messages.isValid());
        const QList<QJsonValue> values{QJsonValue(avatarReferenceA), QJsonValue(avatarReferenceB),
            QJsonValue::Null, QJsonValue("urn:image:not-a-uuid"), QJsonValue(avatarReferenceA)};
        const QStringList expected{avatarReferenceA, avatarReferenceB, {}, {}, avatarReferenceA};
        int epoch = 30;
        for (qsizetype i = 0; i < values.size(); ++i) {
            const auto beforePeer = c.directPeer().value("avatar").toString();
            const auto beforeViewer = c.account().value("avatar").toString();
            state.clear(); streams.clear(); messages.clear();
            QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated",
                avatarReferencePerson(channelMemberA, "avatar-peer", values[i]), epoch++)));
            QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated",
                avatarReferencePerson(userId, "avatar-self", values[i]), epoch++)));
            QCOMPARE(c.directPeer().value("avatar").toString(), expected[i]);
            QCOMPARE(test.sidebar().value("avatar").toString(), expected[i]);
            QCOMPARE(modelMessage(c, returnedId).value("authorAvatar").toString(), expected[i]);
            QCOMPARE(modelMessage(c, messageId).value("authorAvatar").toString(), expected[i]);
            QCOMPARE(c.account().value("avatar").toString(), expected[i]);
            if (beforePeer != expected[i]) { QVERIFY(streams.size() > 0); QVERIFY(messages.size() > 0); }
            if (beforeViewer != expected[i]) QVERIFY(state.size() > 0);
            QCOMPARE(c.directPeer().value("displayName"), names.first);
            QCOMPARE(modelMessage(c, returnedId).value("author"), names.second);
            QCOMPARE(c.account().value("displayName"), accountBefore.value("displayName"));
            QVERIFY(c.directChatDirectoryVerified()); // Malformed avatar is not malformed creator identity.
            QCOMPARE(c.directChatContextKey(), context);
            QCOMPARE(c.selectedStream(), directId); QCOMPARE(c.selectedTopic(), channelDefault);
        }
        auto foreign = controllerEvent("user.updated", avatarReferencePerson(channelMemberA, "avatar-peer", avatarReferenceB), epoch++);
        foreign["user_uuid"] = channelMemberB;
        state.clear(); streams.clear(); messages.clear();
        QVERIFY(!c.applyRealtimeEvent(foreign));
        QCOMPARE(c.directPeer().value("avatar").toString(), avatarReferenceA);
        QCOMPARE(state.size(), 0); QCOMPARE(streams.size(), 0); QCOMPARE(messages.size(), 0);
        QCOMPARE(test.direct.posts, 0); QCOMPARE(test.direct.usersGets, 1);
        for (const auto &request : test.direct.fixture.server.requests)
            QVERIFY(!request.target.path().contains("/files/"));
        c.logout(); QVERIFY(c.account().isEmpty()); QVERIFY(c.directPeer().isEmpty());
        QVERIFY(c.streams().isEmpty()); QVERIFY(c.messages().isEmpty());
    }

    void avatarDisplayDoesNotInheritAnUnrelatedCreatorChoiceFailure()
    {
        AvatarReferenceFixture test(avatarReferenceA); auto &c = test.direct.fixture.controller;
        QVERIFY(test.start()); QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(c.directChatDirectoryVerified());
        auto unrelated = test.directory.at(2).toObject();
        unrelated["first_name"] = QString(129, 'x'); // Existing creator bound; unrelated valid peers remain named.
        test.directory.replace(2, unrelated);
        const auto before = test.direct.usersGets;
        c.refresh(); QTRY_COMPARE(test.direct.usersGets, before + 1);
        QTRY_VERIFY(test.direct.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QTRY_VERIFY(!c.busy());
        QVERIFY(!c.directChatDirectoryVerified()); QVERIFY(!c.channelMembersVerified());
        QVERIFY(!c.canStartDirectChatWith(channelMemberA)); QVERIFY(c.channelMemberChoices().isEmpty());
        QVERIFY(c.isDirectConversation()); QCOMPARE(c.directPeer().value("id").toString(), channelMemberA);
        QCOMPARE(c.directPeer().value("displayName").toString(), QString("avatar-peer"));
        QCOMPARE(c.viewerDisplayName(), QString("avatar-self"));
        QCOMPARE(modelMessage(c, returnedId).value("author").toString(), QString("avatar-peer"));
        QCOMPARE(c.account().value("avatar").toString(), avatarReferenceA);
        QCOMPARE(c.directPeer().value("avatar").toString(), avatarReferenceA);
        QCOMPARE(test.sidebar().value("avatar").toString(), avatarReferenceA);
        QCOMPARE(modelMessage(c, returnedId).value("authorAvatar").toString(), avatarReferenceA);
        QCOMPARE(test.direct.posts, 0); // Display data did not restore creation authority.
    }

    void delayedUsersFromThePreviousOwnerCannotPublishAvatarReferences()
    {
        // All references captured by HTTP callbacks outlive Fixture destruction.
        bool newOwner = false, holdNext = false;
        int usersGets = 0;
        std::function<void()> releaseOld;
        Fixture f;
        f.users = [&](const Request &) {
            ++usersGets;
            Response response{QJsonArray{avatarReferencePerson(newOwner ? returnedId : userId,
                newOwner ? "new-viewer" : "old-viewer", newOwner ? avatarReferenceB : avatarReferenceA),
                avatarReferencePerson(channelMemberA, newOwner ? "new-peer" : "old-peer",
                    newOwner ? avatarReferenceB : avatarReferenceA)}};
            if (holdNext) { holdNext = false; response.defer = [&](std::function<void()> send) { releaseOld = std::move(send); }; }
            return response;
        };
        f.authentication = [&](const Request &request) {
            auto response = sessionResponse(request);
            if (newOwner && request.target.path().endsWith("/actions/introspect")) {
                auto raw = response.data.toObject(); auto user = raw.value("user_info").toObject();
                user["uuid"] = returnedId; raw["user_info"] = user; response.data = raw;
            }
            return response;
        };
        f.messages = [](const Request &) {
            auto row = message("Actual owner-bound history"); row["author_uuid"] = channelMemberA; row["is_own"] = false;
            return Response{QJsonArray{row}};
        };
        QVERIFY(f.start()); QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(f.controller.viewerDisplayName(), QString("old-viewer")); QCOMPARE(usersGets, 1);
        holdNext = true; f.controller.refresh(); QTRY_COMPARE(usersGets, 2); QTRY_VERIFY(bool(releaseOld));
        QPointer<QNetworkReply> oldReply;
        for (auto *reply : f.api.findChildren<QNetworkReply *>())
            if (!reply->isFinished() && reply->url().path() == "/api/workspace/v1/users/") oldReply = reply;
        QVERIFY(oldReply); QVERIFY(!oldReply->isFinished());
        const auto oldGeneration = f.api.generation();
        f.controller.logout();
        QVERIFY(f.controller.account().isEmpty()); QVERIFY(f.controller.messages().isEmpty());
        QTRY_VERIFY(oldReply.isNull() || oldReply->isFinished());
        newOwner = true; QVERIFY(f.start());
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(f.controller.account().value("uuid").toString(), returnedId);
        QCOMPARE(f.controller.viewerDisplayName(), QString("new-viewer")); QCOMPARE(usersGets, 3);
        QCOMPARE(modelMessage(f.controller, messageId).value("author").toString(), QString("new-peer"));
        QVERIFY(f.api.generation() > oldGeneration);
        QCOMPARE(f.controller.account().value("avatar").toString(), avatarReferenceB);
        QCOMPARE(modelMessage(f.controller, messageId).value("authorAvatar").toString(), avatarReferenceB);
        releaseOld(); releaseOld = {}; // Existing defer writer guards cancelled socket; no raw pointer access.
        QVERIFY(oldReply.isNull() || oldReply->isFinished());
        QCOMPARE(f.controller.viewerDisplayName(), QString("new-viewer"));
        QCOMPARE(f.controller.account().value("avatar").toString(), avatarReferenceB);
        QCOMPARE(modelMessage(f.controller, messageId).value("authorAvatar").toString(), avatarReferenceB);
        auto oldEvent = controllerEvent("user.updated", avatarReferencePerson(channelMemberA, "old-peer", avatarReferenceA), 90);
        QVERIFY(!f.controller.applyRealtimeEvent(oldEvent)); // Envelope belongs to old userId.
        QCOMPARE(modelMessage(f.controller, messageId).value("authorAvatar").toString(), avatarReferenceB);
    }

    void genericMessageAuthorUsesSourceUnknownFallback_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"missing-user", "known-name", "null-optional-names",
                                    "blank-optional-names", "own-missing-user", "own-known-name", "server-own-foreign"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }

    void genericMessageAuthorUsesSourceUnknownFallback()
    {
        QFETCH(QString, scenario);
        const QString peer = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        const bool identityOwn = scenario.startsWith("own-");
        const bool serverOwn = scenario == "server-own-foreign";
        const auto author = identityOwn ? userId : peer;
        const bool named = scenario == "known-name" || scenario == "own-known-name";
        const bool usernameOnly = scenario == "null-optional-names" || scenario == "blank-optional-names";
        const QString preferred = "Known peer";
        const QString username = "known-peer";
        QJsonArray directory;
        if (named || usernameOnly) {
            QJsonObject user{{"uuid", author}, {"username", username}, {"source", "iam"}, {"status", "offline"},
                {"last_ping_at", "2026-01-02T10:00:00Z"}, {"created_at", "2026-01-02T10:00:00Z"},
                {"updated_at", "2026-01-02T10:00:00Z"}, {"name", named ? QJsonValue(preferred) : QJsonValue::Null},
                {"display_name", QJsonValue::Null},
                {"first_name", named ? QJsonValue("Known") : scenario == "blank-optional-names" ? QJsonValue(" ") : QJsonValue::Null},
                {"last_name", named ? QJsonValue("peer") : scenario == "blank-optional-names" ? QJsonValue("") : QJsonValue::Null}};
            directory.append(user);
        }
        auto served = message("Actual HTTP author fallback witness");
        served["author_uuid"] = author; served["is_own"] = serverOwn;
        int usersGets = 0, historyGets = 0, mutations = 0;
        Fixture fixture;
        fixture.users = [directory, &usersGets](const Request &) { ++usersGets; return Response{directory}; };
        fixture.messages = [served, &historyGets, &mutations](const Request &request) {
            if (request.method == "GET") ++historyGets; else ++mutations;
            return Response{QJsonArray{served}};
        };
        QVERIFY(fixture.start());
        QTRY_COMPARE(usersGets, 1);
        QTRY_VERIFY(fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QTRY_COMPARE(fixture.controller.messages().size(), 1);
        const auto row = modelMessage(fixture.controller, messageId);
        QVERIFY(!row.isEmpty()); QVERIFY(historyGets > 0); QCOMPARE(mutations, 0);
        QCOMPARE(row.value("id").toString(), messageId);
        QCOMPARE(row.value("authorId").toString(), author);
        QCOMPARE(row.value("topicId").toString(), topicId);
        QCOMPARE(row.value("kind").toString(), QString("markdown"));
        QCOMPARE(row.value("content").toString(), served.value("payload").toObject().value("content").toString());
        QCOMPARE(row.value("own").toBool(), identityOwn || serverOwn);
        const auto expected = named ? preferred : usernameOnly ? username
            : identityOwn || serverOwn ? NativeController::tr("You") : "#" + author.left(8);
        QCOMPARE(row.value("author").toString(), expected);
    }

    void knownTopicOpeningIgnoresAStaleStreamHintWithoutLookup()
    {
        TopicOpeningFixture test;
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        QVERIFY(controller.topicsComplete());
        controller.openTopic(secondTopicId, openingStreamId);
        QCOMPARE(controller.selectedStream(), streamId);
        QCOMPARE(controller.selectedTopic(), secondTopicId);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 2000);
        QCOMPARE(test.lookups, 0);
        QCOMPARE(test.details, 0);
        QCOMPARE(test.catalogs, 0);
        QCOMPARE(test.users, 0);
        QVERIFY2(test.unexpectedRequests.isEmpty(), qPrintable(test.unexpectedRequests.join('\n')));
        QCOMPARE(test.mutations, 0);
        QVERIFY(!controller.topicResolving());
    }

    void canonicalTopicOpeningUsesOwnedReturnedIdentity_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto *mode : {"same-stream", "known-stream-move", "missing-stream-canonical-uuid",
                                "direct-read", "encrypted-read", "archived-read", "provider-read"})
            QTest::newRow(mode) << QString(mode);
    }
    void canonicalTopicOpeningUsesOwnedReturnedIdentity()
    {
        QFETCH(QString, mode);
        TopicOpeningFixture test;
        const bool same = mode == "same-stream";
        const bool known = mode == "known-stream-move";
        test.includeDestination = known;
        test.resolved["stream_uuid"] = same ? streamId : openingStreamId;
        if (mode == "missing-stream-canonical-uuid") test.resolved["uuid"] = openingCanonicalId;
        if (mode == "direct-read") { test.destination["private"] = true; test.destination["direct_user_uuid"] = channelMemberA; }
        if (mode == "encrypted-read") { test.destination["encryption"] = true; test.destination["current_encryption_key_uuid"] = channelMemberA; }
        if (mode == "archived-read") test.destination["is_archived"] = true;
        if (mode == "provider-read") { test.destination["source_name"] = "zulip"; test.destination["provider_uuid"] = channelMemberA; test.destination["provider_identity"] = "synthetic-provider"; }
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        controller.openTopic(openingRequestedId, streamId);
        QTRY_COMPARE_WITH_TIMEOUT(test.lookups, 1, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(controller.selectedTopic(), test.resolved.value("uuid").toString(), 2000);
        QCOMPARE(controller.selectedStream(), test.resolved.value("stream_uuid").toString());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy() && !controller.topicResolving(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(controller.topicsComplete(), 2000);
        QCOMPARE(test.details, same || known ? 0 : 1);
        QCOMPARE(test.targetTopicLists, 1);
        QCOMPARE(test.topicHistory, 1);
        QCOMPARE(test.aggregateHistory, 0);
        QCOMPARE(test.catalogs, 0);
        QCOMPARE(test.users, 0);
        QVERIFY2(test.unexpectedRequests.isEmpty(), qPrintable(test.unexpectedRequests.join('\n')));
        QCOMPARE(test.mutations, 0);
        QCOMPARE(test.historyTargets.size(), 1);
        const QUrlQuery history(test.historyTargets.first());
        QCOMPARE(history.queryItemValue("stream_uuid"), controller.selectedStream());
        QCOMPARE(history.queryItemValue("topic_uuid"), controller.selectedTopic());
        QVERIFY(!controller.canSend()); // Opening must not synthesize a default composer draft.
    }

    void canonicalTopicOpeningRejectsUnownedOrInvalidMetadata_data()
    {
        QTest::addColumn<QString>("fault");
        for (const auto *fault : {"viewer", "project", "uuid", "stream", "boolean", "401", "403", "404", "503"})
            QTest::newRow(fault) << QString(fault);
    }
    void canonicalTopicOpeningRejectsUnownedOrInvalidMetadata()
    {
        QFETCH(QString, fault);
        TopicOpeningFixture test;
        bool status = false;
        const auto code = fault.toInt(&status);
        if (status) test.lookupStatus = code;
        test.alterLookup = [&](QJsonObject &row) {
            if (fault == "viewer") row["user_uuid"] = channelMemberA;
            if (fault == "project") row["project_id"] = channelMemberA;
            if (fault == "uuid") row["uuid"] = "not-a-uuid";
            if (fault == "stream") row["stream_uuid"] = "not-a-stream";
            if (fault == "boolean") row["is_done"] = "false";
        };
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        controller.setDraft("Retain the original draft");
        controller.openTopic(openingRequestedId, openingStreamId);
        QTRY_COMPARE_WITH_TIMEOUT(test.lookups, 1, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.topicResolving(), 2000);
        QVERIFY(!controller.error().isEmpty());
        QCOMPARE(controller.selectedStream(), streamId);
        QCOMPARE(controller.selectedTopic(), topicId);
        QCOMPARE(controller.draft(), "Retain the original draft");
        QCOMPARE(test.details, 0);
        QCOMPARE(test.catalogs, 0);
        QCOMPARE(test.users, 0);
        QVERIFY2(test.unexpectedRequests.isEmpty(), qPrintable(test.unexpectedRequests.join('\n')));
        QCOMPARE(test.aggregateHistory + test.topicHistory, 0);
        QCOMPARE(test.mutations, 0);
    }

    void newerKnownTopicOpeningPhysicallyCancelsTheOldLookup()
    {
        TopicOpeningFixture test;
        test.lookupDelay = 30000;
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        controller.openTopic(openingRequestedId, openingStreamId);
        QTRY_COMPARE_WITH_TIMEOUT(test.lookups, 1, 2000);
        QPointer<QNetworkReply> old;
        for (auto *reply : test.fixture.api.findChildren<QNetworkReply *>())
            if (reply->url().path().endsWith(openingRequestedId)) old = reply;
        QVERIFY(old);
        controller.openTopic(secondTopicId, openingStreamId);
        QTRY_VERIFY_WITH_TIMEOUT(!old, 2000);
        QCOMPARE(controller.selectedStream(), streamId);
        QCOMPARE(controller.selectedTopic(), secondTopicId);
        QVERIFY(!controller.topicResolving());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 2000);
        QCOMPARE(test.details, 0);
        QCOMPARE(test.lookups, 1);
        QVERIFY(controller.error().isEmpty());
    }

    void apiGenerationChangeClearsTopicResolvingWithoutOldCallback()
    {
        TopicOpeningFixture test;
        test.lookupDelay = 500;
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        controller.openTopic(openingRequestedId);
        QTRY_COMPARE_WITH_TIMEOUT(test.lookups, 1, 2000);
        QVERIFY(controller.topicResolving());
        const auto generation = test.fixture.api.generation();
        test.fixture.api.setAccessToken("synthetic-rotated-token");
        QVERIFY(test.fixture.api.generation() > generation);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.topicResolving(), 2000);
        QCOMPARE(controller.selectedStream(), streamId);
        QCOMPARE(controller.selectedTopic(), topicId);
        QCOMPARE(test.details, 0);
        QCOMPARE(test.mutations, 0);
    }

    void failedPartialTopicCatalogKeepsOwnedMessagesVisibleButCannotAuthorizeWrites_data()
    {
        QTest::addColumn<bool>("withCache");
        QTest::newRow("without-cache") << false;
        QTest::newRow("with-cache") << true;
    }
    void failedPartialTopicCatalogKeepsOwnedMessagesVisibleButCannotAuthorizeWrites()
    {
        QFETCH(bool, withCache);
        QTemporaryDir directory;
        std::unique_ptr<NativeCacheService> cache;
        TopicOpeningFixture test;
        FileClient files(&test.fixture.api);
        if (withCache) {
            cache = std::make_unique<NativeCacheService>(directory.path(), NativeCache::Policy{}, true);
            test.fixture.controller.useCache(cache.get());
        }
        test.fixture.controller.useFiles(&files);
        test.topicsStatus = 503;
        auto row = message("Owned HTTP message stays visible", returnedId, openingRequestedId);
        row["stream_uuid"] = openingStreamId;
        row["read"] = false;
        test.history = {row};
        QVERIFY(test.start());
        if (cache) { QTRY_VERIFY_WITH_TIMEOUT(cache->available(), 2000); QVERIFY(flushCache(*cache)); }
        auto &controller = test.fixture.controller;
        controller.openTopic(openingRequestedId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.selectedTopic(), openingRequestedId, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(controller.messages().size(), 1, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy() && !controller.topicResolving(), 2000);
        QVERIFY(!controller.topicsComplete());
        QCOMPARE(modelMessage(controller, returnedId).value("content").toString(), "Owned HTTP message stays visible");
        controller.setDraft("A partial topic row cannot authorize this draft");
        QVERIFY(!controller.canSend());
        QVERIFY(!controller.canAttach());
        QVERIFY(!modelMessage(controller, returnedId).value("canReact").toBool());
        controller.send();
        controller.markMessagesReadUpTo(returnedId);
        controller.markVisibleMessagesRead(QVariantList{returnedId});
        controller.starMessage(returnedId);
        controller.editMessage(returnedId, "Do not send this edit");
        controller.deleteMessage(returnedId);
        QCOMPARE(test.mutations, 0);
        controller.copyMessage(returnedId);
        QCOMPARE(QGuiApplication::clipboard()->text(), "Owned HTTP message stays visible");
        if (cache) {
            QVERIFY(flushCache(*cache));
            const auto page = cacheEntry(*cache, cache->lease(), {NativeCache::Kind::Topics, openingStreamId, {}, {}});
            QVERIFY(!page || !page->fresh);
        }
        QCOMPARE(test.lookups, 1);
        QCOMPARE(test.details, 1);
        QCOMPARE(test.targetTopicLists, 1);
        QCOMPARE(test.topicHistory, 1);
        QCOMPARE(test.aggregateHistory, 0);
    }

    void selectedDeletedTopicResolutionKeepsOwnedNavigationAndWriteBarriers_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"canonical-owned-alias", "newer-known-topic-wins", "logout-cancels",
                                    "404-keeps-unresolved-topic-locked", "foreign-viewer-keeps-unresolved-topic-locked"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }
    void selectedDeletedTopicResolutionKeepsOwnedNavigationAndWriteBarriers()
    {
        QFETCH(QString, scenario);
        // These handler captures outlive Fixture and every deferred send closure.
        std::function<void()> releaseLookup;
        bool retired = false;
        int uploads = 0;
        TopicOpeningFixture test;
        FileClient files(&test.fixture.api);
        auto &controller = test.fixture.controller;
        controller.useFiles(&files);
        test.includeDestination = true;
        test.resolved = creationTopic(openingCanonicalId, openingStreamId, "Canonical merged topic");
        auto canonicalMessage = message("Actual canonical alias history", returnedId, openingCanonicalId);
        canonicalMessage["stream_uuid"] = openingStreamId;
        test.history = QJsonArray{canonicalMessage};
        test.fixture.files = [&uploads](const Request &request) -> Response {
            if (request.method == "POST") ++uploads;
            return {QJsonObject{}, 503}; // Any upload here is an actual guard violation.
        };
        const QString lookupPath = "/api/workspace/v1/messenger/stream_topics/" + topicId;
        auto raw = test.resolved;
        if (scenario.startsWith("foreign-viewer")) raw["user_uuid"] = channelMemberA;
        Response lookup{scenario.startsWith("404-") ? QJsonObject{} : raw,
                        scenario.startsWith("404-") ? 404 : 200};
        lookup.defer = [&releaseLookup](std::function<void()> send) { releaseLookup = std::move(send); };
        test.additionalLookups.insert(lookupPath, lookup);
        // After the admitted deletion, a fresh origin list must not resurrect T.
        // Only this new test's exact origin-list response changes; legacy Fixture
        // and destination-list/canonical-history handling remain untouched.
        const auto previous = test.fixture.server.handler;
        test.fixture.server.handler = [previous, &retired](const Request &request) -> Response {
            if (retired && request.method == "GET"
                && request.target.path() == "/api/workspace/v1/messenger/stream_topics/"
                && QUrlQuery(request.target).queryItemValue("stream_uuid") == streamId)
                return {QJsonArray{creationTopic(secondTopicId, streamId, "Other known topic")}};
            return previous(request);
        };
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QFile input(directory.filePath("canonical-alias-write-barrier.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly)); QCOMPARE(input.write("synthetic"), qint64(9)); input.close();
        const QVariantList selection{QUrl::fromLocalFile(input.fileName())};
        QVERIFY(test.start());
        QTRY_VERIFY_WITH_TIMEOUT(test.fixture.api.findChildren<QNetworkReply *>().isEmpty(), 2000);
        QCOMPARE(controller.selectedStream(), streamId); QCOMPARE(controller.selectedTopic(), topicId);
        QVERIFY(controller.topicsComplete());
        controller.setDraft("Original topic draft must not be sent after deletion");
        QVERIFY(controller.canSend()); QVERIFY(controller.canAttach()); // Non-vacuous capability baseline.
        QCOMPARE(test.lookups, 0); QCOMPARE(test.mutations, 0); QCOMPARE(uploads, 0);

        retired = true;
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("topic.deleted",
            QJsonObject{{"uuid", topicId}, {"stream_uuid", streamId}}, 77)));
        // The event, not a direct openTopic call, must start the exact resolver.
        QTRY_COMPARE_WITH_TIMEOUT(test.lookups, 1, 2000);
        QCOMPARE(test.lookupTargets, QStringList{lookupPath});
        QVERIFY(releaseLookup);
        QPointer<QNetworkReply> pending;
        for (auto *reply : test.fixture.api.findChildren<QNetworkReply *>())
            if (reply->url().path() == lookupPath) pending = reply;
        QVERIFY(pending); QVERIFY(!pending->isFinished());
        QSignalSpy finished(pending.data(), &QNetworkReply::finished); QVERIFY(finished.isValid());
        QCOMPARE(finished.size(), 0); QVERIFY(controller.topicResolving());
        QCOMPARE(controller.selectedStream(), streamId); QCOMPARE(controller.selectedTopic(), topicId);
        QCOMPARE(controller.draft(), QString("Original topic draft must not be sent after deletion"));
        QVERIFY(!controller.canSend()); QVERIFY(!controller.canAttach());
        controller.send(); controller.attachFiles(selection);
        QVERIFY(!controller.fileBusy()); QCOMPARE(test.mutations, 0); QCOMPARE(uploads, 0);
        QCOMPARE(test.aggregateHistory + test.topicHistory, 0);

        const bool latest = scenario == "newer-known-topic-wins", logout = scenario == "logout-cancels";
        if (latest) {
            controller.selectTopic(secondTopicId);
            QTRY_VERIFY_WITH_TIMEOUT(!pending, 2000); // Existing physical-cancellation contract.
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy() && !controller.topicResolving(), 2000);
            QCOMPARE(controller.selectedStream(), streamId); QCOMPARE(controller.selectedTopic(), secondTopicId);
            controller.setDraft("New explicit route draft");
            QVERIFY(controller.canSend()); QVERIFY(controller.canAttach());
        } else if (logout) {
            controller.logout();
            QTRY_VERIFY_WITH_TIMEOUT(!pending, 2000);
            QVERIFY(!controller.authenticated()); QVERIFY(!controller.topicResolving());
            QVERIFY(controller.selectedStream().isEmpty()); QVERIFY(controller.selectedTopic().isEmpty());
            QVERIFY(!controller.canSend()); QVERIFY(!controller.canAttach());
        }
        // Exactly one immutable response delivery, also safe after socket cancel.
        auto deliver = std::move(releaseLookup); releaseLookup = {}; QVERIFY(deliver); deliver();
        QTRY_VERIFY_WITH_TIMEOUT(test.fixture.api.findChildren<QNetworkReply *>().isEmpty(), 2000);
        QCOMPARE(finished.size(), 1);
        const bool invalid = scenario.startsWith("404-") || scenario.startsWith("foreign-viewer");
        if (latest) {
            QCOMPARE(controller.selectedStream(), streamId); QCOMPARE(controller.selectedTopic(), secondTopicId);
            QCOMPARE(controller.draft(), QString("New explicit route draft"));
            QCOMPARE(test.topicHistory, 1); QCOMPARE(test.targetTopicLists, 0);
        } else if (logout) {
            QVERIFY(!controller.authenticated()); QVERIFY(controller.selectedStream().isEmpty());
            QVERIFY(controller.selectedTopic().isEmpty()); QVERIFY(!controller.canSend()); QVERIFY(!controller.canAttach());
            QCOMPARE(test.topicHistory, 0); QCOMPARE(test.targetTopicLists, 0);
        } else if (invalid) {
            QVERIFY(!controller.topicResolving()); QVERIFY(!controller.error().isEmpty());
            // Source retains the unresolved explicit route; it does not silently
            // turn a permanently deleted/foreign T into an aggregate selection.
            QCOMPARE(controller.selectedStream(), streamId); QCOMPARE(controller.selectedTopic(), topicId);
            QCOMPARE(controller.draft(), QString("Original topic draft must not be sent after deletion"));
            QVERIFY(!controller.canSend()); QVERIFY(!controller.canAttach());
            controller.send(); controller.attachFiles(selection);
            QVERIFY(!controller.fileBusy());
            QCOMPARE(test.topicHistory, 0); QCOMPARE(test.targetTopicLists, 0);
        } else {
            QTRY_COMPARE_WITH_TIMEOUT(controller.selectedStream(), openingStreamId, 2000);
            QCOMPARE(controller.selectedTopic(), openingCanonicalId);
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy() && !controller.topicResolving(), 2000);
            QVERIFY(controller.topicsComplete());
            QCOMPARE(controller.messages().size(), 1);
            QCOMPARE(modelMessage(controller, returnedId).value("content").toString(), QString("Actual canonical alias history"));
            QCOMPARE(test.topicHistory, 1); QCOMPARE(test.targetTopicLists, 1);
            controller.setDraft("Fresh canonical route draft");
            QVERIFY(controller.canSend()); QVERIFY(controller.canAttach()); // Authority returns only on owned complete target.
            QVERIFY(controller.error().isEmpty());
        }
        QTRY_VERIFY_WITH_TIMEOUT(test.fixture.api.findChildren<QNetworkReply *>().isEmpty(), 2000);
        QCOMPARE(test.lookups, 1); QCOMPARE(test.aggregateHistory, 0);
        QCOMPARE(test.details, 0); QCOMPARE(test.catalogs, 0); QCOMPARE(test.users, 0);
        QCOMPARE(test.mutations, 0); QCOMPARE(uploads, 0);
        QVERIFY2(test.unexpectedRequests.isEmpty(), qPrintable(test.unexpectedRequests.join(QStringLiteral("; "))));
        QCOMPARE(test.historyTargets.size(), logout || invalid ? 0 : 1);
        for (const auto &target : test.historyTargets) {
            const QUrlQuery query(target);
            QCOMPARE(query.queryItemValue("stream_uuid"), latest ? streamId : openingStreamId);
            QCOMPARE(query.queryItemValue("topic_uuid"), latest ? secondTopicId : openingCanonicalId);
        }
    }

    void selectedDeletedTopicBarrierPrecedesPublicationAndKeepsLatestIntent_data()
    {
        QTest::addColumn<bool>("selectNewer");
        QTest::newRow("first-publication-is-locked-then-404-and-live") << false;
        QTest::newRow("first-publication-selects-a-newer-known-topic") << true;
    }

    void selectedDeletedTopicBarrierPrecedesPublicationAndKeepsLatestIntent()
    {
        QFETCH(bool, selectNewer);
        std::function<void()> releaseLookup;
        bool retired = false, armed = false, observed = false;
        QString firstSignal, observedStream, observedTopic, observedDraft;
        int observedRows = -1, uploads = 0;
        bool observedSend = true, observedAttach = true;
        TopicOpeningFixture test;
        auto &controller = test.fixture.controller;
        FileClient files(&test.fixture.api);
        controller.useFiles(&files);
        const QString lookupPath = "/api/workspace/v1/messenger/stream_topics/" + topicId;
        Response lookup{QJsonObject{}, 404};
        lookup.defer = [&releaseLookup](std::function<void()> send) { releaseLookup = std::move(send); };
        test.additionalLookups.insert(lookupPath, lookup);
        auto oldMessage = message("Actual old selected-topic content", messageId, topicId);
        const auto previous = test.fixture.server.handler;
        test.fixture.server.handler = [previous, oldMessage, &retired](const Request &request) -> Response {
            auto response = previous(request); // Keep the real fixture's HTTP accounting.
            if (request.method == "GET" && request.target.path() == "/api/workspace/v1/messenger/messages/"
                && QUrlQuery(request.target).queryItemValue("stream_uuid") == streamId
                && QUrlQuery(request.target).queryItemValue("topic_uuid") == topicId)
                response.data = QJsonArray{oldMessage};
            if (retired && request.method == "GET"
                && request.target.path() == "/api/workspace/v1/messenger/stream_topics/"
                && QUrlQuery(request.target).queryItemValue("stream_uuid") == streamId)
                response.data = QJsonArray{creationTopic(secondTopicId, streamId, "Other known topic")};
            return response;
        };
        test.fixture.files = [&uploads](const Request &request) -> Response {
            if (request.method == "POST") ++uploads;
            return {QJsonObject{}, 503};
        };
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QFile input(directory.filePath("first-notify-write-barrier.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly)); QCOMPARE(input.write("synthetic"), qint64(9)); input.close();
        const QVariantList selection{QUrl::fromLocalFile(input.fileName())};
        QVERIFY(test.start());
        QTRY_VERIFY_WITH_TIMEOUT(test.fixture.api.findChildren<QNetworkReply *>().isEmpty(), 2000);
        QCOMPARE(controller.messages().size(), 1);
        QCOMPARE(modelMessage(controller, messageId).value("content").toString(), QString("Actual old selected-topic content"));
        controller.setDraft("Keep the explicit original draft");
        QVERIFY(controller.canSend()); QVERIFY(controller.canAttach());
        const auto observer = [&](const QString &signal) {
            if (!armed) return;
            armed = false; observed = true; firstSignal = signal;
            observedStream = controller.selectedStream(); observedTopic = controller.selectedTopic();
            observedDraft = controller.draft(); observedRows = controller.messages().size();
            observedSend = controller.canSend(); observedAttach = controller.canAttach();
            controller.send(); controller.attachFiles(selection);
            if (selectNewer) controller.selectTopic(secondTopicId);
        };
        const auto topics = connect(&controller, &NativeController::topicsChanged, &controller,
            [&] { observer(QStringLiteral("topicsChanged")); });
        const auto messages = connect(&controller, &NativeController::messagesChanged, &controller,
            [&] { observer(QStringLiteral("messagesChanged")); });
        const auto state = connect(&controller, &NativeController::stateChanged, &controller,
            [&] { observer(QStringLiteral("stateChanged")); });
        const auto snapshot = connect(&controller, &NativeController::snapshotFailed, &controller,
            [&] { observer(QStringLiteral("snapshotFailed")); });
        const auto disconnectObservers = qScopeGuard([&] {
            disconnect(topics); disconnect(messages); disconnect(state); disconnect(snapshot);
        });
        retired = armed = true;
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("topic.deleted",
            QJsonObject{{"uuid", topicId}, {"stream_uuid", streamId}}, 78)));
        QVERIFY(observed); QVERIFY(!firstSignal.isEmpty());
        QCOMPARE(observedStream, streamId); QCOMPARE(observedTopic, topicId);
        QCOMPARE(observedDraft, QString("Keep the explicit original draft"));
        QCOMPARE(observedRows, 0); QVERIFY(!observedSend); QVERIFY(!observedAttach);
        QVERIFY(!controller.fileBusy()); QCOMPARE(uploads, 0); QCOMPARE(test.mutations, 0);
        if (selectNewer) {
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy() && test.topicHistory == 1, 2000);
            QCOMPARE(controller.selectedStream(), streamId); QCOMPARE(controller.selectedTopic(), secondTopicId);
            QCOMPARE(test.lookups, 0); QVERIFY(!releaseLookup); QVERIFY(!controller.topicResolving());
            controller.setDraft("New explicit user intent"); QVERIFY(controller.canSend()); QVERIFY(controller.canAttach());
        } else {
            QTRY_COMPARE_WITH_TIMEOUT(test.lookups, 1, 2000); QVERIFY(releaseLookup);
            QCOMPARE(test.lookupTargets, QStringList{lookupPath});
            QVERIFY(controller.topicResolving()); QCOMPARE(controller.messages().size(), 0);
            auto deliver = std::move(releaseLookup); releaseLookup = {}; deliver();
            QTRY_VERIFY_WITH_TIMEOUT(test.fixture.api.findChildren<QNetworkReply *>().isEmpty(), 2000);
            QVERIFY(!controller.topicResolving()); QVERIFY(!controller.error().isEmpty());
            QCOMPARE(controller.selectedStream(), streamId); QCOMPARE(controller.selectedTopic(), topicId);
            QVERIFY(controller.applyRealtimeEvent(controllerEvent("message.created", oldMessage, 79)));
            QCOMPARE(controller.messages().size(), 0); // Admitted real RT cannot refill the retired route.
            QVERIFY(!controller.canSend()); QVERIFY(!controller.canAttach());
            QCOMPARE(test.topicHistory, 0);
        }
        QTRY_VERIFY_WITH_TIMEOUT(test.fixture.api.findChildren<QNetworkReply *>().isEmpty(), 2000);
        QCOMPARE(test.aggregateHistory, 0); QCOMPARE(test.catalogs, 0); QCOMPARE(test.details, 0);
        QCOMPARE(test.users, 0); QCOMPARE(test.mutations, 0); QCOMPARE(uploads, 0);
        QVERIFY2(test.unexpectedRequests.isEmpty(), qPrintable(test.unexpectedRequests.join(QStringLiteral("; "))));
    }

    void selectedTopicFollowsItsOwnedRealtimeMoveWithoutResolverGet()
    {
        TopicOpeningFixture test;
        test.includeDestination = true;
        test.resolved = creationTopic(topicId, openingStreamId, "Moved origin topic");
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        controller.setDraft("Draft remains under its original conversation");
        test.canonicalSeen = true;
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("topic.updated", test.resolved, 25)));
        QTRY_COMPARE_WITH_TIMEOUT(controller.selectedStream(), openingStreamId, 2000);
        QCOMPARE(controller.selectedTopic(), topicId);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 2000);
        QCOMPARE(test.lookups, 0);
        QCOMPARE(test.details, 0);
        QCOMPARE(test.aggregateHistory, 0);
        QCOMPARE(test.topicHistory, 1);
        QCOMPARE(test.mutations, 0);
    }

    void invalidTopicOpeningAndExistingSelectTopicRemainKnownOnly()
    {
        TopicOpeningFixture test;
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        controller.selectTopic(openingRequestedId);
        QCOMPARE(controller.selectedTopic(), topicId);
        controller.openTopic("not-a-topic", openingStreamId);
        QCOMPARE(controller.selectedStream(), streamId);
        QCOMPARE(controller.selectedTopic(), topicId);
        QCOMPARE(test.lookups, 0);
        QCOMPARE(test.details, 0);
        QCOMPARE(test.mutations, 0);
    }


    void canonicalDestinationRejectsForeignOrIncompleteStreamMetadata_data()
    {
        QTest::addColumn<QString>("fault");
        for (const auto *fault : {"viewer", "project", "uuid", "boolean", "missing-owner", "401", "403", "404", "503"})
            QTest::newRow(fault) << QString(fault);
    }
    void canonicalDestinationRejectsForeignOrIncompleteStreamMetadata()
    {
        QFETCH(QString, fault);
        TopicOpeningFixture test;
        bool status = false;
        const auto code = fault.toInt(&status);
        if (status) test.detailStatus = code;
        if (fault == "viewer") test.destination["user_uuid"] = channelMemberA;
        if (fault == "project") test.destination["project_id"] = channelMemberA;
        if (fault == "uuid") test.destination["uuid"] = channelMemberA;
        if (fault == "boolean") test.destination["encryption"] = "false";
        if (fault == "missing-owner") test.destination.remove("owner");
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        controller.setDraft("Retain this draft after untrusted destination metadata");
        controller.openTopic(openingRequestedId);
        QTRY_COMPARE_WITH_TIMEOUT(test.details, 1, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.topicResolving(), 2000);
        QVERIFY(!controller.error().isEmpty());
        QCOMPARE(controller.selectedStream(), streamId);
        QCOMPARE(controller.selectedTopic(), topicId);
        QCOMPARE(controller.draft(), "Retain this draft after untrusted destination metadata");
        QCOMPARE(controller.streams().size(), 1);
        QCOMPARE(test.lookups, 1);
        QCOMPARE(test.targetTopicLists + test.topicHistory + test.aggregateHistory, 0);
        QCOMPARE(test.catalogs + test.users + test.mutations, 0);
        QVERIFY2(test.unexpectedRequests.isEmpty(), qPrintable(test.unexpectedRequests.join('\n')));
    }

    void topicOpeningFixtureRecordsTheActualWorkspaceDirectoryAndUnexpectedRoutes()
    {
        TopicOpeningFixture test;
        QVERIFY(test.start());
        int completed = 0;
        test.fixture.api.request("GET", "/api/workspace/v1/users/", {}, {},
            [&](const ApiResult &result) { QVERIFY(result.ok()); ++completed; });
        QTRY_COMPARE_WITH_TIMEOUT(completed, 1, 2000);
        QCOMPARE(test.users, 1);
        QVERIFY(test.unexpectedRequests.isEmpty());
        test.fixture.api.request("GET", "/api/workspace/v1/unexpected-topic-opening-probe", {}, {},
            [&](const ApiResult &result) { QCOMPARE(result.status, 404); ++completed; });
        QTRY_COMPARE_WITH_TIMEOUT(completed, 2, 2000);
        QCOMPARE(test.users, 1);
        QCOMPARE(test.unexpectedRequests,
            QStringList{"GET /api/workspace/v1/unexpected-topic-opening-probe"});
        QCOMPARE(test.mutations, 0);
    }

    void duplicateOpeningAndCurrentKnownCancellationHaveNotifiedState()
    {
        TopicOpeningFixture test;
        test.lookupDelay = 30000;
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        QList<bool> resolvingStates;
        QObject observer;
        connect(&controller, &NativeController::stateChanged, &observer,
            [&] { resolvingStates.append(controller.topicResolving()); });
        controller.openTopic(openingRequestedId);
        QTRY_COMPARE_WITH_TIMEOUT(test.lookups, 1, 2000);
        QVERIFY(resolvingStates.contains(true));
        const auto held = test.openingReply();
        QVERIFY(held);
        controller.openTopic(openingRequestedId, openingStreamId);
        QCOMPARE(test.lookups, 1);
        QCOMPARE(test.openingReply(), held);
        controller.openTopic(topicId, openingStreamId);
        QTRY_VERIFY_WITH_TIMEOUT(!held, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 2000);
        QVERIFY(!controller.topicResolving());
        QVERIFY(!resolvingStates.isEmpty());
        QCOMPARE(resolvingStates.last(), false);
        QCOMPARE(controller.selectedStream(), streamId);
        QCOMPARE(controller.selectedTopic(), topicId);
        QCOMPARE(test.lookups, 1);
        QCOMPARE(test.details, 0);
        QCOMPARE(test.topicHistory, 1); // Existing selectTopic deliberately reloads even this topic.
        QCOMPARE(test.aggregateHistory, 0);
        QCOMPARE(test.catalogs + test.users + test.mutations, 0);
        QVERIFY2(test.unexpectedRequests.isEmpty(), qPrintable(test.unexpectedRequests.join('\n')));
        QVERIFY(controller.error().isEmpty());
    }

    void latestUnknownOpeningCancelsEachEarlierLookupStage_data()
    {
        QTest::addColumn<bool>("detail");
        QTest::newRow("topic-lookup") << false;
        QTest::newRow("stream-detail") << true;
    }
    void latestUnknownOpeningCancelsEachEarlierLookupStage()
    {
        QFETCH(bool, detail);
        TopicOpeningFixture test;
        if (detail) test.detailDelay = 30000; else test.lookupDelay = 30000;
        const auto secondPath = "/api/workspace/v1/messenger/stream_topics/" + openingCanonicalId;
        auto second = creationTopic(openingCanonicalId, streamId, "Latest canonical topic");
        test.additionalLookups.insert(secondPath, Response{second});
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        controller.openTopic(openingRequestedId);
        QTRY_COMPARE_WITH_TIMEOUT(test.lookups, 1, 2000);
        if (detail) QTRY_COMPARE_WITH_TIMEOUT(test.details, 1, 2000);
        const auto held = test.openingReply(detail);
        QVERIFY(held);
        test.resolved = second;
        controller.openTopic(openingCanonicalId, openingStreamId);
        QTRY_VERIFY_WITH_TIMEOUT(!held, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(controller.selectedTopic(), openingCanonicalId, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy() && !controller.topicResolving(), 2000);
        QCOMPARE(controller.selectedStream(), streamId);
        QCOMPARE(test.lookupTargets, QStringList({"/api/workspace/v1/messenger/stream_topics/" + openingRequestedId,
            secondPath}));
        QCOMPARE(test.details, detail ? 1 : 0);
        QCOMPARE(test.targetTopicLists, 1);
        QCOMPARE(test.topicHistory, 1);
        QCOMPARE(test.aggregateHistory, 0);
        QCOMPARE(test.catalogs + test.users + test.mutations, 0);
        QVERIFY2(test.unexpectedRequests.isEmpty(), qPrintable(test.unexpectedRequests.join('\n')));
        QVERIFY(controller.error().isEmpty());
    }

    void topicOpeningLifetimeLossCancelsEachPhysicalStage_data()
    {
        QTest::addColumn<QString>("change");
        QTest::addColumn<bool>("detail");
        for (const auto *change : {"logout", "token", "cache-replaced", "cache-destroyed"})
            for (bool detail : {false, true})
                QTest::newRow(qPrintable(QString(change) + (detail ? "-detail" : "-topic")))
                    << QString(change) << detail;
    }
    void topicOpeningLifetimeLossCancelsEachPhysicalStage()
    {
        QFETCH(QString, change);
        QFETCH(bool, detail);
        QTemporaryDir firstDirectory, secondDirectory;
        QVERIFY(firstDirectory.isValid() && secondDirectory.isValid());
        TopicOpeningFixture test;
        std::unique_ptr<NativeCacheService> cache, replacement;
        if (change.startsWith("cache-")) {
            cache = std::make_unique<NativeCacheService>(firstDirectory.path(), NativeCache::Policy{}, true);
            test.fixture.controller.useCache(cache.get());
        }
        if (detail) test.detailDelay = 30000; else test.lookupDelay = 30000;
        QVERIFY(test.start());
        if (cache) { QTRY_VERIFY_WITH_TIMEOUT(cache->available(), 2000); QVERIFY(flushCache(*cache)); }
        auto &controller = test.fixture.controller;
        controller.openTopic(openingRequestedId);
        QTRY_COMPARE_WITH_TIMEOUT(test.lookups, 1, 2000);
        if (detail) QTRY_COMPARE_WITH_TIMEOUT(test.details, 1, 2000);
        const auto held = test.openingReply(detail);
        QVERIFY(held);
        QVERIFY(controller.topicResolving());
        if (change == "logout") controller.logout();
        else if (change == "token") {
            const auto generation = test.fixture.api.generation();
            test.fixture.api.setAccessToken("synthetic-replaced-opening-token");
            QVERIFY(test.fixture.api.generation() > generation);
        } else if (change == "cache-replaced") {
            replacement = std::make_unique<NativeCacheService>(secondDirectory.path(), NativeCache::Policy{}, true);
            controller.useCache(replacement.get());
        } else cache.reset();
        QTRY_VERIFY_WITH_TIMEOUT(!held, 2000);
        QVERIFY(!controller.topicResolving());
        QCOMPARE(controller.selectedStream(), change == "logout" ? QString{} : streamId);
        QCOMPARE(controller.selectedTopic(), change == "logout" ? QString{} : topicId);
        QCOMPARE(test.lookups, 1);
        QCOMPARE(test.details, detail ? 1 : 0);
        QCOMPARE(test.mutations, 0);
        // Replacement/destroy recovery may refresh the current catalog. It must
        // never commit the delayed target, regardless of optional recovery GETs.
        for (const auto &target : test.historyTargets)
            QVERIFY(QUrlQuery(target).queryItemValue("stream_uuid") != openingStreamId);
        QVERIFY2(test.unexpectedRequests.isEmpty(), qPrintable(test.unexpectedRequests.join('\n')));
    }

    void topicOpeningPublicationHonoursReentrantSelection_data()
    {
        QTest::addColumn<QString>("phase");
        QTest::newRow("admission-to-current-known") << QString("admission");
        QTest::newRow("detail-to-new-unknown") << QString("detail");
        QTest::newRow("route-to-origin-stream") << QString("route");
    }
    void topicOpeningPublicationHonoursReentrantSelection()
    {
        QFETCH(QString, phase);
        TopicOpeningFixture test;
        const auto secondPath = "/api/workspace/v1/messenger/stream_topics/" + openingCanonicalId;
        const auto second = creationTopic(openingCanonicalId, openingStreamId, "Reentrant canonical topic");
        test.additionalLookups.insert(secondPath, Response{second});
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        bool replaced = false;
        QObject observer;
        const auto replace = [&] {
            if (replaced) return;
            replaced = true;
            if (phase == "admission") controller.openTopic(topicId);
            else if (phase == "detail") {
                test.resolved = second;
                controller.openTopic(openingCanonicalId);
            } else controller.selectStream(streamId);
        };
        if (phase == "detail") connect(&controller, &NativeController::streamsChanged, &observer, replace);
        else connect(&controller, &NativeController::stateChanged, &observer, [&] {
            if ((phase == "admission" && controller.topicResolving() && controller.selectedStream() == streamId)
                || (phase == "route" && controller.selectedStream() == openingStreamId)) replace();
        });
        controller.openTopic(openingRequestedId);
        QTRY_VERIFY_WITH_TIMEOUT(replaced, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.topicResolving() && !controller.busy(), 2000);
        QCOMPARE(controller.selectedStream(), phase == "detail" ? openingStreamId : streamId);
        QCOMPARE(controller.selectedTopic(), phase == "detail" ? openingCanonicalId : phase == "admission" ? topicId : QString{});
        QCOMPARE(test.lookups, phase == "admission" ? 0 : phase == "detail" ? 2 : 1);
        QCOMPARE(test.details, phase == "admission" ? 0 : 1);
        QCOMPARE(test.mutations + test.catalogs + test.users, 0);
        for (const auto &target : test.historyTargets)
            QVERIFY(QUrlQuery(target).queryItemValue("topic_uuid") != openingRequestedId);
        QVERIFY2(test.unexpectedRequests.isEmpty(), qPrintable(test.unexpectedRequests.join('\n')));
        QVERIFY(controller.error().isEmpty());
    }

    void topicOpeningPublicationMayDestroyItsFixture_data()
    {
        QTest::addColumn<bool>("afterDetail");
        QTest::newRow("admission") << false;
        QTest::newRow("owned-stream-detail") << true;
    }
    void topicOpeningPublicationMayDestroyItsFixture()
    {
        QFETCH(bool, afterDetail);
        auto test = std::make_unique<TopicOpeningFixture>();
        QVERIFY(test->start());
        QPointer<NativeController> controller = &test->fixture.controller;
        int lookups = -1, details = -1, histories = -1;
        QObject observer;
        const auto destroy = [&] {
            lookups = test->lookups;
            details = test->details;
            histories = test->aggregateHistory + test->topicHistory;
            test.reset();
        };
        if (afterDetail) connect(controller, &NativeController::streamsChanged, &observer, destroy);
        else connect(controller, &NativeController::stateChanged, &observer, [&] {
            if (controller && controller->topicResolving()) destroy();
        });
        controller->openTopic(openingRequestedId);
        QTRY_VERIFY_WITH_TIMEOUT(controller.isNull(), 2000);
        QVERIFY(!test);
        QCOMPARE(lookups, afterDetail ? 1 : 0);
        QCOMPARE(details, afterDetail ? 1 : 0);
        QCOMPARE(histories, 0);
    }

    void seededTopicMustUseTheCompleteListAndRecoverAnExplicitRetry()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        NativeCacheService cache(directory.path(), NativeCache::Policy{}, true);
        TopicOpeningFixture test;
        test.fixture.controller.useCache(&cache);
        QVERIFY(test.start());
        QTRY_VERIFY_WITH_TIMEOUT(cache.available(), 2000);
        QVERIFY(flushCache(cache));
        const auto old = creationTopic(secondTopicId, openingStreamId, "Older cached sibling");
        QVERIFY(cache.put(cache.lease(), {NativeCache::Kind::Topics, openingStreamId, {}, {}}, QJsonArray{old}));
        const auto page = cacheEntry(cache, cache.lease(), {NativeCache::Kind::Topics, openingStreamId, {}, {}});
        QVERIFY(page && page->fresh);
        test.topicsDelay = 30000;
        auto &controller = test.fixture.controller;
        controller.openTopic(openingRequestedId);
        QTRY_COMPARE_WITH_TIMEOUT(controller.selectedTopic(), openingRequestedId, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(test.targetTopicLists, 1, 2000);
        QVERIFY(!controller.topicsComplete());
        QCOMPARE(controller.topics().size(), 1);
        QCOMPARE(controller.topics().first().toMap().value("id").toString(), openingRequestedId);
        controller.setDraft("Only the complete owned list may authorize sending");
        QVERIFY(!controller.canSend());
        test.topicsDelay = 0;
        controller.selectTopic(openingRequestedId);
        QTRY_COMPARE_WITH_TIMEOUT(test.targetTopicLists, 2, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(controller.topicsComplete() && !controller.busy(), 2000);
        QCOMPARE(controller.selectedTopic(), openingRequestedId);
        QCOMPARE(test.lookups, 1);
        QCOMPARE(test.details, 1);
        QCOMPARE(test.users + test.catalogs + test.mutations, 0);
        QVERIFY2(test.unexpectedRequests.isEmpty(), qPrintable(test.unexpectedRequests.join('\n')));
        QVERIFY(flushCache(cache));
        const auto authoritative = cacheEntry(cache, cache.lease(), {NativeCache::Kind::Topics, openingStreamId, {}, {}});
        QVERIFY(authoritative && authoritative->fresh);
        QCOMPARE(authoritative->rows.size(), 1);
        QCOMPARE(authoritative->rows.first().toObject().value("uuid").toString(), openingRequestedId);
    }

    void completeTopicListMayRemoveTheCanonicalSeedWithoutInventingMembership()
    {
        TopicOpeningFixture test;
        test.omitResolvedFromList = true;
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        controller.openTopic(openingRequestedId);
        QTRY_COMPARE_WITH_TIMEOUT(test.targetTopicLists, 1, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(controller.topicsComplete() && !controller.busy(), 2000);
        QCOMPARE(controller.selectedStream(), openingStreamId);
        QVERIFY(controller.selectedTopic().isEmpty());
        QVERIFY(controller.topics().isEmpty());
        QVERIFY(!controller.canSend());
        QCOMPARE(test.lookups, 1);
        QCOMPARE(test.details, 1);
        QCOMPARE(test.catalogs + test.users + test.mutations, 0);
        QVERIFY2(test.unexpectedRequests.isEmpty(), qPrintable(test.unexpectedRequests.join('\n')));
    }

    void pendingTopicOpeningDistinguishesMetadataFromSecurityEvents_data()
    {
        QTest::addColumn<QString>("change");
        for (const auto *change : {"name-and-counter", "role", "topic-deleted"})
            QTest::newRow(change) << QString(change);
    }
    void pendingTopicOpeningDistinguishesMetadataFromSecurityEvents()
    {
        QFETCH(QString, change);
        TopicOpeningFixture test;
        test.lookupDelay = 30000;
        QVERIFY(test.start());
        auto &controller = test.fixture.controller;
        controller.openTopic(openingRequestedId);
        QTRY_COMPARE_WITH_TIMEOUT(test.lookups, 1, 2000);
        const auto held = test.openingReply();
        QVERIFY(held);
        if (change == "topic-deleted") {
            QVERIFY(controller.applyRealtimeEvent(controllerEvent("topic.deleted",
                QJsonObject{{"uuid", openingRequestedId}, {"stream_uuid", openingStreamId}}, 80)));
        } else {
            auto row = creationChannel(streamId, "Renamed channel");
            row["unread_count"] = 8;
            row["active_unread_count"] = 8;
            if (change == "role") row["role"] = "member";
            QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.updated", row, 80)));
        }
        if (change == "name-and-counter") {
            QVERIFY(controller.topicResolving());
            QVERIFY(held);
            controller.openTopic(topicId);
        }
        QTRY_VERIFY_WITH_TIMEOUT(!held, 2000);
        QVERIFY(!controller.topicResolving());
        QCOMPARE(controller.selectedStream(), streamId);
        QCOMPARE(controller.selectedTopic(), topicId);
        QCOMPARE(test.lookups, 1);
        QCOMPARE(test.details, 0);
        QCOMPARE(test.catalogs + test.users + test.mutations, 0);
        QVERIFY2(test.unexpectedRequests.isEmpty(), qPrintable(test.unexpectedRequests.join('\n')));
    }

    void unknownLivePlacementRespectsLoadedOlderBoundary_data() {
        QTest::addColumn<QString>("kind"); QTest::addColumn<bool>("empty"); QTest::addColumn<bool>("older");
        for(const auto *kind:{"message.created","message.updated"}) {
            QTest::newRow(qPrintable(QString(kind)+"-older"))<<QString(kind)<<false<<true;
            QTest::newRow(qPrintable(QString(kind)+"-newer"))<<QString(kind)<<false<<false;
            QTest::newRow(qPrintable(QString(kind)+"-empty"))<<QString(kind)<<true<<true;
        }
    }
    void unknownLivePlacementRespectsLoadedOlderBoundary() {
        QFETCH(QString,kind); QFETCH(bool,empty); QFETCH(bool,older); Fixture fixture;
        QMap<int,QJsonObject> rows; for(int n=1;n<=70;++n) rows.insert(n,historyMessage(n));
        fixture.messages=[&](const Request &request)->Response {
            if(empty) return {QJsonArray{},200,0,{{"X-Pagination-Marker","valid-older-cursor"}}};
            return historyResponse(rows,request);
        };
        QVERIFY(fixture.start()); auto &c=fixture.controller; QVERIFY(c.canLoadOlder());
        const auto before=c.messages().size(); const auto gets=fixture.totalHistoryGets();
        auto incoming=historyMessage(older?0:100);
        QVERIFY(c.applyRealtimeEvent(controllerEvent(kind,incoming)));
        const bool insert=!older&&!empty;
        QCOMPARE(!modelMessage(c,incoming.value("uuid").toString()).isEmpty(),insert);
        QCOMPARE(c.messages().size(),before+(insert?1:0)); QCOMPARE(fixture.totalHistoryGets(),gets);
        QVERIFY(c.canLoadOlder());
    }
    void livePlacementLeavesTheFormerSelectedConversation_data() {
        QTest::addColumn<bool>("crossStream"); QTest::addColumn<bool>("aggregate");
        QTest::newRow("topic-same-stream") << false << false;
        QTest::newRow("topic-cross-stream") << true << false;
        QTest::newRow("aggregate-same-stream-control") << false << true;
        QTest::newRow("aggregate-cross-stream") << true << true;
    }
    void livePlacementLeavesTheFormerSelectedConversation() {
        QFETCH(bool,crossStream); QFETCH(bool,aggregate); Fixture fixture;
        auto row=message("Same identity after a placement move"); fixture.aggregateMessages=true;
        fixture.messages=[&](const Request &request) -> Response {
            const auto query=QUrlQuery(request.target); const auto stream=query.queryItemValue("stream_uuid"),topic=query.queryItemValue("topic_uuid");
            return {row.value("stream_uuid").toString()==stream && (topic.isEmpty() || row.value("topic_uuid").toString()==topic)?QJsonArray{row}:QJsonArray{}};
        };
        QVERIFY(fixture.start()); auto &c=fixture.controller;
        if(aggregate) { c.selectStream(streamId); QTRY_VERIFY(!c.busy()); }
        QCOMPARE(c.messages().size(),1); const auto selectedTopic=c.selectedTopic();
        row["topic_uuid"]=secondTopicId; if(crossStream)row["stream_uuid"]=otherArchiveId;
        const auto gets=fixture.totalHistoryGets();
        QVERIFY(c.applyRealtimeEvent(controllerEvent("message.updated",row)));
        QCOMPARE(c.selectedStream(),streamId); QCOMPARE(c.selectedTopic(),selectedTopic);
        if(aggregate && !crossStream) {
            QCOMPARE(c.messages().size(),1); QCOMPARE(modelMessage(c,messageId).value("topicId").toString(),secondTopicId);
        } else {
            QVERIFY2(modelMessage(c,messageId).isEmpty(),"A moved message still belongs to the former selected conversation");
        }
        QCOMPARE(fixture.totalHistoryGets(),gets);
    }
    void liveMoveRetainsIndexedHistoryBodyOutsideSavedDestination_data()
    {
        QTest::addColumn<bool>("destinationWindow");
        QTest::newRow("no-destination-window") << false;
        QTest::newRow("outside-older-bound") << true;
    }

    void liveMoveRetainsIndexedHistoryBodyOutsideSavedDestination()
    {
        QFETCH(bool, destinationWindow);
        auto served = message("Known history body"); served["user_uuid"] = userId;
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.controller.useCache(&cache);
        fixture.messages = [served](const Request &) { return Response{QJsonArray{served}}; };
        QVERIFY(fixture.start()); QTRY_VERIFY(cache.available()); QVERIFY(flushCache(cache));
        QTRY_COMPARE(fixture.controller.messages().size(), 1);
        const auto lease = cache.lease();
        const NativeCache::Key source{NativeCache::Kind::History, streamId, topicId, {}};
        const NativeCache::Key destination{NativeCache::Kind::History, otherArchiveId, secondTopicId, {}};
        const NativeCache::Key destinationAggregate{NativeCache::Kind::History, otherArchiveId, {}, {}};
        const NativeCache::Key bodyKey{NativeCache::Kind::MessageBody, {}, {}, messageId.toUtf8()};
        const auto savedSource = cacheEntry(cache, lease, source); QVERIFY(savedSource);
        QCOMPARE(savedSource->rows, QJsonArray{served});
        QVERIFY(!cacheEntry(cache, lease, bodyKey)); // The normal History response did not seed a standalone Body.
        const auto readBody = [&](std::optional<NativeCache::MessageBody> &result) {
            struct Read { bool done = false; std::optional<NativeCache::MessageBody> value; };
            const auto state = std::make_shared<Read>();
            cache.getMessageBody(lease, messageId, &cache, [state](auto value) { state->value = std::move(value); state->done = true; });
            const bool done = QTest::qWaitFor([state] { return state->done; }, 2000);
            result = state->value; return done;
        };
        std::optional<NativeCache::MessageBody> before;
        QVERIFY(readBody(before)); QVERIFY(before); QCOMPARE(before->row, served);
        auto latest = message("Destination newer row", returnedId, secondTopicId);
        latest["stream_uuid"] = otherArchiveId; latest["user_uuid"] = userId;
        latest["created_at"] = "2026-01-02T10:01:00.000Z";
        if (destinationWindow) { QVERIFY(cache.put(lease, destination, {latest}, "destination-older")); QVERIFY(flushCache(cache)); }
        auto moved = served; moved["stream_uuid"] = otherArchiveId; moved["topic_uuid"] = secondTopicId;
        moved["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Canonical moved body"}};
        const auto gets = fixture.totalHistoryGets();
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", moved)));
        QCOMPARE(fixture.controller.selectedStream(), streamId); QCOMPARE(fixture.controller.selectedTopic(), topicId);
        QVERIFY(modelMessage(fixture.controller, messageId).isEmpty());
        QCOMPARE(fixture.totalHistoryGets(), gets); QVERIFY(flushCache(cache));
        const auto previousPage = cacheEntry(cache, lease, source); QVERIFY(previousPage); QVERIFY(previousPage->rows.isEmpty());
        const auto target = cacheEntry(cache, lease, destination); QCOMPARE(target.has_value(), destinationWindow);
        if (target) { QCOMPARE(target->rows, QJsonArray{latest}); QCOMPARE(target->nextMarker, QByteArray("destination-older")); }
        QVERIFY(!cacheEntry(cache, lease, destinationAggregate));
        std::optional<NativeCache::MessageBody> retained;
        QVERIFY(readBody(retained));
        QVERIFY2(retained.has_value(), "The real Controller move must retain the owned body without loading a destination page");
        QCOMPARE(retained->row, moved);
    }

    void wholeTopicMoveFollowsCanonicalAndEvictsVacatedBucket()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativeCacheService cache(directory.path(), NativeCache::Policy{}, true);
        Fixture fixture; fixture.controller.useCache(&cache);
        int vacatedGets = 0, movedGets = 0;
        fixture.streams = [](const Request &) {
            return Response{QJsonArray{
                QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Native channel"}, {"encryption", false}},
                QJsonObject{{"uuid", otherArchiveId}, {"project_id", projectId}, {"name", "Receiving channel"}, {"encryption", false}}}};
        };
        fixture.topics = [](const Request &request) {
            const auto movedSide = request.target.toString().contains(otherArchiveId);
            return Response{QJsonArray{creationTopic(topicId, movedSide ? otherArchiveId : streamId,
                movedSide ? "Moved topic" : "Native topic")}};
        };
        fixture.messages = [&](const Request &request) -> Response {
            const auto query = QUrlQuery(request.target);
            const auto stream = query.queryItemValue("stream_uuid");
            if (stream == streamId) ++vacatedGets; else ++movedGets;
            auto a = historyMessage(1); auto b = historyMessage(2);
            if (!stream.isEmpty()) { a["stream_uuid"] = stream; b["stream_uuid"] = stream; }
            return Response{QJsonArray{a, b}};
        };
        // Manual start: the fixture's default flow expects a single-stream catalog.
        fixture.controller.login(fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QVERIFY(QTest::qWaitFor([&]{return fixture.controller.projects().size()==1;},2000));
        fixture.controller.openProject(projectId);
        QVERIFY(QTest::qWaitFor([&]{return fixture.controller.streams().size()==2;},2000));
        fixture.controller.selectStream(streamId);
        QVERIFY(QTest::qWaitFor([&]{return !fixture.controller.topics().isEmpty();},2000));
        fixture.controller.selectTopic(topicId);
        QVERIFY(QTest::qWaitFor([&]{return !fixture.controller.busy();},2000));
        QTRY_VERIFY(cache.available());
        QTRY_COMPARE(fixture.controller.messages().size(), 2);
        const auto lease = cache.lease();
        QVERIFY(cache.put(lease, {NativeCache::Kind::History, streamId, topicId, {}}, {historyMessage(1)}, "vacated-older"));
        QVERIFY(cache.put(lease, {NativeCache::Kind::History, streamId, {}, {}}, {historyMessage(1), historyMessage(2)}));
        auto foreign = historyMessage(9);
        foreign["stream_uuid"] = otherArchiveId; foreign["topic_uuid"] = secondTopicId;
        QVERIFY(cache.put(lease, {NativeCache::Kind::History, otherArchiveId, secondTopicId, {}}, {foreign}));
        QVERIFY(flushCache(cache));
        QVERIFY(cacheEntry(cache, lease, {NativeCache::Kind::History, streamId, topicId, {}}));
        QVERIFY(cacheEntry(cache, lease, {NativeCache::Kind::History, streamId, {}, {}}));
        QVERIFY2(cacheEntry(cache, lease, {NativeCache::Kind::History, otherArchiveId, secondTopicId, {}}), "pre: unrelated page must be seeded");
        const auto vacatedBaseline = vacatedGets;
        const auto movedTopic = creationTopic(topicId, otherArchiveId, "Moved topic");
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated", movedTopic)));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.controller.selectedStream(), otherArchiveId, 4000);
        QCOMPARE(fixture.controller.selectedTopic(), topicId);
        QVERIFY(flushCache(cache));
        QVERIFY2(!cacheEntry(cache, lease, {NativeCache::Kind::History, streamId, topicId, {}}),
            "A whole-topic move is authoritative; the vacated topic bucket must not hydrate the former route");
        QVERIFY2(!cacheEntry(cache, lease, {NativeCache::Kind::History, streamId, {}, {}}),
            "The vacated stream aggregate bucket must not hydrate messages that moved to another channel");
        QVERIFY(cacheEntry(cache, lease, {NativeCache::Kind::History, otherArchiveId, secondTopicId, {}}));
        QTRY_COMPARE(fixture.controller.messages().size(), 2);
        QCOMPARE(fixture.controller.messages().size(), 2);
        const auto firstId = historyMessage(1).value("uuid").toString();
        const auto secondId = historyMessage(2).value("uuid").toString();
        QCOMPARE(modelMessage(fixture.controller, firstId).size() > 0, true);
        QCOMPARE(modelMessage(fixture.controller, secondId).size() > 0, true);
        QCOMPARE(vacatedGets, vacatedBaseline);
        const auto stormGets = fixture.totalHistoryGets();
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated", creationTopic(topicId, otherArchiveId, "Storm one"), 21)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated", creationTopic(topicId, otherArchiveId, "Storm two"), 22)));
        QCOMPARE(fixture.controller.selectedStream(), otherArchiveId);
        QCOMPARE(fixture.controller.selectedTopic(), topicId);
        QCOMPARE(fixture.controller.messages().size(), 2);
        QCOMPARE(fixture.totalHistoryGets(), stormGets);
        int movedTopicRows = 0;
        for (const auto &row : fixture.controller.topics())
            if (row.toMap().value("id").toString() == topicId) ++movedTopicRows;
        QCOMPARE(movedTopicRows, 1);
    }

    void delayedOldHistoryCannotResurrectMovedPlacement() {
        Fixture fixture; auto row=message("Current placement"); int reads=0;
        fixture.messages=[&](const Request &request)->Response {
            ++reads; const auto topic=QUrlQuery(request.target).queryItemValue("topic_uuid");
            return {topic==row.value("topic_uuid").toString()?QJsonArray{row}:QJsonArray{},200,reads==2?150:0,{}};
        };
        QVERIFY(fixture.start()); auto &c=fixture.controller; QCOMPARE(c.messages().size(),1);
        c.refresh(); QTRY_VERIFY(reads>=2);
        row["topic_uuid"]=secondTopicId;
        QVERIFY(c.applyRealtimeEvent(controllerEvent("message.updated",row)));
        QTRY_VERIFY(!c.busy());
        QVERIFY2(modelMessage(c,messageId).isEmpty(),"An older in-flight REST page restored moved membership");
    }
    void archiveMetadataKindsAndForeignOwners_data() {
        QTest::addColumn<QString>("kind");
        for (const auto &kind : {"ordinary", "member", "direct", "encrypted", "provider", "foreign-user", "foreign-project"}) QTest::newRow(kind) << QString(kind);
    }
    void archiveMetadataKindsAndForeignOwners() {
        QFETCH(QString, kind); ArchiveFixture test;
        if (kind == "member") test.archived["role"] = "member";
        if (kind == "direct") { test.archived["direct_user_uuid"] = otherArchiveId; test.archived["private"] = true; }
        if (kind == "encrypted") { test.archived["encryption"] = true; test.archived["current_encryption_key_uuid"] = returnedId; }
        if (kind == "provider") { test.archived["source_name"] = "zulip"; test.archived["provider_uuid"] = returnedId; test.archived["provider_identity"] = "synthetic-stream"; }
        if (kind == "foreign-user") test.archived["user_uuid"] = otherArchiveId;
        if (kind == "foreign-project") test.archived["project_id"] = otherArchiveId;
        if (kind.startsWith("foreign")) {
            auto &c = test.fixture.controller; c.login(test.fixture.server.origin().toString(), "user", "password", "");
            QTRY_COMPARE(c.projects().size(), 1); c.openProject(projectId); QTRY_VERIFY(!c.error().isEmpty());
            QVERIFY(c.archivedChats().isEmpty()); QVERIFY(!c.canUnarchiveChat(archiveId)); return;
        }
        QVERIFY(test.start()); auto &c = test.fixture.controller; QCOMPARE(c.archivedChats().size(), 1);
        const auto row = c.archivedChats().first().toMap(); QCOMPARE(row.value("encrypted").toBool(), kind == "encrypted");
        QCOMPARE(row.value("isDirect").toBool(), kind == "direct"); QCOMPARE(row.value("name").toString(), QString("Archived channel"));
        QVERIFY(c.canUnarchiveChat(archiveId)); c.unarchiveChat(archiveId); QTRY_VERIFY(c.archivedChats().isEmpty()); QCOMPARE(test.posts, 1);
    }
    void archiveCanonicalResponseRequiresFullExactIdentity_data() {
        QTest::addColumn<QString>("field");
        for (const auto &field : {"uuid", "project_id", "user_uuid", "owner", "description", "role", "is_archived", "updated_at", "unread_count"}) QTest::newRow(field) << QString(field);
    }
    void archiveCanonicalResponseRequiresFullExactIdentity() {
        QFETCH(QString, field); ArchiveFixture test;
        const auto alter = [field](QJsonObject &row) {
            if (field == "description" || field == "unread_count") row.remove(field);
            else if (field == "is_archived") row[field] = "false";
            else if (field == "updated_at") row[field] = "not-a-date";
            else if (field == "role") row[field] = "invented-role";
            else if (field == "owner") row[field] = "not-a-uuid";
            else row[field] = otherArchiveId;
        };
        test.postDto = test.getDto = alter; QVERIFY(test.start()); auto &c = test.fixture.controller;
        c.unarchiveChat(archiveId); QTRY_COMPARE(test.gets, 1); QTRY_VERIFY(!archivedChat(c).value("pending").toBool());
        QCOMPARE(c.archivedChats().size(), 1); QCOMPARE(c.streams().size(), 1); QCOMPARE(test.posts, 1);
        QVERIFY(!c.archivedChats().first().toMap().value("error").toString().isEmpty());
    }
    void archiveAmbiguityReconcilesTheSameUuidWithoutAutomaticPost_data() {
        QTest::addColumn<bool>("applied"); QTest::newRow("applied") << true; QTest::newRow("not-applied") << false;
    }
    void archiveAmbiguityReconcilesTheSameUuidWithoutAutomaticPost() {
        QFETCH(bool, applied); ArchiveFixture test; test.postStatus = 503; test.applyPost = applied; QVERIFY(test.start()); auto &c = test.fixture.controller;
        c.unarchiveChat(archiveId); QTRY_COMPARE(test.gets, 1);
        if (applied) { QTRY_VERIFY(c.archivedChats().isEmpty()); QCOMPARE(test.posts, 1); return; }
        QTRY_VERIFY(!archivedChat(c).value("pending").toBool()); QCOMPARE(test.posts, 1);
        test.getStatus = 503; c.unarchiveChat(archiveId); QTRY_COMPARE(test.gets, 2);
        QTRY_VERIFY(!archivedChat(c).value("pending").toBool()); QCOMPARE(test.posts, 1);
        test.getStatus = test.postStatus = 200; test.applyPost = true;
        c.unarchiveChat(archiveId); QTRY_VERIFY(c.archivedChats().isEmpty()); QCOMPARE(test.gets, 3); QCOMPARE(test.posts, 2);
    }
    void archiveTerminalRefusalIsExplicitlyRetryable_data() {
        QTest::addColumn<int>("status"); for (int value : {400,401,403,404,422}) QTest::newRow(qPrintable(QString::number(value))) << value;
    }
    void archiveTerminalRefusalIsExplicitlyRetryable() {
        QFETCH(int, status); ArchiveFixture test; test.postStatus = status; test.applyPost = false; QVERIFY(test.start()); auto &c = test.fixture.controller;
        c.unarchiveChat(archiveId); QTRY_COMPARE(test.posts, 1); QTRY_VERIFY(!archivedChat(c).value("pending").toBool());
        QCOMPARE(test.gets, 0); QVERIFY(c.canUnarchiveChat(archiveId)); test.postStatus = 200; test.applyPost = true;
        c.unarchiveChat(archiveId); QTRY_VERIFY(c.archivedChats().isEmpty()); QCOMPARE(test.posts, 2); QCOMPARE(test.gets, 0);
    }
    void archivePendingRowsAreIndependentAndHistoryRemainsOpenable() {
        ArchiveFixture test; test.includeOther = true; test.postDelay = 180; QVERIFY(test.start()); auto &c = test.fixture.controller;
        c.unarchiveChat(archiveId); c.unarchiveChat(archiveId); c.unarchiveChat(otherArchiveId); QTRY_COMPARE(test.posts, 2);
        c.openArchivedChat(archiveId); QCOMPARE(c.selectedStream(), archiveId); QVERIFY(c.selectedTopic().isEmpty());
        QTRY_VERIFY(c.archivedChats().isEmpty()); QCOMPARE(c.streams().size(), 3); QCOMPARE(c.selectedStream(), archiveId); QCOMPARE(test.posts, 2);
        for (const auto &request : test.fixture.server.requests) if (request.target.path().endsWith("/actions/unarchive/invoke")) {
            QCOMPARE(request.method, QByteArray("POST")); QVERIFY(request.body.isEmpty()); QCOMPARE(request.headers.value("content-length").toInt(), 0);
        }
    }
    void archiveNewerRealtimeAndUnrelatedFieldsWinOverDelayedResponse() {
        ArchiveFixture test; test.postDelay = 180; QVERIFY(test.start()); auto &c = test.fixture.controller;
        c.unarchiveChat(archiveId); QTRY_COMPARE(test.posts, 1);
        auto newer = test.archived; newer["is_archived"] = true; newer["updated_at"] = "2026-01-02T10:02:00.123456Z";
        newer["name"] = "Concurrent rename"; newer["unread_count"] = 7; newer["active_unread_count"] = 7; newer["notification_mode"] = "muted";
        QVERIFY(c.applyRealtimeEvent(controllerEvent("stream.updated", newer)));
        QTRY_VERIFY(!archivedChat(c).value("pending").toBool()); QCOMPARE(c.archivedChats().size(), 1); QCOMPARE(c.streams().size(), 1);
        QCOMPARE(c.archivedChats().first().toMap().value("name").toString(), QString("Concurrent rename"));
        newer["is_archived"] = false; newer["updated_at"] = "2026-01-02T10:03:00Z"; QVERIFY(c.applyRealtimeEvent(controllerEvent("stream.updated", newer)));
        QCOMPARE(c.archivedChats().size(), 0); const auto row = c.streams().last().toMap(); QCOMPARE(row.value("unread").toLongLong(), 7); QCOMPARE(row.value("name").toString(), QString("Concurrent rename"));
    }
    void archiveDispatchAndRepliesAreFencedByOwnerTrustAndAccess_data() {
        QTest::addColumn<QString>("change"); QTest::addColumn<bool>("beforeDispatch");
        for (const auto &change : {"logout", "token", "refresh", "acl", "delete", "cache"})
            for (bool before : {false,true}) QTest::newRow(qPrintable(QString(change)+(before?"-dispatch":"-reply"))) << QString(change) << before;
    }
    void archiveDispatchAndRepliesAreFencedByOwnerTrustAndAccess() {
        QFETCH(QString, change); QFETCH(bool, beforeDispatch); ArchiveFixture test; test.postDelay = 120;
        test.applyPost = false; test.postDto = [](QJsonObject &row) { row["is_archived"] = false; row["updated_at"] = "2026-01-02T10:01:00Z"; };
        QVERIFY(test.start()); auto &c = test.fixture.controller;
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        bool changed = false;
        const auto mutate = [&] {
            if (changed) return; changed = true;
            if (change == "logout") c.logout();
            else if (change == "token") test.fixture.api.setAccessToken("replacement-generation");
            else if (change == "refresh") c.refresh();
            else if (change == "cache") c.useCache(&cache);
            else if (change == "delete") QVERIFY(c.applyRealtimeEvent(controllerEvent("stream.deleted", {{"uuid", archiveId}})));
            else { auto row = test.archived; row["role"] = "member"; row["is_archived"] = true; QVERIFY(c.applyRealtimeEvent(controllerEvent("stream.updated", row))); }
        };
        QMetaObject::Connection observer;
        if (beforeDispatch) observer = connect(&c, &NativeController::streamsChanged, &c, mutate);
        c.unarchiveChat(archiveId);
        if (beforeDispatch) { disconnect(observer); QVERIFY(changed); QCOMPARE(test.posts, 0); }
        else { QTRY_COMPARE(test.posts, 1); mutate(); }
        // Wait until handled HTTP sockets settle; canceled generations may
        // close early. Idle keep-alive connections are not pending responses.
        QTRY_VERIFY(([&] { const auto sockets = test.fixture.server.findChildren<QTcpSocket *>();
            return std::none_of(sockets.cbegin(), sockets.cend(), [](QTcpSocket *socket) {
                return socket->property("handled").toBool() && socket->state() == QAbstractSocket::ConnectedState;
            }); })());
        if (change == "delete") QVERIFY(c.archivedChats().isEmpty());
        if (change == "logout") { QVERIFY(c.archivedChats().isEmpty()); QVERIFY(c.streams().isEmpty()); }
        for (const auto &row : c.streams()) QVERIFY(row.toMap().value("id").toString() != archiveId);
    }
    void archiveTopicActionsRemainVerifiedAndAccessLossStopsThem() {
        ArchiveFixture test; auto &c = test.fixture.controller;
        auto row = message("old text"); row["stream_uuid"] = archiveId; row["read"] = false;
        row["reactions"] = QJsonObject{}; row["reaction_users"] = QJsonObject{};
        QJsonObject sent; int sends = 0, edits = 0, deletes = 0, reads = 0, reacts = 0, reopens = 0;
        test.history = [&](const Request &) { return Response{QJsonArray{row}}; };
        test.fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path.endsWith("/actions/read_up_to/invoke")) { ++reads; row["read"] = true; return {row}; }
            if (request.method == "POST" && path.endsWith("/messages/")) {
                ++sends; sent = message(request.body.value("payload").toObject().value("content").toString(), request.body.value("uuid").toString());
                sent["stream_uuid"] = archiveId; return {sent, 201};
            }
            if (request.method == "PUT") { ++edits; row["payload"] = request.body.value("payload"); return {row}; }
            if (request.method == "DELETE") { ++deletes; return {QJsonObject{}}; }
            return {path.endsWith(messageId) ? row : sent};
        };
        const QJsonObject fact{{"uuid", returnedId}, {"project_id", projectId}, {"message_uuid", messageId},
            {"user_uuid", userId}, {"emoji_name", "thumbs_up"}, {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        test.fixture.reactions = [&](const Request &request) -> Response {
            if (request.method == "POST") { ++reacts; row["reactions"] = QJsonObject{{"thumbs_up",1}}; row["reaction_users"] = QJsonObject{{"thumbs_up",QJsonArray{userId}}}; return {fact,201}; }
            return {reacts ? QJsonArray{fact} : QJsonArray{}};
        };
        test.fixture.topicActions = [&](const Request &) { ++reopens; test.closed = false; return Response{creationTopic(topicId, archiveId, "Archived topic")}; };
        FileClient files(&test.fixture.api); c.useFiles(&files); QVERIFY(test.start());
        c.openArchivedChat(archiveId); QTRY_VERIFY(!c.busy()); QVERIFY(!c.canAttach()); QVERIFY(!c.canSend());
        QVERIFY(!c.canCreateTopicIn(archiveId)); c.selectTopic(topicId); QTRY_VERIFY(!c.busy());
        c.setDraft("new text"); QVERIFY(c.canSend()); QVERIFY(c.canAttach());
        c.markVisibleMessagesRead({messageId}); QTRY_COMPARE(reads, 1); QTRY_VERIFY(modelMessage(c,messageId).value("read").toBool());
        c.toggleReaction(messageId,"thumbs_up"); QTRY_COMPARE(reacts,1); QTRY_VERIFY(!modelMessage(c,messageId).value("reactionBusy").toBool());
        c.editMessage(messageId,"edited text"); QTRY_COMPARE(edits,1); QTRY_COMPARE(modelMessage(c,messageId).value("content").toString(),QString("edited text"));
        c.send(); QTRY_COMPARE(sends,1); QTRY_VERIFY(!c.busy()); QVERIFY(c.draft().isEmpty());
        c.deleteMessage(messageId); QTRY_COMPARE(deletes,1); QTRY_VERIFY(modelMessage(c,messageId).isEmpty());
        test.closed = true; auto closed = creationTopic(topicId,archiveId,"Archived topic"); closed["is_done"] = true;
        QVERIFY(c.applyRealtimeEvent(controllerEvent("topic.updated",closed))); c.setDraft("closed draft");
        QVERIFY(!c.canSend()); QVERIFY(!c.canAttach()); QVERIFY(c.canReopenTopic()); c.reopenTopic(); QTRY_COMPARE(reopens,1); QTRY_VERIFY(!c.topicClosed());
        QVERIFY(c.canSend()); QVERIFY(c.canAttach());
        QVERIFY(c.applyRealtimeEvent(controllerEvent("stream.deleted",{{"uuid",archiveId}})));
        QVERIFY(c.selectedStream().isEmpty()); QVERIFY(!c.canSend()); QVERIFY(!c.canAttach()); QVERIFY(c.archivedChats().isEmpty());
        c.send(); c.reopenTopic(); c.deleteMessage(messageId); c.toggleReaction(messageId,"thumbs_up"); c.markVisibleMessagesRead({messageId});
        QCOMPARE(sends,1); QCOMPARE(reopens,1); QCOMPARE(deletes,1); QCOMPARE(reacts,1); QCOMPARE(reads,1);
    }
    void archiveAttachmentsAndDownloadsKeepTheirVerifiedStream() {
        ArchiveFixture test; FileClient files(&test.fixture.api); auto &c = test.fixture.controller; c.useFiles(&files);
        QTemporaryDir directory; QFile input(directory.filePath("report.txt")); QVERIFY(input.open(QIODevice::WriteOnly)); QCOMPARE(input.write("{}"),2); input.close();
        int uploads = 0, downloads = 0;
        test.fixture.files = [&](const Request &request) -> Response {
            if (request.target.path().endsWith("/actions/download")) { ++downloads; return {QJsonObject{}}; }
            if (request.method == "POST") ++uploads;
            return {QJsonObject{{"uuid",returnedId},{"stream_uuid",archiveId},{"name","report.txt"},{"content_type","text/plain"},
                {"size_bytes",2},{"hash",QString::fromLatin1(QCryptographicHash::hash("{}",QCryptographicHash::Sha256).toHex())}}};
        };
        QVERIFY(test.start()); c.openArchivedChat(archiveId); QTRY_VERIFY(!c.busy());
        c.attachFiles({QUrl::fromLocalFile(input.fileName())}); QCOMPARE(uploads,0);
        c.selectTopic(topicId); QTRY_VERIFY(!c.busy()); QVERIFY(c.canAttach()); c.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QTRY_VERIFY(!c.fileBusy()); QCOMPARE(uploads,1); QVERIFY(c.draft().contains("urn:file:"+returnedId));
        const auto destination=directory.filePath("saved.txt"); c.downloadFile(returnedId,QUrl::fromLocalFile(destination)); QTRY_VERIFY(!c.fileBusy());
        QCOMPARE(downloads,1); QFile output(destination); QVERIFY(output.open(QIODevice::ReadOnly)); QCOMPARE(output.readAll(),QByteArray("{}"));
    }
    void archiveCatalogAndRestoreSurviveTheExistingAsyncCacheRestart() {
        QTemporaryDir directory; ArchiveFixture test;
        NativeCacheService first(directory.path(),{},true); test.fixture.controller.useCache(&first); QVERIFY(test.start()); auto &c = test.fixture.controller;
        QVERIFY(flushCache(first)); auto entry=cacheEntry(first,first.lease(),{NativeCache::Kind::Streams,{},{},{}}); QVERIFY(entry);
        QVERIFY(std::any_of(entry->rows.cbegin(),entry->rows.cend(),[](const QJsonValue &v){return v.toObject().value("uuid")==archiveId && v.toObject().value("is_archived").toBool();}));
        c.unarchiveChat(archiveId); QTRY_VERIFY(c.archivedChats().isEmpty()); QVERIFY(flushCache(first));
        entry=cacheEntry(first,first.lease(),{NativeCache::Kind::Streams,{},{},{}}); QVERIFY(entry);
        bool found=false; for(const auto &v:entry->rows) if(v.toObject().value("uuid")==archiveId){found=true; QVERIFY(!v.toObject().value("is_archived").toBool()); QVERIFY(v.toObject().value("updated_at").toString().startsWith("2026-01-02T10:01:00"));} QVERIFY(found);
        c.useCache(nullptr); first.shutdown();
        NativeCacheService second(directory.path(),{},true); ApiClient api(nullptr,true); Session session(&api); NativeController restored(&api,&session); restored.useCache(&second);
        restored.login(test.fixture.server.origin().toString(),"user","password",""); QTRY_COMPARE(restored.projects().size(),1); restored.openProject(projectId);
        QTRY_COMPARE(restored.streams().size(),2); QVERIFY(restored.archivedChats().isEmpty()); QVERIFY(flushCache(second));
        QCOMPARE(test.posts,1);
    }
    void archiveTrustChangeNotifiesThePublishedRowGate() {
        ArchiveFixture test; QVERIFY(test.start()); auto &c=test.fixture.controller;
        QVERIFY(archivedChat(c).value("canUnarchive").toBool());
        const auto property=c.metaObject()->property(c.metaObject()->indexOfProperty("archivedChats"));
        QSignalSpy changes(&c,property.notifySignal()); QVERIFY(changes.isValid());
        test.fixture.api.setAccessToken("replacement-generation");
        QVERIFY(!archivedChat(c).value("canUnarchive").toBool()); QVERIFY(changes.count()>0);
    }
    void archiveUncertainRowReconcilesAgainAfterTheSameOwnerRefresh() {
        ArchiveFixture test; test.applyPost=false; test.postStatus=test.getStatus=503; QVERIFY(test.start()); auto &c=test.fixture.controller;
        c.unarchiveChat(archiveId); QTRY_COMPARE(test.gets,1); QTRY_VERIFY(!archivedChat(c).value("pending").toBool());
        test.fixture.api.setAccessToken("replacement-generation"); c.refresh(); QTRY_VERIFY(c.canUnarchiveChat(archiveId));
        test.getStatus=test.postStatus=200; test.applyPost=true; c.unarchiveChat(archiveId);
        QTRY_VERIFY(c.archivedChats().isEmpty()); QCOMPARE(test.posts,2); QCOMPARE(test.gets,2);
    }
    void archiveDirectLabelsUseStoredFallbackAndCurrentDirectoryNotifications() {
        ArchiveFixture test; test.archived["direct_user_uuid"]=channelMemberA; test.archived["private"]=true;
        QVERIFY(test.start()); auto &c=test.fixture.controller; QCOMPARE(archivedChat(c).value("name").toString(),QString("Archived channel"));
        QSignalSpy changes(&c,&NativeController::archivedChatsChanged);
        QVERIFY(c.applyRealtimeEvent(controllerEvent("user.updated",{{"uuid",channelMemberA},{"username","alice"},{"first_name","Alice"}})));
        QCOMPARE(archivedChat(c).value("name").toString(),QString("Alice")); QVERIFY(changes.count()>0);
        c.logout(); QVERIFY(c.archivedChats().isEmpty());
    }
    void archiveSavedRowsAreVisibleBeforeTheDelayedCatalogButNeverMutable() {
        ArchiveFixture test; QTemporaryDir directory;
        { NativeCache seed(directory.path(),{},true); QVERIFY(seed.open(test.fixture.server.origin(),projectId,userId));
          QVERIFY(seed.put(seed.lease(),{NativeCache::Kind::Streams,{},{},{}},{creationChannel(streamId),test.archived})); }
        NativeCacheService cache(directory.path(),{},true); auto &c=test.fixture.controller; c.useCache(&cache); test.catalogDelay=600;
        c.login(test.fixture.server.origin().toString(),"user","password",""); QTRY_COMPARE(c.projects().size(),1); c.openProject(projectId);
        QTRY_VERIFY(c.cacheStale() && c.archivedChats().size()==1); QVERIFY(!c.canUnarchiveChat(archiveId));
        c.unarchiveChat(archiveId); QCOMPARE(test.posts,0); QCOMPARE(test.gets,0);
        QTRY_VERIFY(c.canUnarchiveChat(archiveId)); QCOMPARE(test.gets,0); QCOMPARE(test.posts,0);
    }
    void archiveRealtimeMovesTheSameRowWithoutDeletingHistoryOrItsTopicRoute() {
        ArchiveFixture test; test.fixture.messages=[](const Request &){return Response{QJsonArray{message()}};};
        QTemporaryDir directory; NativeCacheService cache(directory.path(),{},true); auto &c=test.fixture.controller; c.useCache(&cache); QVERIFY(test.start());
        QVERIFY(flushCache(cache)); const NativeCache::Key history{NativeCache::Kind::History,streamId,topicId,{}};
        QVERIFY(cacheEntry(cache,cache.lease(),history)); c.setDraft("Archive retains the actual topic");
        auto changed=creationChannel(streamId,"Active channel"); changed["is_archived"]=true; changed["updated_at"]="2026-01-02T10:02:00Z";
        QVERIFY(c.applyRealtimeEvent(controllerEvent("stream.updated",changed))); QCOMPARE(c.streams().size(),0); QCOMPARE(c.archivedChats().size(),2);
        QCOMPARE(c.selectedStream(),streamId); QCOMPARE(c.selectedTopic(),topicId); QVERIFY(c.canSend()); QVERIFY(!c.canCreateTopicIn(streamId));
        const auto saved=cacheEntry(cache,cache.lease(),history); QVERIFY(saved); QVERIFY(!saved->rows.isEmpty()); QVERIFY(!saved->fresh);
        changed["is_archived"]=false; changed["updated_at"]="2026-01-02T10:03:00Z"; QVERIFY(c.applyRealtimeEvent(controllerEvent("stream.updated",changed)));
        QCOMPARE(c.streams().size(),1); QCOMPARE(c.archivedChats().size(),1); QCOMPARE(c.selectedStream(),streamId); QCOMPARE(test.posts,0);
    }
    void archiveReconciliationAccessLossCannotManufactureRows_data() {
        QTest::addColumn<int>("status"); QTest::newRow("forbidden")<<403; QTest::newRow("missing")<<404;
    }
    void archiveReconciliationAccessLossCannotManufactureRows() {
        QFETCH(int,status); ArchiveFixture test; test.postStatus=503; test.getStatus=status; QVERIFY(test.start()); auto &c=test.fixture.controller;
        c.openArchivedChat(archiveId); QTRY_VERIFY(!c.busy()); c.unarchiveChat(archiveId);
        QTRY_VERIFY(c.archivedChats().isEmpty()); QVERIFY(c.selectedStream().isEmpty()); QCOMPARE(c.streams().size(),1); QCOMPARE(test.posts,1); QCOMPARE(test.gets,1);
        c.openArchivedChat(archiveId); c.unarchiveChat(archiveId); QCOMPARE(test.posts,1); QVERIFY(c.selectedStream().isEmpty());
    }
    void archiveUncertainRecoverySurvivesTerminalMapPressure() {
        ArchiveFixture test; test.applyPost=false; test.postStatus=test.getStatus=503;
        QStringList extra;
        for(int n=1;n<=128;++n) { const auto id=QString("92929292-9292-4292-8292-%1").arg(n,12,16,QLatin1Char('0'));
            auto row=creationChannel(id,"Archived"); row["is_archived"]=true; test.additional.insert(id,row); extra.append(id); }
        QVERIFY(test.start()); auto &c=test.fixture.controller;
        c.unarchiveChat(archiveId); QTRY_COMPARE(test.gets,1); QTRY_VERIFY(!archivedChat(c).value("pending").toBool());
        test.postStatus=403;
        for(const auto &id:extra) { c.unarchiveChat(id); QTRY_VERIFY(!archivedChat(c,id).value("pending").toBool()); }
        QCOMPARE(test.posts,129); test.postStatus=test.getStatus=200; test.applyPost=true;
        c.unarchiveChat(archiveId); QTRY_VERIFY(archivedChat(c).isEmpty()); QCOMPARE(test.posts,130); QCOMPARE(test.gets,2);
    }
    void archiveUnresolvedCapacityNeverDropsAnUnknownOutcome() {
        ArchiveFixture test; test.applyPost=false; test.postStatus=test.getStatus=503;
        QStringList extra;
        for(int n=1;n<=128;++n) { const auto id=QString("93939393-9393-4393-8393-%1").arg(n,12,16,QLatin1Char('0'));
            auto row=creationChannel(id,"Archived"); row["is_archived"]=true; test.additional.insert(id,row); extra.append(id); }
        QVERIFY(test.start()); auto &c=test.fixture.controller;
        c.unarchiveChat(archiveId); QTRY_COMPARE(test.gets,1); QTRY_VERIFY(!archivedChat(c).value("pending").toBool());
        for(int n=0;n<127;++n) { c.unarchiveChat(extra[n]); QTRY_VERIFY(!archivedChat(c,extra[n]).value("pending").toBool()); }
        QCOMPARE(test.posts,128); QCOMPARE(test.gets,128);
        QVERIFY(!c.canUnarchiveChat(extra.last())); QVERIFY(!archivedChat(c,extra.last()).value("canUnarchive").toBool());
        c.unarchiveChat(extra.last()); QCOMPARE(test.posts,128); QCOMPARE(test.gets,128);
        // Full capacity must not prevent reconciliation of an existing row.
        QVERIFY(c.canUnarchiveChat(archiveId)); test.getStatus=test.postStatus=200; test.applyPost=true;
        c.unarchiveChat(archiveId); QTRY_VERIFY(archivedChat(c).isEmpty()); QCOMPARE(test.gets,129); QCOMPARE(test.posts,129);
        QVERIFY(c.canUnarchiveChat(extra.last())); QVERIFY(archivedChat(c,extra.last()).value("canUnarchive").toBool());
    }
    void archiveTargetAuthorityCannotRejoinAnOldReplyAfterAba() {
        ArchiveFixture test; test.applyPost=false; auto &c=test.fixture.controller;
        bool changed=false, admitted=false;
        test.postDto=[&](QJsonObject &canonical) {
            canonical["is_archived"]=false; canonical["updated_at"]="2026-01-02T10:01:00Z";
            if(changed)return;
            changed=true;
            auto altered=test.archived; altered["role"]="member";
            // Real HTTP has already admitted the command. Both owner-scoped
            // events arrive before its queued response, without a newer date
            // masking the authority lifetime that this test exercises.
            const bool lost=c.applyRealtimeEvent(controllerEvent("stream.updated",altered,31));
            const bool restored=c.applyRealtimeEvent(controllerEvent("stream.updated",test.archived,32));
            admitted=lost && restored;
        };
        QVERIFY(test.start()); c.unarchiveChat(archiveId);
        QTRY_VERIFY(changed); QVERIFY(admitted);
        QTRY_VERIFY(([&] { const auto sockets=test.fixture.server.findChildren<QTcpSocket *>();
            return std::none_of(sockets.cbegin(),sockets.cend(),[](QTcpSocket *socket) {
                return socket->property("handled").toBool() && socket->state()==QAbstractSocket::ConnectedState;
            }); })());
        QVERIFY2(!archivedChat(c).isEmpty(),"An authority tuple restored after ABA accepted the old canonical reply");
        QCOMPARE(test.posts,1); QCOMPARE(test.gets,0);
        QVERIFY(c.canUnarchiveChat(archiveId)); test.applyPost=true;
        c.unarchiveChat(archiveId); QTRY_VERIFY(archivedChat(c).isEmpty());
        QCOMPARE(test.gets,1); QCOMPARE(test.posts,2);
    }
    void archiveRejectedProjectionCannotKeepThePreviousCatalogFresh_data() {
        QTest::addColumn<bool>("requiredFull");
        QTest::newRow("optional-full")<<false;QTest::newRow("both-queues-full")<<true;
    }
    void archiveRejectedProjectionCannotKeepThePreviousCatalogFresh() {
        QFETCH(bool,requiredFull);ArchiveFixture test; QTemporaryDir directory;
        QSemaphore release; std::atomic<bool> entered=false,block=false;
        NativeCacheService cache(directory.path(),{},true,[&] {
            if(block.exchange(false)) {entered=true;release.acquire();}
            return QDateTime::currentMSecsSinceEpoch();
        });
        auto &c=test.fixture.controller;c.useCache(&cache);QVERIFY(test.start());QVERIFY(flushCache(cache));
        const NativeCache::Key catalog{NativeCache::Kind::Streams,{},{},{}};
        const auto before=cacheEntry(cache,cache.lease(),catalog);QVERIFY(before);QVERIFY(before->fresh);
        auto unblock=qScopeGuard([&]{release.release();});block=true;
        QVERIFY(cache.put(cache.lease(),{NativeCache::Kind::Users,{},{},{}},{QJsonObject{{"uuid",userId},{"username","self"}}}));
        QTRY_VERIFY(entered.load());
        int completed=0;
        for(int n=0;n<NativeCacheService::MaximumQueuedJobs-1;++n)
            QVERIFY(cache.put(cache.lease(),{NativeCache::Kind::History,streamId,topicId,QByteArray::number(n)},
                              {message()}, {}, &c,[&](bool){++completed;}));
        int cleaned=0;
        if(requiredFull)for(int n=0;n<64;++n)
            QVERIFY(cache.invalidate(cache.lease(),NativeCache::Kind::History,archiveId,returnedId,&c,[&](bool okay){if(okay)++cleaned;}));
        c.unarchiveChat(archiveId);QTRY_VERIFY(c.archivedChats().isEmpty());
        QCOMPARE(test.posts,1);QCOMPARE(test.gets,0);
        if(requiredFull)QVERIFY(!cache.markStale(cache.lease(),NativeCache::Kind::Streams));
        QVERIFY2(!cache.put(cache.lease(),{NativeCache::Kind::History,streamId,topicId,"full-probe"},{message()}),
                 "The canonical local projection must meet the actual full optional queue");
        release.release();unblock.dismiss();QTRY_COMPARE(completed,NativeCacheService::MaximumQueuedJobs-1);
        if(requiredFull)QTRY_COMPARE(cleaned,64);
        QVERIFY(flushCache(cache));
        const auto saved=cacheEntry(cache,cache.lease(),catalog);QVERIFY(saved);
        QVERIFY2(!saved->fresh,"A rejected canonical local projection left the previous catalog authoritative");
    }
    void archiveProjectedCatalogCannotBecomeFreshWhenStaleAdmissionIsFull() {
        ArchiveFixture test; QTemporaryDir directory;
        QSemaphore release; std::atomic<bool> entered=false,block=false;
        NativeCacheService cache(directory.path(),{},true,[&] {
            if(block.exchange(false)) {entered=true;release.acquire();}
            return QDateTime::currentMSecsSinceEpoch();
        });
        auto &c=test.fixture.controller;c.useCache(&cache);QVERIFY(test.start());QVERIFY(flushCache(cache));
        auto unblock=qScopeGuard([&]{release.release();});block=true;
        QVERIFY(cache.put(cache.lease(),{NativeCache::Kind::Users,{},{},{}},{QJsonObject{{"uuid",userId},{"username","self"}}}));
        QTRY_VERIFY(entered.load());
        int cleaned=0;
        for(int n=0;n<64;++n)QVERIFY(cache.invalidate(cache.lease(),NativeCache::Kind::History,streamId,topicId,&c,[&](bool okay){if(okay)++cleaned;}));
        QSignalSpy pressure(&cache,&NativeCacheService::errorChanged);
        c.unarchiveChat(archiveId);QTRY_VERIFY(c.archivedChats().isEmpty());
        QCOMPARE(test.posts,1);QCOMPARE(test.gets,0);
        // The fixed path does not need another control slot. Prove the same
        // required queue is still full with a separate rejected probe.
        QVERIFY(!cache.markStale(cache.lease(),NativeCache::Kind::Streams));
        QVERIFY2(pressure.size()>0,"The actual stale control must meet a full required queue");
        QVERIFY(cache.error().contains("cleanup queue is busy"));
        release.release();unblock.dismiss();QTRY_COMPARE(cleaned,64);QVERIFY(flushCache(cache));
        const auto saved=cacheEntry(cache,cache.lease(),{NativeCache::Kind::Streams,{},{},{}});QVERIFY(saved);
        bool projected=false;
        for(const auto &value:saved->rows)if(value.toObject()["uuid"].toString()==archiveId){projected=true;QVERIFY(!value.toObject()["is_archived"].toBool());}
        QVERIFY2(projected,"The actual canonical local projection must have reached the cache");
        QVERIFY2(!saved->fresh,"An admitted local projection became a fresh catalog when its separate stale control was rejected");
    }
    void archiveAccessCleanupCannotCrossARealSynchronousCacheError() {
        ArchiveFixture test; test.postStatus=503; test.getStatus=403; test.applyPost=false;
        QTemporaryDir firstDirectory,secondDirectory; QSemaphore release; std::atomic<bool> entered=false,block=false;
        NativeCacheService first(firstDirectory.path(),{},true,[&] {
            if(block.exchange(false)) { entered=true; release.acquire(); }
            return QDateTime::currentMSecsSinceEpoch();
        });
        NativeCacheService second(secondDirectory.path(),{},true); auto &c=test.fixture.controller; c.useCache(&first); QVERIFY(test.start()); QVERIFY(flushCache(first));
        const NativeCache::Key savedHistory{NativeCache::Kind::History,archiveId,topicId,{}};
        { NativeCache seed(secondDirectory.path(),{},true); QVERIFY(seed.open(test.fixture.server.origin(),projectId,userId));
          auto row=message(); row["stream_uuid"]=archiveId; QVERIFY(seed.put(seed.lease(),savedHistory,{row})); }
        auto unblock=qScopeGuard([&]{release.release();}); block=true;
        QVERIFY(first.put(first.lease(),{NativeCache::Kind::Users,{},{},{}},{QJsonObject{{"uuid",userId},{"username","self"}}}));
        QTRY_VERIFY(entered.load());
        for(int n=0;n<64;++n) QVERIFY(first.invalidate(first.lease(),NativeCache::Kind::History,streamId,topicId,&c,[](bool){}));
        bool switched=false;
        const auto observer=connect(&first,&NativeCacheService::errorChanged,&c,[&] {
            if(switched)return; switched=true; c.useCache(&second); c.selectStream(streamId);
        });
        c.unarchiveChat(archiveId); QTRY_VERIFY(switched); disconnect(observer); QTRY_VERIFY(second.available());
        QVERIFY(flushCache(second)); const auto retained=cacheEntry(second,second.lease(),savedHistory);
        QVERIFY2(retained.has_value(),"The old access-failure cleanup erased history in the replacement cache");
        QCOMPARE(c.selectedStream(),streamId); QCOMPARE(test.posts,1); QCOMPARE(test.gets,1);
    }
    void archiveCatalogKeepsTheActiveProjectionStable() {
        ArchiveFixture test; QVERIFY(test.start()); auto &c = test.fixture.controller;
        QCOMPARE(c.streams().size(), 1); QTRY_COMPARE(c.archivedChats().size(), 1);
        const auto row = c.archivedChats().first().toMap(); QCOMPARE(row.value("id").toString(), archiveId);
        QCOMPARE(row.value("name").toString(), QString("Archived channel")); QVERIFY(!row.value("isDirect").toBool());
        QVERIFY(c.canUnarchiveChat(archiveId)); QCOMPARE(test.posts, 0); QCOMPARE(test.gets, 0);
    }
    void archiveOpensTheExactAggregateWithoutMutation() {
        ArchiveFixture test; QVERIFY(test.start()); auto &c = test.fixture.controller;
        c.openArchivedChat(archiveId); QCOMPARE(c.selectedStream(), archiveId); QCOMPARE(c.conversationKind(), QString("stream"));
        QVERIFY(c.selectedTopic().isEmpty()); QTRY_VERIFY(!c.busy()); QVERIFY(!c.canSend()); QVERIFY(!c.canAttach());
        QCOMPARE(test.posts, 0); QVERIFY(!c.messages().isEmpty());
    }
    void archiveRestoresOnlyAfterTheCanonicalPost() {
        ArchiveFixture test; test.postDelay = 120; QVERIFY(test.start()); auto &c = test.fixture.controller;
        c.unarchiveChat(archiveId); c.unarchiveChat(archiveId); QTRY_COMPARE(test.posts, 1);
        QCOMPARE(c.archivedChats().size(), 1); QVERIFY(c.archivedChats().first().toMap().value("pending").toBool());
        QTRY_COMPARE(c.archivedChats().size(), 0); QCOMPARE(c.streams().size(), 2);
        QCOMPARE(c.selectedStream(), streamId); QCOMPARE(c.selectedTopic(), topicId); QCOMPARE(test.gets, 0);
    }

    void topicCreationPublicWitnessesBindOnlyTheVerifiedOwnersIntent()
    {
        TopicCreationFixture test;
        auto &controller = test.fixture.controller;
        QVERIFY(controller.property("topicCreationAttemptId").toString().isEmpty());
        QVERIFY(controller.property("topicCreationContextKey").toString().isEmpty());
        QVERIFY(test.fixture.start());
        const auto key = controller.property("topicCreationContextKey").toString();
        QCOMPARE(key.size(), 64);
        controller.createTopic(streamId, " ");
        QVERIFY(controller.property("topicCreationAttemptId").toString().isEmpty());
        QCOMPARE(test.posts, 0);
        test.postStatus = 503;
        controller.createTopic(streamId, "Exact topic identity");
        const auto attempt = controller.property("topicCreationAttemptId").toString();
        QVERIFY(isUuid(attempt));
        QCOMPARE(controller.property("topicCreationContextKey").toString(), key);
        QTRY_VERIFY(controller.topicCreationUncertain());
        QCOMPARE(test.posts, 1); QCOMPARE(test.created.value("uuid").toString(), attempt);
        QCOMPARE(controller.property("topicCreationAttemptId").toString(), attempt);
        controller.checkTopicCreation();
        QTRY_VERIFY(!controller.topicCreationBusy());
        QVERIFY(!controller.topicCreationUncertain());
        QCOMPARE(controller.property("topicCreationAttemptId").toString(), attempt);
        QCOMPARE(test.posts, 1); QCOMPARE(test.confirmations, 1); QCOMPARE(test.catalogs, 1);
        controller.logout();
        QVERIFY(controller.property("topicCreationAttemptId").toString().isEmpty());
        QVERIFY(controller.property("topicCreationContextKey").toString().isEmpty());
        QVERIFY(test.fixture.start());
        QCOMPARE(controller.property("topicCreationAttemptId").toString(), attempt);
    }

    void topicCreationExactSignalIdentitySurvivesAnEarlierListenerStartingTheNextIntent()
    {
        TopicCreationFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller;
        bool startedNext = false, laterFormOpen = false;
        QString firstId, boundId;
        connect(&controller, &NativeController::topicCreated, &controller, [&](const QString &stream, const QString &id) {
            if (startedNext) return;
            startedNext = true; firstId = id; laterFormOpen = true;
            test.postDelay = 200;
            controller.createTopic(stream, "Same name");
            boundId = controller.property("topicCreationAttemptId").toString();
        });
        connect(&controller, &NativeController::topicCreated, &controller, [&](const QString &stream, const QString &id) {
            // The signal is already being delivered. Exact UUID matching is the
            // UI's only way to avoid closing the newly admitted same-target form.
            if (stream == streamId && id == boundId) laterFormOpen = false;
        });
        controller.createTopic(streamId, "Same name");
        QTRY_COMPARE(test.posts, 2);
        QVERIFY(startedNext); QVERIFY(isUuid(firstId)); QVERIFY(isUuid(boundId));
        QVERIFY(firstId != boundId); QVERIFY(laterFormOpen);
        QVERIFY(controller.topicCreationBusy()); QCOMPARE(controller.topicCreationName(), QString("Same name"));
    }

    void topicCreationContextIgnoresBenignRowsAndCompletedOldIntentEvents()
    {
        TopicCreationFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller;
        QSignalSpy completed(&controller, &NativeController::topicCreated);
        controller.createTopic(streamId, "Completed topic"); QTRY_COMPARE(completed.size(), 1);
        const auto key = controller.property("topicCreationContextKey").toString();
        QCOMPARE(key.size(), 64);
        auto topic = test.created; topic["name"] = "Renamed completed topic";
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("topic.updated", topic)));
        QCOMPARE(controller.property("topicCreationContextKey").toString(), key);
        auto stream = QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Benign label"},
            {"encryption", false}, {"unread_count", 7}};
        const auto acl = controller.streams().first().toMap().value("contextKey").toString();
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.updated", stream, 13)));
        QCOMPARE(controller.property("topicCreationContextKey").toString(), key);
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("user.updated", QJsonObject{{"uuid", userId}, {"username", "new-label"}}, 14)));
        QCOMPARE(controller.property("topicCreationContextKey").toString(), key);
        stream["role"] = "member";
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.updated", stream, 15)));
        QCOMPARE(controller.property("topicCreationContextKey").toString(), key);
        QVERIFY(controller.streams().first().toMap().value("contextKey").toString() != acl);
        QCOMPARE(completed.size(), 1);
    }

    void topicCreationContextChangesOnlyOnTheActualShellLifetime_data()
    {
        QTest::addColumn<QString>("change");
        for (const auto *change : {"token", "route", "catalog", "cache", "logout"})
            QTest::newRow(change) << QString::fromLatin1(change);
    }
    void topicCreationContextChangesOnlyOnTheActualShellLifetime()
    {
        QFETCH(QString, change);
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        TopicCreationFixture test; QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        const auto key = controller.property("topicCreationContextKey").toString();
        QCOMPARE(key.size(), 64);
        if (change == "token") test.fixture.api.setAccessToken("topic-lifetime-rotated-token");
        else if (change == "route") controller.selectTopic({});
        else if (change == "catalog") controller.refresh();
        else if (change == "cache") controller.useCache(&cache);
        else controller.logout();
        QVERIFY(controller.property("topicCreationContextKey").toString() != key);
        if (change == "logout") QVERIFY(controller.property("topicCreationContextKey").toString().isEmpty());
        QCOMPARE(test.posts, 0);
    }

    void topicPublicationSubscribersCanDestroyTheController_data()
    {
        QTest::addColumn<QString>("window");
        for (const auto *window : {"stream", "topic", "final"})
            QTest::newRow(window) << QString::fromLatin1(window);
    }
    void topicPublicationSubscribersCanDestroyTheController()
    {
        QFETCH(QString, window);
        TopicCreationFixture test; QVERIFY(test.fixture.start());
        QPointer<NativeController> current = new NativeController(&test.fixture.api, &test.fixture.session);
        current->refresh(); QTRY_COMPARE(current->streams().size(), 1);
        current->selectStream(streamId); QTRY_COMPARE(current->topics().size(), 1);
        current->selectTopic(topicId); QTRY_VERIFY(!current->busy());
        connect(current, &NativeController::stateChanged, &test.fixture.session, [&] {
            if (!current || test.created.isEmpty()) return;
            const bool location = window == "stream" ? current->selectedTopic().isEmpty()
                : current->selectedTopic() == test.created.value("uuid").toString();
            const bool stage = window == "final" ? !current->topicCreationBusy() : current->topicCreationBusy();
            if (location && stage) delete current.data();
        });
        current->createTopic(streamId, "Destroyed publication subscriber");
        QTRY_VERIFY(current.isNull());
        QCOMPARE(test.posts, 1); QCOMPARE(test.confirmations, 1); QCOMPARE(test.catalogs, 1);
    }

    void topicPublicationLifetimeFence_data()
    {
        QTest::addColumn<QString>("window");
        QTest::addColumn<QString>("change");
        for (const auto *window : {"stream", "topic", "final"})
            for (const auto *change : {"token", "cache", "acl"})
                QTest::newRow(qPrintable(QString("%1-%2").arg(window, change)))
                    << QString::fromLatin1(window) << QString::fromLatin1(change);
    }

    void topicPublicationLifetimeFence()
    {
        QFETCH(QString, window); QFETCH(QString, change);
        QTemporaryDir first, second;
        NativeCacheService cache(first.path(), {}, true), replacement(second.path(), {}, true);
        TopicCreationFixture test;
        if (change == "cache") test.fixture.controller.useCache(&cache);
        QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller;
        QSignalSpy completed(&controller, &NativeController::topicCreated);
        bool changed = false, accepted = true;
        connect(&controller, &NativeController::stateChanged, &controller, [&] {
            if (changed || test.created.isEmpty()) return;
            const auto id = test.created.value("uuid").toString();
            const bool location = window == "stream" ? controller.selectedStream() == streamId && controller.selectedTopic().isEmpty()
                : controller.selectedStream() == streamId && controller.selectedTopic() == id;
            const bool stage = window == "final" ? !controller.topicCreationBusy() : controller.topicCreationBusy();
            if (!location || !stage) return;
            changed = true;
            if (change == "token") test.fixture.api.setAccessToken("topic-publication-rotated-token");
            else if (change == "cache") controller.useCache(&replacement);
            else {
                auto row = creationChannel(streamId, "ACL changed during publication");
                row["role"] = "member";
                accepted = controller.applyRealtimeEvent(controllerEvent("stream.updated", row));
            }
        });
        controller.createTopic(streamId, "Publication fence");
        QTRY_VERIFY(changed); QVERIFY(accepted);
        QTRY_COMPARE(test.fixture.api.findChildren<QNetworkReply *>().size(), 0);
        QCOMPARE(test.posts, 1);
        QCOMPARE(completed.size(), 0);
        QVERIFY(!controller.topicCreationBusy()); QVERIFY(controller.topicCreationUncertain());
        const auto attempt = controller.topicCreationAttemptId();
        QCOMPARE(attempt, test.created.value("uuid").toString());
        controller.refresh(); QTRY_VERIFY(controller.canCheckTopicCreation());
        controller.checkTopicCreation(); QTRY_COMPARE(completed.size(), 1);
        QCOMPARE(controller.topicCreationAttemptId(), attempt); QCOMPARE(test.posts, 1);
    }

    void topicPublicationSameTargetReplacementDoesNotCloseLaterForm()
    {
        TopicCreationFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller;
        bool replaced = false, laterFormOpen = false;
        QString firstId;
        QSignalSpy completed(&controller, &NativeController::topicCreated);
        connect(&controller, &NativeController::topicCreated, &controller, [&](const QString &stream, const QString &) {
            // This is the actual current Main.qml completion rule: stream only.
            if (stream == streamId) laterFormOpen = false;
        });
        connect(&controller, &NativeController::stateChanged, &controller, [&] {
            if (replaced || test.created.isEmpty() || controller.topicCreationBusy()
                || controller.selectedTopic() != test.created.value("uuid").toString()) return;
            replaced = true;
            firstId = test.created.value("uuid").toString();
            laterFormOpen = true;
            test.postDelay = 200;
            controller.createTopic(streamId, "Same name");
        });
        controller.createTopic(streamId, "Same name");
        QTRY_COMPARE(test.posts, 2);
        QVERIFY(replaced); QVERIFY(!firstId.isEmpty());
        QVERIFY(controller.topicCreationBusy());
        QCOMPARE(controller.topicCreationName(), QString("Same name"));
        QCOMPARE(controller.topicCreationStream(), streamId);
        QVERIFY(firstId != test.created.value("uuid").toString());
        QVERIFY(laterFormOpen);
        QCOMPARE(completed.size(), 0);
    }

    void directChatNewPairUsesExactSourceBodyAndVerifiedAggregate()
    {
        DirectChatFixture test; auto &controller = test.fixture.controller;
        QVERIFY(!controller.canStartDirectChatWith(channelMemberA)); QVERIFY(test.fixture.start());
        QTRY_VERIFY(controller.directChatDirectoryVerified());
        QSignalSpy complete(&controller, &NativeController::directChatOpened);
        QVERIFY(controller.canStartDirectChatWith(channelMemberA));
        const auto users = test.usersGets, catalogs = test.streamGets, history = test.fixture.totalHistoryGets();
        controller.startDirectChat(channelMemberA); controller.startDirectChat(channelMemberA);
        QTRY_COMPARE(complete.size(), 1);
        QCOMPARE(controller.directChatPhase(), "ready"); QCOMPARE(test.posts, 1); QCOMPARE(test.gets, 2);
        QCOMPARE(test.topicGets, 1); QCOMPARE(test.streamGets - catalogs, 1); QCOMPARE(test.usersGets, users);
        QCOMPARE(test.bodies.first(), QJsonObject({{"name", "Direct"}, {"description", "Private workspace"},
            {"source_name", "native"}, {"direct_user_uuid", channelMemberA}}));
        QCOMPARE(controller.directChatForm().value("expectedStreamId").toString(), directId);
        QCOMPARE(controller.selectedStream(), directId); QVERIFY(controller.selectedTopic().isEmpty());
        QCOMPARE(controller.conversationKind(), "stream"); QVERIFY(!controller.canSend()); QVERIFY(!controller.canAttach());
        QVERIFY(controller.isDirectConversation()); QCOMPARE(controller.directPeer().value("id").toString(), channelMemberA);
        QCOMPARE(controller.title(), "Alice Adams"); QCOMPARE(controller.topics().size(), 1);
        QTRY_COMPARE(test.fixture.totalHistoryGets() - history, 1);
    }

    void directChatExistingPairOpensWithoutPost_data()
    {
        QTest::addColumn<QString>("variation");
        for (const auto *entry : {"ordinary", "peer-created", "renamed", "flags", "closed-default"})
            QTest::newRow(entry) << QString(entry);
    }
    void directChatExistingPairOpensWithoutPost()
    {
        QFETCH(QString, variation); DirectChatFixture test; test.seed();
        if (variation == "peer-created") test.created["owner"] = channelMemberA;
        if (variation == "renamed") { test.created["name"] = "Existing renamed conversation"; test.created["description"] = "Actual metadata"; }
        if (variation == "flags") { test.created["invite_only"] = true; test.created["announce"] = true; }
        if (variation == "closed-default") test.defaultTopic["is_done"] = true;
        QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QTRY_VERIFY(controller.directChatDirectoryVerified()); QSignalSpy done(&controller, &NativeController::directChatOpened);
        controller.startDirectChat(channelMemberA.toUpper()); QTRY_COMPARE(done.size(), 1);
        QCOMPARE(test.posts, 0); QCOMPARE(test.gets, 1); QCOMPARE(controller.title(), "Alice Adams");
        for (const auto &row : controller.streams()) if (row.toMap().value("id").toString() == directId) {
            QVERIFY(row.toMap().value("isDirect").toBool()); QCOMPARE(row.toMap().value("peerId").toString(), channelMemberA);
            QCOMPARE(row.toMap().value("displayName").toString(), "Alice Adams");
        }
        controller.selectTopic(channelDefault); QTRY_VERIFY(!controller.busy());
        QCOMPARE(controller.title(), "Alice Adams"); controller.setDraft("Actual topic draft");
        QCOMPARE(controller.canSend(), variation != "closed-default");
        QCOMPARE(controller.selectedTopic(), channelDefault); QVERIFY(controller.isDirectConversation());
    }

    void directChatDirectoryAndIdentityRejectUntrustedEntry_data()
    {
        QTest::addColumn<QString>("entry");
        for (const auto *entry : {"self", "foreign", "invalid", "empty", "unverified", "before-login"}) QTest::newRow(entry) << QString(entry);
    }
    void directChatDirectoryAndIdentityRejectUntrustedEntry()
    {
        QFETCH(QString, entry); DirectChatFixture test; auto &controller = test.fixture.controller;
        if (entry != "before-login") QVERIFY(test.fixture.start());
        if (entry == "unverified") { test.fixture.users = [](const Request &) { return Response{QJsonObject{}, 503}; }; controller.refresh(); QTRY_VERIFY(!controller.busy()); }
        const auto peer = entry == "self" ? userId : entry == "foreign" ? returnedId : entry == "invalid" ? "not-a-uuid"
            : entry == "empty" ? QString{} : channelMemberA;
        QVERIFY(!controller.canStartDirectChatWith(peer)); controller.startDirectChat(peer);
        QCOMPARE(test.gets, 0); QCOMPARE(test.posts, 0); QVERIFY(controller.directChatForm().isEmpty());
    }

    void directChatLookupCannotTurnInvalidIdentityIntoCreateAuthority_data()
    {
        QTest::addColumn<QString>("field");
        for (const auto *field : {"401", "403", "500", "uuid", "project_id", "user_uuid", "owner", "direct_user_uuid",
             "private", "source_name", "provider_uuid", "role", "encryption", "invalid-current-key",
             "nonencrypted-current-key", "created_at", "description", "default_topic_uuid"})
            QTest::newRow(field) << QString(field);
    }
    void directChatLookupCannotTurnInvalidIdentityIntoCreateAuthority()
    {
        QFETCH(QString, field); DirectChatFixture test; test.seed();
        if (field == "401" || field == "403" || field == "500") test.lookupStatus = field.toInt();
        else if (field == "private") test.created[field] = false;
        else if (field == "encryption") test.created[field] = "true"; // Malformed type; encrypted+null is now a valid pair.
        else if (field == "invalid-current-key") { test.created["encryption"] = true; test.created["current_encryption_key_uuid"] = "not-a-uuid"; }
        else if (field == "nonencrypted-current-key") test.created["current_encryption_key_uuid"] = returnedId;
        else if (field == "source_name") test.created[field] = "zulip";
        else if (field == "role") test.created[field] = "member";
        else if (field == "created_at") test.created[field] = "invalid-date";
        else if (field == "description") test.created[field] = 1;
        else test.created[field] = returnedId;
        QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QTRY_VERIFY(controller.directChatDirectoryVerified()); QSignalSpy done(&controller, &NativeController::directChatOpened);
        controller.startDirectChat(channelMemberA); QTRY_VERIFY(!controller.directChatBusy());
        QCOMPARE(done.size(), 0); QCOMPARE(test.posts, 0); QVERIFY(controller.selectedStream() != directId);
        if (field != "500" && field != "default_topic_uuid") QVERIFY(!controller.canRetryDirectChat());
        controller.retryDirectChat(); QTRY_VERIFY(!controller.directChatBusy()); QCOMPARE(test.posts, 0);
    }

    void directChatUnsupportedExistingPairNeverDowngradesOrUnarchives_data()
    {
        // Valid encrypted pairs are covered by the positive known/null-current table below.
        QTest::addColumn<bool>("encrypted"); QTest::newRow("archived") << false;
    }
    void directChatUnsupportedExistingPairNeverDowngradesOrUnarchives()
    {
        QFETCH(bool, encrypted); DirectChatFixture test; test.seed();
        if (encrypted) { test.created["encryption"] = true; test.created["current_encryption_key_uuid"] = returnedId; }
        else test.created["is_archived"] = true;
        QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller; QTRY_VERIFY(controller.directChatDirectoryVerified());
        controller.startDirectChat(channelMemberA); QTRY_COMPARE(controller.directChatPhase(), "unsupported");
        QCOMPARE(test.posts, 0); QCOMPARE(test.topicGets, 0); QVERIFY(!controller.canCheckDirectChat()); QVERIFY(!controller.canRetryDirectChat());
        QCOMPARE(controller.directChatForm().value("confirmedStreamId").toString(), directId);
        QVERIFY(controller.selectedStream() != directId);
    }

    void directChatEncryptedExistingPairPreservesCanonicalMetadata_data()
    {
        QTest::addColumn<bool>("currentKeyKnown"); QTest::addColumn<bool>("peerCreated");
        QTest::newRow("known-current-own-created") << true << false;
        QTest::newRow("null-current-own-created") << false << false;
        QTest::newRow("known-current-peer-created") << true << true;
        QTest::newRow("null-current-peer-created") << false << true;
    }
    void directChatEncryptedExistingPairPreservesCanonicalMetadata()
    {
        QFETCH(bool, currentKeyKnown); QFETCH(bool, peerCreated);
        DirectChatFixture test;
        auto pair = creationDirect(); pair["encryption"] = true;
        pair["current_encryption_key_uuid"] = currentKeyKnown ? QJsonValue(returnedId) : QJsonValue(QJsonValue::Null);
        if (peerCreated) pair["owner"] = channelMemberA;
        test.seed(pair); const auto originalPair = test.created, originalTopic = test.defaultTopic;
        auto &controller = test.fixture.controller;
        FileClient files(&test.fixture.api); controller.useFiles(&files);
        QVERIFY(test.fixture.start()); QTRY_VERIFY(controller.directChatDirectoryVerified());
        const auto catalogs = test.streamGets, users = test.usersGets;
        QSignalSpy done(&controller, &NativeController::directChatOpened); QVERIFY(done.isValid());
        controller.startDirectChat(channelMemberA);
        QTRY_COMPARE(done.size(), 1); QCOMPARE(controller.directChatPhase(), QString("ready"));
        QCOMPARE(test.gets, 1); QCOMPARE(test.posts, 0); QCOMPARE(test.topicGets, 1);
        QCOMPARE(test.streamGets - catalogs, 1); QCOMPARE(test.usersGets, users);
        QCOMPARE(done.first().at(0).toString(), directId);
        QCOMPARE(done.first().at(1).toString(), channelMemberA);
        QCOMPARE(done.first().at(2).toString(), channelDefault);
        QCOMPARE(controller.selectedStream(), directId); QVERIFY(controller.selectedTopic().isEmpty());
        QCOMPARE(controller.conversationKind(), QString("stream")); QVERIFY(controller.isDirectConversation());
        QCOMPARE(controller.directPeer().value("id").toString(), channelMemberA);
        QCOMPARE(test.created, originalPair); QCOMPARE(test.defaultTopic, originalTopic);
        QCOMPARE(controller.encryption().value("currentKeyUuid").toString(), currentKeyKnown ? returnedId : QString{});
        controller.selectTopic(channelDefault); QTRY_VERIFY(!controller.busy());
        QCOMPARE(controller.selectedTopic(), channelDefault);
        controller.setDraft("Must stay local without a Vault key");
        QVERIFY(!controller.encryption().value("available").toBool());
        QVERIFY(!controller.canSend()); QVERIFY(!controller.canAttach());
        controller.send(); controller.attachFiles({QUrl::fromLocalFile("/nonexistent/cassi-encrypted-direct-no-upload")});
        QCOMPARE(controller.draft(), QString("Must stay local without a Vault key"));
        for (const auto &request : test.fixture.server.requests)
            if (request.target.path().startsWith("/api/workspace/v1/messenger/")) QCOMPARE(request.method, QByteArray("GET"));
        QCOMPARE(test.posts, 0); QCOMPARE(test.created, originalPair); QCOMPARE(test.defaultTopic, originalTopic);
    }

    void directChatOrdinaryPostMayReturnEncryptedExistingPair_data()
    {
        QTest::addColumn<bool>("currentKeyKnown");
        QTest::newRow("known-current") << true; QTest::newRow("null-current") << false;
    }
    void directChatOrdinaryPostMayReturnEncryptedExistingPair()
    {
        QFETCH(bool, currentKeyKnown); DirectChatFixture test;
        // A pair may appear between the initial404 and the idempotent ordinary
        // create. The POST body still never requests encryption or bootstrap.
        test.alterPost = [&](QJsonObject &row) {
            row["encryption"] = true;
            row["current_encryption_key_uuid"] = currentKeyKnown ? QJsonValue(returnedId) : QJsonValue(QJsonValue::Null);
            test.created = row; // The subsequent actual GET/catalog return the same canonical encrypted pair.
        };
        auto &controller = test.fixture.controller;
        FileClient files(&test.fixture.api); controller.useFiles(&files);
        QVERIFY(test.fixture.start()); QTRY_VERIFY(controller.directChatDirectoryVerified());
        const auto catalogs = test.streamGets;
        QSignalSpy done(&controller, &NativeController::directChatOpened); QVERIFY(done.isValid());
        controller.startDirectChat(channelMemberA); QTRY_COMPARE(done.size(), 1);
        QCOMPARE(controller.directChatPhase(), QString("ready"));
        QCOMPARE(test.posts, 1); QCOMPARE(test.gets, 2); QCOMPARE(test.topicGets, 1);
        QCOMPARE(test.streamGets - catalogs, 1); QCOMPARE(test.bodies.size(), 1);
        QCOMPARE(test.bodies.first(), QJsonObject({{"name", "Direct"}, {"description", "Private workspace"},
            {"source_name", "native"}, {"direct_user_uuid", channelMemberA}}));
        QCOMPARE(controller.selectedStream(), directId); QVERIFY(controller.selectedTopic().isEmpty());
        QVERIFY(test.created.value("encryption").toBool());
        QCOMPARE(test.created.value("current_encryption_key_uuid"),
            currentKeyKnown ? QJsonValue(returnedId) : QJsonValue(QJsonValue::Null));
        int pairPosts = 0;
        for (const auto &request : test.fixture.server.requests) {
            if (!request.target.path().startsWith("/api/workspace/v1/messenger/")) continue;
            if (request.method != "GET") {
                QCOMPARE(request.method, QByteArray("POST"));
                QCOMPARE(request.target.path(), QString("/api/workspace/v1/messenger/streams/"));
                ++pairPosts;
            }
        }
        QCOMPARE(pairPosts, 1); // No member write, key request, bootstrap or message POST.
        controller.selectTopic(channelDefault); QTRY_VERIFY(!controller.busy());
        controller.setDraft("No plaintext fallback after an encrypted canonical result");
        QVERIFY(!controller.canSend()); QVERIFY(!controller.canAttach());
        const auto requestsBeforeSend = test.fixture.server.requests.size(); controller.send();
        QCOMPARE(test.fixture.server.requests.size(), requestsBeforeSend);
        QCOMPARE(controller.draft(), QString("No plaintext fallback after an encrypted canonical result"));
    }

    void directChatUnknownPostRecoversOnlyThroughExplicitActions_data()
    {
        QTest::addColumn<QString>("outcome");
        QTest::newRow("applied503") << QString("applied503"); QTest::newRow("not-applied503") << QString("not-applied503");
        QTest::newRow("malformed201") << QString("malformed201");
    }
    void directChatUnknownPostRecoversOnlyThroughExplicitActions()
    {
        QFETCH(QString, outcome); DirectChatFixture test; test.postStatus = outcome == "malformed201" ? 201 : 503;
        test.applyPost = outcome != "not-applied503";
        if (outcome == "malformed201") test.alterPost = [](QJsonObject &row) { row["uuid"] = returnedId; };
        QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller; QTRY_VERIFY(controller.directChatDirectoryVerified());
        QSignalSpy done(&controller, &NativeController::directChatOpened);
        controller.startDirectChat(channelMemberA); QTRY_VERIFY(controller.directChatUncertain());
        const auto form = controller.directChatForm(); QCOMPARE(test.posts, 1);
        controller.checkDirectChat(); QTRY_VERIFY(!controller.directChatBusy()); QCOMPARE(test.posts, 1);
        if (test.applyPost) { QCOMPARE(done.size(), 1); return; }
        QVERIFY(controller.directChatUncertain()); controller.startDirectChat(channelMemberA);
        QTRY_VERIFY(!controller.directChatBusy()); QCOMPARE(test.posts, 1); QVERIFY(controller.canRetryDirectChat());
        QCOMPARE(controller.directChatForm().value("attemptId"), form.value("attemptId"));
        test.postStatus = 201; test.applyPost = true; controller.retryDirectChat(); QTRY_COMPARE(done.size(), 1);
        QCOMPARE(test.posts, 2); QCOMPARE(test.bodies[0], test.bodies[1]);
        QCOMPARE(controller.directChatForm().value("attemptId"), form.value("attemptId"));
        QCOMPARE(controller.directChatForm().value("expectedStreamId").toString(), directId);
    }

    void directChatDefinitePostFailureAllowsOnlyANewExplicitAttempt_data()
    {
        QTest::addColumn<int>("status"); for (int status : {400, 401, 403, 404}) QTest::newRow(qPrintable(QString::number(status))) << status;
    }
    void directChatDefinitePostFailureAllowsOnlyANewExplicitAttempt()
    {
        QFETCH(int, status); DirectChatFixture test; test.postStatus = status; test.applyPost = false;
        QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller; QTRY_VERIFY(controller.directChatDirectoryVerified());
        controller.startDirectChat(channelMemberA); QTRY_COMPARE(controller.directChatPhase(), "failed");
        const auto form = controller.directChatForm(); QCOMPARE(test.posts, 1); QVERIFY(!controller.canRetryDirectChat());
        controller.checkDirectChat(); controller.retryDirectChat(); QCOMPARE(test.posts, 1);
        test.postStatus = 201; test.applyPost = true; controller.startDirectChat(channelMemberA);
        QTRY_COMPARE(controller.directChatPhase(), "ready"); QCOMPARE(test.posts, 2);
        QVERIFY(controller.directChatForm().value("attemptId") != form.value("attemptId"));
        QCOMPARE(controller.directChatForm().value("expectedStreamId"), form.value("expectedStreamId"));
    }

    void directChatAndChannelShareOneMutationAdmissionGate()
    {
        DirectChatFixture direct; QVERIFY(direct.fixture.start()); auto &controller = direct.fixture.controller;
        QTRY_VERIFY(controller.directChatDirectoryVerified()); direct.postDelay = 150;
        controller.startDirectChat(channelMemberA); QTRY_COMPARE(direct.posts, 1);
        QVERIFY(!controller.canCreateChannel()); controller.createChannel("Do not overlap", "", false, false, {});
        QVERIFY(controller.channelCreationForm().isEmpty()); QTRY_COMPARE(controller.directChatPhase(), "ready");
        ChannelCreationFixture channel; QVERIFY(channel.fixture.start()); QTRY_VERIFY(channel.fixture.controller.channelMembersVerified());
        channel.postDelay = 150; channel.fixture.controller.createChannel("One flow", "", false, false, {}); QTRY_COMPARE(channel.posts, 1);
        QVERIFY(!channel.fixture.controller.canStartDirectChatWith(channelMemberA));
        channel.fixture.controller.startDirectChat(channelMemberA); QVERIFY(channel.fixture.controller.directChatForm().isEmpty());
        QTRY_COMPARE(channel.fixture.controller.channelCreationPhase(), "ready");
    }

    void directChatPublicationCannotCompleteAReentrantReplacementIntent()
    {
        DirectChatFixture test; QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QTRY_VERIFY(controller.directChatDirectoryVerified()); QSignalSpy done(&controller, &NativeController::directChatOpened);
        bool replaced = false; QVariantMap replacement;
        connect(&controller, &NativeController::stateChanged, &controller, [&] {
            if (replaced || controller.selectedStream() != directId || controller.directChatBusy()) return;
            replaced = true; test.lookupStatus = 503; test.getDelay = 150;
            controller.startDirectChat(channelMemberB); replacement = controller.directChatForm();
        });
        controller.startDirectChat(channelMemberA); QTRY_VERIFY(replaced);
        QCOMPARE(controller.directChatForm().value("peerUuid").toString(), channelMemberB);
        QCOMPARE(controller.directChatPhase(), "looking-up"); QVERIFY(controller.directChatBusy());
        QCOMPARE(controller.directChatForm().value("attemptId"), replacement.value("attemptId"));
        QTRY_VERIFY(!controller.directChatBusy()); QVERIFY(controller.directChatUncertain()); QCOMPARE(done.size(), 0);
        QCOMPARE(test.posts, 1);
    }

    void directChatReadEventsCannotPublishTheEarlierSavedTopicsSnapshot()
    {
        DirectChatFixture test; QVERIFY(test.fixture.start()); test.seed();
        test.defaultTopic["unread_count"] = 7; test.defaultTopic["active_unread_count"] = 7;
        auto &controller = test.fixture.controller; QTRY_VERIFY(controller.directChatDirectoryVerified());
        bool read = false; QSignalSpy done(&controller, &NativeController::directChatOpened);
        connect(&controller, &NativeController::stateChanged, &controller, [&] {
            if (read || controller.directChatPhase() != "refreshing-catalogs") return;
            read = true; test.defaultTopic["unread_count"] = 0; test.defaultTopic["active_unread_count"] = 0;
            QVERIFY(controller.applyRealtimeEvent(controllerEvent("topic.read", test.defaultTopic)));
        });
        controller.startDirectChat(channelMemberA); QTRY_COMPARE(done.size(), 1); QVERIFY(read);
        QCOMPARE(controller.topics().first().toMap().value("unread").toLongLong(), 0);
        QCOMPARE(test.posts, 0); QVERIFY(test.topicGets >= 2); QVERIFY(test.topicGets <= 4);
    }

    void directChatCanonicalPairMatchesBackendVectorsAndReciprocalViewer()
    {
        QCOMPARE(expectedDirectId(channelMemberA), directId);
        QCOMPARE(expectedDirectId(userId, channelMemberA), directId);
        QCOMPARE(expectedDirectId(channelMemberA, userId, returnedId), QString("c002ebce-130a-571f-ab1d-9dc19b58dd7f"));
        for (const auto &identity : {qMakePair(channelMemberA, userId), qMakePair(userId, returnedId)}) {
            DirectChatFixture test; const auto viewer = identity.first;
            const auto project = identity.second == userId ? projectId : returnedId;
            const auto peer = identity.second == userId ? userId : channelMemberA;
            const auto expected = expectedDirectId(peer, viewer, project);
            const auto fallback = test.fixture.server.handler;
            test.fixture.server.handler = [&, fallback](const Request &request) {
                auto response = fallback(request); const auto path = request.target.path();
                if (path.endsWith("/actions/introspect")) {
                    auto row = response.data.toObject(); row["project_id"] = project; row["user_info"] = QJsonObject{{"uuid", viewer}}; response.data = row;
                } else if (path == "/api/core/v1/iam/projects/") response.data = QJsonArray{QJsonObject{{"uuid", project}, {"name", "Project"}, {"status", "ACTIVE"}}};
                else if (path.endsWith("/streams/") && request.method == "GET") {
                    auto row = creationChannel(streamId); row["project_id"] = project; row["user_uuid"] = viewer; response.data = QJsonArray{row};
                } else if (path.endsWith("/users/")) response.data = QJsonArray{QJsonObject{{"uuid", peer}, {"username", "peer"}}};
                else if (path == "/api/workspace/v1/messenger/streams/" + expected) response = {QJsonObject{}, 503};
                return response;
            };
            auto &controller = test.fixture.controller;
            controller.login(test.fixture.server.origin().toString(), "synthetic", "synthetic", "");
            QTRY_COMPARE(controller.projects().size(), 1); controller.openProject(project);
            QTRY_VERIFY(controller.directChatDirectoryVerified()); QTRY_VERIFY(controller.canStartDirectChatWith(peer));
            controller.startDirectChat(peer); QTRY_VERIFY(!controller.directChatBusy());
            QCOMPARE(controller.directChatForm().value("expectedStreamId").toString(), expected); QCOMPARE(test.posts, 0);
        }
    }

    void directChatContextLossFencesEveryDelayedStage_data()
    {
        QTest::addColumn<QString>("stage"); QTest::addColumn<QString>("change");
        for (const auto *stage : {"lookup", "post", "canonical", "default", "catalog"})
            for (const auto *change : {"route", "logout", "token", "snapshot", "cache-replace", "cache-destroy"})
                QTest::newRow(qPrintable(QString(stage) + ":" + change)) << QString(stage) << QString(change);
    }
    void directChatContextLossFencesEveryDelayedStage()
    {
        QFETCH(QString, stage); QFETCH(QString, change); DirectChatFixture test;
        QTemporaryDir first, second;
        auto cache = std::make_unique<NativeCacheService>(first.path(), NativeCache::Policy{}, true);
        NativeCacheService replacement(second.path(), {}, true);
        if (change.startsWith("cache-")) test.fixture.controller.useCache(cache.get());
        QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QTRY_VERIFY(controller.directChatDirectoryVerified());
        const auto fallback = test.fixture.server.handler; bool delayed = false;
        test.fixture.server.handler = [&, fallback](const Request &request) {
            auto result = fallback(request); const auto path = request.target.path();
            const bool target = (stage == "lookup" && path.endsWith(directId) && test.gets == 1)
                || (stage == "post" && request.method == "POST" && path.endsWith("/streams/"))
                || (stage == "canonical" && path.endsWith(directId) && test.gets == 2)
                || (stage == "default" && path.endsWith("/stream_topics/") && !test.created.isEmpty())
                || (stage == "catalog" && path.endsWith("/streams/") && request.method == "GET" && !test.created.isEmpty());
            if (target && !delayed) { delayed = true; result.delay = 150; }
            return result;
        };
        QSignalSpy done(&controller, &NativeController::directChatOpened); controller.startDirectChat(channelMemberA);
        QTRY_VERIFY(delayed); const auto form = controller.directChatForm(); const auto key = controller.directChatContextKey();
        if (change == "route") controller.selectStream({});
        else if (change == "logout") controller.logout();
        else if (change == "cache-replace") controller.useCache(&replacement);
        else if (change == "cache-destroy") cache.reset();
        else if (change == "snapshot") controller.reloadSnapshot();
        else {
            test.fixture.authentication = [](const Request &request) {
                if (request.target.path().endsWith("/actions/get_token/invoke")) return Response{QJsonObject{
                    {"access_token", "rotated-direct-token"}, {"refresh_token", "synthetic-refresh"}, {"expires_in", 3600}}};
                return sessionResponse(request);
            };
            test.fixture.session.requestRefresh();
        }
        QTRY_VERIFY(test.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(done.size(), 0); QCOMPARE(test.posts, stage == "lookup" ? 0 : 1);
        QVERIFY(!controller.directChatBusy()); QVERIFY(controller.selectedStream() != directId);
        QVERIFY(controller.directChatContextKey() != key);
        if (change == "logout") QVERIFY(controller.directChatForm().isEmpty());
        else { QVERIFY(controller.directChatUncertain()); QCOMPARE(controller.directChatForm().value("attemptId"), form.value("attemptId")); }
    }

    void directChatOwnerAndProjectChangesCannotPublishAnOlderAttempt_data()
    {
        QTest::addColumn<QString>("stage"); QTest::addColumn<bool>("projectChange");
        for (const auto *stage : {"lookup", "post", "canonical", "default", "catalog"})
            for (bool project : {false, true}) QTest::newRow(qPrintable(QString(stage) + (project ? ":project" : ":owner"))) << QString(stage) << project;
    }
    void directChatOwnerAndProjectChangesCannotPublishAnOlderAttempt()
    {
        QFETCH(QString, stage); QFETCH(bool, projectChange); DirectChatFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller; QTRY_VERIFY(controller.directChatDirectoryVerified());
        const auto fallback = test.fixture.server.handler; bool delayed = false;
        test.fixture.server.handler = [&, fallback](const Request &request) {
            auto result = fallback(request); const auto path = request.target.path();
            if (!delayed && ((stage == "lookup" && path.endsWith(directId) && test.gets == 1)
                || (stage == "post" && request.method == "POST" && path.endsWith("/streams/"))
                || (stage == "canonical" && path.endsWith(directId) && test.gets == 2)
                || (stage == "default" && path.endsWith("/stream_topics/") && !test.created.isEmpty())
                || (stage == "catalog" && path.endsWith("/streams/") && request.method == "GET" && !test.created.isEmpty()))) {
                delayed = true; result.delay = 150;
            }
            return result;
        };
        QSignalSpy done(&controller, &NativeController::directChatOpened); controller.startDirectChat(channelMemberA); QTRY_VERIFY(delayed);
        const auto form = controller.directChatForm(); const auto newProject = projectChange ? returnedId : projectId;
        const auto newUser = projectChange ? userId : returnedId;
        test.fixture.server.handler = [fallback, newProject, newUser](const Request &request) {
            auto result = fallback(request); const auto path = request.target.path();
            if (path.endsWith("/actions/introspect")) {
                auto row = result.data.toObject(); row["project_id"] = newProject; row["user_info"] = QJsonObject{{"uuid", newUser}}; result.data = row;
            } else if (path == "/api/core/v1/iam/projects/") result.data = QJsonArray{QJsonObject{{"uuid", newProject}, {"name", "Other"}, {"status", "ACTIVE"}}};
            else if (path.endsWith("/streams/") && request.method == "GET") {
                auto row = creationChannel(streamId); row["project_id"] = newProject; row["user_uuid"] = newUser; row["owner"] = newUser;
                result.data = QJsonArray{row}; result.delay = 0;
            }
            return result;
        };
        controller.login(test.fixture.server.origin().toString(), "another-synthetic", "synthetic", "");
        QTRY_COMPARE(controller.projects().size(), 1); controller.openProject(newProject);
        QTRY_VERIFY(controller.authenticated()); QTRY_COMPARE(controller.streams().size(), 1);
        QTRY_VERIFY(test.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(done.size(), 0); QVERIFY(controller.directChatForm().isEmpty());
        QCOMPARE(test.posts, stage == "lookup" ? 0 : 1);
        test.fixture.server.handler = fallback;
        controller.login(test.fixture.server.origin().toString(), "synthetic", "synthetic", "");
        QTRY_COMPARE(controller.projects().size(), 1); controller.openProject(projectId); QTRY_VERIFY(controller.authenticated());
        QTRY_VERIFY(controller.directChatDirectoryVerified()); QTRY_VERIFY(controller.canCheckDirectChat());
        QCOMPARE(controller.directChatForm().value("attemptId"), form.value("attemptId"));
        controller.checkDirectChat(); QTRY_VERIFY(!controller.directChatBusy());
        QCOMPARE(test.posts, stage == "lookup" ? 0 : 1);
        QCOMPARE(done.size(), stage == "lookup" ? 0 : 1);
    }

    void directChatUnsafeTargetChangesAndAbaCannotPublishAnEarlierSnapshot_data()
    {
        QTest::addColumn<QString>("change");
        for (const auto *change : {"stream-delete", "default-delete", "default-replace", "binding-delete", "binding-role", "private-aba", "encrypted-aba", "topic-aba", "late-topic-update"})
            QTest::newRow(change) << QString(change);
    }
    void directChatUnsafeTargetChangesAndAbaCannotPublishAnEarlierSnapshot()
    {
        QFETCH(QString, change); DirectChatFixture test; QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QTRY_VERIFY(controller.directChatDirectoryVerified()); QSignalSpy done(&controller, &NativeController::directChatOpened);
        test.catalogDelay = 150; const auto catalogs = test.streamGets;
        controller.startDirectChat(channelMemberA); QTRY_COMPARE(test.streamGets, catalogs + 1);
        const auto key = controller.directChatContextKey(); const auto form = controller.directChatForm();
        if (change == "stream-delete") QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.deleted", QJsonObject{{"uuid", directId}})));
        else if (change == "default-delete") QVERIFY(controller.applyRealtimeEvent(controllerEvent("topic.deleted", QJsonObject{{"uuid", channelDefault}, {"stream_uuid", directId}})));
        else if (change.startsWith("binding")) {
            auto row = creationBinding(userId, 900); row["stream_uuid"] = directId; row["role"] = "member";
            QVERIFY(controller.applyRealtimeEvent(controllerEvent(change == "binding-delete" ? "stream_binding.deleted" : "stream_binding.updated", row)));
        } else if (change == "topic-aba" || change == "late-topic-update") {
            auto topic = test.defaultTopic; topic["name"] = "A newer topic";
            QVERIFY(controller.applyRealtimeEvent(controllerEvent("topic.updated", topic)));
            if (change == "topic-aba") QVERIFY(controller.applyRealtimeEvent(controllerEvent("topic.updated", test.defaultTopic, 13)));
            else test.defaultTopic = topic;
        } else {
            auto row = test.created;
            if (change == "private-aba") row["private"] = false;
            else if (change == "encrypted-aba") { row["encryption"] = true; row["current_encryption_key_uuid"] = returnedId; }
            else row["default_topic_uuid"] = returnedId;
            QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.updated", row)));
            if (change.endsWith("aba")) QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.updated", test.created, 13)));
        }
        QTRY_VERIFY(test.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(done.size(), 0); QCOMPARE(test.posts, 1); QVERIFY(controller.directChatUncertain());
        QVERIFY(controller.selectedStream() != directId); QCOMPARE(controller.directChatForm().value("attemptId"), form.value("attemptId"));
        if (change != "topic-aba" && change != "late-topic-update") { QVERIFY(controller.directChatContextKey() != key); QVERIFY(!controller.canRetryDirectChat()); }
        test.catalogDelay = 0; controller.checkDirectChat(); QTRY_COMPARE(done.size(), 1); QCOMPARE(test.posts, 1);
        if (change == "late-topic-update") QCOMPARE(controller.topics().first().toMap().value("name").toString(), "A newer topic");
    }

    void directChatMatchingOwnEventsUseOnlyBoundedReadReconciliation()
    {
        DirectChatFixture test; RealtimeGateway gateway(test.fixture.server.serverPort()); QVector<QJsonObject> events;
        test.fixture.realtime = [&events](const Request &request) {
            if (request.target.path().endsWith("/epoch/")) return Response{QJsonObject{{"epoch_generation", "controller-generation"},
                {"epoch_version", 10}, {"current_epoch_version", 10}, {"minimum_epoch_version", 1}}};
            QJsonArray rows; const auto after = QUrlQuery(request.target).queryItemValue("epoch_version>").toInt();
            for (const auto &event : events) if (event.value("epoch_version").toInt() > after) rows.append(event);
            return Response{rows};
        };
        QVERIFY(test.fixture.start(gateway.origin())); auto &controller = test.fixture.controller;
        RealtimeClient realtime(&test.fixture.api, &test.fixture.session); controller.useRealtime(&realtime);
        QTRY_COMPARE(controller.syncState(), "live"); QTRY_COMPARE(gateway.sockets.size(), 1);
        test.getDelay = test.topicDelay = test.catalogDelay = 70;
        const auto fallback = test.fixture.server.handler; QSet<QString> sent;
        test.fixture.server.handler = [&, fallback](const Request &request) {
            auto result = fallback(request); if (test.created.isEmpty()) return result;
            const auto path = request.target.path(); QString kind; QJsonObject payload;
            if (request.method == "GET" && path.endsWith(directId)) { kind = "stream.created"; payload = test.created; }
            else if (path.endsWith("/stream_topics/")) { kind = "topic.created"; payload = test.defaultTopic; }
            else if (request.method == "GET" && path.endsWith("/streams/")) {
                kind = "stream_bindings.created"; auto first = creationBinding(userId, 900), second = creationBinding(channelMemberA, 901);
                for (auto *row : {&first, &second}) { (*row)["stream_uuid"] = directId; (*row)["role"] = "owner"; }
                payload = {{"uuid", directId}, {"items", QJsonArray{first, second}}};
            }
            if (!kind.isEmpty() && !sent.contains(kind)) {
                sent.insert(kind); const auto event = controllerEvent(kind, payload, 10 + sent.size()); events.append(event);
                QTimer::singleShot(0, &controller, [&gateway, event] { gateway.sockets.last()->sendTextMessage(QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact))); });
            }
            return result;
        };
        QSignalSpy done(&controller, &NativeController::directChatOpened); controller.startDirectChat(channelMemberA);
        QTRY_VERIFY2(done.size() == 1, qPrintable(controller.directChatPhase() + ": " + controller.directChatError()
            + QString(" gets=%1 topics=%2 catalogs=%3 events=%4 ack=%5").arg(test.gets).arg(test.topicGets).arg(test.streamGets).arg(sent.size()).arg(realtime.acknowledgedEpoch())));
        QTRY_COMPARE(realtime.acknowledgedEpoch(), quint64(13));
        QCOMPARE(test.posts, 1); QVERIFY(test.gets <= 5); QVERIFY(test.topicGets <= 4); QVERIFY(test.streamGets <= 5);
        QCOMPARE(controller.directChatPhase(), "ready"); QCOMPARE(sent.size(), 3);
    }

    void directChatReconciliationExhaustionKeepsExplicitGetOnlyRecovery()
    {
        DirectChatFixture test; QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QTRY_VERIFY(controller.directChatDirectoryVerified()); const auto fallback = test.fixture.server.handler; int interleavings = 0;
        test.fixture.server.handler = [&, fallback](const Request &request) {
            auto result = fallback(request);
            if (!test.created.isEmpty() && request.target.path().endsWith(directId) && interleavings < 4) {
                ++interleavings; result.delay = 40;
                QTimer::singleShot(0, &controller, [&] { QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.created", test.created, 20 + interleavings))); });
            }
            return result;
        };
        QSignalSpy done(&controller, &NativeController::directChatOpened); controller.startDirectChat(channelMemberA);
        QTRY_VERIFY(controller.directChatUncertain()); QCOMPARE(done.size(), 0); QCOMPARE(test.posts, 1); QCOMPARE(interleavings, 4);
        controller.checkDirectChat(); QTRY_COMPARE(done.size(), 1); QCOMPARE(test.posts, 1);
    }

    void directChatBenignDirectoryAndCounterChangesKeepContextAndPeerIdentity()
    {
        DirectChatFixture test; QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QTRY_VERIFY(controller.directChatDirectoryVerified()); test.getDelay = 150;
        controller.startDirectChat(channelMemberA); const auto form = controller.directChatForm(); const auto key = controller.directChatContextKey();
        auto existing = creationChannel(streamId); existing["unread_count"] = 7;
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.read", existing)));
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("user.updated", QJsonObject{{"uuid", channelMemberA}, {"username", "alice"}, {"first_name", "Updated"}})));
        QCOMPARE(controller.directChatContextKey(), key); QCOMPARE(controller.directChatForm().value("peerUuid"), form.value("peerUuid"));
        QTRY_COMPARE(controller.directChatPhase(), "ready"); QCOMPARE(controller.title(), "Updated"); QCOMPARE(test.posts, 1);
    }

    void directChatCatalogCacheSurvivesColdFailureAndWarmNavigationAvoidsExtraGet()
    {
        QTemporaryDir directory; DirectChatFixture test; QVERIFY(test.fixture.start());
        {
            NativeCacheService cache(directory.path(), {}, true); NativeController controller(&test.fixture.api, &test.fixture.session);
            controller.useCache(&cache); QTRY_VERIFY(controller.directChatDirectoryVerified()); QTRY_VERIFY(controller.canStartDirectChatWith(channelMemberA));
            QSignalSpy done(&controller, &NativeController::directChatOpened); controller.startDirectChat(channelMemberA); QTRY_COMPARE(done.size(), 1);
            QTRY_VERIFY(!controller.busy()); QVERIFY(flushCache(cache));
            auto saved = cacheEntry(cache, cache.lease(), {NativeCache::Kind::Streams, {}, {}, {}}); QVERIFY(saved);
            bool found = false; for (const auto &value : saved->rows) if (value.toObject().value("uuid").toString() == directId) {
                found = true; QVERIFY(value.toObject().value("private").toBool()); QCOMPARE(value.toObject().value("direct_user_uuid").toString(), channelMemberA);
            }
            QVERIFY(found); controller.selectStream(streamId); QTRY_VERIFY(!controller.busy()); QVERIFY(flushCache(cache));
            const auto topics = test.topicGets, catalogs = test.streamGets, users = test.usersGets, history = test.fixture.totalHistoryGets();
            controller.selectStream(directId); QTRY_VERIFY(!controller.busy()); QTRY_COMPARE(controller.topics().size(), 1);
            QCOMPARE(test.topicGets, topics); QCOMPARE(test.streamGets, catalogs); QCOMPARE(test.usersGets, users); QCOMPARE(test.fixture.totalHistoryGets(), history);
            QVERIFY(flushCache(cache));
        }
        const auto fallback = test.fixture.server.handler;
        test.fixture.server.handler = [fallback](const Request &request) {
            if (request.method == "GET" && (request.target.path().contains("/messenger/") || request.target.path().endsWith("/users/"))) return Response{QJsonObject{}, 503};
            return fallback(request);
        };
        NativeCacheService reopened(directory.path(), {}, true); NativeController restored(&test.fixture.api, &test.fixture.session);
        restored.useCache(&reopened); QTRY_COMPARE(restored.streams().size(), 2); QVERIFY(restored.cacheStale());
        restored.selectStream(directId); QTRY_COMPARE(restored.topics().size(), 1);
        QVERIFY(restored.isDirectConversation()); QCOMPARE(restored.directPeer().value("id").toString(), channelMemberA);
        QCOMPARE(restored.title(), "Alice Adams"); QVERIFY(!restored.canStartDirectChatWith(channelMemberA));
        QVERIFY(!restored.canSend()); QVERIFY(!restored.canAttach()); QCOMPARE(test.posts, 1);
    }

    void directChatSubscribersMayDestroyControllerAtEveryBoundary_data()
    {
        QTest::addColumn<QString>("boundary");
        for (const auto *boundary : {"looking-up", "creating", "verifying-stream", "verifying-default", "refreshing-catalogs", "streams", "topics", "opened"})
            QTest::newRow(boundary) << QString(boundary);
    }
    void directChatSubscribersMayDestroyControllerAtEveryBoundary()
    {
        QFETCH(QString, boundary); DirectChatFixture test; QVERIFY(test.fixture.start());
        QPointer<NativeController> controller = new NativeController(&test.fixture.api, &test.fixture.session);
        const auto cleanup = qScopeGuard([&] { delete controller; });
        controller->refresh();
        QTRY_VERIFY(controller && controller->canStartDirectChatWith(channelMemberA));
        connect(controller, &NativeController::stateChanged, &test.fixture.controller, [&] {
            if (controller && controller->directChatPhase() == boundary) delete controller;
        });
        connect(controller, &NativeController::streamsChanged, &test.fixture.controller, [&] {
            if (controller && boundary == "streams" && test.gets >= 2) delete controller;
        });
        connect(controller, &NativeController::topicsChanged, &test.fixture.controller, [&] {
            if (controller && boundary == "topics" && controller->selectedStream() == directId) delete controller;
        });
        connect(controller, &NativeController::directChatOpened, &test.fixture.controller, [&] { if (controller && boundary == "opened") delete controller; });
        controller->startDirectChat(channelMemberA); QTRY_VERIFY(controller.isNull());
        QTRY_VERIFY(test.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(test.posts <= 1);
    }

    void directChatCollectionsAreBoundedAndValidateCompleteDtos_data()
    {
        QTest::addColumn<QString>("stage"); QTest::addColumn<QString>("corruption");
        for (const auto *stage : {"default", "catalog"})
            for (const auto *corruption : {"page-cap", "marker-loop", "rows", "bytes", "duplicate", "viewer", "missing-target", "malformed"})
                QTest::newRow(qPrintable(QString(stage) + ":" + corruption)) << QString(stage) << QString(corruption);
    }
    void directChatCollectionsAreBoundedAndValidateCompleteDtos()
    {
        QFETCH(QString, stage); QFETCH(QString, corruption); DirectChatFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller; QTRY_VERIFY(controller.directChatDirectoryVerified());
        const auto fallback = test.fixture.server.handler; int pages = 0;
        test.fixture.server.handler = [&, fallback](const Request &request) {
            auto result = fallback(request);
            if (test.created.isEmpty() || request.method != "GET" || !(stage == "default"
                ? request.target.path().endsWith("/stream_topics/") : request.target.path().endsWith("/streams/"))) return result;
            ++pages;
            if (corruption == "page-cap" || corruption == "marker-loop") {
                result.data = QJsonArray{};
                result.headers["X-Pagination-Marker"] = corruption == "marker-loop" ? "repeated" : QByteArray::number(pages);
            } else if (corruption == "rows") {
                QJsonArray rows; for (int i = 0; i < 1000; ++i) rows.append(QJsonObject{{"uuid", userId}});
                result.data = rows; result.headers["X-Pagination-Marker"] = QByteArray::number(pages);
            } else if (corruption == "bytes") {
                result.data = QJsonArray{QJsonObject{{"content", QString(8 * 1024 * 1024, 'x')}}};
                result.headers["X-Pagination-Marker"] = QByteArray::number(pages);
            } else if (corruption == "malformed") result.data = QJsonObject{};
            else {
                auto rows = result.data.toArray();
                if (corruption == "duplicate") rows.append(rows.last());
                else if (corruption == "missing-target") rows = QJsonArray{};
                else { auto row = rows.last().toObject(); row["user_uuid"] = returnedId; rows[rows.size() - 1] = row; }
                result.data = rows;
            }
            return result;
        };
        QSignalSpy changed(&controller, &NativeController::stateChanged), done(&controller, &NativeController::directChatOpened);
        controller.startDirectChat(channelMemberA);
        while (controller.directChatBusy()) QVERIFY(changed.wait());
        QCOMPARE(done.size(), 0); QCOMPARE(test.posts, 1); QVERIFY(controller.directChatUncertain());
        QVERIFY(pages <= 500); if (corruption == "page-cap") QVERIFY(pages >= 499);
        if (corruption == "marker-loop") QCOMPARE(pages, 2);
        if (corruption == "rows") QCOMPARE(pages, stage == "default" ? 101 : 100);
        if (corruption == "bytes") QCOMPARE(pages, 4);
    }

    void directChatPagedPublicationKeepsExactNavigationRequestCounts_data()
    {
        QTest::addColumn<QString>("stage"); QTest::newRow("default") << QString("default"); QTest::newRow("catalog") << QString("catalog");
    }
    void directChatPagedPublicationKeepsExactNavigationRequestCounts()
    {
        QFETCH(QString, stage); DirectChatFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller; QTRY_VERIFY(controller.directChatDirectoryVerified());
        const auto fallback = test.fixture.server.handler; int pages = 0; bool correctMarker = true;
        test.fixture.server.handler = [&, fallback](const Request &request) {
            auto result = fallback(request);
            if (!test.created.isEmpty() && request.method == "GET" && (stage == "default"
                ? request.target.path().endsWith("/stream_topics/") : request.target.path().endsWith("/streams/"))) {
                ++pages; const auto marker = QUrlQuery(request.target).queryItemValue("page_marker");
                if (marker.isEmpty()) { result.data = QJsonArray{}; result.headers["X-Pagination-Marker"] = "valid-next"; }
                else correctMarker &= marker == "valid-next";
            }
            return result;
        };
        const auto users = test.usersGets, catalogs = test.streamGets, history = test.fixture.totalHistoryGets();
        QSignalSpy done(&controller, &NativeController::directChatOpened); controller.startDirectChat(channelMemberA); QTRY_COMPARE(done.size(), 1);
        QCOMPARE(pages, 2); QVERIFY(correctMarker); QCOMPARE(test.posts, 1); QCOMPARE(test.gets, 2); QCOMPARE(test.usersGets, users);
        QCOMPARE(test.topicGets, stage == "default" ? 2 : 1); QCOMPARE(test.streamGets - catalogs, stage == "catalog" ? 2 : 1);
        QTRY_COMPARE(test.fixture.totalHistoryGets() - history, 1);
    }

    void directChatUncertainOtherPeerReplacementFencesLateFirstOwnerCredit()
    {
        DirectChatFixture test; QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QTRY_VERIFY(controller.directChatDirectoryVerified()); test.postDelay = 150;
        QSignalSpy done(&controller, &NativeController::directChatOpened); controller.startDirectChat(channelMemberA); QTRY_COMPARE(test.posts, 1);
        const auto first = controller.directChatForm(); controller.selectStream({});
        QVERIFY(controller.directChatUncertain()); QVERIFY(controller.canStartDirectChatWith(channelMemberB));
        test.created = {}; test.postDelay = 0; controller.startDirectChat(channelMemberB);
        QTRY_COMPARE(done.size(), 1); QCOMPARE(controller.directChatForm().value("peerUuid").toString(), channelMemberB);
        QVERIFY(controller.directChatForm().value("attemptId") != first.value("attemptId"));
        QCOMPARE(controller.selectedStream(), expectedDirectId(channelMemberB));
        QTRY_VERIFY(test.fixture.api.findChildren<QNetworkReply *>().isEmpty()); QCOMPARE(done.size(), 1); QCOMPARE(test.posts, 2);
        QCOMPARE(done.first()[1].toString(), channelMemberB);
    }

    void directChatActivationCannotCreditRevokedSameOwnerContext_data()
    {
        QTest::addColumn<QString>("change"); for (const auto *change : {"token", "cache", "acl", "ready-token"}) QTest::newRow(change) << QString(change);
    }
    void directChatActivationCannotCreditRevokedSameOwnerContext()
    {
        QFETCH(QString, change); DirectChatFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller; QTRY_VERIFY(controller.directChatDirectoryVerified());
        QTemporaryDir path; NativeCacheService replacement(path.path(), {}, true);
        bool revoked = false; QSignalSpy done(&controller, &NativeController::directChatOpened);
        connect(&controller, &NativeController::stateChanged, &controller, [&] {
            if (revoked || controller.selectedStream() != directId || controller.directChatBusy()
                || ((change == "ready-token") != (controller.directChatPhase() == "ready"))) return;
            revoked = true;
            if (change == "cache") controller.useCache(&replacement);
            else if (change == "acl") { auto stream = test.created; stream["role"] = "member"; QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.updated", stream))); }
            else test.fixture.api.setAccessToken("rotated-at-activation");
        });
        controller.startDirectChat(channelMemberA); QTRY_VERIFY(revoked);
        QTRY_VERIFY(test.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(done.size(), 0); QVERIFY(controller.directChatUncertain()); QCOMPARE(test.posts, 1);
    }

    void directChatCompletedTargetEventsDoNotInvalidateAnUnrelatedNewForm_data()
    {
        QTest::addColumn<QString>("change");
        for (const auto *change : {"topic-delete", "stream-role", "binding-role"}) QTest::newRow(change) << QString(change);
    }
    void directChatCompletedTargetEventsDoNotInvalidateAnUnrelatedNewForm()
    {
        QFETCH(QString, change); DirectChatFixture test; QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QTRY_VERIFY(controller.directChatDirectoryVerified()); QSignalSpy done(&controller, &NativeController::directChatOpened);
        controller.startDirectChat(channelMemberA); QTRY_COMPARE(done.size(), 1);
        const auto directKey = controller.directChatContextKey(), channelKey = controller.channelCreationContextKey();
        if (change == "topic-delete") QVERIFY(controller.applyRealtimeEvent(controllerEvent("topic.deleted", QJsonObject{{"uuid", channelDefault}, {"stream_uuid", directId}})));
        else if (change == "stream-role") { auto row = test.created; row["role"] = "member"; QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.updated", row))); }
        else { auto row = creationBinding(userId, 900); row["stream_uuid"] = directId; row["role"] = "member";
            QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream_binding.updated", row))); }
        QCOMPARE(controller.directChatContextKey(), directKey); QCOMPARE(controller.channelCreationContextKey(), channelKey);
        QCOMPARE(controller.directChatPhase(), "ready"); QVERIFY(controller.canStartDirectChatWith(channelMemberB));
        QVERIFY(controller.canCreateChannel()); QCOMPARE(test.posts, 1); QCOMPARE(done.size(), 1);
    }

    void channelCreationPublicGateAndInitialPhase()
    {
        ChannelCreationFixture test; auto &controller = test.fixture.controller;
        QVERIFY(!controller.canCreateChannel()); QVERIFY(controller.channelCreationContextKey().isEmpty());
        QCOMPARE(controller.channelCreationPhase(), QString("idle")); QVERIFY(controller.channelCreationForm().isEmpty());
        QVERIFY(test.fixture.start()); QTRY_VERIFY(controller.canCreateChannel());
        QVERIFY(!controller.channelCreationContextKey().isEmpty());
        QVERIFY(!controller.canCheckChannelCreation()); QVERIFY(!controller.canRetryChannelMembers());
    }

    void channelCreationPublishesVerifiedAggregateAndKeepsMembers_data()
    {
        QTest::addColumn<bool>("members"); QTest::newRow("empty") << false; QTest::newRow("selected") << true;
    }
    void channelCreationPublishesVerifiedAggregateAndKeepsMembers()
    {
        QFETCH(bool, members); ChannelCreationFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller; QSignalSpy complete(&controller, &NativeController::channelCreated);
        const auto users = test.usersGets, streams = test.streamGets, history = test.fixture.totalHistoryGets();
        controller.createChannel("  Created channel  ", "  Description  ", true, true,
            members ? QVariantList{channelMemberA, channelMemberB, channelMemberA, userId} : QVariantList{});
        QTRY_VERIFY(!controller.channelCreationBusy());
        QVERIFY2(controller.channelCreationPhase() == "ready", qPrintable(controller.channelCreationPhase() + ": " + controller.channelCreationError()));
        QCOMPARE(complete.size(), 1); QCOMPARE(test.posts, 1); QCOMPARE(test.gets, 1); QCOMPARE(test.topicGets, 1);
        QCOMPARE(test.streamGets - streams, 1); QCOMPARE(test.usersGets, users);
        QCOMPARE(test.memberPosts, members ? 1 : 0); QCOMPARE(test.bindingGets, 0);
        QCOMPARE(controller.channelCreationPhase(), QString("ready")); QVERIFY(!controller.channelCreationBusy());
        QVERIFY(controller.canCreateChannel()); QCOMPARE(controller.selectedStream(), test.created.value("uuid").toString());
        QVERIFY(controller.selectedTopic().isEmpty()); QCOMPARE(controller.conversationKind(), QString("stream"));
        QVERIFY(!controller.canSend()); QVERIFY(!controller.canAttach()); QCOMPARE(controller.topics().size(), 1);
        QCOMPARE(controller.channelCreationForm().value("confirmedStreamId").toString(), controller.selectedStream());
        QCOMPARE(test.streamBodies.first(), QJsonObject({{"uuid", controller.selectedStream()}, {"name", "Created channel"},
            {"description", "Description"}, {"source_name", "native"}, {"invite_only", true}, {"announce", true},
            {"encryption", false}, {"current_encryption_key_uuid", QJsonValue::Null}}));
        if (members) QCOMPARE(test.memberBodies.first(), QJsonObject({{"member", QJsonArray{channelMemberA, channelMemberB}}}));
        QTRY_COMPARE(test.fixture.totalHistoryGets() - history, 1); // Only the new aggregate route.
    }

    void channelDirectoryIsVerifiedDeduplicatedAndDoesNotChangeFormLifetime()
    {
        ChannelCreationFixture test;
        const auto source = test.fixture.users;
        test.fixture.users = [source](const Request &request) {
            auto result = source(request); auto rows = result.data.toArray(); rows.append(rows.at(1)); result.data = rows; return result;
        };
        QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QVERIFY(controller.channelMembersVerified()); QCOMPARE(controller.channelMemberChoices().size(), 2);
        const auto alice = controller.channelMemberChoices().first().toMap();
        QCOMPARE(alice, QVariantMap({{"id", channelMemberA}, {"displayName", "Alice Adams"},
            {"username", "alice"}, {"email", "alice@example.invalid"}}));
        const auto context = controller.channelCreationContextKey();
        auto stream = creationChannel(streamId, "Renamed by somebody"); stream["unread_count"] = 12;
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.read", stream)));
        auto topic = creationTopic(topicId, streamId, "Renamed topic"); topic["unread_count"] = 9;
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("topic.read", topic)));
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("user.updated", QJsonObject{{"uuid", channelMemberA},
            {"username", "alice"}, {"first_name", "Alice"}, {"last_name", "Updated"}, {"email", "new@example.invalid"}})));
        QCOMPARE(controller.channelCreationContextKey(), context); QVERIFY(controller.channelMembersVerified());
        QCOMPARE(controller.channelMemberChoices().first().toMap().value("displayName").toString(), "Alice Updated");
        QCOMPARE(controller.channelMemberChoices().last().toMap().value("displayName").toString(), "bob");
        controller.selectStream({}); QVERIFY(controller.channelCreationContextKey() != context);
        controller.logout(); QVERIFY(controller.channelMemberChoices().isEmpty()); QVERIFY(!controller.channelMembersVerified());
    }

    void channelWithoutMembersDoesNotWaitForTheDirectory()
    {
        ChannelCreationFixture test; test.fixture.users = [](const Request &) { return Response{QJsonObject{}, 503}; };
        QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller; QVERIFY(!controller.channelMembersVerified());
        QVERIFY(controller.canCreateChannel()); const auto users = test.usersGets;
        QSignalSpy done(&controller, &NativeController::channelCreated);
        controller.createChannel("No members", "", false, false, {}); QTRY_COMPARE(done.size(), 1);
        QCOMPARE(test.memberPosts, 0); QCOMPARE(test.usersGets, users);
    }

    void channelCreationValidatesUnicodeAndSelectedIdsBeforeAnyPost_data()
    {
        QTest::addColumn<QString>("name"); QTest::addColumn<QString>("description");
        QTest::addColumn<QVariantList>("members"); QTest::addColumn<bool>("allowed");
        const auto astral = QString::fromUcs4(U"🦊");
        QTest::newRow("blank-ecma") << QString("\ufeff\u00a0 \t") << QString{} << QVariantList{} << false;
        QTest::newRow("name255") << astral.repeated(255) << QString{} << QVariantList{} << true;
        QTest::newRow("name256") << astral.repeated(256) << QString{} << QVariantList{} << false;
        QTest::newRow("description10000") << QString("valid") << astral.repeated(10000) << QVariantList{} << true;
        QTest::newRow("description10001") << QString("valid") << astral.repeated(10001) << QVariantList{} << false;
        QTest::newRow("unpaired-surrogate") << QString(QChar(0xd800)) << QString{} << QVariantList{} << false;
        QTest::newRow("bad-description-surrogate") << QString("valid") << QString(QChar(0xdc00)) << QVariantList{} << false;
        QTest::newRow("invalid-id") << QString("valid") << QString{} << QVariantList{"not-a-uuid"} << false;
        QTest::newRow("coerced-id") << QString("valid") << QString{} << QVariantList{42} << false;
        QTest::newRow("foreign-id") << QString("valid") << QString{} << QVariantList{returnedId} << false;
        QTest::newRow("only-self") << QString("valid") << QString{} << QVariantList{userId} << true;
    }
    void channelCreationValidatesUnicodeAndSelectedIdsBeforeAnyPost()
    {
        QFETCH(QString, name); QFETCH(QString, description); QFETCH(QVariantList, members); QFETCH(bool, allowed);
        ChannelCreationFixture test; QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QSignalSpy done(&controller, &NativeController::channelCreated);
        controller.createChannel(name, description, false, false, members);
        if (allowed) { QTRY_COMPARE(done.size(), 1); QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 0); }
        else {
            QVERIFY(!controller.channelCreationError().isEmpty()); QCOMPARE(controller.channelCreationPhase(), "idle");
            QVERIFY(controller.channelCreationForm().value("attemptId").toString().isEmpty());
            QCOMPARE(test.posts, 0); QCOMPARE(test.memberPosts, 0); QCOMPARE(done.size(), 0);
        }
    }

    void channelCreationCanonicalIdentityMustMatch_data()
    {
        QTest::addColumn<QString>("field"); QTest::addColumn<QJsonValue>("value"); QTest::addColumn<bool>("post");
        for (const bool post : {true, false}) {
            const auto prefix = post ? "post-" : "get-";
            const QList<QPair<QString, QJsonValue>> invalid{{"uuid", returnedId}, {"project_id", returnedId},
                {"owner", returnedId}, {"user_uuid", returnedId}, {"role", "member"}, {"role", "unknown-role"},
                {"source_name", "zulip"}, {"provider_uuid", returnedId}, {"provider_identity", "provider-value"},
                {"private", true}, {"direct_user_uuid", returnedId}, {"encryption", true},
                {"current_encryption_key_uuid", returnedId}, {"is_archived", true}, {"default_topic_uuid", "bad"},
                {"unread_count", -1}, {"active_unread_count", "0"}, {"notification_mode", "unknown"},
                {"created_at", "bad"}, {"updated_at", QJsonValue::Null}, {"invite_only", "false"}};
            int n = 0;
            for (const auto &entry : invalid)
                QTest::newRow(qPrintable(QString(prefix) + entry.first + QString::number(n++))) << entry.first << entry.second << post;
        }
    }
    void channelCreationCanonicalIdentityMustMatch()
    {
        QFETCH(QString, field); QFETCH(QJsonValue, value); QFETCH(bool, post);
        ChannelCreationFixture test; QVERIFY(test.fixture.start());
        const auto alter = [=](QJsonObject &row) { row[field] = value; };
        if (post) test.alterPost = alter; else test.alterGet = alter;
        auto &controller = test.fixture.controller; QSignalSpy done(&controller, &NativeController::channelCreated);
        controller.createChannel("Frozen request", "", false, false, {});
        QTRY_VERIFY(!controller.channelCreationBusy()); QVERIFY(controller.channelCreationUncertain());
        QCOMPARE(done.size(), 0); QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 0);
        QCOMPARE(test.gets, post ? 0 : 1); QVERIFY(controller.canCheckChannelCreation());
        test.alterPost = {}; test.alterGet = {}; controller.checkChannelCreation();
        QTRY_COMPARE(done.size(), 1); QCOMPARE(test.posts, 1);
    }

    void channelCreationAmbiguousPostRecoveryNeverAddsMembersImplicitly()
    {
        ChannelCreationFixture test; QVERIFY(test.fixture.start()); test.postStatus = 503;
        auto &controller = test.fixture.controller; QSignalSpy done(&controller, &NativeController::channelCreated);
        controller.createChannel("Stable attempt", "Frozen description", true, false, {channelMemberA, channelMemberB});
        QTRY_VERIFY(controller.channelCreationUncertain()); const auto original = controller.channelCreationForm();
        QVERIFY(!original.value("attemptId").toString().isEmpty()); QVERIFY(original.value("confirmedStreamId").toString().isEmpty());
        controller.createChannel("Different request", "", false, true, {}); QCOMPARE(test.posts, 1);
        test.getStatus = 404; controller.checkChannelCreation(); QTRY_VERIFY(!controller.channelCreationBusy());
        QVERIFY(controller.channelCreationUncertain()); QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 0);
        test.getStatus = 200; controller.checkChannelCreation(); QTRY_COMPARE(controller.channelCreationPhase(), "members-required");
        QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 0); QCOMPARE(test.bindingGets, 1); QCOMPARE(done.size(), 0);
        QVERIFY(controller.canRetryChannelMembers()); QVERIFY(!controller.canCreateChannel()); QVERIFY(!controller.canCheckChannelCreation());
        QCOMPARE(controller.channelCreationForm().value("memberUuids"), original.value("memberUuids"));
        QCOMPARE(controller.channelCreationForm().value("attemptId"), original.value("attemptId"));
        QVERIFY(!controller.channelCreationForm().value("membersAttempted").toBool());
        controller.retryChannelMembers(); QTRY_COMPARE(done.size(), 1);
        QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 1); QCOMPARE(test.bindingGets, 2);
        QCOMPARE(test.memberBodies.first(), QJsonObject({{"member", QJsonArray{channelMemberA, channelMemberB}}}));
    }

    void channelMemberRecoveryPreservesTheConfirmedStreamAndFrozenSet_data()
    {
        QTest::addColumn<QString>("outcome");
        for (const auto *outcome : {"terminal", "committed-503", "partial-503", "malformed-200", "promoted-existing"})
            QTest::newRow(outcome) << QString(outcome);
    }
    void channelMemberRecoveryPreservesTheConfirmedStreamAndFrozenSet()
    {
        QFETCH(QString, outcome); ChannelCreationFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller; QSignalSpy done(&controller, &NativeController::channelCreated);
        if (outcome == "terminal" || outcome == "promoted-existing") { test.addStatus = 403; test.applyMembers = false; }
        else if (outcome != "malformed-200") test.addStatus = 503;
        else test.alterBindings = [](QJsonArray &rows) { rows.removeLast(); };
        if (outcome == "partial-503") test.applyMembers = false;
        controller.createChannel("Member recovery", "", false, false, {channelMemberA, channelMemberB});
        QTRY_VERIFY(!controller.channelCreationBusy()); QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 1); QCOMPARE(done.size(), 0);
        const auto id = test.created.value("uuid").toString(); QCOMPARE(controller.channelCreationForm().value("confirmedStreamId").toString(), id);
        QVERIFY(!controller.canCreateChannel());
        if (outcome == "terminal" || outcome == "promoted-existing") {
            QCOMPARE(controller.channelCreationPhase(), "members-failed"); QVERIFY(controller.canRetryChannelMembers());
            controller.checkChannelCreation(); QCOMPARE(test.bindingGets, 0);
        } else { QVERIFY(controller.channelCreationUncertain()); QVERIFY(!controller.canRetryChannelMembers()); }
        if (outcome == "promoted-existing") {
            auto a = creationBinding(channelMemberA, 1), b = creationBinding(channelMemberB, 2);
            a["stream_uuid"] = id; b["stream_uuid"] = id; a["role"] = "administrator"; b["role"] = "moderator";
            a["who_uuid"] = returnedId; b["who_uuid"] = returnedId; test.bindings = {a, b};
        }
        test.addStatus = 200; test.applyMembers = true; test.alterBindings = {};
        if (outcome == "terminal" || outcome == "promoted-existing") controller.retryChannelMembers();
        else controller.checkChannelCreation();
        if (outcome == "partial-503") {
            QTRY_COMPARE(controller.channelCreationPhase(), "members-required"); QCOMPARE(test.memberPosts, 1); QCOMPARE(done.size(), 0);
            controller.retryChannelMembers();
        }
        QTRY_COMPARE(done.size(), 1); QCOMPARE(test.posts, 1); QCOMPARE(controller.selectedStream(), id);
        QCOMPARE(test.memberPosts, outcome == "terminal" || outcome == "partial-503" ? 2 : 1);
        for (const auto &body : test.memberBodies) QCOMPARE(body, QJsonObject({{"member", QJsonArray{channelMemberA, channelMemberB}}}));
    }

    void channelMembershipDeletionFencesAnOlderSuccessfulAddResponse()
    {
        ChannelCreationFixture test; QVERIFY(test.fixture.start()); test.addDelay = 150;
        auto &controller = test.fixture.controller; QSignalSpy done(&controller, &NativeController::channelCreated);
        controller.createChannel("Member was removed", "", false, false, {channelMemberA, channelMemberB});
        QTRY_COMPARE(test.memberPosts, 1); QVERIFY(!test.bindings.isEmpty());
        const auto removed = test.bindings.first().toObject(); test.bindings.removeFirst();
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream_binding.deleted", QJsonObject{
            {"uuid", removed.value("uuid")}, {"stream_uuid", test.created.value("uuid")}, {"user_uuid", channelMemberA}})));
        QTRY_VERIFY(!controller.channelCreationBusy()); QVERIFY(controller.channelCreationUncertain()); QCOMPARE(done.size(), 0);
        controller.checkChannelCreation(); QTRY_COMPARE(controller.channelCreationPhase(), "members-required");
        QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 1);
        test.addDelay = 0; controller.retryChannelMembers(); QTRY_COMPARE(done.size(), 1);
        QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 2);
    }

    void channelLiveMetadataDuringPublishCannotSaveAnOlderFreshCatalog()
    {
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        ChannelCreationFixture test; test.fixture.controller.useCache(&cache); QVERIFY(test.fixture.start()); QVERIFY(flushCache(cache));
        auto &controller = test.fixture.controller; QSignalSpy done(&controller, &NativeController::channelCreated);
        bool injected = false;
        connect(&controller, &NativeController::streamsChanged, &controller, [&] {
            if (injected || test.created.isEmpty() || controller.channelCreationPhase() != "refreshing-catalogs") return;
            injected = true; test.created["name"] = "Live channel name";
            QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.updated", test.created)));
        });
        controller.createChannel("Older response name", "", false, false, {});
        QTRY_VERIFY(injected); QTRY_VERIFY(!controller.channelCreationBusy());
        QVERIFY(controller.channelCreationUncertain()); QCOMPARE(done.size(), 0); QVERIFY(flushCache(cache));
        const auto entry = cacheEntry(cache, cache.lease(), {NativeCache::Kind::Streams, {}, {}, {}});
        QVERIFY(!entry || !entry->fresh);
        controller.checkChannelCreation(); QTRY_COMPARE(done.size(), 1); QCOMPARE(test.posts, 1);
        const auto fresh = cacheEntry(cache, cache.lease(), {NativeCache::Kind::Streams, {}, {}, {}}); QVERIFY(fresh); QVERIFY(fresh->fresh);
        bool found = false;
        for (const auto &value : fresh->rows) if (value.toObject().value("uuid") == test.created.value("uuid")) {
            found = true; QCOMPARE(value.toObject().value("name").toString(), "Live channel name");
        }
        QVERIFY(found);
    }

    void channelRouteChangeInsideActivationCannotClaimNavigationReady()
    {
        ChannelCreationFixture test; QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QSignalSpy done(&controller, &NativeController::channelCreated); bool changed = false;
        connect(&controller, &NativeController::topicsChanged, &controller, [&] {
            if (changed || test.created.isEmpty() || controller.selectedStream() != test.created.value("uuid").toString()) return;
            changed = true; controller.selectStream(streamId);
        });
        controller.createChannel("Navigation interrupted", "", false, false, {});
        QTRY_VERIFY(changed); QTRY_VERIFY(!controller.channelCreationBusy()); QCOMPARE(done.size(), 0);
        QVERIFY(controller.channelCreationUncertain()); QVERIFY(controller.canCheckChannelCreation());
        controller.checkChannelCreation(); QTRY_COMPARE(done.size(), 1); QCOMPARE(test.posts, 1);
    }

    void channelBindingBatchUsesTheSourcePayloadUuidAsStreamIdentity()
    {
        ChannelCreationFixture test; QVERIFY(test.fixture.start()); test.addDelay = 150;
        auto &controller = test.fixture.controller; QSignalSpy done(&controller, &NativeController::channelCreated);
        controller.createChannel("Source batch identity", "", false, false, {channelMemberA, channelMemberB});
        QTRY_COMPARE(test.memberPosts, 1);
        // events.py create_stream_bindings_created_event emits uuid=stream UUID,
        // items=[full bindings], with no outer stream_uuid property.
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream_bindings.created", QJsonObject{
            {"uuid", test.created.value("uuid")}, {"items", test.bindings}})));
        QTRY_COMPARE(done.size(), 1); QCOMPARE(controller.channelCreationPhase(), "ready");
        QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 1); QCOMPARE(test.bindingGets, 1);
    }

    void channelOwnAccessLossBeforeMemberDispatchStopsTheMutation_data()
    {
        QTest::addColumn<QString>("change");
        for (const auto *change : {"binding-role", "binding-delete", "stream-encryption", "stream-role", "stream-private"})
            QTest::newRow(change) << QString(change);
    }
    void channelOwnAccessLossBeforeMemberDispatchStopsTheMutation()
    {
        QFETCH(QString, change); ChannelCreationFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller; QSignalSpy done(&controller, &NativeController::channelCreated);
        bool injected = false;
        const auto before = controller.channelCreationContextKey();
        connect(&controller, &NativeController::stateChanged, &controller, [&] {
            if (injected || controller.channelCreationPhase() != "adding-members") return;
            injected = true;
            if (change.startsWith("binding")) {
                auto binding = creationBinding(userId, 900); binding["stream_uuid"] = test.created.value("uuid"); binding["role"] = "member";
                QVERIFY(controller.applyRealtimeEvent(controllerEvent(change == "binding-role" ? "stream_binding.updated" : "stream_binding.deleted", binding)));
            } else {
                auto stream = test.created;
                if (change == "stream-encryption") { stream["encryption"] = true; stream["current_encryption_key_uuid"] = returnedId; }
                else if (change == "stream-role") stream["role"] = "member";
                else { stream["private"] = true; stream["direct_user_uuid"] = returnedId; }
                QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.updated", stream)));
            }
        });
        controller.createChannel("Do not add after access loss", "", false, false, {channelMemberA});
        QTRY_VERIFY(injected); QTRY_VERIFY(!controller.channelCreationBusy());
        QVERIFY(controller.channelCreationUncertain()); QCOMPARE(done.size(), 0); QCOMPARE(test.memberPosts, 0);
        QVERIFY(!controller.canRetryChannelMembers()); QVERIFY(controller.channelCreationContextKey() != before);
        QCOMPARE(controller.channelCreationForm().value("memberUuids").toList(), QVariantList{channelMemberA});
    }

    void channelConfirmedStreamDeletionIsTerminalAndAllowsAnExplicitNewUuid()
    {
        ChannelCreationFixture test; QVERIFY(test.fixture.start()); test.catalogDelay = 150;
        auto &controller = test.fixture.controller; QSignalSpy done(&controller, &NativeController::channelCreated);
        const auto catalogs = test.streamGets;
        controller.createChannel("Deleted channel", "", false, false, {channelMemberA});
        QTRY_COMPARE(test.streamGets, catalogs + 1); const auto deleted = test.created.value("uuid").toString();
        QVERIFY(controller.channelCreationForm().value("confirmedStreamId").toString() == deleted);
        test.includeCreated = false;
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.deleted", QJsonObject{{"uuid", deleted}})));
        QTRY_VERIFY(test.fixture.api.findChildren<QNetworkReply *>().isEmpty()); QCOMPARE(done.size(), 0);
        QVERIFY(controller.canCreateChannel()); QVERIFY(!controller.canCheckChannelCreation()); QVERIFY(!controller.canRetryChannelMembers());
        QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 1);
        test.catalogDelay = 0; test.includeCreated = true;
        controller.createChannel("Explicit new attempt", "", true, false, {channelMemberA});
        QTRY_COMPARE(done.size(), 1); QCOMPARE(test.posts, 2);
        QVERIFY(test.created.value("uuid").toString() != deleted);
        QCOMPARE(test.streamBodies.first().value("uuid").toString(), deleted);
        QCOMPARE(test.streamBodies.last().value("uuid").toString(), controller.selectedStream());
    }

    void channelDefaultDeletionRetainsTheStreamAndAllowsGetOnlyRecovery()
    {
        ChannelCreationFixture test; QVERIFY(test.fixture.start()); test.catalogDelay = 150;
        auto &controller = test.fixture.controller; QSignalSpy done(&controller, &NativeController::channelCreated);
        const auto catalogs = test.streamGets;
        controller.createChannel("Default was removed", "", false, false, {channelMemberA});
        QTRY_COMPARE(test.streamGets, catalogs + 1); const auto id = test.created.value("uuid").toString();
        test.created["default_topic_uuid"] = QJsonValue::Null; test.includeDefault = false;
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("topic.deleted", QJsonObject{{"uuid", channelDefault}, {"stream_uuid", id}})));
        QTRY_VERIFY(test.fixture.api.findChildren<QNetworkReply *>().isEmpty()); QCOMPARE(done.size(), 0);
        QCOMPARE(controller.channelCreationForm().value("confirmedStreamId").toString(), id);
        QVERIFY(!controller.canCreateChannel()); QVERIFY(controller.canCheckChannelCreation()); QVERIFY(!controller.canRetryChannelMembers());
        test.catalogDelay = 0; controller.checkChannelCreation(); QTRY_VERIFY(!controller.channelCreationBusy());
        QVERIFY(controller.channelCreationUncertain()); QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 1);
        // Another authorized caller selects an existing replacement default.
        test.created["default_topic_uuid"] = returnedId; test.defaultTopic["uuid"] = returnedId; test.includeDefault = true;
        controller.checkChannelCreation(); QTRY_COMPARE(done.size(), 1);
        QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 1); QCOMPARE(controller.selectedStream(), id);
        QCOMPARE(controller.channelCreationForm().value("defaultTopicId").toString(), returnedId);
    }

    void channelMatchingOwnCreationRealtimeSettlesWithoutManualCheck_data()
    {
        QTest::addColumn<QString>("stage");
        for (const auto *stage : {"canonical", "default", "catalog"}) QTest::newRow(stage) << QString(stage);
    }
    void channelMatchingOwnCreationRealtimeSettlesWithoutManualCheck()
    {
        QFETCH(QString, stage); ChannelCreationFixture test;
        RealtimeGateway gateway(test.fixture.server.serverPort()); QVector<QJsonObject> events;
        test.fixture.realtime = [&events](const Request &request) {
            if (request.target.path().endsWith("/epoch/")) return Response{QJsonObject{{"epoch_generation", "controller-generation"},
                {"epoch_version", 10}, {"current_epoch_version", 10}, {"minimum_epoch_version", 1}}};
            QJsonArray rows; const auto after = QUrlQuery(request.target).queryItemValue("epoch_version>").toInt();
            for (const auto &event : events) if (event.value("epoch_version").toInt() > after) rows.append(event);
            return Response{rows};
        };
        QVERIFY(test.fixture.start(gateway.origin())); auto &controller = test.fixture.controller;
        RealtimeClient realtime(&test.fixture.api, &test.fixture.session); controller.useRealtime(&realtime);
        QTRY_COMPARE(controller.syncState(), "live"); QTRY_COMPARE(gateway.sockets.size(), 1);
        if (stage == "canonical") test.getDelay = 150;
        else if (stage == "default") test.topicDelay = 150; else test.catalogDelay = 150;
        const auto streams = test.streamGets, users = test.usersGets, history = test.fixture.totalHistoryGets();
        QSignalSpy done(&controller, &NativeController::channelCreated);
        controller.createChannel("Ordinary own realtime", "", true, false, {channelMemberA, channelMemberB});
        if (stage == "canonical") QTRY_COMPARE(test.gets, 1);
        else if (stage == "default") QTRY_COMPARE(test.topicGets, 1); else QTRY_COMPARE(test.streamGets, streams + 1);
        const auto event = stage == "canonical" ? controllerEvent("stream.created", test.created, 11)
            : stage == "default" ? controllerEvent("topic.created", test.defaultTopic, 11)
            : controllerEvent("stream_bindings.created", QJsonObject{{"uuid", test.created.value("uuid")}, {"items", test.bindings}}, 11);
        events.append(event); gateway.sockets.last()->sendTextMessage(QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact)));
        QTRY_COMPARE(realtime.acknowledgedEpoch(), quint64(11)); QTRY_VERIFY(!controller.channelCreationBusy());
        QVERIFY2(controller.channelCreationPhase() == "ready", qPrintable(controller.channelCreationPhase()
            + QString(" [stream GET %1 / topic GET %2 / catalog GET %3]").arg(test.gets).arg(test.topicGets).arg(test.streamGets - streams)));
        QCOMPARE(done.size(), 1); QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 1); QCOMPARE(test.usersGets, users);
        QVERIFY(test.gets <= 2); QVERIFY(test.topicGets <= 2); QVERIFY(test.streamGets - streams <= 2);
        QTRY_COMPARE(test.fixture.totalHistoryGets() - history, 1);
    }

    void channelCompletedAttemptDoesNotCaptureAnUnrelatedNewFormLifetime()
    {
        ChannelCreationFixture test; QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QSignalSpy done(&controller, &NativeController::channelCreated);
        controller.createChannel("Already complete", "", false, false, {}); QTRY_COMPARE(done.size(), 1);
        controller.selectStream(streamId); QTRY_VERIFY(!controller.busy()); const auto key = controller.channelCreationContextKey();
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("topic.deleted", QJsonObject{
            {"uuid", channelDefault}, {"stream_uuid", test.created.value("uuid")}})));
        QCOMPARE(controller.channelCreationContextKey(), key); QVERIFY(controller.canCreateChannel());
        QCOMPARE(controller.channelCreationPhase(), "ready");
    }

    void channelOwnReadReconciliationIsBoundedAndNeverReplaysWrites()
    {
        ChannelCreationFixture test; QVERIFY(test.fixture.start()); test.getDelay = 40;
        auto &controller = test.fixture.controller; QSignalSpy done(&controller, &NativeController::channelCreated);
        const auto fallback = test.fixture.server.handler;
        test.fixture.server.handler = [&test, &controller, fallback](const Request &request) {
            auto response = fallback(request);
            if (request.method == "GET" && request.target.path() == "/api/workspace/v1/messenger/streams/" + test.created.value("uuid").toString()) {
                const auto row = test.created;
                QTimer::singleShot(0, &controller, [&controller, row] {
                    controller.applyRealtimeEvent(controllerEvent("stream.created", row));
                });
            }
            return response;
        };
        controller.createChannel("Bounded reconciliation", "", false, false, {channelMemberA});
        QTRY_VERIFY(!controller.channelCreationBusy());
        QCOMPARE(test.gets, 4); QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 0); QCOMPARE(done.size(), 0);
        QVERIFY(controller.channelCreationUncertain()); QVERIFY(controller.canCheckChannelCreation());
        test.fixture.server.handler = fallback; test.getDelay = 0;
        controller.checkChannelCreation(); QTRY_COMPARE(controller.channelCreationPhase(), "members-required");
        QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 0);
        controller.retryChannelMembers(); QTRY_COMPARE(done.size(), 1);
        QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 1);
    }

    void channelUnsafeAbaCannotBeErasedByMatchingOwnEvents_data()
    {
        QTest::addColumn<QString>("stage");
        for (const auto *stage : {"canonical", "default", "members", "catalog"}) QTest::newRow(stage) << QString(stage);
    }
    void channelUnsafeAbaCannotBeErasedByMatchingOwnEvents()
    {
        QFETCH(QString, stage); ChannelCreationFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller; QSignalSpy done(&controller, &NativeController::channelCreated);
        if (stage == "canonical") test.getDelay = 150;
        else if (stage == "default") test.topicDelay = 150;
        else if (stage == "members") test.addDelay = 150; else test.catalogDelay = 150;
        const auto streams = test.streamGets;
        controller.createChannel("Do not accept older A", "", false, false, {channelMemberA});
        if (stage == "canonical") QTRY_COMPARE(test.gets, 1);
        else if (stage == "default") QTRY_COMPARE(test.topicGets, 1);
        else if (stage == "members") QTRY_COMPARE(test.memberPosts, 1); else QTRY_COMPARE(test.streamGets, streams + 1);
        auto changed = test.created; changed["description"] = "Newer B";
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.updated", changed)));
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.updated", test.created)));
        // A subsequent matching own event must not turn an unsafe ABA into a
        // harmless retry. Neither value equality nor last-event-kind is a fence.
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.created", test.created)));
        QTRY_VERIFY(!controller.channelCreationBusy());
        QVERIFY(controller.channelCreationUncertain()); QCOMPARE(done.size(), 0); QCOMPARE(test.posts, 1);
        QCOMPARE(test.gets, 1); QVERIFY(test.memberPosts <= 1);
        test.getDelay = test.topicDelay = test.addDelay = test.catalogDelay = 0;
        controller.checkChannelCreation(); QTRY_VERIFY(!controller.channelCreationBusy());
        QCOMPARE(test.posts, 1);
        if (test.memberPosts) { QCOMPARE(done.size(), 1); QCOMPARE(test.memberPosts, 1); }
        else { QCOMPARE(controller.channelCreationPhase(), "members-required"); QVERIFY(controller.canRetryChannelMembers()); }
    }

    void channelCollectionTraversalHasResourceCeilings_data()
    {
        QTest::addColumn<QString>("resource"); QTest::addColumn<QString>("stage");
        for (const auto *stage : {"default", "bindings", "catalog"})
            for (const auto *resource : {"repeated-marker", "pages", "rows", "bytes"})
                QTest::newRow(qPrintable(QString(stage) + '-' + resource)) << QString(resource) << QString(stage);
    }
    void channelCollectionTraversalHasResourceCeilings()
    {
        QFETCH(QString, resource); QFETCH(QString, stage); ChannelCreationFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller; QSignalSpy done(&controller, &NativeController::channelCreated);
        const auto fallback = test.fixture.server.handler; int requests = 0;
        test.fixture.server.handler = [&test, fallback, &requests, resource, stage](const Request &request) {
            const auto path = request.target.path();
            const bool target = !test.created.isEmpty() && request.method == "GET"
                && ((stage == "default" && path.endsWith("/stream_topics/"))
                    || (stage == "bindings" && path.endsWith("/stream_bindings/"))
                    || (stage == "catalog" && path.endsWith("/streams/")));
            if (!target) return fallback(request);
            ++requests; QJsonArray rows;
            if (resource == "rows") for (int n = 0; n < 1000; ++n) rows.append(QJsonObject{});
            if (resource == "bytes") rows.append(QJsonObject{{"padding", QString(1024 * 1024, 'x')}});
            return Response{rows, 200, 0, {{"X-Pagination-Marker", resource == "repeated-marker" ? QByteArray("same") : QByteArray::number(requests)}}};
        };
        if (stage == "bindings") test.postStatus = 503;
        QSignalSpy changed(&controller, &NativeController::stateChanged);
        controller.createChannel("Bounded catalogs", "", false, false, {channelMemberA});
        if (stage == "bindings") {
            QTRY_VERIFY(controller.channelCreationUncertain()); controller.checkChannelCreation();
        }
        // Wait for actual completions rather than adding polling latency to
        // 500 local requests. The same default timeout bounds the whole walk.
        while (controller.channelCreationBusy()) QVERIFY(changed.wait());
        QVERIFY(controller.channelCreationUncertain()); QCOMPARE(done.size(), 0); QCOMPARE(test.posts, 1);
        QVERIFY(requests > 0); QVERIFY(requests <= 500);
        if (resource == "pages") QCOMPARE(requests, 500);
        if (resource == "repeated-marker") QCOMPARE(requests, 2);
        if (resource == "rows") QCOMPARE(requests, 101);
        if (resource == "bytes") QCOMPARE(requests, 32);
        QCOMPARE(test.memberPosts, stage == "catalog" ? 1 : 0);
    }

    void channelContextLossFencesEveryDelayedStage_data()
    {
        QTest::addColumn<QString>("stage"); QTest::addColumn<QString>("change");
        for (const auto *stage : {"post", "canonical", "default", "members", "bindings", "catalog"})
            for (const auto *change : {"route", "token", "logout", "cache-replace", "cache-destroy", "snapshot"})
                QTest::newRow(qPrintable(QString(stage) + '-' + change)) << QString(stage) << QString(change);
    }
    void channelContextLossFencesEveryDelayedStage()
    {
        QFETCH(QString, stage); QFETCH(QString, change);
        QTemporaryDir first, second;
        auto cache = std::make_unique<NativeCacheService>(first.path(), NativeCache::Policy{}, true);
        NativeCacheService replacement(second.path(), {}, true);
        ChannelCreationFixture test;
        if (change.startsWith("cache-")) test.fixture.controller.useCache(cache.get());
        QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QSignalSpy done(&controller, &NativeController::channelCreated);
        if (stage == "post") test.postDelay = 150;
        else if (stage == "canonical") test.getDelay = 150;
        else if (stage == "default") test.topicDelay = 150;
        else if (stage == "members") test.addDelay = 150;
        else if (stage == "bindings") { test.postStatus = 503; test.bindingDelay = 150; }
        else test.catalogDelay = 150;
        const auto streams = test.streamGets;
        controller.createChannel("Frozen stage context", "", false, true, {channelMemberA});
        if (stage == "bindings") { QTRY_VERIFY(controller.channelCreationUncertain()); controller.checkChannelCreation(); }
        if (stage == "post") QTRY_COMPARE(test.posts, 1);
        else if (stage == "canonical") QTRY_COMPARE(test.gets, 1);
        else if (stage == "default") QTRY_COMPARE(test.topicGets, 1);
        else if (stage == "members") QTRY_COMPARE(test.memberPosts, 1);
        else if (stage == "bindings") QTRY_COMPARE(test.bindingGets, 1);
        else QTRY_COMPARE(test.streamGets, streams + 1);
        const auto form = controller.channelCreationForm(); const auto context = controller.channelCreationContextKey();
        if (change == "route") controller.selectStream({});
        else if (change == "logout") controller.logout();
        else if (change == "cache-replace") controller.useCache(&replacement);
        else if (change == "cache-destroy") cache.reset();
        else if (change == "snapshot") controller.reloadSnapshot();
        else {
            test.fixture.authentication = [](const Request &request) {
                if (request.target.path().endsWith("/actions/get_token/invoke"))
                    return Response{QJsonObject{{"access_token", "rotated-channel-token"}, {"refresh_token", "synthetic-refresh"}, {"expires_in", 3600}}};
                return sessionResponse(request);
            };
            test.fixture.session.requestRefresh();
        }
        QTRY_VERIFY(test.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(!controller.channelCreationBusy()); QCOMPARE(done.size(), 0); QCOMPARE(test.posts, 1);
        QCOMPARE(test.memberPosts, stage == "members" || stage == "catalog" ? 1 : 0);
        QVERIFY(controller.selectedStream() != test.created.value("uuid").toString());
        QVERIFY(controller.channelCreationContextKey() != context);
        if (change == "logout") { QVERIFY(controller.channelCreationForm().isEmpty()); return; }
        QVERIFY(controller.channelCreationUncertain());
        QCOMPARE(controller.channelCreationForm().value("attemptId"), form.value("attemptId"));
        QCOMPARE(controller.channelCreationForm().value("memberUuids"), form.value("memberUuids"));
        controller.createChannel("Do not replace an unknown attempt", "", false, false, {});
        QCOMPARE(test.posts, 1); QCOMPARE(controller.channelCreationForm().value("attemptId"), form.value("attemptId"));
    }

    void channelCreatedCatalogsSurviveColdMessengerFailureWithoutTrustingCachedAcl()
    {
        QTemporaryDir directory; ChannelCreationFixture test; QVERIFY(test.fixture.start()); QString id;
        {
            NativeCacheService cache(directory.path(), {}, true);
            NativeController current(&test.fixture.api, &test.fixture.session); current.useCache(&cache);
            QTRY_COMPARE(current.streams().size(), 1); QTRY_VERIFY(current.canCreateChannel()); QTRY_VERIFY(current.channelMembersVerified());
            QSignalSpy done(&current, &NativeController::channelCreated);
            current.createChannel("Saved new channel", "", true, false, {channelMemberA}); QTRY_COMPARE(done.size(), 1);
            id = current.selectedStream(); QVERIFY(flushCache(cache));
            QVERIFY(cacheEntry(cache, cache.lease(), {NativeCache::Kind::Streams, {}, {}, {}}));
            QVERIFY(cacheEntry(cache, cache.lease(), {NativeCache::Kind::Topics, id, {}, {}}));
        }
        const auto fallback = test.fixture.server.handler;
        test.fixture.server.handler = [fallback](const Request &request) {
            if (request.method == "GET" && (request.target.path().contains("/messenger/") || request.target.path().endsWith("/users/")))
                return Response{QJsonObject{}, 503};
            return fallback(request);
        };
        NativeCacheService reopened(directory.path(), {}, true);
        NativeController restored(&test.fixture.api, &test.fixture.session); restored.useCache(&reopened);
        QTRY_COMPARE(restored.streams().size(), 2); QVERIFY(!restored.canCreateChannel()); QVERIFY(restored.cacheStale());
        restored.selectStream(id); QTRY_COMPARE(restored.topics().size(), 1);
        QCOMPARE(restored.topics().first().toMap().value("id").toString(), channelDefault);
        QVERIFY(!restored.canCreateTopicIn(id)); QVERIFY(!restored.canSend()); QVERIFY(!restored.canAttach());
        QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 1);
    }

    void channelOwnerOrProjectChangeCannotPublishAnOlderStage_data()
    {
        QTest::addColumn<QString>("stage"); QTest::addColumn<bool>("projectChange");
        for (const auto *stage : {"post", "canonical", "default", "members", "bindings", "catalog"}) {
            QTest::newRow(qPrintable(QString(stage) + "-owner")) << QString(stage) << false;
            QTest::newRow(qPrintable(QString(stage) + "-project")) << QString(stage) << true;
        }
    }
    void channelOwnerOrProjectChangeCannotPublishAnOlderStage()
    {
        QFETCH(QString, stage); QFETCH(bool, projectChange); ChannelCreationFixture test; QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller; QSignalSpy done(&controller, &NativeController::channelCreated);
        if (stage == "post") test.postDelay = 150;
        else if (stage == "canonical") test.getDelay = 150;
        else if (stage == "default") test.topicDelay = 150;
        else if (stage == "members") test.addDelay = 150;
        else if (stage == "bindings") { test.postStatus = 503; test.bindingDelay = 150; }
        else test.catalogDelay = 150;
        const auto catalogs = test.streamGets;
        controller.createChannel("Belongs to the original identity", "", false, false, {channelMemberA});
        if (stage == "bindings") { QTRY_VERIFY(controller.channelCreationUncertain()); controller.checkChannelCreation(); }
        if (stage == "post") QTRY_COMPARE(test.posts, 1);
        else if (stage == "canonical") QTRY_COMPARE(test.gets, 1);
        else if (stage == "default") QTRY_COMPARE(test.topicGets, 1);
        else if (stage == "members") QTRY_COMPARE(test.memberPosts, 1);
        else if (stage == "bindings") QTRY_COMPARE(test.bindingGets, 1);
        else QTRY_COMPARE(test.streamGets, catalogs + 1);
        const auto form = controller.channelCreationForm();
        const auto newProject = projectChange ? returnedId : projectId;
        const auto newUser = projectChange ? userId : returnedId;
        const auto fallback = test.fixture.server.handler;
        test.fixture.server.handler = [fallback, newProject, newUser](const Request &request) {
            auto result = fallback(request); const auto path = request.target.path();
            if (path.endsWith("/actions/introspect")) {
                auto raw = result.data.toObject(); raw["project_id"] = newProject;
                raw["user_info"] = QJsonObject{{"uuid", newUser}}; result.data = raw;
            } else if (path == "/api/core/v1/iam/projects/") {
                result.data = QJsonArray{QJsonObject{{"uuid", newProject}, {"name", "Another project"}, {"status", "ACTIVE"}}};
            } else if (path.endsWith("/streams/") && request.method == "GET") {
                QJsonArray rows;
                for (const auto &value : result.data.toArray()) {
                    auto raw = value.toObject(); raw["project_id"] = newProject; raw["user_uuid"] = newUser; raw["owner"] = newUser; rows.append(raw);
                }
                result.data = rows; result.delay = 0;
            }
            return result;
        };
        controller.login(test.fixture.server.origin().toString(), "another-synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(controller.projects().size(), 1); controller.openProject(newProject);
        QTRY_VERIFY(controller.authenticated()); QTRY_COMPARE(controller.streams().size(), 2);
        QTRY_VERIFY(test.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QVERIFY(controller.channelCreationForm().isEmpty()); QCOMPARE(controller.channelCreationPhase(), "idle");
        QCOMPARE(done.size(), 0); QCOMPARE(test.posts, 1);
        test.fixture.server.handler = fallback;
        controller.login(test.fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(controller.projects().size(), 1); controller.openProject(projectId);
        QTRY_VERIFY(controller.authenticated()); QTRY_COMPARE(controller.streams().size(), 2);
        QVERIFY(controller.channelCreationUncertain());
        QCOMPARE(controller.channelCreationForm().value("attemptId"), form.value("attemptId"));
        QCOMPARE(controller.channelCreationForm().value("memberUuids"), form.value("memberUuids"));
        test.postDelay = test.getDelay = test.topicDelay = test.addDelay = test.bindingDelay = test.catalogDelay = 0;
        controller.checkChannelCreation(); QTRY_VERIFY(!controller.channelCreationBusy());
        QCOMPARE(test.posts, 1);
        if (stage == "members" || stage == "catalog") QCOMPARE(done.size(), 1);
        else { QCOMPARE(controller.channelCreationPhase(), "members-required"); QCOMPARE(done.size(), 0); }
    }

    void channelFollowupDtosRejectMalformedIdentityAndMembership_data()
    {
        QTest::addColumn<QString>("stage"); QTest::addColumn<QString>("field"); QTest::addColumn<QJsonValue>("value");
        for (const auto *stage : {"default", "members", "bindings", "catalog"}) {
            const auto add = [stage](const char *field, QJsonValue value) {
                QTest::newRow(qPrintable(QString(stage) + '-' + field)) << QString(stage) << QString(field) << value;
            };
            add("project_id", returnedId); add("uuid", "not-a-uuid"); add("created_at", "bad-date");
            add("updated_at", 42); add("duplicate", QJsonValue::Null);
            if (QString(stage) != "catalog") add("stream_uuid", returnedId);
            if (QString(stage) == "default") {
                add("user_uuid", returnedId); add("is_default", false); add("is_done", "false");
                add("notification_mode", "unknown-mode"); add("unread_count", -1); add("name", QString(129, 'x'));
            } else if (QString(stage) == "members" || QString(stage) == "bindings") {
                add("user_uuid", "not-a-uuid"); add("who_uuid", "not-a-uuid"); add("role", "unknown-role");
                add("notification_mode", "unknown-mode");
            } else { add("owner", returnedId); add("default_topic_uuid", returnedId); add("role", "guest"); }
        }
    }
    void channelFollowupDtosRejectMalformedIdentityAndMembership()
    {
        QFETCH(QString, stage); QFETCH(QString, field); QFETCH(QJsonValue, value);
        ChannelCreationFixture test; QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QSignalSpy done(&controller, &NativeController::channelCreated); const auto fallback = test.fixture.server.handler;
        test.fixture.server.handler = [&test, stage, field, value, fallback](const Request &request) {
            auto response = fallback(request); const auto path = request.target.path();
            const bool target = !test.created.isEmpty()
                && ((stage == "default" && path.endsWith("/stream_topics/"))
                    || (stage == "members" && path.endsWith("/actions/add_users/invoke"))
                    || (stage == "bindings" && path.endsWith("/stream_bindings/"))
                    || (stage == "catalog" && path.endsWith("/streams/") && request.method == "GET"));
            if (target) {
                auto rows = response.data.toArray(); auto row = rows.last().toObject();
                if (field == "duplicate") rows.append(row);
                else { row[field] = value; rows.replace(rows.size() - 1, row); }
                response.data = rows;
            }
            return response;
        };
        if (stage == "bindings") test.postStatus = 503;
        controller.createChannel("Strict follow-up DTOs", "", false, false, {channelMemberA});
        if (stage == "bindings") {
            QTRY_VERIFY(controller.channelCreationUncertain());
            auto row = creationBinding(channelMemberA, 100); row["stream_uuid"] = test.created.value("uuid"); test.bindings = QJsonArray{row};
            controller.checkChannelCreation();
        }
        QTRY_VERIFY(!controller.channelCreationBusy()); QVERIFY(controller.channelCreationUncertain());
        QCOMPARE(done.size(), 0); QCOMPARE(test.posts, 1);
        QVERIFY(controller.selectedStream() != test.created.value("uuid").toString());
        QCOMPARE(test.memberPosts, stage == "members" || stage == "catalog" ? 1 : 0);
    }

    void channelCreationSubscribersMayDestroyTheController_data()
    {
        QTest::addColumn<QString>("boundary");
        for (const auto *boundary : {"creating", "verifying-stream", "verifying-default", "adding-members",
                 "checking-members", "refreshing-catalogs", "streams", "topics", "ready", "uncertain", "completed"})
            QTest::newRow(boundary) << QString(boundary);
    }
    void channelCreationSubscribersMayDestroyTheController()
    {
        QFETCH(QString, boundary); ChannelCreationFixture test; QVERIFY(test.fixture.start());
        QPointer<NativeController> current = new NativeController(&test.fixture.api, &test.fixture.session);
        const auto cleanup = qScopeGuard([&current] { delete current; });
        current->refresh();
        QTRY_COMPARE(current->streams().size(), 1); QTRY_VERIFY(current->channelMembersVerified());
        if (boundary == "checking-members" || boundary == "uncertain") test.postStatus = 503;
        QObject::connect(current, &NativeController::stateChanged, current, [&current, boundary] {
            if (current && current->channelCreationPhase() == boundary) delete current;
        });
        QObject::connect(current, &NativeController::streamsChanged, current, [&current, &test, boundary] {
            if (boundary != "streams" || !current || test.created.isEmpty()) return;
            for (const auto &row : current->streams()) if (row.toMap().value("id") == test.created.value("uuid").toVariant()) { delete current; return; }
        });
        QObject::connect(current, &NativeController::topicsChanged, current, [&current, &test, boundary] {
            if (boundary == "topics" && current && current->selectedStream() == test.created.value("uuid").toString()) delete current;
        });
        QObject::connect(current, &NativeController::channelCreated, current, [&current, boundary] {
            if (boundary == "completed") delete current;
        });
        current->createChannel("Subscriber lifetime", "", false, false, {channelMemberA});
        if (boundary == "checking-members") {
            QTRY_VERIFY(current && current->channelCreationUncertain()); current->checkChannelCreation();
        }
        QTRY_VERIFY(current.isNull()); QTRY_VERIFY(test.fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(test.posts, boundary == "creating" ? 0 : 1); QVERIFY(test.memberPosts <= 1);
    }

    void channelAllOwnCreationEventsShareOneBoundedReadOnlyReconciliationBudget()
    {
        ChannelCreationFixture test; RealtimeGateway gateway(test.fixture.server.serverPort()); QVector<QJsonObject> events;
        test.fixture.realtime = [&events](const Request &request) {
            if (request.target.path().endsWith("/epoch/")) return Response{QJsonObject{{"epoch_generation", "controller-generation"},
                {"epoch_version", 10}, {"current_epoch_version", 10}, {"minimum_epoch_version", 1}}};
            QJsonArray rows; const auto after = QUrlQuery(request.target).queryItemValue("epoch_version>").toInt();
            for (const auto &event : events) if (event.value("epoch_version").toInt() > after) rows.append(event);
            return Response{rows};
        };
        QVERIFY(test.fixture.start(gateway.origin())); auto &controller = test.fixture.controller;
        RealtimeClient realtime(&test.fixture.api, &test.fixture.session); controller.useRealtime(&realtime);
        QTRY_COMPARE(controller.syncState(), "live"); QTRY_COMPARE(gateway.sockets.size(), 1);
        test.getDelay = test.topicDelay = test.addDelay = 70;
        const auto fallback = test.fixture.server.handler; QSet<QString> sent;
        test.fixture.server.handler = [&test, &controller, &events, &gateway, &sent, fallback](const Request &request) {
            auto result = fallback(request); if (test.created.isEmpty()) return result;
            const auto path = request.target.path(); QString kind; QJsonObject payload;
            if (request.method == "GET" && path == "/api/workspace/v1/messenger/streams/" + test.created.value("uuid").toString()) {
                kind = "stream.created"; payload = test.created;
            } else if (request.method == "GET" && path.endsWith("/stream_topics/")) {
                kind = "topic.created"; payload = test.defaultTopic;
            } else if (request.method == "POST" && path.endsWith("/actions/add_users/invoke")) {
                kind = "stream_bindings.created"; payload = {{"uuid", test.created.value("uuid")}, {"items", test.bindings}};
            }
            if (!kind.isEmpty() && !sent.contains(kind)) {
                sent.insert(kind); const auto event = controllerEvent(kind, payload, 10 + sent.size()); events.append(event);
                QTimer::singleShot(0, &controller, [&gateway, event] {
                    gateway.sockets.last()->sendTextMessage(QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact)));
                });
            }
            return result;
        };
        QSignalSpy done(&controller, &NativeController::channelCreated); const auto streams = test.streamGets;
        controller.createChannel("All own events", "", false, false, {channelMemberA, channelMemberB});
        QTRY_COMPARE(done.size(), 1); QTRY_COMPARE(realtime.acknowledgedEpoch(), quint64(13));
        QCOMPARE(controller.channelCreationPhase(), "ready"); QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 1);
        QCOMPARE(test.gets, 2); QCOMPARE(test.topicGets, 2); QCOMPARE(test.bindingGets, 1); QCOMPARE(test.streamGets, streams + 1);
    }

    void channelPostErrorsDistinguishDefiniteFailureFromUnknownOutcome_data()
    {
        QTest::addColumn<int>("status"); QTest::addColumn<bool>("definite");
        for (const int status : {400, 401, 403, 404, 409, 429, 500, 503})
            QTest::newRow(qPrintable(QString::number(status))) << status << (status == 400 || status == 401 || status == 403 || status == 404);
    }
    void channelPostErrorsDistinguishDefiniteFailureFromUnknownOutcome()
    {
        QFETCH(int, status); QFETCH(bool, definite); ChannelCreationFixture test; QVERIFY(test.fixture.start()); test.postStatus = status;
        auto &controller = test.fixture.controller; QSignalSpy done(&controller, &NativeController::channelCreated);
        controller.createChannel("One definite attempt", "", false, false, {channelMemberA});
        QTRY_VERIFY(!controller.channelCreationBusy()); const auto id = test.created.value("uuid").toString();
        QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 0); QCOMPARE(test.gets, 0); QCOMPARE(done.size(), 0);
        QCOMPARE(controller.channelCreationForm().value("confirmedStreamId").toString(), QString{});
        QCOMPARE(controller.canCreateChannel(), definite); QCOMPARE(controller.canCheckChannelCreation(), !definite);
        if (definite) {
            QCOMPARE(controller.channelCreationPhase(), "failed"); test.postStatus = 201;
            controller.createChannel("Explicit new attempt", "", false, false, {}); QTRY_COMPARE(done.size(), 1);
            QCOMPARE(test.posts, 2); QVERIFY(test.created.value("uuid").toString() != id);
        } else {
            controller.createChannel("Do not replay", "", false, false, {}); QCOMPARE(test.posts, 1);
            controller.checkChannelCreation(); QTRY_COMPARE(controller.channelCreationPhase(), "members-required");
            QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 0);
        }
    }

    void channelPagedCatalogsPublishOnceAndWarmRevisitAvoidsRedundantGet_data()
    {
        QTest::addColumn<QString>("stage");
        for (const auto *stage : {"default", "bindings", "catalog"}) QTest::newRow(stage) << QString(stage);
    }
    void channelPagedCatalogsPublishOnceAndWarmRevisitAvoidsRedundantGet()
    {
        QFETCH(QString, stage); QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        ChannelCreationFixture test; test.fixture.controller.useCache(&cache); QVERIFY(test.fixture.start());
        auto &controller = test.fixture.controller; const auto fallback = test.fixture.server.handler; int pages = 0; bool validMarker = true;
        test.fixture.server.handler = [&test, fallback, stage, &pages, &validMarker](const Request &request) {
            auto response = fallback(request); const auto path = request.target.path();
            const bool target = !test.created.isEmpty() && request.method == "GET"
                && ((stage == "default" && path.endsWith("/stream_topics/"))
                    || (stage == "bindings" && path.endsWith("/stream_bindings/"))
                    || (stage == "catalog" && path.endsWith("/streams/")));
            if (target) {
                ++pages; auto rows = response.data.toArray();
                if (QUrlQuery(request.target).queryItemValue("page_marker").isEmpty()) {
                    // An empty page with a next cursor is valid and bounded.
                    response.data = QJsonArray{}; response.headers["X-Pagination-Marker"] = "next-valid-page";
                } else {
                    validMarker &= QUrlQuery(request.target).queryItemValue("page_marker") == "next-valid-page";
                    response.data = rows;
                }
            }
            return response;
        };
        if (stage == "bindings") test.postStatus = 503;
        QSignalSpy done(&controller, &NativeController::channelCreated);
        controller.createChannel("Paged and cached channel", "", false, false, stage == "bindings" ? QVariantList{channelMemberA} : QVariantList{});
        if (stage == "bindings") {
            QTRY_VERIFY(controller.channelCreationUncertain()); auto row = creationBinding(channelMemberA, 100);
            row["stream_uuid"] = test.created.value("uuid"); test.bindings = {row}; controller.checkChannelCreation();
        }
        QTRY_COMPARE(done.size(), 1); QCOMPARE(pages, 2); QVERIFY(validMarker); QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, 0);
        QTRY_VERIFY(!controller.busy()); QVERIFY(flushCache(cache)); const auto id = controller.selectedStream();
        controller.selectStream(streamId); QTRY_VERIFY(!controller.busy()); QVERIFY(flushCache(cache));
        const auto topics = test.topicGets, history = test.fixture.totalHistoryGets(), catalogs = test.streamGets, users = test.usersGets;
        controller.selectStream(id); QTRY_VERIFY(!controller.busy()); QTRY_COMPARE(controller.topics().size(), 1);
        QCOMPARE(test.topicGets, topics); QCOMPARE(test.fixture.totalHistoryGets(), history);
        QCOMPARE(test.streamGets, catalogs); QCOMPARE(test.usersGets, users); QCOMPARE(test.posts, 1); QCOMPARE(done.size(), 1);
        QCOMPARE(controller.conversationKind(), "stream"); QVERIFY(!controller.canSend());
    }

    void channelDefaultChangeBetweenReadStagesCannotPublishTheEarlierTopicSnapshot_data()
    {
        QTest::addColumn<QString>("boundary");
        for (const auto *boundary : {"adding-members", "checking-members", "refreshing-catalogs"})
            QTest::newRow(boundary) << QString(boundary);
    }
    void channelDefaultChangeBetweenReadStagesCannotPublishTheEarlierTopicSnapshot()
    {
        QFETCH(QString, boundary); ChannelCreationFixture test; QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QSignalSpy done(&controller, &NativeController::channelCreated); bool updated = false;
        if (boundary == "checking-members") test.postStatus = 503;
        QObject::connect(&controller, &NativeController::stateChanged, &controller, [&] {
            if (updated || controller.channelCreationPhase() != boundary) return;
            updated = true; test.defaultTopic["name"] = "Newer topic metadata";
            controller.applyRealtimeEvent(controllerEvent("topic.updated", test.defaultTopic));
        });
        controller.createChannel("A complete snapshot must stay current", "", false, false,
            boundary == "refreshing-catalogs" ? QVariantList{} : QVariantList{channelMemberA});
        if (boundary == "checking-members") {
            QTRY_VERIFY(controller.channelCreationUncertain()); auto row = creationBinding(channelMemberA, 100);
            row["stream_uuid"] = test.created.value("uuid"); test.bindings = {row}; controller.checkChannelCreation();
        }
        QTRY_VERIFY(!controller.channelCreationBusy()); QVERIFY(updated);
        QCOMPARE(done.size(), 0); QVERIFY(controller.channelCreationUncertain());
        QCOMPARE(test.posts, 1); QVERIFY(test.memberPosts <= 1);
        controller.checkChannelCreation(); QTRY_COMPARE(done.size(), 1);
        QCOMPARE(controller.topics().first().toMap().value("name").toString(), QString("Newer topic metadata"));
        QCOMPARE(test.posts, 1); QCOMPARE(test.memberPosts, boundary == "adding-members" ? 1 : 0);
    }


    void aggregateAndTopicWarmNavigationHasNoRedundantHttp()
    {
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.aggregateMessages = true; fixture.extraTopic = true; fixture.controller.useCache(&cache);
        auto a = message(), b = message("B", returnedId, secondTopicId);
        fixture.messages = [=](const Request &request) {
            const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            return Response{topic.isEmpty() ? QJsonArray{a, b} : QJsonArray{a}};
        };
        fixture.controller.login(fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(fixture.controller.projects().size(), 1); fixture.controller.openProject(projectId);
        QTRY_COMPARE(fixture.controller.streams().size(), 1); fixture.controller.selectStream(streamId);
        QTRY_COMPARE(fixture.controller.messages().size(), 2); QTRY_VERIFY(!fixture.controller.busy()); QVERIFY(flushCache(cache));
        fixture.controller.selectTopic(topicId); QTRY_COMPARE(fixture.controller.messages().size(), 1);
        QTRY_VERIFY(!fixture.controller.busy()); QVERIFY(flushCache(cache));
        QCOMPARE(fixture.aggregateHistoryGets, 1); QCOMPARE(fixture.topicHistoryGets, 1); QCOMPARE(fixture.totalHistoryGets(), 2);
        for (int i = 0; i < 3; ++i) {
            fixture.controller.selectStream(streamId); QTRY_COMPARE(fixture.controller.messages().size(), 2); QTRY_VERIFY(!fixture.controller.busy());
            fixture.controller.selectTopic(topicId); QTRY_COMPARE(fixture.controller.messages().size(), 1); QTRY_VERIFY(!fixture.controller.busy());
        }
        QCOMPARE(fixture.aggregateHistoryGets, 1); QCOMPARE(fixture.topicHistoryGets, 1); QCOMPARE(fixture.totalHistoryGets(), 2);
    }

    void aggregateRefreshTraversalIsBounded_data()
    {
        QTest::addColumn<bool>("large"); QTest::newRow("unique-empty-cursors") << false; QTest::newRow("32MiB-payload") << true;
    }
    void aggregateRefreshTraversalIsBounded()
    {
        QFETCH(bool, large); Fixture fixture; fixture.aggregateMessages = true;
        bool traverse = false; int pages = 0;
        fixture.messages = [&](const Request &request) -> Response {
            if (QUrlQuery(request.target).hasQueryItem("topic_uuid") || !traverse) return {QJsonArray{historyMessage(1)}};
            ++pages; QJsonArray items;
            if (large) for (int i = 0; i < 30; ++i) {
                auto row = historyMessage(1000 + pages * 30 + i);
                row["payload"] = QJsonObject{{"kind", "markdown"}, {"content", QString(40000, 'x')}}; items.append(row);
            }
            return {items, 200, 0, {{"X-Pagination-Marker", QByteArray::number(pages)}}};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic({}); QTRY_COMPARE(fixture.controller.messages().size(), 1);
        QSignalSpy failed(&fixture.controller, &NativeController::snapshotFailed);
        traverse = true; fixture.controller.refresh(); QVERIFY(failed.wait());
        QVERIFY(!fixture.controller.error().isEmpty());
        QCOMPARE(pages, large ? 28 : 500); QVERIFY(!fixture.controller.busy());
        QCOMPARE(fixture.controller.messages().size(), 1);
    }

    void aggregateReactionUsesActualTopicAndCanonicalMetadataOnly()
    {
        Fixture fixture; fixture.aggregateMessages = true; fixture.extraTopic = true;
        auto target = message("B payload", returnedId, secondTopicId); target["reactions"] = QJsonObject{}; target["reaction_users"] = QJsonObject{};
        bool added = false; int mutations = 0;
        const QJsonObject fact{{"uuid", messageId}, {"project_id", projectId}, {"source_name", "native"},
            {"message_uuid", returnedId}, {"user_uuid", userId}, {"emoji_name", "heart"},
            {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        fixture.reactions = [&](const Request &request) -> Response {
            if (request.method == "POST") { ++mutations; added = true; return {fact}; }
            return {added ? QJsonArray{fact} : QJsonArray{}};
        };
        fixture.messages = [&](const Request &request) -> Response {
            if (request.target.path().endsWith("/messages/"))
                return {QUrlQuery(request.target).hasQueryItem("topic_uuid") ? QJsonArray{message()} : QJsonArray{message(), target}};
            auto canonical = target; canonical["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "old response content must not replace current B"}};
            canonical["reactions"] = QJsonObject{{"heart", 1}}; canonical["reaction_users"] = QJsonObject{{"heart", QJsonArray{userId}}}; return {canonical};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic({}); QTRY_COMPARE(fixture.controller.messages().size(), 2);
        fixture.controller.toggleReaction(returnedId, "heart"); QTRY_COMPARE(mutations, 1);
        QTRY_VERIFY(!modelMessage(fixture.controller, returnedId).value("reactionBusy").toBool());
        const auto row = modelMessage(fixture.controller, returnedId); QCOMPARE(row.value("content").toString(), QString("B payload"));
        QCOMPARE(row.value("reactions").toList().first().toMap().value("count").toInt(), 1);
        QCOMPARE(row.value("topicId").toString(), secondTopicId);
    }

    void aggregateReadCountersDoNotCancelAnotherTopicConfirmation_data()
    {
        QTest::addColumn<bool>("failLast"); QTest::newRow("both-confirmed") << false; QTest::newRow("last-forbidden") << true;
    }
    void aggregateReadCountersDoNotCancelAnotherTopicConfirmation()
    {
        QFETCH(bool, failLast);
        Fixture fixture; fixture.aggregateMessages = true; fixture.extraTopic = true;
        auto a = message("A"); a["read"] = false; auto b = message("B", returnedId, secondTopicId); b["read"] = false;
        int read = 0, streamGets = 0;
        fixture.topics = [](const Request &) {
            auto a = creationTopic(topicId, streamId, "A"), b = creationTopic(secondTopicId, streamId, "B");
            a["unread_count"] = b["unread_count"] = 3;
            a["active_unread_count"] = b["active_unread_count"] = 3; return Response{QJsonArray{a, b}};
        };
        fixture.counters = [&](const Request &request) -> Response {
            if (request.target.path().contains("/streams/")) { ++streamGets; return {QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                {"name", "Channel"}, {"encryption", false}, {"unread_count", 6 - read}, {"active_unread_count", 6 - read},
                {"passive_unread_count", 0}, {"last_message_uuid", QJsonValue::Null}}}; }
            const auto id = request.target.path().section('/', -1); auto topic = creationTopic(id, streamId, "unchanged name");
            topic["unread_count"] = 2; topic["active_unread_count"] = 2;
            return {topic, 200, id == topicId ? 120 : 0};
        };
        fixture.messages = [&](const Request &request) -> Response {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) {
                const auto isA = request.target.path().contains(messageId);
                if (!isA && failLast) return {QJsonObject{}, 403};
                ++read; auto result = isA ? a : b; result["read"] = true; return {result};
            }
            return {QUrlQuery(request.target).hasQueryItem("topic_uuid") ? QJsonArray{a} : QJsonArray{a, b}};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic({}); QTRY_COMPARE(fixture.controller.messages().size(), 2);
        fixture.controller.markVisibleMessagesRead({messageId, returnedId});
        const auto unread = [&](const QString &id) {
            for (const auto &row : fixture.controller.topics()) if (row.toMap().value("id") == id) return row.toMap().value("unread").toInt();
            return -1;
        };
        if (!failLast) QTRY_COMPARE(unread(secondTopicId), 2);
        QTRY_COMPARE(unread(topicId), 2);
        QTRY_COMPARE(fixture.controller.streams().first().toMap().value("unread").toInt(), failLast ? 5 : 4);
        QCOMPARE(read, failLast ? 1 : 2); QCOMPARE(streamGets, 1);
    }

    void aggregateReadBatchMakesOneStreamCounterRequestForManyTopics()
    {
        Fixture fixture; fixture.aggregateMessages = true;
        QJsonArray topics, rows; QVariantList visible;
        for (int n = 0; n < 10; ++n) {
            const auto topic = n == 0 ? topicId : historyMessage(1000 + n).value("uuid").toString();
            topics.append(creationTopic(topic, streamId, QString::number(n)));
            auto row = historyMessage(n + 1); row["topic_uuid"] = topic; row["read"] = false; rows.append(row); visible.append(row.value("uuid").toString());
        }
        int streamGets = 0, topicGets = 0, readPosts = 0;
        fixture.topics = [=](const Request &) { return Response{topics}; };
        fixture.counters = [&](const Request &request) -> Response {
            if (request.target.path().contains("/streams/")) { ++streamGets; return {QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                {"name", "Channel"}, {"encryption", false}, {"unread_count", 0}, {"active_unread_count", 0}, {"passive_unread_count", 0}, {"last_message_uuid", QJsonValue::Null}}}; }
            ++topicGets; return {creationTopic(request.target.path().section('/', -1), streamId, "Topic")};
        };
        fixture.messages = [&](const Request &request) -> Response {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) {
                ++readPosts; const auto id = request.target.path().split('/').value(6);
                for (const auto &value : rows) if (value.toObject().value("uuid") == id) { auto result = value.toObject(); result["read"] = true; return {result}; }
            }
            const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid"); QJsonArray result;
            for (const auto &row : rows) if (topic.isEmpty() || row.toObject().value("topic_uuid") == topic) result.append(row);
            return {result};
        };
        // This fixture has ten real topics, not only the default two aliases.
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [&](const Request &request) {
            if (request.target.path().startsWith("/api/workspace/v1/messenger/stream_topics/")
                && isUuid(request.target.path().section('/', -1))) return fixture.counters(request);
            return fallback(request);
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic({}); QTRY_COMPARE(fixture.controller.messages().size(), 10);
        fixture.controller.markVisibleMessagesRead(visible); QTRY_COMPARE(topicGets, 10); QTRY_VERIFY(streamGets > 0);
        QCOMPARE(readPosts, 10); QCOMPARE(streamGets, 1);
    }

    void aggregateDeferredStreamReadRefreshIsFencedByRouteAndSession_data()
    {
        QTest::addColumn<QString>("change"); for (const auto &value : {"route", "logout", "refresh"}) QTest::newRow(value) << QString(value);
    }
    void aggregateDeferredStreamReadRefreshIsFencedByRouteAndSession()
    {
        QFETCH(QString, change); Fixture fixture; fixture.aggregateMessages = true; fixture.extraTopic = true;
        auto a = message("A"); a["read"] = false; auto b = message("B", returnedId, secondTopicId); b["read"] = false;
        int streamGets = 0, bPosts = 0;
        fixture.counters = [&](const Request &request) -> Response {
            if (request.target.path().contains("/streams/")) { ++streamGets; return {QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                {"name", "Channel"}, {"encryption", false}, {"unread_count", 0}, {"active_unread_count", 0}, {"passive_unread_count", 0}, {"last_message_uuid", QJsonValue::Null}}}; }
            return {creationTopic(request.target.path().section('/', -1), streamId, "Topic")};
        };
        fixture.messages = [&](const Request &request) -> Response {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) {
                if (request.target.path().contains(returnedId)) { ++bPosts; return {QJsonObject{}, 403, 150}; }
                auto result = a; result["read"] = true; return {result};
            }
            return {QUrlQuery(request.target).hasQueryItem("topic_uuid") ? QJsonArray{a} : QJsonArray{a, b}};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic({}); QTRY_COMPARE(fixture.controller.messages().size(), 2);
        fixture.controller.markVisibleMessagesRead({messageId, returnedId}); QTRY_COMPARE(bPosts, 1);
        if (change == "route") fixture.controller.selectStream({});
        else if (change == "logout") fixture.controller.logout(); else fixture.session.requestRefresh();
        QTRY_VERIFY(fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(streamGets, 0);
    }

    void aggregateTopicDeletionRemovesRowsWithoutAnUnrequestedRefill()
    {
        Fixture fixture; fixture.aggregateMessages = true; fixture.extraTopic = true;
        fixture.messages = [](const Request &request) { return Response{QUrlQuery(request.target).hasQueryItem("topic_uuid")
            ? QJsonArray{message()} : QJsonArray{message(), message("B", returnedId, secondTopicId)}}; };
        QVERIFY(fixture.start()); fixture.controller.selectTopic({}); QTRY_COMPARE(fixture.controller.messages().size(), 2);
        QTRY_VERIFY(fixture.api.findChildren<QNetworkReply *>().isEmpty()); const auto before = fixture.totalHistoryGets();
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.deleted", QJsonObject{{"uuid", secondTopicId}, {"stream_uuid", streamId}})));
        QCOMPARE(fixture.controller.messages().size(), 1); QVERIFY(modelMessage(fixture.controller, returnedId).isEmpty());
        QCOMPARE(fixture.controller.topics().size(), 1); QCOMPARE(fixture.totalHistoryGets(), before);
        QVERIFY(fixture.api.findChildren<QNetworkReply *>().isEmpty());
    }

    void aggregatePaginationReconcilesLoadedRowsAndRecoversDeletedMarker()
    {
        Fixture fixture; fixture.aggregateMessages = true; fixture.extraTopic = true;
        QMap<int, QJsonObject> rows; int missingMarkers = 0;
        for (int n = 1; n <= 120; ++n) { auto row = historyMessage(n); row["topic_uuid"] = n % 2 ? topicId : secondTopicId; rows[n] = row; }
        fixture.messages = [&](const Request &request) {
            auto scope = rows; const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            if (!topic.isEmpty()) for (auto it = scope.begin(); it != scope.end();) {
                if (it->value("topic_uuid") != topic) it = scope.erase(it); else ++it;
            }
            auto response = historyResponse(scope, request); if (response.status == 404) ++missingMarkers; return response;
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic({}); QTRY_COMPARE(fixture.controller.messages().size(), 50);
        fixture.controller.loadOlder(); QTRY_COMPARE(fixture.controller.messages().size(), 100);
        auto &edited = rows[30]; edited["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Older B edit"}};
        rows.remove(21); fixture.controller.loadOlder();
        QTRY_COMPARE(fixture.controller.messages().size(), 119); QTRY_VERIFY(!fixture.controller.busy());
        QCOMPARE(missingMarkers, 1); QVERIFY(fixture.controller.error().isEmpty());
        QCOMPARE(modelMessage(fixture.controller, historyMessage(30).value("uuid").toString()).value("content").toString(), QString("Older B edit"));
        QVERIFY(modelMessage(fixture.controller, historyMessage(21).value("uuid").toString()).isEmpty());
    }

    void aggregateConfirmedReadPersistsBothPageFamiliesThroughOfflineRestart()
    {
        QTemporaryDir directory; Fixture fixture; fixture.aggregateMessages = true; fixture.extraTopic = true;
        auto a = message("Saved A"); a["read"] = false; auto b = message("Saved B", returnedId, secondTopicId); b["read"] = false;
        bool offline = false; int reads = 0;
        fixture.messages = [&](const Request &request) -> Response {
            if (offline) return {QJsonObject{}, 503};
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) { ++reads; auto result = a; result["read"] = true; return {result}; }
            return {QUrlQuery(request.target).hasQueryItem("topic_uuid") ? QJsonArray{a} : QJsonArray{a, b}};
        };
        {
            NativeCacheService cache(directory.path(), {}, true); NativeController current(&fixture.api, &fixture.session); current.useCache(&cache);
            current.login(fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
            QTRY_COMPARE(current.projects().size(), 1); current.openProject(projectId); QTRY_COMPARE(current.streams().size(), 1);
            current.selectStream(streamId); QTRY_COMPARE(current.messages().size(), 2); QTRY_VERIFY(!current.busy()); QVERIFY(flushCache(cache));
            current.selectTopic(topicId); QTRY_COMPARE(current.messages().size(), 1); QTRY_VERIFY(!current.busy()); QVERIFY(flushCache(cache));
            current.markMessagesReadUpTo(messageId); QTRY_VERIFY(modelMessage(current, messageId).value("read").toBool()); QVERIFY(flushCache(cache));
            QCOMPARE(fixture.totalHistoryGets(), 2);
        }
        offline = true;
        fixture.streams = fixture.topics = fixture.users = [](const Request &) { return Response{QJsonObject{}, 503}; };
        NativeCacheService cache(directory.path(), {}, true); ApiClient api(nullptr, true); Session session(&api); NativeController current(&api, &session); current.useCache(&cache);
        current.login(fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(current.projects().size(), 1); current.openProject(projectId); QTRY_COMPARE(current.streams().size(), 1);
        current.selectStream(streamId); QTRY_COMPARE(current.messages().size(), 2); QTRY_VERIFY(!current.busy());
        QVERIFY(modelMessage(current, messageId).value("read").toBool()); QVERIFY(!modelMessage(current, returnedId).value("read").toBool());
        current.selectTopic(topicId); QTRY_COMPARE(current.messages().size(), 1); QTRY_VERIFY(!current.busy());
        QVERIFY(modelMessage(current, messageId).value("read").toBool()); QVERIFY(current.cacheStale());
        QVERIFY(!current.canSend()); QVERIFY(!modelMessage(current, messageId).value("canReact").toBool());
        QCOMPARE(reads, 1); QCOMPARE(fixture.aggregateHistoryGets, 2); QCOMPARE(fixture.topicHistoryGets, 2);
    }

    void aggregateRejectsMalformedRowsWithoutPartiallyPublishing_data()
    {
        QTest::addColumn<QString>("fault");
        for (const auto &name : {"project", "stream", "topic", "timestamp", "duplicate"}) QTest::newRow(name) << QString(name);
    }
    void aggregateRejectsMalformedRowsWithoutPartiallyPublishing()
    {
        QFETCH(QString, fault); Fixture fixture; fixture.aggregateMessages = true; fixture.extraTopic = true; bool invalid = false;
        fixture.messages = [&](const Request &request) -> Response {
            auto row = message(); QJsonArray result{row};
            if (invalid && !QUrlQuery(request.target).hasQueryItem("topic_uuid")) {
                if (fault == "project") row["project_id"] = returnedId;
                if (fault == "stream") row["stream_uuid"] = returnedId;
                if (fault == "topic") row.remove("topic_uuid");
                if (fault == "timestamp") row["created_at"] = "untrusted";
                result.append(row);
            }
            return {result};
        };
        QVERIFY(fixture.start()); invalid = true; fixture.controller.selectTopic({});
        QTRY_VERIFY(!fixture.controller.error().isEmpty()); QVERIFY(fixture.controller.messages().isEmpty());
    }

    void aggregateMutationsVerifyTheActualPlacementIdentity_data()
    {
        QTest::addColumn<QString>("operation"); QTest::addColumn<QString>("fault");
        for (const auto &op : {"edit", "star"}) for (const auto &fault : {"none", "topic", "author"})
            QTest::newRow(qPrintable(QString(op) + '-' + fault)) << QString(op) << QString(fault);
    }
    void aggregateMutationsVerifyTheActualPlacementIdentity()
    {
        QFETCH(QString, operation); QFETCH(QString, fault); Fixture fixture; fixture.aggregateMessages = true; fixture.extraTopic = true;
        auto original = message("Actual B content", returnedId, secondTopicId), canonical = original;
        int mutations = 0, confirmations = 0;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method == "PUT" || request.method == "POST") {
                ++mutations;
                if (operation == "edit") canonical["payload"] = request.body.value("payload"); else canonical["starred"] = true;
                return {canonical};
            }
            if (request.target.path().endsWith("/messages/")) {
                return {QUrlQuery(request.target).hasQueryItem("topic_uuid") ? QJsonArray{message()} : QJsonArray{message(), original}};
            }
            ++confirmations; auto reply = canonical;
            if (fault == "topic") reply["topic_uuid"] = topicId;
            if (fault == "author") reply["author_uuid"] = messageId;
            return {reply};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic({}); QTRY_COMPARE(fixture.controller.messages().size(), 2);
        if (operation == "edit") fixture.controller.editMessage(returnedId, "New B content"); else fixture.controller.starMessage(returnedId);
        QTRY_COMPARE(confirmations, 1); QCOMPARE(mutations, 1);
        if (fault != "none") QTRY_VERIFY(!fixture.controller.error().isEmpty());
        else if (operation == "edit") QTRY_COMPARE(modelMessage(fixture.controller, returnedId).value("content").toString(), QString("New B content"));
        else QTRY_VERIFY(modelMessage(fixture.controller, returnedId).value("starred").toBool());
        const auto row = modelMessage(fixture.controller, returnedId);
        QCOMPARE(row.value("topicId").toString(), secondTopicId); QCOMPARE(row.value("authorId").toString(), userId);
        if (fault != "none") { QCOMPARE(row.value("content").toString(), QString("Actual B content")); QVERIFY(!row.value("starred").toBool()); }
    }

    void aggregateReadFailureContinuesWithAnotherTopic_data()
    {
        QTest::addColumn<QString>("fault");
        for (const auto &fault : {"forbidden", "transient", "wrong-topic", "wrong-owner"}) QTest::newRow(fault) << QString(fault);
    }
    void aggregateReadFailureContinuesWithAnotherTopic()
    {
        QFETCH(QString, fault); Fixture fixture; fixture.aggregateMessages = true; fixture.extraTopic = true;
        auto first = message("A"); first["read"] = false;
        auto second = message("B", returnedId, secondTopicId); second["read"] = false;
        int a = 0, b = 0;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) {
                const auto isA = request.target.path().contains(messageId); auto row = isA ? first : second; row["read"] = true;
                if (isA) {
                    ++a;
                    if (fault == "forbidden" || fault == "transient") return {QJsonObject{}, fault == "forbidden" ? 403 : 503};
                    if (fault == "wrong-topic") row["topic_uuid"] = secondTopicId;
                    if (fault == "wrong-owner") row["user_uuid"] = messageId;
                } else ++b;
                return {row};
            }
            return {QUrlQuery(request.target).hasQueryItem("topic_uuid") ? QJsonArray{first} : QJsonArray{first, second}};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic({}); QTRY_COMPARE(fixture.controller.messages().size(), 2);
        fixture.controller.markVisibleMessagesRead({messageId, returnedId});
        QTRY_VERIFY(modelMessage(fixture.controller, returnedId).value("read").toBool());
        QCOMPARE(a, fault == "transient" ? 3 : 1); QCOMPARE(b, 1);
        QVERIFY(!modelMessage(fixture.controller, messageId).value("read").toBool());
        fixture.controller.markVisibleMessagesRead({messageId, returnedId});
        QCOMPARE(b, 1); QCOMPARE(a, fault == "transient" ? 3 : 1);
    }

    void aggregateReadHasBoundedInputAndDoesNotOverwriteConcurrentContent()
    {
        Fixture fixture; fixture.aggregateMessages = true; fixture.extraTopic = true; int reads = 0;
        auto first = message("Captured A"); first["read"] = false;
        auto second = message("Hidden B", returnedId, secondTopicId); second["read"] = false;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) { ++reads; auto result = first; result["read"] = true; return {result, 200, 100}; }
            return {QUrlQuery(request.target).hasQueryItem("topic_uuid") ? QJsonArray{first} : QJsonArray{first, second}};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic({}); QTRY_COMPARE(fixture.controller.messages().size(), 2);
        QVariantList tooMany; for (int i = 0; i < 257; ++i) tooMany.append(messageId);
        fixture.controller.markVisibleMessagesRead(tooMany); QCOMPARE(reads, 0);
        fixture.controller.markVisibleMessagesRead({messageId}); QTRY_COMPARE(reads, 1);
        auto live = first; live["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Newer edited A"}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", live)));
        QTRY_VERIFY(modelMessage(fixture.controller, messageId).value("read").toBool());
        QCOMPARE(modelMessage(fixture.controller, messageId).value("content").toString(), QString("Newer edited A"));
        QVERIFY(!modelMessage(fixture.controller, returnedId).value("read").toBool());
    }

    void aggregateReadDepartureCancelsOtherTopicIntents()
    {
        Fixture fixture; fixture.aggregateMessages = true; fixture.extraTopic = true; int reads = 0;
        auto first = message(); first["read"] = false; auto second = message("B", returnedId, secondTopicId); second["read"] = false;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) { ++reads; auto result = first; result["read"] = true; return {result, 200, 150}; }
            return {QUrlQuery(request.target).hasQueryItem("topic_uuid") ? QJsonArray{first} : QJsonArray{first, second}};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic({}); QTRY_COMPARE(fixture.controller.messages().size(), 2);
        fixture.controller.markVisibleMessagesRead({messageId, returnedId}); QTRY_COMPARE(reads, 1);
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy());
        QTRY_VERIFY(fixture.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(reads, 1); QVERIFY(!modelMessage(fixture.controller, messageId).value("read").toBool());
    }

    void aggregateTopicRailUsesActivityThenMutedAndClosedGroups()
    {
        Fixture fixture; fixture.aggregateMessages = true;
        const QString mutedId = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", closedId = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
        auto old = creationTopic(topicId, streamId, "Z old"), recent = creationTopic(secondTopicId, streamId, "A recent");
        auto muted = creationTopic(mutedId, streamId, "Muted"), closed = creationTopic(closedId, streamId, "Closed");
        old["updated_at"] = "2026-01-01T00:00:00.000001Z"; recent["updated_at"] = "2026-01-02T00:00:00.000001Z";
        muted["updated_at"] = "2026-01-03T00:00:00Z"; muted["notification_mode"] = "mute";
        closed["updated_at"] = "2026-01-04T00:00:00Z"; closed["is_done"] = true;
        fixture.topics = [=](const Request &) { return Response{QJsonArray{closed, muted, old, recent}}; };
        QVERIFY(fixture.start());
        const auto topics = fixture.controller.topics();
        QCOMPARE(topics.at(0).toMap().value("id").toString(), secondTopicId);
        QCOMPARE(topics.at(1).toMap().value("id").toString(), topicId);
        QCOMPARE(topics.at(2).toMap().value("id").toString(), mutedId);
        QCOMPARE(topics.at(3).toMap().value("id").toString(), closedId);
    }

    void aggregateVisibleReadCoalescesSeparatelyForEachActualTopic()
    {
        Fixture fixture; fixture.aggregateMessages = true; fixture.extraTopic = true;
        QMap<QString, QJsonObject> rows;
        for (int n = 1; n <= 5; ++n) {
            auto row = historyMessage(n); row["topic_uuid"] = n % 2 ? topicId : secondTopicId;
            row["read"] = false; rows.insert(row.value("uuid").toString(), row);
        }
        QList<QString> anchors;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) {
                const auto id = request.target.path().split('/').value(6); anchors.append(id);
                auto anchor = rows.value(id); anchor["read"] = true;
                return {anchor, 200, 30};
            }
            if (!request.target.path().endsWith("/messages/")) return {rows.value(request.target.path().section('/', -1))};
            QJsonArray result; const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            for (const auto &row : rows) if (topic.isEmpty() || row.value("topic_uuid") == topic) result.prepend(row);
            return {result};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic({}); QTRY_COMPARE(fixture.controller.messages().size(), 5);
        const auto a1 = historyMessage(1).value("uuid").toString();
        const auto a3 = historyMessage(3).value("uuid").toString();
        const auto b2 = historyMessage(2).value("uuid").toString();
        const QVariantList visible{a1, b2, a3};
        fixture.controller.markVisibleMessagesRead(visible); fixture.controller.markVisibleMessagesRead(visible);
        QTRY_COMPARE(anchors.size(), 2);
        QVERIFY(anchors.contains(a3)); QVERIFY(anchors.contains(b2));
        QTRY_VERIFY(modelMessage(fixture.controller, a3).value("read").toBool());
        QTRY_VERIFY(modelMessage(fixture.controller, b2).value("read").toBool());
        QVERIFY(modelMessage(fixture.controller, a1).value("read").toBool());
        QVERIFY(!modelMessage(fixture.controller, historyMessage(4).value("uuid").toString()).value("read").toBool());
        QVERIFY(!modelMessage(fixture.controller, historyMessage(5).value("uuid").toString()).value("read").toBool());
    }

    void aggregateLoadsActualTopicsAndRealtimeWithoutADefaultComposer()
    {
        Fixture fixture; fixture.aggregateMessages = true; fixture.extraTopic = true;
        auto first = message("A body"); auto second = message("B body", returnedId, secondTopicId);
        second["created_at"] = "2026-01-02T10:00:00.000001Z";
        int aggregateGets = 0;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method != "GET") return {QJsonObject{}, 400};
            const auto query = QUrlQuery(request.target);
            if (!query.hasQueryItem("topic_uuid")) { ++aggregateGets; return {QJsonArray{second, first}}; }
            return {QJsonArray{first}};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic({});
        QTRY_COMPARE(fixture.controller.messages().size(), 2);
        QCOMPARE(fixture.controller.property("conversationKind").toString(), QString("stream"));
        QVERIFY(fixture.controller.property("hasConversation").toBool()); QVERIFY(aggregateGets > 0);
        const auto a = modelMessage(fixture.controller, messageId), b = modelMessage(fixture.controller, returnedId);
        QCOMPARE(a.value("topicId").toString(), topicId); QCOMPARE(b.value("topicId").toString(), secondTopicId);
        QCOMPARE(a.value("authorId").toString(), userId); QCOMPARE(b.value("topicName").toString(), QString("Second topic"));
        fixture.controller.setDraft("must not become implicit General draft");
        QVERIFY(!fixture.controller.beginReply(messageId)); // An implicit General stream has no reply slot.
        QVERIFY(fixture.controller.replyMessageId().isEmpty());
        QVERIFY(fixture.controller.draft().isEmpty()); QVERIFY(!fixture.controller.canSend()); QVERIFY(!fixture.controller.canAttach());
        first["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Live A"}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", first)));
        QCOMPARE(modelMessage(fixture.controller, messageId).value("content").toString(), QString("Live A"));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated", creationTopic(secondTopicId, streamId, "Renamed B"))));
        QCOMPARE(modelMessage(fixture.controller, returnedId).value("topicName").toString(), QString("Renamed B"));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.deleted", QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId}, {"topic_uuid", secondTopicId}})));
        QCOMPARE(fixture.controller.messages().size(), 1);
    }

    void topicCreationPostsOnceConfirmsAndNavigatesAfterTheCatalog()
    {
        TopicCreationFixture test; auto &controller = test.fixture.controller;
        QVERIFY(test.fixture.start()); controller.setDraft("Existing draft");
        test.postDelay = 50; test.catalogDelay = 80;
        QSignalSpy created(&controller, &NativeController::topicCreated);
        QVERIFY(controller.canCreateTopicIn(streamId));
        controller.createTopic(streamId, "  Created topic  ");
        controller.createTopic(streamId, "Duplicate");
        QVERIFY(controller.topicCreationBusy());
        QCOMPARE(controller.topicCreationName(), "Created topic");
        QTRY_COMPARE(test.posts, 1);
        QVERIFY(isUuid(test.created.value("uuid").toString()));
        QTRY_COMPARE(test.confirmations, 1);
        QCOMPARE(created.size(), 0);
        QTRY_COMPARE(created.size(), 1);
        QCOMPARE(test.posts, 1); QCOMPARE(test.catalogs, 1);
        QCOMPARE(controller.selectedTopic(), test.created.value("uuid").toString());
        QCOMPARE(controller.topics().size(), 2);
        controller.selectTopic(topicId); QCOMPARE(controller.draft(), "Existing draft");
        for (const auto &request : test.fixture.server.requests)
            if (request.method == "POST" && request.target.path().endsWith("/stream_topics/")) {
                QCOMPARE(request.body.size(), 3);
                QCOMPARE(request.body.value("name").toString(), "Created topic");
                QCOMPARE(request.headers.value("authorization"), QByteArray("Bearer synthetic-project-token"));
            }
    }

    void topicCreationNameValidationCountsUnicodeCodePoints_data()
    {
        QTest::addColumn<QString>("name"); QTest::addColumn<bool>("valid");
        QTest::newRow("empty") << QString("  ") << false;
        QTest::newRow("129-ascii") << QString(129, 'a') << false;
        QTest::newRow("128-astral") << QString::fromUtf8("😀").repeated(128) << true;
        QTest::newRow("129-astral") << QString::fromUtf8("😀").repeated(129) << false;
    }
    void topicCreationNameValidationCountsUnicodeCodePoints()
    {
        QFETCH(QString, name); QFETCH(bool, valid);
        TopicCreationFixture test; QVERIFY(test.fixture.start());
        test.fixture.controller.createTopic(streamId, name);
        if (valid) { QTRY_COMPARE(test.posts, 1); QTRY_VERIFY(!test.fixture.controller.topicCreationBusy()); }
        else {
            QCOMPARE(test.posts, 0); QVERIFY(!test.fixture.controller.topicCreationError().isEmpty());
            QCOMPARE(test.fixture.controller.topicCreationStream(), streamId);
            QCOMPARE(test.fixture.controller.topicCreationName(), name.trimmed());
        }
    }

    void topicCreationKeepsAmbiguousIdentityForCheckOnly()
    {
        TopicCreationFixture test; QVERIFY(test.fixture.start()); test.postStatus = 503;
        auto &controller = test.fixture.controller;
        QSignalSpy created(&controller, &NativeController::topicCreated);
        controller.createTopic(streamId, "Committed but acknowledgement lost");
        QTRY_COMPARE(test.posts, 1); QTRY_VERIFY(controller.topicCreationUncertain());
        const auto id = test.created.value("uuid").toString();
        controller.createTopic(streamId, "Do not repeat");
        test.getStatus = 404; controller.checkTopicCreation(); controller.checkTopicCreation();
        QTRY_COMPARE(test.confirmations, 1); QTRY_VERIFY(!controller.topicCreationBusy());
        QVERIFY(controller.topicCreationUncertain()); QCOMPARE(created.size(), 0); QCOMPARE(test.posts, 1);
        test.getStatus = 200; controller.checkTopicCreation();
        QTRY_COMPARE(created.size(), 1);
        QCOMPARE(test.confirmations, 2); QCOMPARE(test.posts, 1); QCOMPARE(controller.selectedTopic(), id);
    }

    void topicCreationRejectsInvalidCanonicalIdentity_data()
    {
        QTest::addColumn<QString>("field");
        for (const auto *field : {"uuid", "project_id", "stream_uuid", "user_uuid", "name", "is_done", "unread_count"})
            QTest::newRow(field) << QString::fromLatin1(field);
    }
    void topicCreationRejectsInvalidCanonicalIdentity()
    {
        QFETCH(QString, field);
        TopicCreationFixture test; QVERIFY(test.fixture.start());
        test.alterGet = [field](QJsonObject &row) {
            row[field] = field == "name" ? QJsonValue(42) : field == "is_done" ? QJsonValue("false")
                : field == "unread_count" ? QJsonValue(-1) : QJsonValue(returnedId);
        };
        QSignalSpy created(&test.fixture.controller, &NativeController::topicCreated);
        test.fixture.controller.createTopic(streamId, "Validation");
        QTRY_COMPARE(test.confirmations, 1); QTRY_VERIFY(!test.fixture.controller.topicCreationBusy());
        QVERIFY(test.fixture.controller.topicCreationUncertain()); QCOMPARE(created.size(), 0);
        QCOMPARE(test.fixture.controller.selectedTopic(), topicId); QCOMPARE(test.catalogs, 0);
    }

    void topicCreationRouteChangeSuppressesDelayedNavigation()
    {
        TopicCreationFixture test; QVERIFY(test.fixture.start()); test.postDelay = 100;
        auto &controller = test.fixture.controller; QSignalSpy created(&controller, &NativeController::topicCreated);
        controller.createTopic(streamId, "Route fence"); QTRY_COMPARE(test.posts, 1);
        controller.selectTopic({});
        QTRY_VERIFY(!controller.topicCreationBusy());
        QCOMPARE(controller.selectedTopic(), QString{}); QCOMPARE(created.size(), 0);
        QVERIFY(controller.topicCreationUncertain()); QCOMPARE(test.confirmations, 0);
        controller.checkTopicCreation(); QTRY_COMPARE(created.size(), 1); QCOMPARE(test.posts, 1);
    }

    void topicCreationDoesNotInventOwnerOrEncryptionRestrictions_data()
    {
        QTest::addColumn<QString>("kind");
        for (const auto *kind : {"encrypted", "direct", "provider", "guest", "member", "moderator", "administrator", "owner"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void topicCreationDoesNotInventOwnerOrEncryptionRestrictions()
    {
        QFETCH(QString, kind);
        TopicCreationFixture test;
        test.fixture.streams = [kind](const Request &) {
            const QSet<QString> roles{"guest", "member", "moderator", "administrator", "owner"};
            return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Channel"},
                {"encryption", kind == "encrypted"}, {"role", roles.contains(kind) ? kind : QString("member")},
                {"direct_user_uuid", kind == "direct" ? QJsonValue(returnedId) : QJsonValue::Null},
                {"source_name", kind == "provider" ? "zulip" : "native"}}}};
        };
        QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        controller.selectStream({}); QVERIFY(controller.canCreateTopicIn(streamId));
        QSignalSpy created(&controller, &NativeController::topicCreated);
        controller.createTopic(streamId, "Metadata only"); QTRY_COMPARE(created.size(), 1);
        QCOMPARE(controller.selectedStream(), streamId); QCOMPARE(controller.selectedTopic(), test.created.value("uuid").toString());
        QCOMPARE(test.posts, 1); QCOMPARE(test.catalogs, 1);
    }

    void topicCreationFromAnUnselectedStreamNavigatesOnlyAfterVerifiedCatalog()
    {
        TopicCreationFixture test; QVERIFY(test.fixture.start());
        test.fixture.streams = [](const Request &) {
            return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "First"}, {"encryption", false}},
                QJsonObject{{"uuid", returnedId}, {"project_id", projectId}, {"name", "Target"}, {"encryption", false}}}};
        };
        auto &controller = test.fixture.controller; controller.refresh(); QTRY_COMPARE(controller.streams().size(), 2);
        QTRY_VERIFY(!controller.busy()); QSignalSpy created(&controller, &NativeController::topicCreated);
        test.catalogDelay = 80; controller.createTopic(returnedId, "Target topic"); QTRY_COMPARE(test.catalogs, 1);
        QCOMPARE(controller.selectedStream(), streamId); QCOMPARE(created.size(), 0);
        QTRY_COMPARE(created.size(), 1); QCOMPARE(controller.selectedStream(), returnedId);
        QCOMPARE(controller.selectedTopic(), test.created.value("uuid").toString()); QCOMPARE(test.catalogs, 1);
    }

    void topicCreationPostFailuresHaveDistinctRetrySemantics_data()
    {
        QTest::addColumn<int>("status"); QTest::addColumn<bool>("uncertain");
        for (const auto status : {400, 401, 403, 404, 408, 409, 429, 503})
            QTest::newRow(QByteArray::number(status)) << status << (status != 400 && status != 401 && status != 403 && status != 404);
    }
    void topicCreationPostFailuresHaveDistinctRetrySemantics()
    {
        QFETCH(int, status); QFETCH(bool, uncertain);
        TopicCreationFixture test; QVERIFY(test.fixture.start()); test.postStatus = status;
        auto &controller = test.fixture.controller; QSignalSpy created(&controller, &NativeController::topicCreated);
        controller.createTopic(streamId, "Preserved input"); QTRY_COMPARE(test.posts, 1); QTRY_VERIFY(!controller.topicCreationBusy());
        QCOMPARE(controller.topicCreationUncertain(), uncertain); QVERIFY(!controller.topicCreationError().isEmpty());
        QCOMPARE(controller.topicCreationName(), "Preserved input"); QCOMPARE(test.confirmations, 0); QCOMPARE(created.size(), 0);
        const auto id = test.created.value("uuid").toString();
        test.postStatus = 201; controller.createTopic(streamId, "Explicit next attempt");
        if (uncertain) { QCOMPARE(test.posts, 1); QCOMPARE(test.created.value("uuid").toString(), id); }
        else { QTRY_COMPARE(created.size(), 1); QCOMPARE(test.posts, 2); QVERIFY(test.created.value("uuid").toString() != id); }
    }

    void topicCreationCanonicalGetIsFencedByLiveUpdateOrDeletion_data()
    {
        QTest::addColumn<bool>("deleted"); QTest::newRow("update") << false; QTest::newRow("delete") << true;
    }
    void topicCreationCanonicalGetIsFencedByLiveUpdateOrDeletion()
    {
        QFETCH(bool, deleted);
        TopicCreationFixture test; QVERIFY(test.fixture.start()); test.getDelay = 120;
        auto &controller = test.fixture.controller; QSignalSpy created(&controller, &NativeController::topicCreated);
        controller.createTopic(streamId, "Before live event"); QTRY_COMPARE(test.confirmations, 1);
        auto live = test.created; live["name"] = "Newer live name";
        QVERIFY(controller.applyRealtimeEvent(controllerEvent(deleted ? "topic.deleted" : "topic.updated", live)));
        QTRY_VERIFY(!controller.topicCreationBusy()); QCOMPARE(created.size(), 0); QCOMPARE(controller.selectedTopic(), topicId);
        QTRY_COMPARE(test.fixture.api.findChildren<QNetworkReply *>().size(), 0);
        if (deleted) {
            QVERIFY(!controller.topicCreationUncertain());
            controller.checkTopicCreation(); QCOMPARE(test.confirmations, 1);
            const auto topics = controller.topics();
            QVERIFY(std::none_of(topics.cbegin(), topics.cend(), [&](const QVariant &row) {
                return row.toMap().value("id").toString() == test.created.value("uuid").toString();
            }));
        } else {
            QVERIFY(controller.topicCreationUncertain()); test.created = live; test.getDelay = 0;
            controller.checkTopicCreation(); QTRY_COMPARE(created.size(), 1);
            QCOMPARE(controller.topics().last().toMap().value("name").toString(), "Newer live name");
        }
    }

    void topicCreationCatalogFailureRemainsCheckOnlyAndDoesNotHidePending()
    {
        TopicCreationFixture test; QVERIFY(test.fixture.start()); test.catalogStatus = 503;
        auto &controller = test.fixture.controller; QSignalSpy created(&controller, &NativeController::topicCreated);
        controller.createTopic(streamId, "Created, catalog unavailable"); QTRY_COMPARE(test.catalogs, 1);
        QTRY_VERIFY(!controller.topicCreationBusy()); QVERIFY(controller.topicCreationUncertain());
        QVERIFY(controller.topicCreationError().contains("created")); QCOMPARE(created.size(), 0);
        test.catalogStatus = 200; controller.checkTopicCreation(); QTRY_COMPARE(created.size(), 1);
        QCOMPARE(test.posts, 1); QCOMPARE(test.confirmations, 2); QCOMPARE(test.catalogs, 2);
    }

    void topicCreationCatalogPaginationIsCompleteAndCachedWithoutRedundantGet()
    {
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        TopicCreationFixture test; test.fixture.controller.useCache(&cache); QVERIFY(test.fixture.start());
        const auto fallback = test.fixture.server.handler;
        int pages = 0;
        test.fixture.server.handler = [&](const Request &request) -> Response {
            if (test.posts && request.method == "GET" && request.target.path().endsWith("/stream_topics/")) {
                ++pages;
                if (QUrlQuery(request.target).queryItemValue("page_marker").isEmpty())
                    return {QJsonArray{creationTopic(topicId, streamId, "Native topic")}, 200, 0, {{"X-Pagination-Marker", "second-page"}}};
                return {QJsonArray{test.created}};
            }
            return fallback(request);
        };
        auto &controller = test.fixture.controller; QSignalSpy created(&controller, &NativeController::topicCreated);
        controller.createTopic(streamId, "Paged topic"); QTRY_COMPARE(created.size(), 1); QCOMPARE(pages, 2);
        QVERIFY(flushCache(cache));
        const auto saved = cacheEntry(cache, cache.lease(), {NativeCache::Kind::Topics, streamId, {}, {}});
        QVERIFY(saved); QVERIFY(saved->fresh); QCOMPARE(saved->rows.size(), 2); QVERIFY(saved->nextMarker.isEmpty());
        QCOMPARE(saved->rows.last().toObject().value("name").toString(), QString("Paged topic"));
        QVERIFY(saved->rows.last().toObject().value("is_done").isBool());
        controller.selectStream({}); controller.selectStream(streamId); QTRY_COMPARE(controller.topics().size(), 2);
        QCOMPARE(pages, 2); // Current authoritative cache avoids another catalog request.
    }

    void topicCreationReentrantNavigationDoesNotStrandTheIntent()
    {
        TopicCreationFixture test; QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QSignalSpy created(&controller, &NativeController::topicCreated);
        bool left = false;
        connect(&controller, &NativeController::topicsChanged, &controller, [&] {
            if (!left && test.posts && controller.topics().size() == 2 && controller.selectedTopic().isEmpty()) {
                left = true; controller.selectStream({});
            }
        });
        controller.createTopic(streamId, "Do not override navigation"); QTRY_VERIFY(left);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.topicCreationBusy(), 1000);
        QVERIFY(controller.topicCreationUncertain()); QCOMPARE(created.size(), 0); QCOMPARE(controller.selectedStream(), QString{});
    }

    void topicCreationSessionAndAclChangesFenceDelayedReplies_data()
    {
        QTest::addColumn<QString>("change");
        for (const auto *change : {"logout", "token", "archive", "role", "cache-replace"}) QTest::newRow(change) << QString::fromLatin1(change);
    }
    void topicCreationSessionAndAclChangesFenceDelayedReplies()
    {
        QFETCH(QString, change);
        QTemporaryDir first, second; NativeCacheService cache(first.path(), {}, true), replacement(second.path(), {}, true);
        TopicCreationFixture test; if (change == "cache-replace") test.fixture.controller.useCache(&cache);
        QVERIFY(test.fixture.start()); test.getDelay = 150;
        auto &controller = test.fixture.controller; QSignalSpy created(&controller, &NativeController::topicCreated);
        controller.createTopic(streamId, "Captured identity"); QTRY_COMPARE(test.confirmations, 1);
        const auto id = test.created.value("uuid").toString();
        if (change == "logout") controller.logout();
        else if (change == "token") {
            test.fixture.authentication = [](const Request &request) {
                if (request.target.path().endsWith("/actions/get_token/invoke"))
                    return Response{QJsonObject{{"access_token", "rotated-topic-token"}, {"refresh_token", "synthetic-refresh"}, {"expires_in", 3600}}};
                return sessionResponse(request);
            };
            test.fixture.session.requestRefresh();
        } else if (change == "cache-replace") controller.useCache(&replacement);
        else {
            auto row = change == "archive" ? creationChannel(streamId, "Changed")
                : QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Changed"}, {"encryption", false}};
            row[change == "archive" ? "is_archived" : "role"] = change == "archive" ? QJsonValue(true) : QJsonValue("new-role");
            QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.updated", row)));
        }
        QTRY_COMPARE(test.fixture.api.findChildren<QNetworkReply *>().size(), 0);
        QCOMPARE(created.size(), 0); QVERIFY(controller.selectedTopic() != id); QVERIFY(!controller.topicCreationBusy());
        QCOMPARE(test.posts, 1);
        if (change == "logout") { QVERIFY(test.fixture.start()); QVERIFY(controller.topicCreationUncertain()); }
        if (change == "token") {
            QTRY_VERIFY(!controller.busy()); QVERIFY(controller.topicCreationUncertain());
            test.getDelay = 0; controller.checkTopicCreation(); QTRY_COMPARE(created.size(), 1); QCOMPARE(test.posts, 1);
        }
    }

    void topicCreationBoundsRequestsEvenForUniqueEmptyPages()
    {
        TopicCreationFixture test; QVERIFY(test.fixture.start());
        const auto fallback = test.fixture.server.handler;
        int pages = 0;
        test.fixture.server.handler = [&](const Request &request) -> Response {
            if (test.posts && request.method == "GET" && request.target.path().endsWith("/stream_topics/"))
                return {QJsonArray{}, 200, 0, {{"X-Pagination-Marker", QByteArray::number(++pages)}}};
            return fallback(request);
        };
        auto &controller = test.fixture.controller; QSignalSpy created(&controller, &NativeController::topicCreated);
        QSignalSpy changed(&controller, &NativeController::stateChanged);
        controller.createTopic(streamId, "No pagination storm");
        QVERIFY2(changed.wait(), qPrintable(QString("Still pending after %1 empty HTTP pages").arg(pages)));
        QVERIFY(!controller.topicCreationBusy());
        QVERIFY(controller.topicCreationUncertain()); QCOMPARE(pages, 500); QCOMPARE(test.posts, 1); QCOMPARE(created.size(), 0);
    }

    void topicCreationDoesNotAcceptAnIncompatiblePostIdentity()
    {
        TopicCreationFixture test; QVERIFY(test.fixture.start());
        test.alterPost = [](QJsonObject &row) { row["uuid"] = returnedId; };
        auto &controller = test.fixture.controller; QSignalSpy created(&controller, &NativeController::topicCreated);
        controller.createTopic(streamId, "Frozen UUID"); QTRY_COMPARE(test.posts, 1);
        QTRY_VERIFY(controller.topicCreationUncertain()); QCOMPARE(test.confirmations, 0); QCOMPARE(created.size(), 0);
        controller.checkTopicCreation(); QTRY_COMPARE(created.size(), 1); QCOMPARE(test.posts, 1);
        QCOMPARE(controller.selectedTopic(), test.created.value("uuid").toString());
    }

    void topicCreationLiveCatalogRevisionRejectsAnOlderPage()
    {
        TopicCreationFixture test; QVERIFY(test.fixture.start()); test.catalogDelay = 100;
        auto &controller = test.fixture.controller; QSignalSpy created(&controller, &NativeController::topicCreated);
        controller.createTopic(streamId, "Old catalog name"); QTRY_COMPARE(test.catalogs, 1);
        test.created["name"] = "New live name";
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("topic.updated", test.created)));
        QTRY_VERIFY(!controller.topicCreationBusy()); QVERIFY(controller.topicCreationUncertain()); QCOMPARE(created.size(), 0);
        QCOMPARE(controller.topics().last().toMap().value("name").toString(), "New live name");
        test.catalogDelay = 0; controller.checkTopicCreation(); QTRY_COMPARE(created.size(), 1);
        QCOMPARE(controller.topics().last().toMap().value("name").toString(), "New live name"); QCOMPARE(test.posts, 1);
    }

    void topicCreationRejectsUnverifiedUnknownAndArchivedChannels()
    {
        TopicCreationFixture test;
        QVERIFY(!test.fixture.controller.canCreateTopicIn(streamId));
        test.fixture.controller.createTopic(streamId, "Signed out"); QCOMPARE(test.posts, 0);
        QVERIFY(test.fixture.start()); auto &controller = test.fixture.controller;
        QVERIFY(!controller.canCreateTopicIn(returnedId));
        controller.createTopic(returnedId, "Unknown"); QCOMPARE(test.posts, 0);
        auto row = creationChannel(streamId, "Archived"); row["is_archived"] = true;
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.updated", row)));
        QVERIFY(!controller.canCreateTopicIn(streamId)); controller.createTopic(streamId, "Archived"); QCOMPARE(test.posts, 0);
    }

    void streamContextKeyReflectsOnlyVerifiedAccessMetadata()
    {
        Fixture fixture; QVERIFY(fixture.start());
        const auto key = fixture.controller.streams().first().toMap().value("contextKey").toString();
        QCOMPARE(key.size(), 64);
        auto row = QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Renamed"}, {"encryption", false}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated", row)));
        QCOMPARE(fixture.controller.streams().first().toMap().value("contextKey").toString(), key);
        row["role"] = "member"; QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated", row, 13)));
        const auto changed = fixture.controller.streams().first().toMap().value("contextKey").toString();
        QVERIFY(changed != key); QCOMPARE(changed.size(), 64);
    }

    void topicCreationNavigationSubscribersCanDestroyTheController()
    {
        TopicCreationFixture test; QVERIFY(test.fixture.start());
        QPointer<NativeController> current = new NativeController(&test.fixture.api, &test.fixture.session);
        current->refresh(); QTRY_VERIFY(!current->busy()); QTRY_COMPARE(current->streams().size(), 1);
        current->selectStream(streamId); QTRY_COMPARE(current->topics().size(), 1);
        connect(current, &NativeController::topicsChanged, &test.fixture.session, [&] {
            if (current && test.posts && current->topics().size() == 2) delete current.data();
        });
        current->createTopic(streamId, "Subscriber lifetime"); QTRY_VERIFY(current.isNull());
        QCOMPARE(test.posts, 1); QCOMPARE(test.confirmations, 1);
    }

    void viewerNameFallsBackWhileDirectoryIsStaleAndBoundsUntrustedNames()
    {
        Fixture fixture;
        fixture.authentication = [](const Request &request) {
            auto response = sessionResponse(request);
            if (request.target.path().endsWith("/actions/introspect")) {
                auto raw = response.data.toObject(); auto profile = raw.value("user_info").toObject();
                profile["first_name"] = "Cassandra"; raw["user_info"] = profile; response.data = raw;
            }
            return response;
        };
        fixture.users = [](const Request &) { return Response{QJsonArray{QJsonObject{{"uuid", userId}, {"username", "cassi"}}}}; };
        QVERIFY(fixture.start()); QCOMPARE(fixture.controller.viewerDisplayName(), "cassi");
        fixture.users = [](const Request &) { return Response{QJsonArray{QJsonObject{{"uuid", userId},
            {"username", "cassi"}, {"first_name", QString(129, 'x')}}}, 200, 80}; };
        fixture.controller.refresh(); QCOMPARE(fixture.controller.viewerDisplayName(), "Cassandra");
        QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(fixture.controller.viewerDisplayName(), "Cassandra");
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("user.updated", QJsonObject{{"uuid", userId},
            {"username", "cassi"}, {"first_name", "<b>Cassi</b>"}})));
        QCOMPARE(fixture.controller.viewerDisplayName(), "<b>Cassi</b>"); // Plain text projection, no markup evaluation.
        fixture.controller.logout(); QCOMPARE(fixture.controller.viewerDisplayName(), QString{});
    }

    void topicCreationUncertainOwnerCannotLeakIntoANewVerifiedAccount()
    {
        TopicCreationFixture test; QVERIFY(test.fixture.start()); test.postDelay = 120;
        auto &controller = test.fixture.controller; QSignalSpy created(&controller, &NativeController::topicCreated);
        controller.createTopic(streamId, "Original owner intent"); QTRY_COMPARE(test.posts, 1);
        const auto attempt = controller.topicCreationAttemptId(), context = controller.topicCreationContextKey();
        QVERIFY(isUuid(attempt)); QCOMPARE(context.size(), 64);
        test.fixture.authentication = [](const Request &request) {
            auto response = sessionResponse(request);
            if (request.target.path().endsWith("/actions/introspect")) {
                auto raw = response.data.toObject(); auto user = raw.value("user_info").toObject();
                user["uuid"] = returnedId; raw["user_info"] = user; response.data = raw;
            }
            return response;
        };
        QVERIFY(test.fixture.start()); QTRY_COMPARE(test.fixture.api.findChildren<QNetworkReply *>().size(), 0);
        QVERIFY(!controller.topicCreationBusy()); QVERIFY(!controller.topicCreationUncertain());
        QCOMPARE(controller.topicCreationStream(), QString{}); QCOMPARE(controller.topicCreationName(), QString{});
        QVERIFY(controller.topicCreationAttemptId().isEmpty()); QVERIFY(controller.topicCreationContextKey() != context);
        QCOMPARE(created.size(), 0); QVERIFY(controller.canCreateTopicIn(streamId));
        test.fixture.authentication = {}; QVERIFY(test.fixture.start()); QVERIFY(controller.topicCreationUncertain());
        QCOMPARE(controller.topicCreationAttemptId(), attempt);
        test.postDelay = 0; controller.checkTopicCreation(); QTRY_COMPARE(created.size(), 1); QCOMPARE(test.posts, 1);
    }

    void createdTopicCatalogSurvivesAColdOfflineMessengerRestart()
    {
        QTemporaryDir directory; TopicCreationFixture test; QVERIFY(test.fixture.start());
        QString id;
        {
            NativeCacheService cache(directory.path(), {}, true);
            NativeController current(&test.fixture.api, &test.fixture.session); current.useCache(&cache);
            QTRY_VERIFY(!current.busy()); QTRY_COMPARE(current.streams().size(), 1);
            current.selectStream(streamId); QTRY_COMPARE(current.topics().size(), 1);
            QSignalSpy created(&current, &NativeController::topicCreated);
            current.createTopic(streamId, "Durable topic"); QTRY_COMPARE(created.size(), 1);
            id = test.created.value("uuid").toString(); QVERIFY(flushCache(cache));
        }
        const auto fallback = test.fixture.server.handler;
        test.fixture.server.handler = [fallback](const Request &request) {
            if (request.method == "GET" && (request.target.path().contains("/messenger/") || request.target.path().endsWith("/users/")))
                return Response{QJsonObject{}, 503};
            return fallback(request);
        };
        NativeCacheService cache(directory.path(), {}, true);
        ApiClient api(nullptr, true); Session session(&api); NativeController current(&api, &session); current.useCache(&cache);
        current.login(test.fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(current.projects().size(), 1); current.openProject(projectId); QTRY_COMPARE(current.streams().size(), 1);
        current.selectStream(streamId); QTRY_COMPARE(current.topics().size(), 2);
        const auto restoredTopics = current.topics();
        QVERIFY(std::any_of(restoredTopics.cbegin(), restoredTopics.cend(), [&id](const QVariant &row) {
            return row.toMap().value("id").toString() == id && row.toMap().value("name").toString() == "Durable topic";
        }));
        QVERIFY(current.cacheStale()); QVERIFY(!current.canCreateTopicIn(streamId)); QCOMPARE(test.posts, 1);
    }

    void viewerDisplayNameUsesOnlyFreshOwnDirectoryNames()
    {
        Fixture fixture;
        fixture.authentication = [](const Request &request) {
            auto response = sessionResponse(request);
            if (request.target.path().endsWith("/actions/introspect")) {
                auto data = response.data.toObject(); auto profile = data.value("user_info").toObject();
                profile["first_name"] = "Cassandra"; data["user_info"] = profile; response.data = data;
            }
            return response;
        };
        fixture.users = [](const Request &) { return Response{QJsonArray{QJsonObject{{"uuid", userId},
            {"first_name", " Cassi "}, {"last_name", " Volkova "}, {"username", "cassi"}, {"name", "Ignore extension"}}}}; };
        QVERIFY(fixture.start());
        QCOMPARE(fixture.controller.viewerDisplayName(), "Cassi Volkova");
        QCOMPARE(fixture.controller.account().value("displayName").toString(), "Cassandra");
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("user.updated", QJsonObject{{"uuid", returnedId},
            {"username", "foreign"}, {"first_name", "Another"}})));
        QCOMPARE(fixture.controller.viewerDisplayName(), "Cassi Volkova");
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("user.updated", QJsonObject{{"uuid", userId},
            {"username", "cassi"}, {"first_name", "Cassi"}, {"last_name", "Updated"}})));
        QCOMPARE(fixture.controller.viewerDisplayName(), "Cassi Updated");
        fixture.controller.logout(); QCOMPARE(fixture.controller.viewerDisplayName(), QString{});
    }

    void realtimeAccessChangesStillEraseSavedHistory_data()
    {
        QTest::addColumn<QString>("field");
        for (const auto *field : {"encryption", "role", "direct_user_uuid"})
            QTest::newRow(field) << QString::fromLatin1(field);
    }
    void realtimeAccessChangesStillEraseSavedHistory()
    {
        QFETCH(QString, field);
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.controller.useCache(&cache);
        fixture.messages = [](const Request &) { return Response{QJsonArray{message()}}; };
        QVERIFY(fixture.start()); QVERIFY(flushCache(cache));
        QJsonObject raw{{"uuid", streamId}, {"project_id", projectId}, {"name", "Changed stream"}, {"encryption", false}};
        raw[field] = field == "role" ? QJsonValue("changed-role") : field == "direct_user_uuid" ? QJsonValue(returnedId) : QJsonValue(true);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated", raw)));
        QVERIFY(flushCache(cache));
        QVERIFY(!cacheEntry(cache, cache.lease(), {NativeCache::Kind::History, streamId, topicId, {}}));
    }

    void realtimeArchiveRetainsVerifiedOrdinaryTopicHistoryAndCapabilities()
    {
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; FileClient files(&fixture.api); fixture.controller.useCache(&cache); fixture.controller.useFiles(&files);
        fixture.streams = [](const Request &) { return Response{QJsonArray{creationChannel(streamId, "Native channel")}}; };
        fixture.messages = [](const Request &) { return Response{QJsonArray{message()}}; };
        QVERIFY(fixture.start()); QVERIFY(flushCache(cache)); auto &controller = fixture.controller;
        controller.setDraft("Retained ordinary topic"); QVERIFY(controller.canSend()); QVERIFY(controller.canAttach());
        const auto requests = fixture.server.requests.size();
        // A partial row cannot newly admit retained Archive identity. Existing
        // route and creation guards are tested below with an actual full DTO.
        QVERIFY(!controller.applyRealtimeEvent(controllerEvent("stream.updated",{{"uuid",streamId},
            {"project_id",projectId},{"name","Partial archive"},{"encryption",false},{"is_archived",true}})));
        auto raw=creationChannel(streamId,"Native channel"); raw["is_archived"]=true;
        raw["updated_at"]="2026-01-02T10:02:00Z";
        QVERIFY(controller.applyRealtimeEvent(controllerEvent("stream.updated",raw)));
        QVERIFY(flushCache(cache));
        const auto history=cacheEntry(cache,cache.lease(),{NativeCache::Kind::History,streamId,topicId,{}});
        QVERIFY(history); QVERIFY(!history->fresh); QCOMPARE(history->rows.size(),1);
        QCOMPARE(controller.selectedStream(),streamId); QCOMPARE(controller.selectedTopic(),topicId);
        QCOMPARE(controller.messages().size(),1); QCOMPARE(controller.draft(),QString("Retained ordinary topic"));
        QVERIFY(controller.canSend()); QVERIFY(controller.canAttach()); QVERIFY(!controller.canCreateTopicIn(streamId));
        controller.createTopic(streamId,"Still archived");
        QVERIFY(controller.streams().isEmpty()); QCOMPARE(controller.archivedChats().size(),1);
        for (int n=requests;n<fixture.server.requests.size();++n) QCOMPARE(fixture.server.requests[n].method,QByteArray("GET"));
    }

    void messageCreatedAtUsesTheValidatedUtcDisplayTimestamp()
    {
        Fixture fixture;
        auto row = message("Timestamp"); row["created_at"] = "2026-11-06T15:34:56.123456+03:00";
        fixture.messages = [row](const Request &) { return Response{QJsonArray{row}}; };
        QVERIFY(fixture.start());
        QCOMPARE(modelMessage(fixture.controller, messageId).value("createdAt").toString(),
                 QString("2026-11-06T12:34:56.123Z"));
    }

    void canonicalMessageConfirmationPreservesNewerFieldRevisions_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"edit-aba", "star-aba", "edit-other-star", "star-other-payload",
                                    "edit-delete", "star-delete", "send-delete"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }
    void canonicalMessageConfirmationPreservesNewerFieldRevisions()
    {
        QFETCH(QString, scenario);
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.controller.useCache(&cache);
        auto row = message("Payload A"); row["reactions"] = QJsonObject{}; row["reaction_users"] = QJsonObject{};
        int confirmations = 0, history = 0;
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/star/invoke")) { row["starred"] = true; return Response{row}; }
            if (request.method == "PUT") { row["payload"] = request.body.value("payload"); return Response{row}; }
            if (request.method == "POST") {
                row = message(request.body.value("payload").toObject().value("content").toString(), request.body.value("uuid").toString());
                return Response{row, 201};
            }
            if (request.target.path().endsWith("/messages/")) { ++history; return Response{QJsonArray{row}}; }
            ++confirmations; return Response{row, 200, 200};
        };
        QVERIFY(fixture.start()); QVERIFY(flushCache(cache));
        if (scenario.startsWith("edit")) fixture.controller.editMessage(messageId, "Payload B");
        else if (scenario.startsWith("star")) fixture.controller.starMessage(messageId);
        else { fixture.controller.setDraft("Payload B"); fixture.controller.send(); }
        QTRY_COMPARE(confirmations, 1);
        const auto id = row.value("uuid").toString();
        if (scenario.endsWith("delete")) {
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.deleted", QJsonObject{
                {"uuid", id}, {"project_id", projectId}, {"stream_uuid", streamId}, {"topic_uuid", topicId}})));
        } else if (scenario == "edit-aba") {
            auto updated = message("Payload C");
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", updated, 12)));
            updated["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Payload A"}};
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", updated, 13)));
            QCOMPARE(modelMessage(fixture.controller, messageId).value("content").toString(), "Payload A");
        } else if (scenario == "star-aba") {
            auto updated = message("Payload A"); updated["starred"] = true;
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", updated, 12)));
            updated["starred"] = false;
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", updated, 13)));
            QVERIFY(!modelMessage(fixture.controller, messageId).value("starred").toBool());
        } else if (scenario == "edit-other-star") {
            auto updated = message("Payload A"); updated["starred"] = true;
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", updated)));
        } else {
            auto updated = message("Payload C"); updated["starred"] = false;
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", updated)));
        }
        // Wait for the actual ApiClient callback/deferred reply destruction,
        // rather than using server socket lifetime as client completion.
        QTRY_COMPARE(fixture.api.findChildren<QNetworkReply *>().size(), 0);
        if (scenario.endsWith("delete")) QVERIFY(modelMessage(fixture.controller, id).isEmpty());
        else if (scenario == "edit-aba") QCOMPARE(modelMessage(fixture.controller, id).value("content").toString(), "Payload A");
        else if (scenario == "star-aba") QVERIFY(!modelMessage(fixture.controller, id).value("starred").toBool());
        else {
            QCOMPARE(modelMessage(fixture.controller, id).value("content").toString(), scenario == "edit-other-star" ? "Payload B" : "Payload C");
            QVERIFY(modelMessage(fixture.controller, id).value("starred").toBool());
        }
        QCOMPARE(confirmations, 1); QCOMPARE(history, 1);
        QVERIFY(flushCache(cache));
        const auto saved = cacheEntry(cache, cache.lease(), {NativeCache::Kind::History, streamId, topicId, {}});
        if (scenario.endsWith("delete")) QVERIFY(!saved);
        else {
            QVERIFY(saved && !saved->fresh);
            const auto raw = saved->rows.first().toObject();
            QCOMPARE(raw.value("payload").toObject().value("content").toString(),
                     modelMessage(fixture.controller, messageId).value("content").toString());
            QCOMPARE(raw.value("starred").toBool(), modelMessage(fixture.controller, messageId).value("starred").toBool());
        }
    }

    void confirmedNonReadMutationSurvivesRestartWithoutAnotherHistoryGet_data()
    {
        QTest::addColumn<QString>("operation");
        QTest::addColumn<bool>("catalogsOffline");
        for (const auto *operation : {"send", "edit", "star", "reaction-add", "reaction-remove", "delete"})
            QTest::newRow(operation) << QString::fromLatin1(operation) << false;
        QTest::newRow("send-all-messenger-offline") << QString("send") << true;
        QTest::newRow("edit-all-messenger-offline") << QString("edit") << true;
        for (const auto *event : {"rt-message-created", "rt-message-updated", "rt-stream-updated", "rt-topic-updated"})
            QTest::newRow(event) << QString::fromLatin1(event) << true;
    }
    void confirmedNonReadMutationSurvivesRestartWithoutAnotherHistoryGet()
    {
        QFETCH(QString, operation); QFETCH(bool, catalogsOffline);
        QTemporaryDir directory;
        Fixture transport;
        QMap<QString, QJsonObject> rows{{messageId, message("Previously saved message")}};
        bool restoring = false, own = operation == "reaction-remove";
        int historyGets = 0, canonicalGets = 0, mutations = 0;
        const QJsonObject fact{{"uuid", returnedId}, {"project_id", projectId}, {"message_uuid", messageId},
            {"user_uuid", userId}, {"emoji_name", "heart"}, {"created_at", "2026-01-02T10:00:00Z"},
            {"updated_at", "2026-01-02T10:00:00Z"}};
        const auto aggregate = [&] {
            rows[messageId]["reactions"] = own ? QJsonObject{{"heart", 1}} : QJsonObject{};
            rows[messageId]["reaction_users"] = own ? QJsonObject{{"heart", QJsonArray{userId}}} : QJsonObject{};
        };
        aggregate();
        transport.messages = [&](const Request &request) {
            const auto path = request.target.path();
            if (path.endsWith("/actions/star/invoke")) {
                ++mutations; rows[messageId]["starred"] = true; return Response{rows[messageId]};
            }
            if (request.method == "DELETE") {
                ++mutations; rows.remove(messageId); return Response{QJsonObject{}, 204};
            }
            if (request.method == "PUT") {
                ++mutations; rows[messageId]["payload"] = request.body.value("payload"); return Response{rows[messageId]};
            }
            if (request.method == "POST") {
                ++mutations; const auto id = request.body.value("uuid").toString();
                auto sent = message(request.body.value("payload").toObject().value("content").toString(), id);
                sent["created_at"] = "2026-01-02T10:00:01.000001Z"; rows.insert(id, sent);
                return Response{sent, 201};
            }
            if (path.endsWith("/messages/")) {
                ++historyGets;
                if (restoring) return Response{QJsonObject{}, 503};
                QJsonArray values; for (const auto &row : rows) values.append(row);
                return Response{values};
            }
            ++canonicalGets;
            return Response{rows.value(path.section('/', -1))};
        };
        transport.reactions = [&](const Request &request) {
            if (request.method == "POST") { ++mutations; own = true; aggregate(); return Response{fact, 201}; }
            if (request.method == "DELETE") { ++mutations; own = false; aggregate(); return Response{QJsonObject{}, 204}; }
            return Response{own ? QJsonArray{fact} : QJsonArray{}};
        };
        const auto enter = [&](NativeController &controller) {
            controller.login(transport.server.origin().toString(), "synthetic-user", "synthetic-password", "");
            if (!QTest::qWaitFor([&] { return controller.projects().size() == 1; }, 2000)) return false;
            controller.openProject(projectId);
            if (!QTest::qWaitFor([&] { return controller.streams().size() == 1; }, 2000)) return false;
            controller.selectStream(streamId);
            if (!QTest::qWaitFor([&] { return controller.topics().size() == 1; }, 2000)) return false;
            controller.selectTopic(topicId);
            return QTest::qWaitFor([&] { return !controller.busy(); }, 2000);
        };
        QString sentId;
        {
            NativeCacheService cache(directory.path(), {}, true);
            ApiClient api(nullptr, true); Session session(&api); NativeController controller(&api, &session);
            controller.useCache(&cache); QVERIFY(enter(controller)); QVERIFY(flushCache(cache));
            QCOMPARE(historyGets, 1);
            if (operation.startsWith("rt-")) {
                const auto kind = operation.mid(3);
                QJsonObject raw;
                if (kind == "message-created") {
                    sentId = returnedId;
                    raw = message("Saved realtime insertion", sentId); raw["created_at"] = "2026-01-02T10:00:01Z";
                } else if (kind == "message-updated") {
                    raw = rows[messageId]; raw["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Saved realtime edit"}};
                } else if (kind == "stream-updated") {
                    raw = {{"uuid", streamId}, {"project_id", projectId}, {"name", "Renamed live channel"},
                        {"encryption", false}, {"unread_count", 1}};
                } else {
                    raw = {{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "Renamed live topic"}};
                }
                QString eventKind = kind; eventKind.replace('-', '.');
                QVERIFY(controller.applyRealtimeEvent(controllerEvent(eventKind, raw)));
            } else if (operation == "send") {
                QSignalSpy sent(&controller, &NativeController::messageSent);
                controller.setDraft("Saved own send"); controller.send(); QTRY_COMPARE(sent.size(), 1);
                sentId = sent.first().first().toString();
                QCOMPARE(modelMessage(controller, sentId).value("content").toString(), "Saved own send");
            } else if (operation == "edit") {
                controller.editMessage(messageId, "Saved confirmed edit");
                QTRY_COMPARE(modelMessage(controller, messageId).value("content").toString(), "Saved confirmed edit");
            } else if (operation == "delete") {
                controller.deleteMessage(messageId); QTRY_VERIFY(controller.messages().isEmpty());
            } else if (operation == "star") {
                controller.starMessage(messageId); QTRY_VERIFY(modelMessage(controller, messageId).value("starred").toBool());
            } else {
                controller.toggleReaction(messageId, "heart");
                QTRY_VERIFY(!modelMessage(controller, messageId).value("reactionBusy").toBool());
                QCOMPARE(modelMessage(controller, messageId).value("reactions").toList().size(), own ? 1 : 0);
            }
            QCOMPARE(mutations, operation.startsWith("rt-") ? 0 : 1);
            QCOMPARE(canonicalGets, operation == "delete" || operation.startsWith("rt-") ? 0 : 1); QCOMPARE(historyGets, 1);
            QVERIFY(flushCache(cache));
        }
        restoring = true;
        if (catalogsOffline) {
            transport.streams = transport.topics = transport.users = [](const Request &) { return Response{QJsonObject{}, 503}; };
        }
        NativeCacheService cache(directory.path(), {}, true);
        ApiClient api(nullptr, true); Session session(&api); NativeController restored(&api, &session);
        restored.useCache(&cache); QVERIFY(enter(restored));
        QTRY_COMPARE(restored.messages().size(), operation == "send" || operation == "rt-message-created" ? 2 : operation == "delete" ? 0 : 1);
        const auto row = modelMessage(restored, messageId);
        if (operation != "delete")
            QCOMPARE(row.value("content").toString(), operation == "edit" ? "Saved confirmed edit" : operation == "rt-message-updated" ? "Saved realtime edit" : "Previously saved message");
        if (operation == "send" || operation == "rt-message-created")
            QCOMPARE(modelMessage(restored, sentId).value("content").toString(), operation == "send" ? "Saved own send" : "Saved realtime insertion");
        if (operation == "star") QVERIFY(row.value("starred").toBool());
        if (operation.startsWith("reaction-")) QCOMPARE(row.value("reactions").toList().size(), own ? 1 : 0);
        if (operation != "delete") QVERIFY(restored.cacheStale());
        restored.setDraft("Local draft awaiting revalidation");
        QVERIFY(!restored.canSend()); QVERIFY(!restored.canLoadOlder()); QVERIFY(!row.value("canReact").toBool());
        QCOMPARE(historyGets, 2);
        QCOMPARE(canonicalGets, operation == "delete" || operation.startsWith("rt-") ? 0 : 1);
        QCOMPARE(mutations, operation.startsWith("rt-") ? 0 : 1);
    }

    void cacheStatusObserversMayDestroyTheController_data()
    {
        QTest::addColumn<QString>("phase");
        for (const auto *phase : {"opening", "logout", "replacement", "cache-destroyed"})
            QTest::newRow(phase) << QString::fromLatin1(phase);
    }
    void cacheStatusObserversMayDestroyTheController()
    {
        QFETCH(QString, phase);
        QTemporaryDir directory; Fixture fixture;
        QPointer<NativeCacheService> cache = new NativeCacheService(directory.path(), {}, true);
        NativeCacheService replacement(directory.filePath("replacement"), {}, true);
        QPointer<NativeController> controller = new NativeController(&fixture.api, &fixture.session);
        controller->useCache(cache);
        if (phase == "opening") connect(cache, &NativeCacheService::stateChanged, this, [&] {
            if (controller && cache->maintenanceBusy()) delete controller.data();
        });
        controller->login(fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(controller->projects().size(), 1); controller->openProject(projectId);
        if (phase == "opening") QTRY_VERIFY(!controller);
        else {
            QTRY_VERIFY(cache->available()); QTRY_VERIFY(!controller->busy());
            if (phase == "cache-destroyed") {
                connect(controller, &NativeController::stateChanged, this, [&] { delete controller.data(); });
                delete cache.data();
            } else {
                connect(cache, &NativeCacheService::stateChanged, this, [&] {
                    if (controller && !cache->available()) delete controller.data();
                });
                if (phase == "logout") controller->logout();
                else controller->useCache(&replacement);
            }
            QVERIFY(!controller);
            if (phase == "logout") QVERIFY(!fixture.session.ready());
        }
        if (cache) delete cache.data();
    }

    void savedAccountForgetRequiresItsCurrentDurableCleanup_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"success", "cleanup-error", "destroy-pending", "replace-pending", "destroy-before-admission", "session-changed"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }
    void savedAccountForgetRequiresItsCurrentDurableCleanup()
    {
        QFETCH(QString, scenario);
        QTemporaryDir directory; ControllerMemorySecrets secrets;
        SessionVault vault(directory.filePath("accounts.ini"), &secrets);
        const QUrl server("https://cache-forget.invalid");
        SavedAccount account{SessionVault::accountId(server, projectId, userId), server, projectId, userId,
                             "synthetic-user", "Synthetic project"};
        bool saved = false;
        vault.save(account, "synthetic-refresh", [&](const QString &error) { QVERIFY(error.isEmpty()); saved = true; });
        QTRY_VERIFY(saved);
        ApiClient api; Session session(&api); session.setVault(&vault);
        NativeController controller(&api, &session);
        QPointer<NativeCacheService> cache = new NativeCacheService(directory.filePath("cache"));
        const auto cleanup = qScopeGuard([&] { if (cache) delete cache.data(); });
        NativeCacheService replacement(directory.filePath("cache"));
        QVERIFY(cache->open(server, projectId, userId)); QTRY_VERIFY(cache->available());
        bool written = false;
        QVERIFY(cache->put(cache->lease(), {NativeCache::Kind::History, streamId, topicId, {}}, QJsonArray{message()}, {}, this,
                           [&](bool okay) { written = okay; }));
        QTRY_VERIFY(written); controller.useCache(cache);
        if (scenario == "destroy-before-admission") {
            connect(&controller, &NativeController::stateChanged, this, [&] { if (cache && controller.busy()) delete cache.data(); });
            controller.forgetSavedAccount(account.id);
        } else {
            const auto connection = QUuid::createUuid().toString();
            {
                auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
                database.setDatabaseName(directory.filePath("cache/cache.sqlite3")); QVERIFY(database.open());
                QSqlQuery writer(database); QVERIFY(writer.exec("BEGIN IMMEDIATE"));
                controller.forgetSavedAccount(account.id);
                QVERIFY(controller.busy()); QCOMPARE(vault.accounts().size(), 1);
                if (scenario == "destroy-pending") {
                    QTimer::singleShot(0, this, [&] { QVERIFY(writer.exec("ROLLBACK")); });
                    delete cache.data();
                } else if (scenario == "replace-pending") {
                    controller.useCache(&replacement); QVERIFY(writer.exec("ROLLBACK"));
                } else if (scenario == "session-changed") {
                    session.logout();
                    QVERIFY(!controller.busy());
                    QVERIFY(writer.exec("ROLLBACK"));
                } else if (scenario == "success") QVERIFY(writer.exec("ROLLBACK"));
                if (cache && (scenario == "session-changed" || scenario == "replace-pending"))
                    QVERIFY(flushCache(*cache));
                QTRY_VERIFY(!controller.busy());
                if (scenario == "success") QTRY_VERIFY(vault.accounts().isEmpty());
                else {
                    QCOMPARE(vault.accounts().size(), 1);
                    if (scenario == "cleanup-error") QVERIFY(!controller.error().isEmpty());
                }
                database.close();
            }
            QSqlDatabase::removeDatabase(connection);
        }
        QVERIFY(!controller.busy());
        if (scenario != "success") QCOMPARE(vault.accounts().size(), 1);
        if (cache) delete cache.data();
    }

    void unrelatedCacheRevisionDuringWarmTopicsLookupFallsBackToRest_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"same-route", "left-route", "signed-out"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }
    void unrelatedCacheRevisionDuringWarmTopicsLookupFallsBackToRest()
    {
        QFETCH(QString, scenario);
        QTemporaryDir directory;
        QSemaphore gate;
        std::atomic_bool hold{false}, entered{false};
        NativeCacheService cache(directory.path(), {}, true, [&] {
            if (hold.exchange(false)) { entered = true; gate.tryAcquire(1, 2000); }
            return 1000;
        });
        Fixture fixture; fixture.controller.useCache(&cache);
        int topics = 0;
        fixture.topics = [&](const Request &) {
            ++topics;
            return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"name", "Current topic"}}}};
        };
        fixture.messages = [](const Request &) { return Response{QJsonArray{message()}}; };
        QVERIFY(fixture.start()); QVERIFY(flushCache(cache)); QCOMPARE(topics, 1);
        hold = true;
        fixture.controller.selectStream({}); fixture.controller.selectStream(streamId);
        QTRY_VERIFY(entered.load());
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("messages.read",
            QJsonObject{{"project_id", projectId}, {"message_uuids", QJsonArray{messageId}}})));
        if (scenario == "left-route") fixture.controller.selectStream({});
        else if (scenario == "signed-out") fixture.controller.logout();
        gate.release();
        if (scenario == "same-route") {
            QTRY_COMPARE(fixture.controller.topics().size(), 1);
            QCOMPARE(topics, 2);
        } else {
            QVERIFY(flushCache(cache));
            QTRY_COMPARE(fixture.server.findChildren<QTcpSocket *>().size(), 0);
            QVERIFY(fixture.controller.topics().isEmpty());
            QCOMPARE(topics, 1);
        }
    }

    void destroyedCacheDuringWarmHistoryLookupFallsBackToRest()
    {
        QTemporaryDir directory; QSemaphore gate;
        std::atomic_bool hold{false}, entered{false};
        QPointer<NativeCacheService> cache = new NativeCacheService(directory.path(), {}, true, [&] {
            if (hold.exchange(false)) { entered = true; gate.tryAcquire(1, 2000); }
            return 1000;
        });
        const auto cleanup = qScopeGuard([&] { if (cache) delete cache.data(); });
        Fixture fixture; fixture.extraTopic = true; fixture.controller.useCache(cache);
        int history = 0;
        fixture.messages = [&](const Request &request) {
            ++history;
            const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            return Response{QJsonArray{message("network after cache loss", topic == topicId ? messageId : returnedId, topic)}};
        };
        QVERIFY(fixture.start()); QVERIFY(flushCache(*cache));
        fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy());
        QVERIFY(flushCache(*cache)); QCOMPARE(history, 2);
        hold = true; fixture.controller.selectTopic(topicId); QTRY_VERIFY(entered.load());
        QTimer::singleShot(0, this, [&] { gate.release(); });
        delete cache.data();
        QTRY_COMPARE(history, 3);
        QTRY_VERIFY(!fixture.controller.busy());
        QCOMPARE(modelMessage(fixture.controller, messageId).value("content").toString(), "network after cache loss");
        QVERIFY(!fixture.controller.cacheStale());
    }

    void obsoleteWarmHistoryReplyCannotStartReadsBeforeRenewalIsVerified()
    {
        QTemporaryDir directory; QSemaphore gate;
        std::atomic_bool hold{false}, entered{false};
        NativeCacheService cache(directory.path(), {}, true, [&] {
            if (hold.exchange(false)) { entered = true; gate.tryAcquire(1, 2000); }
            return 1000;
        });
        Fixture fixture; fixture.extraTopic = true; fixture.controller.useCache(&cache);
        bool renewing = false, verified = false; int premature = 0, history = 0;
        fixture.authentication = [&](const Request &request) {
            if (renewing && request.target.path().endsWith("/actions/get_token/invoke"))
                return Response{QJsonObject{{"access_token", "synthetic-cache-generation-token"},
                    {"refresh_token", "synthetic-refresh"}, {"expires_in", 3600}}};
            auto response = sessionResponse(request);
            if (renewing && request.target.path().endsWith("/actions/introspect")) {
                response.delay = 250;
                gate.release();
            }
            return response;
        };
        fixture.messages = [&](const Request &request) {
            ++history;
            if (renewing && !verified) ++premature;
            const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            return Response{QJsonArray{message("verified page", topic == topicId ? messageId : returnedId, topic)}};
        };
        QVERIFY(fixture.start()); QVERIFY(flushCache(cache));
        fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy());
        QVERIFY(flushCache(cache)); QCOMPARE(history, 2);
        hold = true; fixture.controller.selectTopic(topicId); QTRY_VERIFY(entered.load());
        connect(&fixture.session, &Session::authenticated, this, [&] { verified = true; });
        renewing = true; fixture.session.requestRefresh();
        QTRY_VERIFY(verified); QTRY_VERIFY(!fixture.controller.busy());
        QCOMPARE(premature, 0);
        QCOMPARE(modelMessage(fixture.controller, messageId).value("content").toString(), "verified page");
    }

    void delayedCacheOpenHydratesAfterQuickCatalogFailuresWithoutRetryingRest()
    {
        QTemporaryDir directory; Fixture fixture;
        {
            NativeCache seed(directory.path(),{},true); QVERIFY(seed.open(fixture.server.origin(),projectId,userId));
            QVERIFY(seed.put(seed.lease(),{NativeCache::Kind::Streams,{},{},{}},QJsonArray{QJsonObject{{"uuid",streamId},{"project_id",projectId},{"name","Saved channel"},{"encryption",false}}}));
            QVERIFY(seed.put(seed.lease(),{NativeCache::Kind::Users,{},{},{}},QJsonArray{QJsonObject{{"uuid",userId},{"username","saved-owner"}}}));
            QVERIFY(seed.put(seed.lease(),{NativeCache::Kind::Topics,streamId,{},{}},QJsonArray{QJsonObject{{"uuid",topicId},{"project_id",projectId},{"stream_uuid",streamId},{"name","Saved topic"}}}));
            QVERIFY(seed.put(seed.lease(),{NativeCache::Kind::History,streamId,topicId,{}},QJsonArray{message("Saved while messenger is unavailable")}));
        }
        int streams=0,users=0,topics=0,history=0;
        fixture.streams=[&](const Request &){++streams;return Response{QJsonObject{},503};};
        fixture.users=[&](const Request &){++users;return Response{QJsonObject{},503};};
        fixture.topics=[&](const Request &){++topics;return Response{QJsonObject{},503};};
        fixture.messages=[&](const Request &){++history;return Response{QJsonObject{},503};};
        NativeCacheService cache(directory.path(),{},true); fixture.controller.useCache(&cache);
        const auto connection=QUuid::createUuid().toString();
        {
            auto database=QSqlDatabase::addDatabase("QSQLITE",connection); database.setDatabaseName(directory.filePath("cache.sqlite3")); QVERIFY(database.open());
            QSqlQuery writer(database); QVERIFY(writer.exec("BEGIN IMMEDIATE")); bool released=false;
            QObject releaseContext;
            connect(&fixture.controller,&NativeController::stateChanged,&releaseContext,[&]{
                if(!released && !fixture.controller.error().isEmpty() && cache.maintenanceBusy()){
                    QVERIFY(writer.exec("ROLLBACK"));released=true;
                }
            });
            fixture.controller.login(fixture.server.origin().toString(),"synthetic-user","synthetic-password","");
            QTRY_COMPARE(fixture.controller.projects().size(),1);fixture.controller.openProject(projectId);
            QTRY_VERIFY(released);QTRY_COMPARE(fixture.controller.streams().size(),1);
            QCOMPARE(fixture.controller.streams().first().toMap().value("name").toString(),"Saved channel");
            fixture.controller.selectStream(streamId);QTRY_COMPARE(fixture.controller.topics().size(),1);
            fixture.controller.selectTopic(topicId);QTRY_COMPARE(fixture.controller.messages().size(),1);
            QTRY_VERIFY(!fixture.controller.busy()); QVERIFY(flushCache(cache));
            QCOMPARE(modelMessage(fixture.controller,messageId).value("content").toString(),"Saved while messenger is unavailable");
            QVERIFY(fixture.controller.cacheStale());QVERIFY(!fixture.controller.canSend());QVERIFY(!fixture.controller.canLoadOlder());
            QCOMPARE(streams,1);QCOMPARE(users,1);QCOMPARE(topics,1);QCOMPARE(history,1);database.close();
        }
        QSqlDatabase::removeDatabase(connection);
    }

    void pendingResizeCannotDelayAuthoritativeNavigation()
    {
        QTemporaryDir directory; QSemaphore gate; std::atomic_bool hold{false},entered{false};
        NativeCacheService cache(directory.path(),{},true,[&]{if(hold.exchange(false)){entered=true;gate.tryAcquire(1,2000);}return 1000;});
        Fixture fixture;fixture.extraTopic=true;fixture.controller.useCache(&cache);int history=0;
        fixture.messages=[&](const Request &request){++history;const auto topic=QUrlQuery(request.target).queryItemValue("topic_uuid");
            return Response{QJsonArray{message("Network remains responsive",topic==topicId?messageId:returnedId,topic)}};};
        QVERIFY(fixture.start());QVERIFY(flushCache(cache));hold=true;
        QVERIFY(cache.put(cache.lease(),{NativeCache::Kind::History,streamId,topicId,"blocked"},QJsonArray{message()}));QTRY_VERIFY(entered.load());
        QVERIFY(cache.setDiskLimitBytes(NativeCache::MinimumDiskLimitBytes));QVERIFY(!cache.available());
        fixture.controller.selectTopic(secondTopicId);
        QTRY_COMPARE(modelMessage(fixture.controller,returnedId).value("content").toString(),"Network remains responsive");
        QVERIFY(cache.maintenanceBusy());QCOMPARE(history,2);gate.release();QTRY_VERIFY(cache.available());QVERIFY(flushCache(cache));QCOMPARE(history,2);
    }

    void reopenTopicConfirmsWithoutRepeatingToggle_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"success", "forbidden", "ambiguous-applied", "ambiguous-unapplied",
                "invalid-done", "wrong-topic", "wrong-project", "failed-confirmation", "other-route", "logout", "newer-live-close"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }
    void reopenTopicConfirmsWithoutRepeatingToggle()
    {
        QFETCH(QString, scenario);
        Fixture fixture;
        fixture.extraTopic = true;
        bool done = true;
        bool validToggleRequest = true;
        int mutations = 0, confirmations = 0;
        const auto topic = [&] {
            return QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId},
                               {"name", "Resolved topic"}, {"is_done", done}};
        };
        fixture.topics = [&](const Request &) {
            return Response{QJsonArray{topic(), QJsonObject{{"uuid", secondTopicId}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"name", "Another topic"}, {"is_done", false}}}};
        };
        fixture.topicActions = [&](const Request &request) {
            validToggleRequest = validToggleRequest && request.method == "POST" && request.body.isEmpty();
            ++mutations;
            if (scenario != "forbidden" && scenario != "ambiguous-unapplied") done = false;
            return Response{QJsonObject{}, scenario == "forbidden" ? 403
                : scenario.startsWith("ambiguous-") ? 503 : 200, 80};
        };
        fixture.counters = [&](const Request &request) {
            if (request.target.path().endsWith(topicId)) {
                ++confirmations;
                auto value = topic();
                if (scenario == "invalid-done") value["is_done"] = "false";
                if (scenario == "wrong-topic") value["uuid"] = secondTopicId;
                if (scenario == "wrong-project") value["project_id"] = returnedId;
                return Response{value, scenario == "failed-confirmation" ? 503 : 200,
                    scenario == "newer-live-close" ? 150 : 0};
            }
            return Response{QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Native channel"}}};
        };
        QVERIFY(fixture.start());
        fixture.controller.setDraft("Draft survives reopening");
        QVERIFY(fixture.controller.topicClosed());
        QVERIFY(fixture.controller.canReopenTopic());
        fixture.controller.reopenTopic();
        QVERIFY(!fixture.controller.canReopenTopic());
        fixture.controller.reopenTopic();
        QTRY_COMPARE(mutations, 1);
        QVERIFY(validToggleRequest);
        if (scenario == "other-route") fixture.controller.selectTopic(secondTopicId);
        if (scenario == "logout") fixture.controller.logout();
        if (scenario == "newer-live-close") {
            QTRY_COMPARE(confirmations, 1);
            done = true;
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated", topic())));
        }
        QTRY_VERIFY(!fixture.controller.busy());
        QCOMPARE(mutations, 1);
        if (scenario == "other-route" || scenario == "logout") {
            QCOMPARE(confirmations, 0);
            QCOMPARE(fixture.controller.selectedTopic(), scenario == "logout" ? QString{} : secondTopicId);
            QVERIFY(!fixture.controller.topicClosed());
            return;
        }
        QCOMPARE(confirmations, 1);
        QCOMPARE(fixture.controller.draft(), QString("Draft survives reopening"));
        const bool confirmed = scenario == "success" || scenario == "ambiguous-applied";
        QCOMPARE(fixture.controller.topicClosed(), !confirmed);
        QCOMPARE(fixture.controller.canSend(), confirmed);
        if (confirmed) {
            QVERIFY(fixture.controller.error().isEmpty());
            fixture.controller.reopenTopic();
            QCOMPARE(mutations, 1);
        } else {
            if (scenario == "newer-live-close") {
                QVERIFY(fixture.controller.error().isEmpty());
                // The superseding event starts a separate authoritative topics
                // refresh; ending the toggle does not complete that GET.
                QTRY_VERIFY(fixture.controller.canReopenTopic());
            } else QVERIFY(!fixture.controller.error().isEmpty());
            if (scenario != "forbidden" && scenario != "ambiguous-unapplied" && scenario != "newer-live-close") {
                QVERIFY(!fixture.controller.canReopenTopic());
                fixture.controller.reopenTopic();
                QCOMPARE(mutations, 1);
            }
        }
    }

    void ownershipMatchesTheSourceViewerFallback_data()
    {
        QTest::addColumn<bool>("authorIsViewer");
        QTest::addColumn<bool>("serverOwn");
        for (const bool authorIsViewer : {false, true})
            for (const bool serverOwn : {false, true})
                QTest::newRow(qPrintable(QString("author-%1-flag-%2").arg(authorIsViewer).arg(serverOwn)))
                    << authorIsViewer << serverOwn;
    }
    void ownershipMatchesTheSourceViewerFallback()
    {
        QFETCH(bool, authorIsViewer);
        QFETCH(bool, serverOwn);
        Fixture fixture;
        auto row = message();
        row["author_uuid"] = authorIsViewer ? userId : returnedId;
        row["is_own"] = serverOwn;
        int edits = 0, removals = 0;
        fixture.messages = [&](const Request &request) {
            if (request.method == "PUT") {
                ++edits;
                row["payload"] = request.body.value("payload");
                return Response{row};
            }
            if (request.method == "DELETE") { ++removals; return Response{QJsonObject{}}; }
            return request.target.path().endsWith("/messages/") ? Response{QJsonArray{row}} : Response{row};
        };
        QVERIFY(fixture.start());
        const bool own = authorIsViewer || serverOwn;
        QCOMPARE(modelMessage(fixture.controller, messageId).value("own").toBool(), own);
        fixture.controller.editMessage(messageId, "Changed by the viewer");
        if (own) {
            QTRY_COMPARE(edits, 1);
            QTRY_COMPARE(modelMessage(fixture.controller, messageId).value("content").toString(),
                         QString("Changed by the viewer"));
        }
        fixture.controller.deleteMessage(messageId);
        if (own) { QTRY_COMPARE(removals, 1); QTRY_VERIFY(fixture.controller.messages().isEmpty()); }
        else { QCOMPARE(edits, 0); QCOMPARE(removals, 0); QCOMPARE(fixture.controller.messages().size(), 1); }
    }

    void reactionParticipantNamesUseTheCurrentVerifiedDirectory()
    {
        Fixture fixture;
        fixture.users = [](const Request &) {
            return Response{QJsonArray{QJsonObject{{"uuid", userId}, {"name", "Synthetic Owner"}}}};
        };
        fixture.messages = [](const Request &) {
            auto row = message();
            row["reactions"] = QJsonObject{{"heart", 3}};
            row["reaction_users"] = QJsonObject{{"heart", QJsonArray{userId, returnedId}}};
            return Response{QJsonArray{row}};
        };
        QVERIFY(fixture.start());
        const auto participants = [&] {
            return fixture.controller.messages().first().toMap().value("reactions").toList()
                .first().toMap().value("participants").toList();
        };
        QTRY_COMPARE(participants().value(0).toMap().value("name").toString(), QString("Synthetic Owner"));
        QCOMPARE(participants().size(), 2);
        QCOMPARE(participants().at(1).toMap(), QVariantMap({{"id", returnedId}, {"name", "#77777777"}}));
        const QJsonObject renamed{{"uuid", userId}, {"username", "renamed"},
            {"first_name", "Updated"}, {"last_name", "Owner"}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("user.updated", renamed)));
        QCOMPARE(participants().first().toMap().value("name").toString(), QString("Updated Owner"));
        QCOMPARE(fixture.controller.messages().first().toMap().value("reactions").toList().first()
                     .toMap().value("count").toInt(), 3);
        fixture.controller.logout();
        QVERIFY(fixture.controller.messages().isEmpty());
        fixture.users = [](const Request &) { return Response{QJsonArray{}}; };
        QVERIFY(fixture.start());
        QTRY_COMPARE(participants().first().toMap().value("name").toString(), QString("#22222222"));
    }

    void closedTopicsBlockNewSendsAndUploadsWithoutDiscardingDrafts_data()
    {
        QTest::addColumn<QString>("transition");
        for (const auto *state : {"initially-closed", "realtime-closed", "realtime-reopened", "other-topic-closed"})
            QTest::newRow(state) << QString::fromLatin1(state);
    }

    void closedTopicsBlockNewSendsAndUploadsWithoutDiscardingDrafts()
    {
        QFETCH(QString, transition);
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const auto topic = [](const QString &id, bool done) {
            return QJsonObject{{"uuid", id}, {"project_id", projectId}, {"stream_uuid", streamId},
                {"name", "Synthetic topic"}, {"is_done", done}};
        };
        fixture.topics = [&](const Request &) {
            return Response{QJsonArray{topic(topicId, transition == "initially-closed"), topic(secondTopicId, false)}};
        };
        QVERIFY(fixture.start());
        fixture.controller.setDraft("Keep this draft until the conversation reopens");
        if (transition != "initially-closed") {
            const auto affected = transition == "other-topic-closed" ? secondTopicId : topicId;
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated", topic(affected, true))));
        }
        if (transition == "realtime-reopened")
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated", topic(topicId, false), 13)));
        const bool closed = transition == "initially-closed" || transition == "realtime-closed";
        QCOMPARE(fixture.controller.canSend(), !closed);
        QCOMPARE(fixture.controller.canAttach(), !closed);
        if (closed) {
            const auto before = fixture.server.requests.size();
            fixture.controller.send();
            fixture.controller.attachFiles({QUrl::fromLocalFile("/nonexistent/cassi-closed-topic")});
            QCOMPARE(fixture.server.requests.size(), before);
            QVERIFY(!fixture.controller.fileBusy());
        }
        QCOMPARE(fixture.controller.draft(), "Keep this draft until the conversation reopens");
    }

    void reactionAddedReportsOnlyVerifiedNewLocalAdditions_data()
    {
        QTest::addColumn<QString>("operation");
        for (const auto *operation : {"verified", "remove", "already-owned", "uncertain-replay", "route-replay", "generation-replay", "rejected", "realtime", "logout-on-notify", "route-on-notify"})
            QTest::newRow(operation) << QString::fromLatin1(operation);
    }
    void reactionAddedReportsOnlyVerifiedNewLocalAdditions()
    {
        QFETCH(QString, operation);
        Fixture fixture; fixture.extraTopic = true;
        bool applied = operation == "remove" || operation == "already-owned";
        bool rotated = false; int adds = 0, removes = 0;
        const QJsonObject fact{{"uuid", returnedId}, {"project_id", projectId}, {"message_uuid", messageId}, {"user_uuid", userId},
            {"emoji_name", "heart"}, {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        const auto snapshot = [&] {
            const bool aggregate = applied && operation != "already-owned";
            auto row = message(); row["reactions"] = aggregate ? QJsonObject{{"heart", 1}} : QJsonObject{};
            row["reaction_users"] = aggregate ? QJsonObject{{"heart", QJsonArray{userId}}} : QJsonObject{}; return row;
        };
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith('/') && QUrlQuery(request.target).queryItemValue("topic_uuid") == secondTopicId)
                return Response{QJsonArray{message("other route", returnedId, secondTopicId)}};
            return Response{request.target.path().endsWith('/') ? QJsonValue(QJsonArray{snapshot()}) : QJsonValue(snapshot())};
        };
        fixture.authentication = [&](const Request &request) {
            if (rotated && request.target.path().endsWith("/actions/get_token/invoke"))
                return Response{QJsonObject{{"access_token", "synthetic-reaction-added-rotated"}, {"refresh_token", "synthetic-refresh"}, {"expires_in", 3600}}};
            return sessionResponse(request);
        };
        fixture.reactions = [&](const Request &request) {
            if (request.method == "POST") {
                ++adds;
                if (operation == "rejected") return Response{QJsonObject{}, 403};
                if (operation == "uncertain-replay") return Response{QJsonObject{}, 503};
                applied = true; return Response{fact, 201, operation == "route-replay" || operation == "generation-replay" ? 350 : 0};
            }
            if (request.method == "DELETE") { ++removes; applied = false; return Response{QJsonObject{}, 204}; }
            return Response{applied ? QJsonArray{fact} : QJsonArray{}};
        };
        QVERIFY(fixture.start()); QSignalSpy added(&fixture.controller, &NativeController::reactionAdded);
        bool handledNotification = false;
        if (operation.endsWith("on-notify")) connect(&fixture.controller, &NativeController::messagesChanged, &fixture.controller, [&] {
            const auto row = modelMessage(fixture.controller, messageId);
            if (handledNotification || row.value("reactionBusy").toBool() || row.value("reactions").toList().isEmpty()) return;
            handledNotification = true;
            if (operation == "logout-on-notify") fixture.controller.logout();
            else fixture.controller.selectTopic(secondTopicId);
        });
        if (operation == "realtime") {
            applied = true; QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", snapshot())));
        } else {
            fixture.controller.toggleReaction(messageId, operation == "verified" ? QString::fromUtf8("❤️") : "heart");
            if (operation == "route-replay") {
                QTRY_COMPARE(adds, 1); fixture.controller.selectTopic(secondTopicId);
                QTRY_VERIFY(!fixture.controller.busy()); fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy());
                QTRY_VERIFY(modelMessage(fixture.controller, messageId).value("reactionUncertain").toBool());
                fixture.controller.toggleReaction(messageId, "heart");
            } else if (operation == "generation-replay") {
                QTRY_COMPARE(adds, 1); rotated = true;
                QSignalSpy renewed(&fixture.session, &Session::authenticated); fixture.session.requestRefresh();
                QTRY_COMPARE(renewed.size(), 1); QTRY_VERIFY(!fixture.controller.busy());
                fixture.controller.toggleReaction(messageId, "heart");
            } else if (operation == "uncertain-replay") {
                QTRY_VERIFY(modelMessage(fixture.controller, messageId).value("reactionUncertain").toBool());
                applied = true; fixture.controller.toggleReaction(messageId, "heart");
            }
            QTRY_VERIFY(!modelMessage(fixture.controller, messageId).value("reactionBusy").toBool());
        }
        const bool expected = operation == "verified";
        QCOMPARE(added.size(), expected ? 1 : 0);
        if (expected) {
            QCOMPARE(added.first(), QVariantList({messageId, QString("heart"), fixture.controller.ownerKey(), streamId, topicId}));
            QCOMPARE(adds, 1); QCOMPARE(removes, 0);
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", snapshot())));
            QCOMPARE(added.size(), 1);
            fixture.controller.toggleReaction(messageId, "heart");
            QTRY_VERIFY(!modelMessage(fixture.controller, messageId).value("reactionBusy").toBool());
            QCOMPARE(removes, 1); QCOMPARE(added.size(), 1);
        }
        if (operation == "logout-on-notify") QVERIFY(!fixture.controller.authenticated());
        if (operation == "route-on-notify") QCOMPARE(fixture.controller.selectedTopic(), secondTopicId);
        if (operation == "remove" || operation == "already-owned") { QCOMPARE(adds, 0); QCOMPARE(removes, 1); }
        if (operation.endsWith("replay")) QCOMPARE(adds, 1);
    }

    void realtimeReadRetainsStaleRawHistoryButDeletionCannotResurrectIt_data()
    {
        QTest::addColumn<QString>("operation");
        for (const auto *operation : {"message.read", "messages.read", "stream.read", "topic.read", "message.deleted"})
            QTest::newRow(operation) << QString::fromLatin1(operation);
    }
    void realtimeReadRetainsStaleRawHistoryButDeletionCannotResurrectIt()
    {
        QFETCH(QString, operation);
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.extraTopic = true; fixture.controller.useCache(&cache);
        auto first = message("raw content retained independently of event payload"); first["read"] = false;
        auto second = message("saved off-route history", returnedId, secondTopicId); second["read"] = false;
        fixture.messages = [&](const Request &request) {
            return Response{QJsonArray{QUrlQuery(request.target).queryItemValue("topic_uuid") == topicId ? first : second}};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy());
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy());
        const NativeCache::Key history{NativeCache::Kind::History, streamId, topicId, {}};
        const NativeCache::Key other{NativeCache::Kind::History, streamId, secondTopicId, {}};
        const auto before = cacheEntry(cache, cache.lease(), history); QVERIFY(before && before->fresh);
        QJsonObject payload;
        if (operation == "message.read") {
            payload = first; payload["read"] = true;
            payload["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "event content must not replace saved raw content"}};
        } else if (operation == "messages.read") {
            payload = QJsonObject{{"project_id", projectId}, {"message_uuids", QJsonArray{messageId, returnedId}}};
        } else if (operation == "stream.read") {
            payload = QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Native channel"}, {"encryption", false}};
        } else if (operation == "topic.read") {
            payload = QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "Native topic"}};
        } else payload = QJsonObject{{"uuid", messageId}, {"stream_uuid", streamId}, {"topic_uuid", topicId}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent(operation, payload)));
        const auto saved = cacheEntry(cache, cache.lease(), history);
        if (operation == "message.deleted") { QVERIFY(!saved); QVERIFY(cacheEntry(cache, cache.lease(), other)); return; }
        QVERIFY(saved && !saved->fresh); QCOMPARE(saved->savedAt, before->savedAt); QCOMPARE(saved->nextMarker, before->nextMarker);
        const bool confirmed = operation == "message.read" || operation == "messages.read";
        auto expected = first; expected["read"] = confirmed; QCOMPARE(saved->rows, QJsonArray{expected});
        const auto otherSaved = cacheEntry(cache, cache.lease(), other); QVERIFY(otherSaved);
        QCOMPARE(otherSaved->rows.first().toObject().value("read").toBool(), operation == "messages.read");
        QVERIFY(cacheEntry(cache, cache.lease(), {NativeCache::Kind::Streams, {}, {}, {}}));
        QVERIFY(cacheEntry(cache, cache.lease(), {NativeCache::Kind::Topics, streamId, {}, {}}));
    }

    void confirmedViewportReadSurvivesRestartWithOfflineMessenger()
    {
        QTemporaryDir directory;
        Fixture fixture;
        bool offline = false; int history = 0, reads = 0, counterGets = 0;
        auto row = message("last known raw history"); row["read"] = false;
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) {
                ++reads; row["read"] = true; return Response{row};
            }
            ++history; return offline ? Response{QJsonObject{}, 503} : Response{QJsonArray{row}};
        };
        fixture.counters = [&](const Request &request) {
            ++counterGets;
            return Response{request.target.path().contains("/streams/")
                ? QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Native channel"}, {"encryption", false},
                    {"unread_count", 0}, {"active_unread_count", 0}, {"passive_unread_count", 0}, {"last_message_uuid", messageId}}
                : QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "Native topic"},
                    {"unread_count", 0}, {"active_unread_count", 0}, {"passive_unread_count", 0}, {"last_message_uuid", messageId}}};
        };
        {
            NativeCacheService cache(directory.path(), {}, true);
            NativeController current(&fixture.api, &fixture.session); current.useCache(&cache);
            current.login(fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
            QTRY_COMPARE(current.projects().size(), 1); current.openProject(projectId);
            QTRY_COMPARE(current.streams().size(), 1); current.selectStream(streamId);
            QTRY_COMPARE(current.topics().size(), 1); current.selectTopic(topicId); QTRY_VERIFY(!current.busy());
            QCOMPARE(history, 1); current.markMessagesReadUpTo(messageId);
            QTRY_VERIFY(modelMessage(current, messageId).value("read").toBool());
            QTRY_COMPARE(counterGets, 2); QCOMPARE(reads, 1);
            QCOMPARE(history, 1); // A read acknowledgement must not add a history GET.
        }
        offline = true;
        fixture.streams = [&](const Request &) { return Response{QJsonObject{}, 503}; };
        fixture.topics = [&](const Request &) { return Response{QJsonObject{}, 503}; };
        fixture.users = [&](const Request &) { return Response{QJsonObject{}, 503}; };
        NativeCacheService reopened(directory.path(), {}, true);
        ApiClient api(nullptr, true); Session session(&api); NativeController restored(&api, &session); restored.useCache(&reopened);
        restored.login(fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(restored.projects().size(), 1); restored.openProject(projectId);
        QTRY_COMPARE(restored.streams().size(), 1); restored.selectStream(streamId);
        QTRY_COMPARE(restored.topics().size(), 1); restored.selectTopic(topicId);
        QTRY_COMPARE(restored.messages().size(), 1);
        QCOMPARE(modelMessage(restored, messageId).value("content").toString(), "last known raw history");
        QVERIFY(modelMessage(restored, messageId).value("read").toBool());
        QVERIFY(restored.cacheStale()); QVERIFY(!restored.canSend()); QVERIFY(!restored.canLoadOlder());
        QVERIFY(!modelMessage(restored, messageId).value("canReact").toBool());
        QTRY_VERIFY(!restored.busy()); QCOMPARE(reads, 1); QCOMPARE(history, 2);
    }

    void verifiedTokenRotationReopensColdLeaseAndPreservesDraft()
    {
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.controller.useCache(&cache);
        bool rotate = false, delay = false; int history = 0;
        fixture.authentication = [&](const Request &request) {
            if (rotate && request.target.path().endsWith("/actions/get_token/invoke"))
                return Response{QJsonObject{{"access_token", "synthetic-rotated-cache-token"}, {"refresh_token", "synthetic-refresh"}, {"expires_in", 3600}}};
            return sessionResponse(request);
        };
        fixture.messages = [&](const Request &) {
            ++history; return Response{QJsonArray{message(rotate ? "verified renewal" : "old owner snapshot")}, 200, delay && !rotate ? 250 : 0};
        };
        QVERIFY(fixture.start()); const auto originalLease = cache.lease();
        fixture.controller.setDraft("preserve through renewal"); delay = true; fixture.controller.refresh(); QTRY_COMPARE(history, 2);
        QSignalSpy authenticated(&fixture.session, &Session::authenticated);
        rotate = true; fixture.session.requestRefresh(); QTRY_COMPARE(authenticated.size(), 1);
        QTRY_VERIFY(!fixture.controller.busy()); QTRY_COMPARE(fixture.api.findChildren<QNetworkReply *>().size(), 0);
        QCOMPARE(cache.lease().owner, originalLease.owner); QVERIFY(cache.lease().generation != originalLease.generation);
        QVERIFY(!cacheEntry(cache, originalLease, {NativeCache::Kind::History, streamId, topicId, {}}));
        QCOMPARE(modelMessage(fixture.controller, messageId).value("content").toString(), "verified renewal");
        QCOMPARE(fixture.controller.draft(), "preserve through renewal"); QVERIFY(fixture.controller.canSend()); QCOMPARE(history, 3);
        const auto entry = cacheEntry(cache, cache.lease(), {NativeCache::Kind::History, streamId, topicId, {}});
        QVERIFY(entry); QVERIFY(entry->fresh);
    }

    void websocketReconnectStalesSavedPagesAndBlocksOfflineMutations()
    {
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.extraTopic = true; fixture.controller.useCache(&cache);
        RealtimeGateway gateway(fixture.server.serverPort()); int history = 0;
        fixture.messages = [&](const Request &request) {
            ++history; const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            auto row = message("saved", topic == topicId ? messageId : returnedId, topic); row["read"] = false;
            return Response{QJsonArray{row}};
        };
        fixture.realtime = [&](const Request &request) {
            if (request.target.path().endsWith("/epoch/")) return Response{QJsonObject{{"epoch_generation", "controller-generation"},
                {"epoch_version", 10}, {"current_epoch_version", 10}, {"minimum_epoch_version", 1}}};
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start(gateway.origin()));
        RealtimeClient realtime(&fixture.api, &fixture.session); fixture.controller.useRealtime(&realtime);
        QTRY_COMPARE(fixture.controller.syncState(), "live"); QTRY_COMPARE(gateway.sockets.size(), 1);
        QCOMPARE(history, 2); // Initial route + authoritative realtime baseline.
        fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy());
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 3);
        fixture.controller.setDraft("must wait for reconnect"); QVERIFY(fixture.controller.canSend());
        const auto writes = [&] { return std::count_if(fixture.server.requests.cbegin(), fixture.server.requests.cend(), [](const Request &request) {
            return request.method != "GET" && request.target.path().contains("/messenger/");
        }); };
        const auto reactionQueries = [&] { return std::count_if(fixture.server.requests.cbegin(), fixture.server.requests.cend(), [](const Request &request) {
            return request.target.path().contains("/message_reactions/");
        }); };
        const auto beforeWrites = writes(), beforeReactions = reactionQueries();
        gateway.sockets.first()->close(); QTRY_COMPARE(fixture.controller.syncState(), "retrying");
        QVERIFY(fixture.controller.cacheStale()); QVERIFY(!fixture.controller.canSend());
        const auto saved = cacheEntry(cache, cache.lease(), {NativeCache::Kind::History, streamId, topicId, {}});
        QVERIFY(saved); QVERIFY(!saved->fresh);
        fixture.controller.send(); fixture.controller.starMessage(messageId); fixture.controller.markMessagesReadUpTo(messageId);
        fixture.controller.toggleReaction(messageId, "heart");
        QTRY_COMPARE(fixture.controller.syncState(), "live");
        QVERIFY(fixture.controller.canSend()); QCOMPARE(writes(), beforeWrites); QCOMPARE(reactionQueries(), beforeReactions);
        fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 4);
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 5);
        fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 5);
    }

    void cacheOwnerSwitchFencesOldRepliesAndLogoutClearsOnlyThatOwner()
    {
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture first, second; first.controller.useCache(&cache); second.controller.useCache(&cache);
        bool delay = false; int delayed = 0;
        first.messages = [&](const Request &) { if (delay) ++delayed; return Response{QJsonArray{message(delay ? "late private A" : "private A")}, 200, delay ? 250 : 0}; };
        second.messages = [&](const Request &) { return Response{QJsonArray{message("private B")}}; };
        QVERIFY(first.start()); delay = true; first.controller.refresh(); QTRY_COMPARE(delayed, 1);
        QVERIFY(second.start());
        QCOMPARE(modelMessage(second.controller, messageId).value("content").toString(), "private B");
        QTRY_VERIFY(first.api.findChildren<QNetworkReply *>().isEmpty());
        const auto activeLease = cache.lease();
        auto active = cacheEntry(cache, activeLease, {NativeCache::Kind::History, streamId, topicId, {}});
        QVERIFY(active); QCOMPARE(active->rows.first().toObject().value("payload").toObject().value("content").toString(), "private B");
        first.controller.logout();
        QVERIFY(cache.available()); QCOMPARE(cache.lease().owner, activeLease.owner); QCOMPARE(cache.lease().generation, activeLease.generation);
        active = cacheEntry(cache, activeLease, {NativeCache::Kind::History, streamId, topicId, {}}); QVERIFY(active); QVERIFY(active->fresh);
        QVERIFY(flushCache(cache));
        NativeCache inspector(directory.path(), {}, true);
        QVERIFY(inspector.open(first.api.server(), projectId, userId));
        QVERIFY(!inspector.get(inspector.lease(), {NativeCache::Kind::History, streamId, topicId, {}}));
    }

    void unknownReactionLocationInvalidatesSavedHistorySafely()
    {
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.extraTopic = true; fixture.controller.useCache(&cache); int history = 0;
        fixture.messages = [&](const Request &request) {
            ++history; const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            return Response{QJsonArray{message("saved", topic == topicId ? messageId : returnedId, topic)}};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 2);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message_reaction.created", QJsonObject{{"uuid", topicId}, {"project_id", projectId},
            {"message_uuid", "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"}, {"user_uuid", userId}, {"emoji_name", "heart"}, {"source_name", "native"}})));
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 3);
        fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 4);
    }

    void expiredCacheHydratesButNetworkFailureCannotEnableSending()
    {
        QTemporaryDir directory; qint64 now = 1000;
        NativeCacheService cache(directory.path(), {}, true, [&] { return now; });
        Fixture fixture; fixture.extraTopic = true; fixture.controller.useCache(&cache);
        int history = 0; bool fail = false;
        fixture.messages = [&](const Request &request) {
            ++history; const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            if (fail) return Response{QJsonObject{}, 503, 100};
            return Response{QJsonArray{message(topic, topic == topicId ? messageId : returnedId, topic)}};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy());
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 2);
        now += 120001; fail = true;
        fixture.controller.selectTopic(secondTopicId);
        QTRY_COMPARE(modelMessage(fixture.controller, returnedId).value("content").toString(), secondTopicId);
        fixture.controller.setDraft("wait for verification"); QVERIFY(fixture.controller.cacheStale()); QVERIFY(!fixture.controller.canSend());
        QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 3); QVERIFY(!fixture.controller.error().isEmpty());
        QVERIFY(!fixture.controller.canSend()); QVERIFY(!modelMessage(fixture.controller, returnedId).value("canReact").toBool());
        fail = false; fixture.controller.refresh(); QTRY_VERIFY(!fixture.controller.busy()); QTRY_VERIFY(fixture.controller.canSend());
        QVERIFY(!fixture.controller.cacheStale()); QCOMPARE(history, 4);
    }

    void cachedOlderPagesAreReusedOnlyWithinTheVerifiedLiveProjection()
    {
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.extraTopic = true; fixture.controller.useCache(&cache);
        QMap<int, QJsonObject> rows; for (int i = 1; i <= 70; ++i) rows.insert(i, historyMessage(i));
        int history = 0, markers = 0;
        fixture.messages = [&](const Request &request) {
            if (QUrlQuery(request.target).queryItemValue("topic_uuid") == secondTopicId) return Response{QJsonArray{}};
            ++history; if (!QUrlQuery(request.target).queryItemValue("page_marker").isEmpty()) ++markers;
            return historyResponse(rows, request);
        };
        QVERIFY(fixture.start()); fixture.controller.loadOlder(); QTRY_COMPARE(fixture.controller.messages().size(), 70);
        QCOMPARE(history, 2); QCOMPARE(markers, 1);
        fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy());
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(fixture.controller.messages().size(), 50);
        fixture.controller.loadOlder(); QTRY_COMPARE(fixture.controller.messages().size(), 70); QCOMPARE(history, 2); QCOMPARE(markers, 1);
        // A full baseline makes every cached marker stale. Deleting the old
        // first-page boundary must not send that saved cursor during hydration.
        rows.remove(21); fixture.controller.refresh(); QTRY_VERIFY(!fixture.controller.busy());
        QVERIFY(history >= 4); QCOMPARE(markers, 2); QCOMPARE(fixture.controller.messages().size(), 69);
    }

    void cacheDestructionAndReplacementFenceDelayedWrites()
    {
        QTemporaryDir firstDirectory, secondDirectory;
        auto *firstCache = new NativeCacheService(firstDirectory.path(), {}, true);
        NativeCacheService replacement(secondDirectory.path(), {}, true);
        Fixture fixture; fixture.controller.useCache(firstCache);
        auto row = message("old"); int delayed = 0;
        fixture.messages = [&](const Request &) {
            if (row.value("payload").toObject().value("content") == "old delayed") { ++delayed; return Response{QJsonArray{row}, 200, 250}; }
            return Response{QJsonArray{row}};
        };
        QVERIFY(fixture.start()); row["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "old delayed"}};
        fixture.controller.refresh(); QTRY_COMPARE(delayed, 1);
        delete firstCache;
        row["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "new service authoritative"}};
        fixture.controller.useCache(&replacement); QTRY_VERIFY(!fixture.controller.busy());
        // QNetworkAccessManager can retain an idle server-side connection.
        // Wait for the old HTTP operation, then check its model/cache effects.
        QTRY_COMPARE(fixture.api.findChildren<QNetworkReply *>().size(), 0);
        QCOMPARE(modelMessage(fixture.controller, messageId).value("content").toString(), "new service authoritative");
        const auto entry = cacheEntry(replacement, replacement.lease(), {NativeCache::Kind::History, streamId, topicId, {}});
        QVERIFY(entry); QCOMPARE(entry->rows.first().toObject().value("payload").toObject().value("content").toString(), "new service authoritative");
        QVERIFY(entry->fresh);
    }

    void unavailableCacheFallsBackToAuthoritativeRest()
    {
        QTemporaryDir directory; QFile blocker(directory.filePath("not-a-directory")); QVERIFY(blocker.open(QIODevice::WriteOnly)); blocker.write("fixture"); blocker.close();
        NativeCacheService cache(blocker.fileName(), {}, true);
        Fixture fixture; fixture.extraTopic = true; fixture.controller.useCache(&cache); int history = 0;
        fixture.messages = [&](const Request &request) {
            ++history; const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            return Response{QJsonArray{message("network", topic == topicId ? messageId : returnedId, topic)}};
        };
        QVERIFY(fixture.start()); QVERIFY(!cache.available()); fixture.controller.setDraft("works without storage"); QVERIFY(fixture.controller.canSend());
        fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy());
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 3);
        fixture.controller.setDraft("still verified"); QVERIFY(fixture.controller.canSend()); QVERIFY(!fixture.controller.cacheStale());
    }

    void logoutClearsCurrentCacheBeforeOwnerIdentityDisappears()
    {
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.controller.useCache(&cache);
        fixture.messages = [&](const Request &) { return Response{QJsonArray{message()}}; };
        QVERIFY(fixture.start()); QVERIFY(flushCache(cache)); QVERIFY(cache.stats().entries >= 4);
        fixture.controller.logout(); QVERIFY(!cache.available()); QVERIFY(fixture.controller.messages().isEmpty());
        QVERIFY(cache.open(fixture.api.server(), projectId, userId));
        QVERIFY(!cacheEntry(cache, cache.lease(), {NativeCache::Kind::History, streamId, topicId, {}}));
        QCOMPARE(cache.stats().entries, 0);
    }

    void cacheSnapshotTrustChangeNotifiesExistingMessageRows()
    {
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.controller.useCache(&cache);
        fixture.messages = [&](const Request &) { return Response{QJsonArray{message()}}; };
        QVERIFY(fixture.start());
        bool lastNotifiedCanReact = true;
        connect(&fixture.controller, &NativeController::messagesChanged, &fixture.controller, [&] {
            const auto row = modelMessage(fixture.controller, messageId);
            if (!row.isEmpty()) lastNotifiedCanReact = row.value("canReact").toBool();
        });
        fixture.controller.reloadSnapshot(); QTRY_VERIFY(!fixture.controller.busy());
        QVERIFY(modelMessage(fixture.controller, messageId).value("canReact").toBool());
        QVERIFY(lastNotifiedCanReact);
    }

    void cacheMutationInvalidatesOnlyTheAffectedConversation_data()
    {
        QTest::addColumn<QString>("operation");
        for (const auto *operation : {"star", "edit", "delete", "read", "reaction", "send"}) QTest::newRow(operation) << QString::fromLatin1(operation);
    }
    void cacheMutationInvalidatesOnlyTheAffectedConversation()
    {
        QFETCH(QString, operation);
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.extraTopic = true; fixture.controller.useCache(&cache);
        auto first = message("before"); first["read"] = false; first["reactions"] = QJsonObject{}; first["reaction_users"] = QJsonObject{};
        QMap<QString, QJsonObject> rows{{messageId, first}, {returnedId, message("other route", returnedId, secondTopicId)}};
        int history = 0; bool reacted = false;
        const QJsonObject fact{{"uuid", topicId}, {"project_id", projectId}, {"message_uuid", messageId}, {"user_uuid", userId},
            {"emoji_name", "heart"}, {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        fixture.messages = [&](const Request &request) {
            const auto path = request.target.path();
            if (path.endsWith("/actions/star/invoke")) { rows[messageId]["starred"] = true; return Response{rows.value(messageId)}; }
            if (path.endsWith("/actions/read_up_to/invoke")) { rows[messageId]["read"] = true; return Response{rows.value(messageId)}; }
            if (request.method == "PUT") { rows[messageId]["payload"] = request.body.value("payload"); return Response{rows.value(messageId)}; }
            if (request.method == "DELETE") { rows.remove(messageId); return Response{QJsonObject{}, 204}; }
            if (request.method == "POST") {
                const auto id = request.body.value("uuid").toString();
                rows[id] = message(request.body.value("payload").toObject().value("content").toString(), id);
                return Response{rows.value(id)};
            }
            if (!path.endsWith('/')) return Response{rows.value(path.section('/', -1))};
            ++history; QJsonArray result; const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            for (const auto &row : rows) if (row.value("topic_uuid").toString() == topic) result.append(row);
            return Response{result};
        };
        fixture.reactions = [&](const Request &request) {
            if (request.method == "POST") {
                reacted = true; rows[messageId]["reactions"] = QJsonObject{{"heart", 1}}; rows[messageId]["reaction_users"] = QJsonObject{{"heart", QJsonArray{userId}}};
                return Response{fact, 201};
            }
            return Response{reacted ? QJsonArray{fact} : QJsonArray{}};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy());
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 2);
        if (operation == "star") fixture.controller.starMessage(messageId);
        if (operation == "edit") fixture.controller.editMessage(messageId, "edited");
        if (operation == "delete") fixture.controller.deleteMessage(messageId);
        if (operation == "read") fixture.controller.markMessagesReadUpTo(messageId);
        if (operation == "reaction") fixture.controller.toggleReaction(messageId, "heart");
        if (operation == "send") { fixture.controller.setDraft("sent"); fixture.controller.send(); }
        const auto changed = [&] {
            const auto row = modelMessage(fixture.controller, messageId);
            if (operation == "delete") return row.isEmpty();
            if (operation == "star") return row.value("starred").toBool();
            if (operation == "edit") return row.value("content").toString() == "edited";
            if (operation == "read") return row.value("read").toBool();
            if (operation == "reaction") return row.value("reactions").toList().size() == 1 && !row.value("reactionBusy").toBool();
            return fixture.controller.draft().isEmpty() && fixture.controller.messages().size() == 2;
        };
        QTRY_VERIFY(changed());
        fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 2);
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy());
        QCOMPARE(history, 3); QVERIFY(changed());
    }

    void offRouteRealtimeInvalidatesHistoryWithoutEvictingOtherRoutes()
    {
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.extraTopic = true; fixture.controller.useCache(&cache);
        auto first = message("before"); int history = 0;
        fixture.messages = [&](const Request &request) {
            ++history; const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            return Response{QJsonArray{topic == topicId ? first : message("other", returnedId, secondTopicId)}};
        };
        QVERIFY(fixture.start()); fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 2);
        first["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "live off-route edit"}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", first)));
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 3);
        QCOMPARE(modelMessage(fixture.controller, messageId).value("content").toString(), "live off-route edit");
        fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 3);
    }

    void warmCacheNavigationAvoidsRedundantRestRequests()
    {
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.extraTopic = true; fixture.controller.useCache(&cache);
        int history = 0, topics = 0;
        fixture.topics = [&](const Request &) { ++topics; return Response{QJsonArray{
            QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "One"}},
            QJsonObject{{"uuid", secondTopicId}, {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "Two"}}}}; };
        fixture.messages = [&](const Request &request) {
            ++history; const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            return Response{QJsonArray{message(topic, topic == topicId ? messageId : returnedId, topic)}};
        };
        QVERIFY(fixture.start()); QCOMPARE(history, 1); QCOMPARE(topics, 1);
        fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 2);
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy());
        QCOMPARE(modelMessage(fixture.controller, messageId).value("content").toString(), topicId);
        QCOMPARE(history, 2);
        fixture.controller.selectStream({}); fixture.controller.selectStream(streamId);
        QTRY_COMPARE(fixture.controller.topics().size(), 2); QCOMPARE(topics, 1);
        fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(history, 2);
        QVERIFY(cache.stats().hits >= 3);
    }

    void coldDiskHydrationIsVisibleButCannotAuthorizeMutations()
    {
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.controller.useCache(&cache);
        auto row = message("saved offline text"); row["read"] = false;
        fixture.messages = [&](const Request &) { return Response{QJsonArray{row}}; };
        QVERIFY(fixture.start());
        // Seed a previous process' validated snapshots independently of the
        // warm-navigation test, so this exercises cold hydration itself.
        QVERIFY(cache.open(fixture.api.server(), projectId, userId));
        const auto lease = cache.lease();
        QVERIFY(cache.put(lease, {NativeCache::Kind::Streams, {}, {}, {}}, QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Previously plaintext"}, {"encryption", false}}}));
        QVERIFY(cache.put(lease, {NativeCache::Kind::Users, {}, {}, {}}, QJsonArray{QJsonObject{{"uuid", userId}, {"username", "fallback"}, {"name", "Saved preferred name"}}}));
        QVERIFY(cache.put(lease, {NativeCache::Kind::Topics, streamId, {}, {}}, QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "Saved topic"}}}));
        QVERIFY(cache.put(lease, {NativeCache::Kind::History, streamId, topicId, {}}, QJsonArray{row}));
        QVERIFY(flushCache(cache));
        cache.close();
        QVERIFY(flushCache(cache));
        NativeCacheService reopened(directory.path(), {}, true);
        NativeController restored(&fixture.api, &fixture.session);
        fixture.encrypted = true;
        fixture.users = [&](const Request &) { return Response{QJsonArray{QJsonObject{{"uuid", userId}, {"username", "fallback"}, {"name", "Verified preferred name"}}}, 200, 350}; };
        fixture.streams = [&](const Request &) { return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
            {"name", "Now encrypted"}, {"encryption", true}}}, 200, 350}; };
        fixture.topics = [&](const Request &) { return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "Current"}}}, 200, 350}; };
        fixture.messages = [&](const Request &) {
            auto current = row; current["payload"] = QJsonObject{{"kind", "e2ee"}, {"key_uuid", returnedId},
                {"content", QString::fromLatin1(QByteArray("opaque-never-render").toBase64())}};
            return Response{QJsonArray{current}, 200, 600};
        };
        restored.useCache(&reopened);
        QTRY_COMPARE(restored.streams().size(), 1);
        restored.selectStream(streamId); QTRY_COMPARE(restored.topics().size(), 1);
        restored.selectTopic(topicId);
        QTRY_COMPARE(restored.messages().size(), 1);
        QCOMPARE(modelMessage(restored, messageId).value("content").toString(), "saved offline text");
        QCOMPARE(modelMessage(restored, messageId).value("author").toString(), "Saved preferred name");
        QVERIFY(restored.cacheStale()); QVERIFY(!restored.canLoadOlder());
        restored.setDraft("must not send from stale encryption metadata");
        QVERIFY(!restored.canSend()); QVERIFY(!modelMessage(restored, messageId).value("canReact").toBool());
        const auto before = fixture.server.requests.size();
        restored.send(); restored.editMessage(messageId, "bad"); restored.starMessage(messageId); restored.deleteMessage(messageId);
        restored.markMessagesReadUpTo(messageId); restored.toggleReaction(messageId, "heart");
        QTRY_VERIFY(!restored.busy());
        for (qsizetype i = before; i < fixture.server.requests.size(); ++i) QVERIFY(fixture.server.requests.at(i).method == "GET");
        QVERIFY(!modelMessage(restored, messageId).value("content").toString().contains("opaque-never-render"));
        QVERIFY(!modelMessage(restored, messageId).value("content").toString().contains(QString::fromLatin1(QByteArray("opaque-never-render").toBase64())));
        QCOMPARE(modelMessage(restored, messageId).value("kind").toString(), "e2ee");
        QCOMPARE(modelMessage(restored, messageId).value("author").toString(), "Verified preferred name");
        QVERIFY(!restored.canSend());
    }

    void delayedMessageMutationCannotEraseConfirmedReactions()
    {
        Fixture fixture; int gets = 0; bool applied = false;
        auto row = message(); row["read"] = false; row["reactions"] = QJsonObject{}; row["reaction_users"] = QJsonObject{};
        const QJsonObject fact{{"uuid", returnedId}, {"project_id", projectId}, {"message_uuid", messageId},
            {"user_uuid", userId}, {"emoji_name", "heart"}, {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/star/invoke")) { row["starred"] = true; return Response{row}; }
            if (request.target.path().endsWith("/messages/")) return Response{QJsonArray{row}};
            return Response{row, 200, ++gets == 1 ? 350 : 0};
        };
        fixture.reactions = [&](const Request &request) {
            if (request.method == "POST") {
                applied = true; row["reactions"] = QJsonObject{{"heart", 1}}; row["reaction_users"] = QJsonObject{{"heart", QJsonArray{userId}}};
                return Response{fact, 201};
            }
            return Response{applied ? QJsonArray{fact} : QJsonArray{}};
        };
        QVERIFY(fixture.start()); fixture.controller.starMessage(messageId); QTRY_COMPARE(gets, 1);
        fixture.controller.toggleReaction(messageId, "heart"); QTRY_VERIFY(!modelMessage(fixture.controller, messageId).value("reactionBusy").toBool());
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("messages.read", QJsonObject{{"project_id", projectId}, {"message_uuids", QJsonArray{messageId}}})));
        // Server sockets can close before the client delivers its canonical
        // read-back. Observe confirmation before checking preserved fields.
        QTRY_VERIFY(modelMessage(fixture.controller, messageId).value("starred").toBool());
        QCOMPARE(modelMessage(fixture.controller, messageId).value("reactions").toList().size(), 1);
        QVERIFY(modelMessage(fixture.controller, messageId).value("reactions").toList().first().toMap().value("own").toBool());
        QVERIFY(modelMessage(fixture.controller, messageId).value("starred").toBool());
        QVERIFY(modelMessage(fixture.controller, messageId).value("read").toBool());
    }

    void reactionNotificationCanDestroyControllerBeforeAnyRequest()
    {
        Fixture fixture;
        auto row = message(); row["reactions"] = QJsonObject{}; row["reaction_users"] = QJsonObject{};
        fixture.messages = [&](const Request &) { return Response{QJsonArray{row}}; };
        QVERIFY(fixture.start());
        auto *controller = new NativeController(&fixture.api, &fixture.session);
        QPointer<NativeController> guard(controller);
        controller->refresh(); QTRY_COMPARE(controller->streams().size(), 1);
        controller->selectStream(streamId); QTRY_COMPARE(controller->topics().size(), 1);
        controller->selectTopic(topicId); QTRY_VERIFY(!controller->busy());
        connect(controller, &NativeController::messagesChanged, controller, [controller] { delete controller; });
        controller->toggleReaction(messageId, "heart");
        QVERIFY(guard.isNull());
        QVERIFY(std::none_of(fixture.server.requests.cbegin(), fixture.server.requests.cend(), [](const Request &request) {
            return request.target.path().contains("/message_reactions/");
        }));
    }

    void oldReactionRetryTimerCannotJoinRenewedGeneration()
    {
        Fixture fixture; bool rotate = false, applied = false; int confirmations = 0, mutations = 0;
        auto row = message(); row["reactions"] = QJsonObject{}; row["reaction_users"] = QJsonObject{};
        const QJsonObject fact{{"uuid", returnedId}, {"project_id", projectId}, {"message_uuid", messageId},
            {"user_uuid", userId}, {"emoji_name", "heart"}, {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        fixture.authentication = [&](const Request &request) {
            if (rotate && request.target.path().endsWith("/actions/get_token/invoke"))
                return Response{QJsonObject{{"access_token", "synthetic-rotated-reaction-token"}, {"refresh_token", "synthetic-refresh"}, {"expires_in", 3600}}};
            return sessionResponse(request);
        };
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith("/messages/")) return Response{QJsonArray{row}};
            ++confirmations; return Response{row, 200, applied ? 600 : 0};
        };
        fixture.reactions = [&](const Request &request) {
            if (request.method == "POST") { ++mutations; return Response{fact, 201}; }
            return Response{applied ? QJsonArray{fact} : QJsonArray{}};
        };
        QVERIFY(fixture.start()); fixture.controller.toggleReaction(messageId, "heart");
        QTRY_COMPARE(confirmations, 2);
        QSignalSpy authenticated(&fixture.session, &Session::authenticated);
        rotate = applied = true;
        row["reactions"] = QJsonObject{{"heart", 1}}; row["reaction_users"] = QJsonObject{{"heart", QJsonArray{userId}}};
        fixture.session.requestRefresh(); QTRY_COMPARE(authenticated.size(), 1); QTRY_VERIFY(!fixture.controller.busy());
        fixture.controller.toggleReaction(messageId, "heart"); QTRY_COMPARE(confirmations, 3);
        QTRY_VERIFY(!modelMessage(fixture.controller, messageId).value("reactionBusy").toBool());
        QTRY_COMPARE(fixture.server.findChildren<QTcpSocket *>().size(), 0);
        QCOMPARE(confirmations, 3); QCOMPARE(mutations, 1);
    }

    void localSendSignalRequiresVerifiedCurrentConversation_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"current", "other-route", "ambiguous", "unverified"}) QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }
    void localSendSignalRequiresVerifiedCurrentConversation()
    {
        QFETCH(QString, scenario);
        Fixture fixture; fixture.extraTopic = true; bool gotCanonical = false;
        auto saved = message("sent text", returnedId);
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST") return scenario == "ambiguous" ? Response{QJsonObject{}, 503} : Response{saved};
            if (request.target.path().endsWith(returnedId)) {
                gotCanonical = true; auto value = saved; if (scenario == "unverified") value["is_own"] = false;
                return Response{value, 200, 100};
            }
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start()); QSignalSpy sent(&fixture.controller, &NativeController::messageSent);
        fixture.controller.setDraft("sent text"); fixture.controller.send();
        if (scenario != "ambiguous") QTRY_VERIFY(gotCanonical);
        if (scenario == "other-route") fixture.controller.selectTopic(secondTopicId);
        QTRY_VERIFY(!fixture.controller.busy());
        QCOMPARE(sent.size(), scenario == "current" ? 1 : 0);
        if (!sent.isEmpty()) { QCOMPARE(sent.first().at(0).toString(), returnedId); QCOMPARE(sent.first().at(1).toString(), streamId); QCOMPARE(sent.first().at(2).toString(), topicId); }
    }

    void websocketMovedReactionRefreshesBothLoadedMessages()
    {
        Fixture fixture;
        RealtimeGateway gateway(fixture.server.serverPort());
        QVector<QJsonObject> events;
        QMap<QString, QJsonObject> rows{{messageId, message("old placement")}, {returnedId, message("new placement", returnedId)}};
        for (auto &row : rows) { row["reactions"] = QJsonObject{}; row["reaction_users"] = QJsonObject{}; }
        QSet<QString> fetched;
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith("/messages/")) return Response{QJsonArray{rows.value(messageId), rows.value(returnedId)}};
            const auto id = request.target.path().section('/', -1); fetched.insert(id); return Response{rows.value(id)};
        };
        fixture.realtime = [&](const Request &request) {
            if (request.target.path().endsWith("/epoch/")) return Response{QJsonObject{{"epoch_generation", "controller-generation"},
                {"epoch_version", 10}, {"current_epoch_version", 10}, {"minimum_epoch_version", 1}}};
            QJsonArray result;
            for (const auto &event : events)
                if (event.value("epoch_version").toInt() > QUrlQuery(request.target).queryItemValue("epoch_version>").toInt()) result.append(event);
            return Response{result};
        };
        QVERIFY(fixture.start(gateway.origin()));
        RealtimeClient realtime(&fixture.api, &fixture.session); fixture.controller.useRealtime(&realtime);
        QTRY_COMPARE(fixture.controller.syncState(), "live"); QTRY_COMPARE(gateway.sockets.size(), 1);
        rows[returnedId]["reactions"] = QJsonObject{{"heart", 1}};
        rows[returnedId]["reaction_users"] = QJsonObject{{"heart", QJsonArray{userId}}};
        const auto event = controllerEvent("message_reaction.updated", QJsonObject{{"uuid", topicId}, {"project_id", projectId},
            {"message_uuid", returnedId}, {"user_uuid", userId}, {"emoji_name", "heart"}, {"source_name", "zulip"},
            {"old_message_uuid", messageId}, {"old_emoji_name", "future-provider-emoji"}, {"old_source_name", "zulip"}});
        events.append(event);
        gateway.sockets.last()->sendTextMessage(QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact)));
        QTRY_COMPARE(realtime.acknowledgedEpoch(), quint64(12));
        QTRY_COMPARE(fetched.size(), 2);
        QTRY_COMPARE(modelMessage(fixture.controller, returnedId).value("reactions").toList().size(), 1);
        QVERIFY(modelMessage(fixture.controller, messageId).value("reactions").toList().isEmpty());
        QCOMPARE(modelMessage(fixture.controller, returnedId).value("content").toString(), "new placement");
    }

    void existingProviderAliasKeepsExactOwnedReactionIdentity()
    {
        Fixture fixture; QByteArray method;
        auto row = message(); row["reactions"] = QJsonObject{{"like", 1}}; row["reaction_users"] = QJsonObject{{"like", QJsonArray{userId}}};
        const QJsonObject fact{{"uuid", returnedId}, {"project_id", projectId}, {"message_uuid", messageId},
            {"user_uuid", userId}, {"emoji_name", "like"}, {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        fixture.messages = [&](const Request &request) { return request.target.path().endsWith("/messages/") ? Response{QJsonArray{row}} : Response{row}; };
        fixture.reactions = [&](const Request &request) {
            if (request.method != "GET") { method = request.method; return Response{QJsonObject{}, 403}; }
            return Response{QJsonArray{fact}};
        };
        QVERIFY(fixture.start()); fixture.controller.toggleReaction(messageId, "like");
        QTRY_VERIFY(!method.isEmpty()); QCOMPARE(method, QByteArray("DELETE"));
    }

    void reactionPreflightRejectsForeignOwnershipAndBadPagination_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"foreign-user", "wrong-project", "wrong-message", "duplicate-emoji", "loop-marker", "forbidden"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }

    void reactionPreflightRejectsForeignOwnershipAndBadPagination()
    {
        QFETCH(QString, scenario);
        Fixture fixture; int mutations = 0, reads = 0;
        auto row = message(); row["reactions"] = QJsonObject{}; row["reaction_users"] = QJsonObject{};
        auto fact = QJsonObject{{"uuid", returnedId}, {"project_id", projectId}, {"message_uuid", messageId},
            {"user_uuid", userId}, {"emoji_name", "heart"}, {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        if (scenario == "foreign-user") fact["user_uuid"] = streamId;
        if (scenario == "wrong-project") fact["project_id"] = userId;
        if (scenario == "wrong-message") fact["message_uuid"] = returnedId;
        fixture.messages = [&](const Request &) { return Response{QJsonArray{row}}; };
        fixture.reactions = [&](const Request &request) {
            if (request.method != "GET") { ++mutations; return Response{QJsonObject{}, 403}; }
            ++reads;
            if (scenario == "forbidden") return Response{QJsonArray{}};
            auto rows = scenario == "loop-marker" ? QJsonArray{} : QJsonArray{fact};
            if (scenario == "duplicate-emoji") { auto duplicate = fact; duplicate["uuid"] = topicId; rows.append(duplicate); }
            Response response{rows};
            if (scenario == "loop-marker") response.headers.insert("X-Pagination-Marker", returnedId.toUtf8());
            return response;
        };
        QVERIFY(fixture.start()); fixture.controller.toggleReaction(messageId, "heart");
        QTRY_VERIFY(!fixture.controller.error().isEmpty());
        QVERIFY(!modelMessage(fixture.controller, messageId).value("reactionBusy").toBool());
        QVERIFY(!modelMessage(fixture.controller, messageId).value("reactionUncertain").toBool());
        QCOMPARE(mutations, scenario == "forbidden" ? 1 : 0);
        QVERIFY(reads <= 2);
    }

    void reactionsDoNotMutateAfterRouteChangeOrLeakAcrossGeneration()
    {
        Fixture fixture; fixture.extraTopic = true;
        auto row = message(); row["reactions"] = QJsonObject{}; row["reaction_users"] = QJsonObject{};
        int mutations = 0, preflights = 0;
        fixture.messages = [&](const Request &request) {
            const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            auto value = row; if (topic == secondTopicId) value["topic_uuid"] = secondTopicId;
            return request.target.path().endsWith("/messages/") ? Response{QJsonArray{value}} : Response{row};
        };
        fixture.reactions = [&](const Request &request) {
            if (request.method == "POST") { ++mutations; return Response{QJsonObject{}, 503, 150}; }
            ++preflights; return Response{QJsonArray{}, 200, 100};
        };
        QVERIFY(fixture.start()); fixture.controller.toggleReaction(messageId, "heart");
        QTRY_COMPARE(preflights, 1); fixture.controller.selectTopic(secondTopicId);
        QTRY_VERIFY(!fixture.controller.busy());
        QTRY_COMPARE(fixture.server.findChildren<QTcpSocket *>().size(), 0);
        QCOMPARE(mutations, 0);
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(!fixture.controller.busy());
        fixture.controller.toggleReaction(messageId, "heart"); QTRY_COMPARE(mutations, 1);
        fixture.api.setAccessToken("replacement-test-owner-token");
        QTRY_COMPARE(fixture.server.findChildren<QTcpSocket *>().size(), 0);
        QVERIFY(modelMessage(fixture.controller, messageId).value("reactionBusy").toBool());
        QVERIFY(modelMessage(fixture.controller, messageId).value("reactionUncertain").toBool());
        QCOMPARE(modelMessage(fixture.controller, messageId).value("reactions").toList().first().toMap().value("count").toInt(), 0);
        QCOMPARE(mutations, 1);
        fixture.controller.logout(); QVERIFY(fixture.controller.messages().isEmpty());
    }

    void sameOwnerTokenRotationRetainsUncertainReactionWithoutAnotherPost()
    {
        Fixture fixture; bool rotate = false, applied = false; int mutations = 0;
        auto row = message(); row["reactions"] = QJsonObject{}; row["reaction_users"] = QJsonObject{};
        const QJsonObject fact{{"uuid", returnedId}, {"project_id", projectId}, {"message_uuid", messageId},
            {"user_uuid", userId}, {"emoji_name", "heart"}, {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        fixture.authentication = [&](const Request &request) {
            if (rotate && request.target.path().endsWith("/actions/get_token/invoke"))
                return Response{QJsonObject{{"access_token", "synthetic-rotated-project-token"}, {"refresh_token", "synthetic-refresh"}, {"expires_in", 3600}}};
            return sessionResponse(request);
        };
        fixture.messages = [&](const Request &request) { return request.target.path().endsWith("/messages/") ? Response{QJsonArray{row}} : Response{row}; };
        fixture.reactions = [&](const Request &request) {
            if (request.method == "POST") { ++mutations; return Response{fact, 201, 250}; }
            return Response{applied ? QJsonArray{fact} : QJsonArray{}};
        };
        QVERIFY(fixture.start()); fixture.controller.toggleReaction(messageId, "heart"); QTRY_COMPARE(mutations, 1);
        QSignalSpy authenticated(&fixture.session, &Session::authenticated);
        rotate = true; fixture.session.requestRefresh(); QTRY_COMPARE(authenticated.size(), 1);
        QTRY_VERIFY(!fixture.controller.busy());
        QVERIFY(modelMessage(fixture.controller, messageId).value("reactionUncertain").toBool());
        applied = true; row["reactions"] = QJsonObject{{"heart", 1}}; row["reaction_users"] = QJsonObject{{"heart", QJsonArray{userId}}};
        fixture.controller.toggleReaction(messageId, "heart");
        QTRY_VERIFY(!modelMessage(fixture.controller, messageId).value("reactionBusy").toBool());
        QCOMPARE(mutations, 1);
    }

    void realtimeReactionIdentityRefreshCannotOverwriteNewerSnapshotOrContent()
    {
        Fixture fixture;
        auto row = message("kept content"); row["reactions"] = QJsonObject{}; row["reaction_users"] = QJsonObject{};
        int exactGets = 0;
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith("/messages/")) return Response{QJsonArray{row}};
            ++exactGets;
            auto response = row; response["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "never merge GET content"}};
            return Response{response, 200, exactGets == 1 ? 100 : 0};
        };
        QVERIFY(fixture.start());
        auto payload = QJsonObject{{"uuid", returnedId}, {"project_id", projectId}, {"message_uuid", messageId},
            {"user_uuid", userId}, {"emoji_name", "heart"}, {"source_name", "native"}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message_reaction.created", payload)));
        QTRY_COMPARE(exactGets, 1);
        row["reactions"] = QJsonObject{{"heart", 2}}; row["reaction_users"] = QJsonObject{{"heart", QJsonArray{userId, returnedId}}};
        row["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "newer concurrent content"}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", row, 13)));
        QTRY_VERIFY(exactGets >= 2);
        QTRY_COMPARE(fixture.server.findChildren<QTcpSocket *>().size(), 0);
        const auto model = modelMessage(fixture.controller, messageId);
        QCOMPARE(model.value("content").toString(), "newer concurrent content");
        QCOMPARE(model.value("reactions").toList().first().toMap().value("count").toInt(), 2);
        payload["source_name"] = "arbitrary-image-url";
        QVERIFY(!fixture.controller.applyRealtimeEvent(controllerEvent("message_reaction.deleted", payload, 14)));
        payload["source_name"] = "zulip"; payload["old_message_uuid"] = "not-a-uuid";
        QVERIFY(!fixture.controller.applyRealtimeEvent(controllerEvent("message_reaction.updated", payload, 15)));
    }

    void reactionsUseOwnFactUuidAndPreserveEncryptedContent()
    {
        Fixture fixture; fixture.encrypted = true;
        auto row = message(); row["payload"] = QJsonObject{{"kind", "e2ee"}, {"key_uuid", returnedId},
                {"content", QString::fromLatin1(QByteArray("must-not-be-shown").toBase64())}};
        row["reactions"] = QJsonObject{{"future-provider-emoji", 2}};
        row["reaction_users"] = QJsonObject{}; // Count-only snapshot cannot establish ownership.
        const QJsonObject own{{"uuid", returnedId}, {"project_id", projectId}, {"message_uuid", messageId},
            {"user_uuid", userId}, {"emoji_name", "future-provider-emoji"},
            {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        bool deleted = false, validRequests = true; int mutations = 0;
        fixture.messages = [&](const Request &request) { return request.target.path().endsWith("/messages/") ? Response{QJsonArray{row}} : Response{row}; };
        fixture.reactions = [&](const Request &request) {
            if (request.method == "DELETE") {
                validRequests = validRequests && request.target.path().section('/', -1) == returnedId;
                deleted = true; ++mutations; row["reactions"] = QJsonObject{{"future-provider-emoji", 1}};
                return Response{QJsonObject{}, 204};
            }
            const QUrlQuery query(request.target);
            validRequests = validRequests && query.queryItemValue("message_uuid") == messageId && query.queryItemValue("user_uuid") == userId;
            return Response{deleted ? QJsonArray{} : QJsonArray{own}};
        };
        QVERIFY(fixture.start());
        auto model = modelMessage(fixture.controller, messageId);
        QVERIFY(model.value("canReact").toBool()); QVERIFY(!model.value("reactions").toList().first().toMap().value("ownKnown").toBool());
        fixture.controller.toggleReaction(messageId, "future-provider-emoji");
        QTRY_COMPARE(mutations, 1);
        QTRY_VERIFY(!modelMessage(fixture.controller, messageId).value("reactionBusy").toBool());
        model = modelMessage(fixture.controller, messageId);
        QVERIFY(!model.value("content").toString().contains("must-not-be-shown"));
        QVERIFY(!model.value("content").toString().contains(QString::fromLatin1(QByteArray("must-not-be-shown").toBase64())));
        QCOMPARE(model.value("reactions").toList().first().toMap().value("count").toInt(), 1);
        QVERIFY(!fixture.controller.canSend());
    }

    void reactionsCanonicalAddWaitsForProjectionAndNeverOverwritesMessage()
    {
        Fixture fixture;
        auto row = message("initial"); row["read"] = false;
        row["reactions"] = QJsonObject{}; row["reaction_users"] = QJsonObject{};
        bool applied = false, validBody = true; int mutations = 0, confirmations = 0;
        const QJsonObject fact{{"uuid", returnedId}, {"project_id", projectId}, {"message_uuid", messageId},
            {"user_uuid", userId}, {"emoji_name", "thumbs_up"}, {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith("/messages/")) return Response{QJsonArray{row}};
            ++confirmations;
            auto stale = row; stale["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "stale"}}; stale["read"] = false;
            if (confirmations >= 2) { applied = true; stale["reactions"] = QJsonObject{{"thumbs_up", 1}}; stale["reaction_users"] = QJsonObject{{"thumbs_up", QJsonArray{userId}}}; }
            return Response{stale};
        };
        fixture.reactions = [&](const Request &request) {
            if (request.method == "POST") {
                validBody = request.body == QJsonObject{{"message_uuid", messageId}, {"emoji_name", "thumbs_up"}}; ++mutations;
                return Response{fact, 201};
            }
            return Response{applied ? QJsonArray{fact} : QJsonArray{}};
        };
        QVERIFY(fixture.start());
        fixture.controller.toggleReaction(messageId, " :Thumbs-Up: ");
        fixture.controller.toggleReaction(messageId, "thumbs_up");
        row["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "concurrent edit"}}; row["read"] = true;
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", row)));
        QTRY_COMPARE(mutations, 1);
        QTRY_VERIFY(!modelMessage(fixture.controller, messageId).value("reactionBusy").toBool());
        const auto model = modelMessage(fixture.controller, messageId);
        QCOMPARE(model.value("content").toString(), "concurrent edit"); QVERIFY(model.value("read").toBool());
        const auto reaction = model.value("reactions").toList().first().toMap();
        QCOMPARE(reaction.value("emojiName").toString(), "thumbs_up"); QVERIFY(reaction.value("own").toBool());
        QVERIFY(confirmations >= 2); QCOMPARE(mutations, 1); QVERIFY(validBody);
    }

    void ambiguousReactionKeepsIntentAndExplicitRetryOnlyConfirms()
    {
        Fixture fixture;
        auto row = message(); row["reactions"] = QJsonObject{}; row["reaction_users"] = QJsonObject{};
        int mutations = 0, gets = 0; bool applied = false;
        const QJsonObject fact{{"uuid", returnedId}, {"project_id", projectId}, {"message_uuid", messageId},
            {"user_uuid", userId}, {"emoji_name", "heart"}, {"created_at", "2026-01-02T10:00:00Z"}, {"updated_at", "2026-01-02T10:00:00Z"}};
        fixture.messages = [&](const Request &request) { return request.target.path().endsWith("/messages/") ? Response{QJsonArray{row}} : Response{row}; };
        fixture.reactions = [&](const Request &request) {
            if (request.method == "POST") { ++mutations; return Response{QJsonObject{}, 503}; }
            ++gets; return Response{applied ? QJsonArray{fact} : QJsonArray{}};
        };
        QVERIFY(fixture.start()); fixture.controller.toggleReaction(messageId, "heart");
        QTRY_VERIFY(modelMessage(fixture.controller, messageId).value("reactionUncertain").toBool());
        QCOMPARE(mutations, 1); QVERIFY(gets >= 2 && gets <= 5);
        QVERIFY(modelMessage(fixture.controller, messageId).value("reactionBusy").toBool());
        applied = true; row["reactions"] = QJsonObject{{"heart", 1}}; row["reaction_users"] = QJsonObject{{"heart", QJsonArray{userId}}};
        fixture.controller.toggleReaction(messageId, "heart");
        QTRY_VERIFY(!modelMessage(fixture.controller, messageId).value("reactionBusy").toBool());
        QCOMPARE(mutations, 1);
    }

    void incompleteReadCounterSnapshotsCannotInventZeroUnread()
    {
        Fixture fixture;
        auto row = message(); row["read"] = false;
        const auto streamSnapshot = QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Native channel"}, {"encryption", false},
            {"unread_count", 7}, {"active_unread_count", 5}, {"passive_unread_count", 2}, {"last_message_uuid", returnedId}};
        const auto topicSnapshot = QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "Native topic"},
            {"unread_count", 5}, {"active_unread_count", 3}, {"passive_unread_count", 2}, {"last_message_uuid", returnedId}};
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) { auto response = row; response["read"] = true; return Response{response}; }
            return Response{QJsonArray{row}};
        };
        fixture.counters = [&](const Request &request) {
            auto response = request.target.path().contains("/streams/") ? streamSnapshot : topicSnapshot;
            response.remove("unread_count");
            return Response{response};
        };
        QVERIFY(fixture.start());
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated", streamSnapshot)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated", topicSnapshot, 13)));
        fixture.controller.markMessagesReadUpTo(messageId);
        QTRY_VERIFY(modelMessage(fixture.controller, messageId).value("read").toBool());
        QTRY_VERIFY(!fixture.controller.error().isEmpty());
        QCOMPARE(fixture.controller.streams().first().toMap().value("unread").toInt(), 7);
        QCOMPARE(fixture.controller.topics().first().toMap().value("unread").toInt(), 5);
    }

    void readMarkerOwnerGenerationAndMalformedAcknowledgements_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"generation", "wrong-project", "wrong-id", "wrong-topic", "wrong-owner", "read-string", "changed-time"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }

    void readMarkerOwnerGenerationAndMalformedAcknowledgements()
    {
        QFETCH(QString, scenario);
        Fixture fixture;
        auto row = message(); row["read"] = false;
        int reads = 0, counterGets = 0;
        fixture.counters = [&](const Request &) { ++counterGets; return Response{QJsonObject{}}; };
        fixture.messages = [&](const Request &request) {
            if (!request.target.path().endsWith("/actions/read_up_to/invoke")) return Response{QJsonArray{row}};
            ++reads;
            auto response = row; response["read"] = true;
            if (scenario == "wrong-project") response["project_id"] = streamId;
            if (scenario == "wrong-id") response["uuid"] = returnedId;
            if (scenario == "wrong-topic") response["topic_uuid"] = secondTopicId;
            if (scenario == "wrong-owner") response["user_uuid"] = streamId;
            if (scenario == "read-string") response["read"] = "true";
            if (scenario == "changed-time") response["created_at"] = "2026-01-02T10:00:01Z";
            return Response{response, 200, scenario == "generation" ? 100 : 0, {}};
        };
        QVERIFY(fixture.start());
        fixture.controller.markMessagesReadUpTo(messageId);
        QTRY_COMPARE_WITH_TIMEOUT(reads, 1, 2000);
        if (scenario == "generation") fixture.api.setAccessToken("replacement-token");
        else QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.error().isEmpty(), 2000);
        QTRY_COMPARE(fixture.server.findChildren<QTcpSocket *>().size(), 0);
        QVERIFY(!modelMessage(fixture.controller, messageId).value("read").toBool());
        QCOMPARE(counterGets, 0);
        QCOMPARE(reads, 1);
    }

    void admittedReadUsesBoundedExactGetAndCoalescesNewerViewportAnchor()
    {
        Fixture fixture;
        auto one = message("first", messageId), two = message("second", returnedId);
        one["read"] = two["read"] = false;
        two["created_at"] = "2026-01-02T10:00:01Z";
        int posts = 0, gets = 0;
        fixture.messages = [&](const Request &request) {
            const auto path = request.target.path();
            if (path.endsWith("/actions/read_up_to/invoke")) { ++posts; return Response{path.contains(returnedId) ? two : one}; }
            if (request.method == "GET" && (path.endsWith(messageId) || path.endsWith(returnedId))) {
                ++gets;
                auto row = path.endsWith(returnedId) ? two : one;
                row["read"] = true;
                return Response{row, 200, 100, {}};
            }
            return Response{QJsonArray{two, one}};
        };
        QVERIFY(fixture.start());
        fixture.controller.markMessagesReadUpTo(messageId);
        fixture.controller.markMessagesReadUpTo(returnedId);
        for (int i = 0; i < 100; ++i) fixture.controller.markMessagesReadUpTo(returnedId);
        QTRY_VERIFY_WITH_TIMEOUT(modelMessage(fixture.controller, returnedId).value("read").toBool(), 2000);
        QCOMPARE(posts, 2); QCOMPARE(gets, 2);
        QVERIFY(modelMessage(fixture.controller, messageId).value("read").toBool());
    }

    void unappliedReadAdmissionIsBoundedWithoutRepeatingMutation()
    {
        Fixture fixture;
        auto row = message(); row["read"] = false;
        int posts = 0, gets = 0;
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) { ++posts; return Response{row}; }
            if (request.target.path().endsWith(messageId)) { ++gets; return Response{row}; }
            return Response{QJsonArray{row}};
        };
        QVERIFY(fixture.start());
        fixture.controller.markMessagesReadUpTo(messageId);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.error().isEmpty(), 2500);
        QCOMPARE(posts, 1); QCOMPARE(gets, 3);
        QVERIFY(!modelMessage(fixture.controller, messageId).value("read").toBool());
        for (int i = 0; i < 100; ++i) fixture.controller.markMessagesReadUpTo(messageId);
        QCOMPARE(posts, 1); QCOMPARE(gets, 3);
    }

    void readCountersAreAuthoritativeAndCannotOverwriteLaterRealtimeSnapshots()
    {
        Fixture fixture;
        auto row = message(); row["read"] = false;
        auto streamSnapshot = QJsonObject{{"uuid", streamId}, {"project_id", projectId}, {"name", "Native channel"}, {"encryption", false},
            {"unread_count", 4}, {"active_unread_count", 3}, {"passive_unread_count", 1}, {"last_message_uuid", returnedId}};
        auto topicSnapshot = QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "Native topic"},
            {"unread_count", 2}, {"active_unread_count", 1}, {"passive_unread_count", 1}, {"last_message_uuid", returnedId}};
        int counterGets = 0;
        fixture.counters = [&](const Request &request) { ++counterGets; return Response{request.target.path().contains("/streams/") ? streamSnapshot : topicSnapshot}; };
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) { auto response = row; response["read"] = true; return Response{response}; }
            return Response{QJsonArray{row}};
        };
        QVERIFY(fixture.start());
        fixture.controller.markMessagesReadUpTo(messageId);
        QTRY_COMPARE(counterGets, 2);
        QTRY_COMPARE(fixture.controller.streams().first().toMap().value("unread").toInt(), 4);
        QTRY_COMPARE(fixture.controller.topics().first().toMap().value("unread").toInt(), 2);
        QCOMPARE(fixture.controller.streams().first().toMap().value("activeUnread").toInt(), 3);
        QCOMPARE(fixture.controller.topics().first().toMap().value("lastMessage").toString(), returnedId);
        fixture.counters = [&](const Request &request) { ++counterGets; return Response{request.target.path().contains("/streams/") ? streamSnapshot : topicSnapshot, 200, 150, {}}; };
        // A fresh unread arrival enables another viewport mutation with delayed counter GETs.
        auto incoming = historyMessage(700); incoming["read"] = false;
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", incoming)));
        fixture.messages = [&](const Request &request) {
            auto response = incoming; response["read"] = true;
            return Response{request.target.path().endsWith("/actions/read_up_to/invoke") ? QJsonValue(response) : QJsonValue(QJsonArray{row, incoming})};
        };
        fixture.controller.markMessagesReadUpTo(incoming.value("uuid").toString());
        QTRY_COMPARE(counterGets, 4);
        streamSnapshot["unread_count"] = 7; topicSnapshot["unread_count"] = 5;
        streamSnapshot["last_message_uuid"] = incoming.value("uuid"); topicSnapshot["last_message_uuid"] = incoming.value("uuid");
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated", streamSnapshot, 20)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated", topicSnapshot, 21)));
        QTRY_COMPARE(fixture.server.findChildren<QTcpSocket *>().size(), 0);
        QCOMPARE(fixture.controller.streams().first().toMap().value("unread").toInt(), 7);
        QCOMPARE(fixture.controller.topics().first().toMap().value("unread").toInt(), 5);
        QCOMPARE(fixture.controller.streams().first().toMap().value("lastMessage").toString(), incoming.value("uuid").toString());
    }

    void viewportReadMarkerUpdatesOnlyFlagsThroughExactBoundary()
    {
        Fixture fixture;
        auto earlier = message("earlier", returnedId);
        auto anchor = message("old anchor", messageId);
        auto later = historyMessage(99);
        earlier["created_at"] = "2026-01-02T10:00:00.000100Z";
        anchor["created_at"] = "2026-01-02T10:00:00.000200Z";
        later["created_at"] = "2026-01-02T10:00:00.000300Z";
        earlier["read"] = anchor["read"] = later["read"] = false;
        int reads = 0;
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) {
                ++reads;
                auto response = anchor; response["read"] = true;
                return Response{response, 200, 100, {}};
            }
            return Response{QJsonArray{later, anchor, earlier}};
        };
        QVERIFY(fixture.start());
        QCOMPARE(reads, 0); // Loading a conversation never marks it as read.
        fixture.controller.markMessagesReadUpTo(messageId);
        fixture.controller.markMessagesReadUpTo(messageId);
        QTRY_COMPARE(reads, 1);
        auto updated = anchor; updated["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "live edit"}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", updated)));
        auto arriving = historyMessage(200); arriving["created_at"] = "2026-01-02T10:00:00.000150Z"; arriving["read"] = false;
        const auto arrivingId = arriving.value("uuid").toString();
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", arriving, 13)));
        QTRY_VERIFY(modelMessage(fixture.controller, messageId).value("read").toBool());
        QCOMPARE(modelMessage(fixture.controller, messageId).value("content").toString(), "live edit");
        QVERIFY(modelMessage(fixture.controller, returnedId).value("read").toBool());
        QVERIFY(!modelMessage(fixture.controller, later.value("uuid").toString()).value("read").toBool());
        QVERIFY(!modelMessage(fixture.controller, arrivingId).value("read").toBool());
        // An older queued event cannot undo a locally confirmed monotonic read flag.
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.updated", updated, 14)));
        QVERIFY(modelMessage(fixture.controller, messageId).value("read").toBool());
        fixture.controller.markMessagesReadUpTo(messageId);
        QCOMPARE(reads, 1);
    }

    void failedReadMarkerIsBoundedAndDoesNotChangeFlags()
    {
        Fixture fixture;
        auto row = message(); row["read"] = false;
        int reads = 0;
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) { ++reads; return Response{QJsonObject{}, 503}; }
            return Response{QJsonArray{row}};
        };
        QVERIFY(fixture.start());
        fixture.controller.markMessagesReadUpTo(messageId);
        QTRY_COMPARE(reads, 3);
        QTRY_VERIFY(!fixture.controller.error().isEmpty());
        QVERIFY(!fixture.controller.messages().first().toMap().value("read").toBool());
        for (int i = 0; i < 200; ++i) fixture.controller.markMessagesReadUpTo(messageId);
        QCOMPARE(reads, 3);
    }

    void readMarkerRejectsStaleRouteAndWrongOwnerResponses()
    {
        Fixture fixture;
        fixture.extraTopic = true;
        auto row = message(); row["read"] = false;
        fixture.messages = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) { auto response = row; response["read"] = true; return Response{response, 200, 100, {}}; }
            auto response = row;
            if (QUrlQuery(request.target).queryItemValue("topic_uuid") == secondTopicId) response["topic_uuid"] = secondTopicId;
            return Response{QJsonArray{response}};
        };
        QVERIFY(fixture.start());
        fixture.controller.markMessagesReadUpTo(messageId);
        fixture.controller.selectTopic(secondTopicId);
        QTRY_VERIFY(!fixture.controller.busy());
        // New route contains the same synthetic ID to expose a missing route fence.
        QTRY_COMPARE(fixture.server.findChildren<QTcpSocket *>().size(), 0);
        QVERIFY(!fixture.controller.messages().first().toMap().value("read").toBool());
        fixture.messages = [&](const Request &request) {
            auto response = row; response["topic_uuid"] = secondTopicId;
            if (request.target.path().endsWith("/actions/read_up_to/invoke")) { response["read"] = true; response["user_uuid"] = streamId; return Response{response}; }
            return Response{QJsonArray{response}};
        };
        fixture.controller.markMessagesReadUpTo(messageId);
        QTRY_VERIFY(!fixture.controller.error().isEmpty());
        QVERIFY(!fixture.controller.messages().first().toMap().value("read").toBool());
    }

    void uploadNoticeDoesNotHideDownloadProgressOrSavedDestination()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray bytes("attachment\n");
        QTemporaryDir directory;
        QFile input(directory.filePath("report.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly)); input.write(bytes); input.close();
        fixture.files = [&](const Request &) {
            return Response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId}, {"name", "report.txt"},
                {"content_type", "text/plain"}, {"size_bytes", bytes.size()},
                {"hash", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}}, 201, 0, {}};
        };
        QVERIFY(fixture.start());
        fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QVERIFY(fixture.controller.notice().contains("ready"));
        fixture.files = [&](const Request &request) {
            if (request.target.path().endsWith("/actions/download"))
                return Response{QJsonObject{}}; // Actual body is the two bytes "{}".
            return Response{QJsonObject{{"uuid", returnedId}, {"name", "report.txt"}, {"content_type", "text/plain"},
                {"size_bytes", 2}, {"hash", QString::fromLatin1(QCryptographicHash::hash("{}", QCryptographicHash::Sha256).toHex())}}};
        };
        fixture.controller.downloadFile(returnedId, QUrl::fromLocalFile(directory.filePath("saved.bin")));
        QVERIFY(fixture.controller.notice().contains("Downloading"));
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QVERIFY2(fixture.controller.notice().contains("saved.bin"), qPrintable(fixture.controller.notice()));
        QFile output(directory.filePath("saved.bin")); QVERIFY(output.open(QIODevice::ReadOnly)); QCOMPARE(output.readAll(), "{}");
    }

    void synchronousUploadFailuresDrainWithoutRecursionOrStaleGates()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        QVERIFY(fixture.start());
        fixture.controller.setDraft("Still sendable");
        QVariantList selection;
        for (int i = 0; i < 2048; ++i) selection.append(QUrl::fromLocalFile("/nonexistent/cassi-native-attachment"));
        const auto before = fixture.server.requests.size();
        fixture.controller.attachFiles(selection);
        QVERIFY(!fixture.controller.fileBusy());
        QVERIFY(fixture.controller.canSend());
        QCOMPARE(fixture.controller.draft(), "Still sendable");
        QCOMPARE(fixture.server.requests.size(), before);
    }

    void attachmentBatchQueuesAllFilesWithAtMostFiveActiveRequests()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray bytes("Queued attachment\n");
        QTemporaryDir directory;
        QVariantList selection;
        for (int i = 0; i < 11; ++i) {
            const auto folder = directory.filePath(QString::number(i));
            QVERIFY(QDir().mkpath(folder));
            QFile input(folder + "/report.txt");
            QVERIFY(input.open(QIODevice::WriteOnly));
            QCOMPARE(input.write(bytes), bytes.size());
            selection.append(QUrl::fromLocalFile(input.fileName()));
        }
        int active = 0, maximumActive = 0, started = 0;
        fixture.files = [&](const Request &) {
            maximumActive = std::max(maximumActive, ++active);
            ++started;
            QTimer::singleShot(120, &fixture.server, [&] { --active; });
            return Response{QJsonObject{{"uuid", historyMessage(started).value("uuid")}, {"stream_uuid", streamId},
                {"name", "report.txt"}, {"content_type", "text/plain"}, {"size_bytes", bytes.size()},
                {"hash", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}}, 201, 120, {}};
        };
        QVERIFY(fixture.start());
        fixture.controller.setDraft("Message");
        fixture.controller.attachFiles(selection);
        QVERIFY(!fixture.controller.canSend());
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QCOMPARE(started, 11);
        QCOMPARE(maximumActive, 5);
        QCOMPARE(fixture.controller.draft().count("urn:file:"), 11);
        QVERIFY(fixture.controller.canSend());
    }

    void destroyingFileServiceClearsQueuedAndActiveControllerState()
    {
        Fixture fixture;
        auto files = std::make_unique<FileClient>(&fixture.api);
        fixture.controller.useFiles(files.get());
        QTemporaryDir directory;
        QFile input(directory.filePath("report.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly)); input.write("attachment\n"); input.close();
        int started = 0;
        fixture.files = [&](const Request &) { ++started; return Response{QJsonObject{}, 500, 300, {}}; };
        QVERIFY(fixture.start());
        fixture.controller.setDraft("Original draft");
        QVariantList selection;
        for (int i = 0; i < 9; ++i) selection.append(QUrl::fromLocalFile(input.fileName()));
        fixture.controller.attachFiles(selection);
        QTRY_VERIFY(started >= 5);
        files.reset();
        QVERIFY(!fixture.controller.fileBusy());
        QVERIFY(fixture.controller.canSend());
        QVERIFY(!fixture.controller.canAttach());
        QCOMPARE(fixture.controller.draft(), "Original draft");
        const auto completed = started;
        QTRY_COMPARE(fixture.server.findChildren<QTcpSocket *>().size(), 0);
        QCOMPARE(started, completed);
        QVERIFY(!fixture.controller.notice().contains("Uploading"));
    }

    void replacingFileServiceCancelsOnlyControllerOperations()
    {
        Fixture fixture;
        FileClient oldService(&fixture.api), newService(&fixture.api);
        fixture.controller.useFiles(&oldService);
        const QByteArray bytes("attachment\n");
        QTemporaryDir directory;
        QFile input(directory.filePath("report.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly)); input.write(bytes); input.close();
        int started = 0;
        fixture.files = [&](const Request &) {
            ++started;
            return Response{QJsonObject{{"uuid", historyMessage(started).value("uuid")}, {"stream_uuid", streamId},
                {"name", "report.txt"}, {"content_type", "text/plain"}, {"size_bytes", bytes.size()},
                {"hash", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}}, 201, 150, {}};
        };
        QVERIFY(fixture.start());
        int foreignCompletions = 0;
        bool foreignOk = false;
        oldService.upload(input.fileName(), streamId, {}, [&](const FileResult &result) { ++foreignCompletions; foreignOk = result.ok(); });
        fixture.controller.setDraft("Original draft");
        fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QTRY_COMPARE(started, 2);
        fixture.controller.useFiles(&newService);
        QVERIFY(!fixture.controller.fileBusy());
        QVERIFY(fixture.controller.canSend());
        fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QTRY_COMPARE(foreignCompletions, 1);
        QVERIFY(foreignOk);
        QCOMPARE(fixture.controller.draft().count("urn:file:"), 1);
        QCOMPARE(started, 3);
    }

    void uploadedImageParagraphBoundariesAndLegacyFileVideo_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("initialDraft");
        QTest::addColumn<QString>("typedAfterFirst");
        QTest::addColumn<bool>("secondImage");
        QTest::addColumn<QString>("expectedPattern");
        QTest::newRow("image-empty") << QString("image") << QString() << QString() << false << QString("@FIRST@\n\n");
        QTest::newRow("image-caption") << QString("image") << QString("Caption") << QString() << false << QString("Caption\n\n@FIRST@\n\n");
        QTest::newRow("image-trailing-one") << QString("image") << QString("Caption\n") << QString() << false << QString("Caption\n\n@FIRST@\n\n");
        QTest::newRow("image-trailing-two") << QString("image") << QString("Caption\n\n") << QString() << false << QString("Caption\n\n@FIRST@\n\n");
        QTest::newRow("image-subsequent-caption") << QString("image") << QString() << QString("Later caption") << false << QString("@FIRST@\n\nLater caption");
        QTest::newRow("image-subsequent-image") << QString("image") << QString() << QString() << true << QString("@FIRST@\n\n@SECOND@\n\n");
        QTest::newRow("image-subsequent-caption-and-image") << QString("image") << QString() << QString("Later caption") << true << QString("@FIRST@\n\nLater caption\n\n@SECOND@\n\n");
        QTest::newRow("file-legacy-two-newlines") << QString("file") << QString("Caption\n\n") << QString() << false << QString("Caption\n\n@FIRST@\n");
        QTest::newRow("video-legacy-one-newline") << QString("video") << QString("Caption") << QString() << false << QString("Caption\n@FIRST@\n");
    }

    void uploadedImageParagraphBoundariesAndLegacyFileVideo()
    {
        QFETCH(QString, kind); QFETCH(QString, initialDraft); QFETCH(QString, typedAfterFirst);
        QFETCH(bool, secondImage); QFETCH(QString, expectedPattern);
        // Same public PNG as the actual Main received/upload test. The MP4
        // sample is a synthetic MIME signature only, not playback acceptance.
        const QByteArray png = QByteArray::fromHex("89504e470d0a1a0a0000000d4948445200000060000000400806000000e534720e000000ad4944415478daedd1310d80301440c19a68d28d818da4766a017d4c18a8061430b023004474f81d6e7806dea5be5ddf48f75243cb470b6df45f02000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000260058f727b4f72ca1010000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001300fc64110b8532ff49270000000049454e44ae426082");
        const QByteArray bytes = kind == "image" ? png : kind == "video"
            ? QByteArray::fromHex("000000186674797069736f6d0000000069736f6d6d703432") : QByteArray("plain fixture\n");
        const QString name = kind == "image" ? "image.png" : kind == "video" ? "video.mp4" : "report.txt";
        const QString mime = kind == "image" ? "image/png" : kind == "video" ? "video/mp4" : "text/plain";
        const QString firstId = returnedId, secondId = "99999999-9999-4999-8999-999999999994";
        const auto linkFor = [&](const QString &id) {
            const auto prefix = kind == "file" ? QString{} : QString("!");
            return prefix + '[' + name + "](<urn:" + kind + ':' + id + "?name=" + name
                + "&content_type=" + QString::fromLatin1(QUrl::toPercentEncoding(mime))
                + "&size=" + QString::number(bytes.size()) + ">)";
        };
        const auto expected = expectedPattern.replace("@FIRST@", linkFor(firstId)).replace("@SECOND@", linkFor(secondId));
        int uploads = 0, unexpected = 0;
        bool headersOkay = true;
        Fixture fixture;
        FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QFile input(directory.filePath(name)); QVERIFY(input.open(QIODevice::WriteOnly));
        QCOMPARE(input.write(bytes), qint64(bytes.size())); input.close();
        if (kind == "image") {
            QCOMPARE(bytes.size(), qsizetype(230));
            QCOMPARE(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(),
                QByteArray("9fb39a2b2737f88bdf0b383c4fcea4b0f3636e584b280f10afd18444739ed1f9"));
        }
        fixture.files = [&](const Request &request) -> Response {
            if (request.method != "POST" || request.target.path() != "/api/workspace/v1/messenger/files/") {
                ++unexpected; return Response{QJsonObject{}, 405};
            }
            ++uploads;
            headersOkay = headersOkay
                && request.headers.value("authorization") == QByteArray("Bearer synthetic-project-token")
                && request.headers.value("cookie").isEmpty()
                && request.headers.value("content-type").startsWith("multipart/form-data;")
                && request.headers.value("content-length").toLongLong() > bytes.size();
            return Response{QJsonObject{{"uuid", uploads == 1 ? firstId : secondId}, {"stream_uuid", streamId},
                {"name", name}, {"content_type", mime}, {"size_bytes", bytes.size()},
                {"hash", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}}, 201};
        };
        QVERIFY(fixture.start());
        QCOMPARE(fixture.controller.selectedStream(), streamId); QCOMPARE(fixture.controller.selectedTopic(), topicId);
        QVERIFY(fixture.controller.canAttach());
        fixture.controller.setDraft(initialDraft);
        fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QVERIFY(fixture.controller.fileBusy()); QVERIFY(!fixture.controller.canSend());
        QTRY_COMPARE(uploads, 1); QTRY_VERIFY(!fixture.controller.fileBusy());
        QVERIFY2(fixture.controller.error().isEmpty(), qPrintable(fixture.controller.error()));
        QVERIFY(headersOkay); QCOMPARE(unexpected, 0);
        QVERIFY(fixture.controller.draft().contains(linkFor(firstId)));
        // Public draft input, after a real first upload. No completion signal,
        // private state or fabricated upload result is injected.
        if (!typedAfterFirst.isEmpty()) fixture.controller.setDraft(fixture.controller.draft() + typedAfterFirst);
        if (secondImage) {
            fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
            QVERIFY(fixture.controller.fileBusy()); QVERIFY(!fixture.controller.canSend());
            QTRY_COMPARE(uploads, 2); QTRY_VERIFY(!fixture.controller.fileBusy());
            QVERIFY2(fixture.controller.error().isEmpty(), qPrintable(fixture.controller.error()));
            QVERIFY(fixture.controller.draft().contains(linkFor(secondId)));
        }
        QCOMPARE(uploads, secondImage ? 2 : 1); QVERIFY(headersOkay); QCOMPARE(unexpected, 0);
        QCOMPARE(fixture.controller.selectedStream(), streamId); QCOMPARE(fixture.controller.selectedTopic(), topicId);
        QVERIFY(fixture.controller.canSend());
        qInfo().noquote() << "IMAGE_UPLOAD_BOUNDARY" << QJsonDocument(QJsonObject{
            {"row", QString::fromLatin1(QTest::currentDataTag())}, {"uploads", uploads},
            {"headersOkay", headersOkay}, {"actualDraft", fixture.controller.draft()}, {"expectedDraft", expected}
        }).toJson(QJsonDocument::Compact);
        // Image rows discriminate occupied-side blank paragraphs. File/video
        // rows assert their old serialized bytes, including the video ! prefix.
        QCOMPARE(fixture.controller.draft(), expected);
        NativeMarkdown markdown;
        const auto parts = markdown.render(fixture.controller.draft()).value("parts").toList();
        QStringList imageIds;
        for (const auto &part : parts)
            if (part.toMap().value("kind") == "image") imageIds.append(part.toMap().value("id").toString());
        QCOMPARE(imageIds, kind == "image" ? (secondImage ? QStringList{firstId, secondId} : QStringList{firstId}) : QStringList{});
        QTRY_VERIFY(fixture.api.findChildren<QNetworkReply *>().isEmpty());
    }

    void inlineEditSessionKeepsTheStoredDraftAndClosesOnAConfirmedSave()
    {
        QTemporaryDir draftDirectory;
        NativeDraftService draftStorage(draftDirectory.path()); NativeDrafts drafts(&draftStorage);
        Fixture fixture; fixture.controller.useDrafts(&drafts);
        auto older = message("First message", messageId);
        auto newer = message("Second message", returnedId);
        newer["created_at"] = "2026-01-02T10:00:01.000Z";
        int edits = 0, sends = 0;
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "GET" && path.endsWith("/messages/"))
                return {QJsonArray{older, newer}};
            if (request.method == "PUT") { ++edits; newer["payload"] = request.body.value("payload"); return {newer}; }
            if (request.method == "POST") { ++sends; return {message("racing send", returnedId), 201}; }
            return {path.endsWith(messageId) ? older : newer};
        };
        QVERIFY(fixture.start());
        QTRY_VERIFY(drafts.ready(streamId, topicId));
        fixture.controller.setDraft("half-written note");
        QTRY_COMPARE(drafts.text(streamId, topicId), QString("half-written note"));

        // A Source edit session loads the message body into the composer and never touches
        // the stored draft (message-composer-draft.hook.ts never calls onValueChange while editing).
        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        QCOMPARE(fixture.controller.editingMessageId(), returnedId);
        QCOMPARE(fixture.controller.draft(), QString("Second message"));
        QVERIFY(fixture.controller.canSaveEdit());
        QVERIFY(!fixture.controller.canSend());
        QCOMPARE(drafts.text(streamId, topicId), QString("half-written note"));

        // Typing inside the session rewrites only the edit buffer.
        fixture.controller.setDraft("Second message, edited");
        QCOMPARE(fixture.controller.draft(), QString("Second message, edited"));
        QTest::qWait(200);
        QCOMPARE(drafts.text(streamId, topicId), QString("half-written note"));

        // A blank buffer cannot be saved (reference: send disabled when value.trim() is empty).
        fixture.controller.setDraft("   ");
        QVERIFY(!fixture.controller.canSaveEdit());
        fixture.controller.submitEditMessage();
        QTest::qWait(200);
        QCOMPARE(edits, 0);
        QVERIFY(fixture.controller.editingMessageId() == returnedId);
        fixture.controller.setDraft("Second message, edited");

        // Sending a new message while a session is open is refused.
        fixture.controller.send();
        QTest::qWait(200);
        QCOMPARE(sends, 0);

        // A confirmed server result closes the session and the stored draft returns.
        fixture.controller.submitEditMessage();
        QTRY_COMPARE(edits, 1);
        QTRY_COMPARE(modelMessage(fixture.controller, returnedId).value("content").toString(),
                     QString("Second message, edited"));
        QTRY_VERIFY(fixture.controller.editingMessageId().isEmpty());
        QCOMPARE(fixture.controller.draft(), QString("half-written note"));
        QCOMPARE(drafts.text(streamId, topicId), QString("half-written note"));
        QVERIFY(!fixture.controller.canSaveEdit());
        QVERIFY(fixture.controller.canSend());
    }

    void inlineEditCancelFailureAndRouteChangePreserveTheStoredDraft()
    {
        QTemporaryDir draftDirectory;
        NativeDraftService draftStorage(draftDirectory.path()); NativeDrafts drafts(&draftStorage);
        Fixture fixture; fixture.controller.useDrafts(&drafts); fixture.extraTopic = true;
        auto older = message("First message", messageId);
        auto newer = message("Second message", returnedId);
        newer["created_at"] = "2026-01-02T10:00:01.000Z";
        int edits = 0; bool failing = false;
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "GET" && path.endsWith("/messages/"))
                return {QUrlQuery(request.target).queryItemValue("topic_uuid") == topicId
                    ? QJsonArray{older, newer} : QJsonArray{}};
            if (request.method == "PUT") {
                if (failing) return {QJsonObject{{"detail", "nope"}}, 500};
                ++edits; newer["payload"] = request.body.value("payload"); return {newer};
            }
            return {path.endsWith(messageId) ? older : newer};
        };
        QVERIFY(fixture.start());
        QTRY_VERIFY(drafts.ready(streamId, topicId));
        fixture.controller.setDraft("work in progress");
        QTRY_COMPARE(drafts.text(streamId, topicId), QString("work in progress"));

        // Cancelling restores the pre-edit draft and stores nothing.
        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        fixture.controller.setDraft("Second message, half typed");
        fixture.controller.cancelEditMessage();
        QVERIFY(fixture.controller.editingMessageId().isEmpty());
        QCOMPARE(fixture.controller.draft(), QString("work in progress"));
        QCOMPARE(drafts.text(streamId, topicId), QString("work in progress"));
        QCOMPARE(edits, 0);

        // A failed save keeps the session and the typed text open with an error.
        failing = true;
        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        fixture.controller.setDraft("Second message, retry soon");
        fixture.controller.submitEditMessage();
        QTRY_VERIFY(!fixture.controller.error().isEmpty());
        QCOMPARE(fixture.controller.editingMessageId(), returnedId);
        QCOMPARE(fixture.controller.draft(), QString("Second message, retry soon"));
        fixture.controller.cancelEditMessage();
        QCOMPARE(fixture.controller.draft(), QString("work in progress"));

        // Leaving the conversation ends the session (reference clears the edit session on route change).
        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        fixture.controller.selectTopic(secondTopicId);
        QVERIFY(fixture.controller.editingMessageId().isEmpty());
        QVERIFY(!fixture.controller.canSaveEdit());

        // Up-to-edit-last picks the newest own markdown message.
        fixture.controller.selectTopic(topicId);
        QTRY_VERIFY(!fixture.controller.busy());
        fixture.controller.editLastMessage();
        QCOMPARE(fixture.controller.editingMessageId(), returnedId);
        fixture.controller.cancelEditMessage();

        // A message that cannot be edited reports instead of starting a session.
        fixture.controller.beginEditMessage("00000000-0000-4000-8000-000000000000");
        QVERIFY(fixture.controller.editingMessageId().isEmpty());
        QVERIFY(!fixture.controller.error().isEmpty());
        const QString refusal = fixture.controller.error();
        fixture.controller.selectTopic(secondTopicId);
        QTRY_VERIFY(!fixture.controller.busy());
        fixture.controller.editLastMessage();
        QVERIFY(fixture.controller.editingMessageId().isEmpty());
        QCOMPARE(fixture.controller.error(), refusal);
    }

    // Issue #115: an in-flight edit save belongs to the exact session and buffer revision it
    // submitted. Text typed after Save must survive the completion of the earlier save, and
    // while one save is in flight a second PUT for the same message is refused (the save
    // control is disabled), so concurrent PUTs for one message can never conflict.
    void inlineEditSaveCompletionCannotDiscardTextTypedAfterSave()
    {
        QTemporaryDir draftDirectory;
        NativeDraftService draftStorage(draftDirectory.path()); NativeDrafts drafts(&draftStorage);
        Fixture fixture; fixture.controller.useDrafts(&drafts);
        auto older = message("First message", messageId);
        auto newer = message("Second message", returnedId);
        newer["created_at"] = "2026-01-02T10:00:01.000Z";
        std::function<void()> releaseSave;
        int edits = 0; bool holdFirstPut = true;
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "GET" && path.endsWith("/messages/"))
                return {QJsonArray{older, newer}};
            if (request.method == "PUT") {
                ++edits;
                newer["payload"] = request.body.value("payload");
                Response response{newer};
                if (holdFirstPut) {
                    holdFirstPut = false;
                    response.defer = [&](std::function<void()> send) { releaseSave = std::move(send); };
                }
                return response;
            }
            return {path.endsWith(messageId) ? older : newer};
        };
        QVERIFY(fixture.start());
        QTRY_VERIFY(drafts.ready(streamId, topicId));
        fixture.controller.setDraft("half-written note");
        QTRY_COMPARE(drafts.text(streamId, topicId), QString("half-written note"));

        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        fixture.controller.setDraft("Second message, saved A");
        fixture.controller.submitEditMessage();
        QTRY_VERIFY(static_cast<bool>(releaseSave));

        // Text entered after Save must stay in the buffer while the save is pending, and
        // a second save of the same message cannot be submitted before the first lands.
        fixture.controller.setDraft("Second message, typed B");
        QCOMPARE(fixture.controller.draft(), QString("Second message, typed B"));
        QCOMPARE(fixture.controller.editingMessageId(), returnedId);
        QVERIFY(!fixture.controller.canSaveEdit());

        // Completing the A save verifies body A but must not close the session or clear B:
        // the completion belongs to an older buffer revision of the session.
        releaseSave(); releaseSave = {};
        QTRY_COMPARE(modelMessage(fixture.controller, returnedId).value("content").toString(),
                     QString("Second message, saved A"));
        QCOMPARE(fixture.controller.editingMessageId(), returnedId);
        QCOMPARE(fixture.controller.draft(), QString("Second message, typed B"));
        QCOMPARE(drafts.text(streamId, topicId), QString("half-written note"));
        QTRY_VERIFY(fixture.controller.canSaveEdit());

        // B is savable now; its own confirmation closes the session and the ordinary draft returns.
        fixture.controller.submitEditMessage();
        QTRY_COMPARE(edits, 2);
        QTRY_COMPARE(modelMessage(fixture.controller, returnedId).value("content").toString(),
                     QString("Second message, typed B"));
        QTRY_VERIFY(fixture.controller.editingMessageId().isEmpty());
        QCOMPARE(fixture.controller.draft(), QString("half-written note"));
        QCOMPARE(drafts.text(streamId, topicId), QString("half-written note"));
    }

    // Issue #115 trigger 3b: cancel and reopen the same message while the earlier save is
    // still in flight. The completion belongs to the closed session and must not touch the
    // reopened one.
    void inlineEditSaveCompletionCannotCloseAReopenedSession()
    {
        QTemporaryDir draftDirectory;
        NativeDraftService draftStorage(draftDirectory.path()); NativeDrafts drafts(&draftStorage);
        Fixture fixture; fixture.controller.useDrafts(&drafts);
        auto older = message("First message", messageId);
        auto newer = message("Second message", returnedId);
        newer["created_at"] = "2026-01-02T10:00:01.000Z";
        std::function<void()> releaseSave;
        int edits = 0; bool holdFirstPut = true;
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "GET" && path.endsWith("/messages/"))
                return {QJsonArray{older, newer}};
            if (request.method == "PUT") {
                ++edits;
                newer["payload"] = request.body.value("payload");
                Response response{newer};
                if (holdFirstPut) {
                    holdFirstPut = false;
                    response.defer = [&](std::function<void()> send) { releaseSave = std::move(send); };
                }
                return response;
            }
            return {path.endsWith(messageId) ? older : newer};
        };
        QVERIFY(fixture.start());
        QTRY_VERIFY(drafts.ready(streamId, topicId));
        fixture.controller.setDraft("ordinary draft");
        QTRY_COMPARE(drafts.text(streamId, topicId), QString("ordinary draft"));

        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        fixture.controller.setDraft("Second message, saved A");
        fixture.controller.submitEditMessage();
        QTRY_VERIFY(static_cast<bool>(releaseSave));

        fixture.controller.cancelEditMessage();
        QVERIFY(fixture.controller.editingMessageId().isEmpty());
        QCOMPARE(fixture.controller.draft(), QString("ordinary draft"));

        // Reopen the same UUID while the earlier PUT is still in flight.
        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        fixture.controller.setDraft("Second message, typed B");

        releaseSave(); releaseSave = {};
        QTRY_COMPARE(modelMessage(fixture.controller, returnedId).value("content").toString(),
                     QString("Second message, saved A"));
        // The reopened session survives the old completion untouched.
        QCOMPARE(fixture.controller.editingMessageId(), returnedId);
        QCOMPARE(fixture.controller.draft(), QString("Second message, typed B"));
        QTRY_VERIFY(fixture.controller.canSaveEdit());

        fixture.controller.cancelEditMessage();
        QCOMPARE(fixture.controller.draft(), QString("ordinary draft"));
        QCOMPARE(drafts.text(streamId, topicId), QString("ordinary draft"));
        QCOMPARE(edits, 1);
    }

    // Issue #115: repeated saves of one session cannot run concurrently; a second submit
    // while the first PUT is pending is refused outright.
    void inlineEditSecondSaveIsRefusedWhileFirstIsPending()
    {
        QTemporaryDir draftDirectory;
        NativeDraftService draftStorage(draftDirectory.path()); NativeDrafts drafts(&draftStorage);
        Fixture fixture; fixture.controller.useDrafts(&drafts);
        auto older = message("First message", messageId);
        auto newer = message("Second message", returnedId);
        newer["created_at"] = "2026-01-02T10:00:01.000Z";
        std::function<void()> releaseSave;
        int edits = 0;
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "GET" && path.endsWith("/messages/"))
                return {QJsonArray{older, newer}};
            if (request.method == "PUT") {
                ++edits;
                newer["payload"] = request.body.value("payload");
                Response response{newer};
                response.defer = [&](std::function<void()> send) { releaseSave = std::move(send); };
                return response;
            }
            return {path.endsWith(messageId) ? older : newer};
        };
        QVERIFY(fixture.start());
        QTRY_VERIFY(drafts.ready(streamId, topicId));

        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        fixture.controller.setDraft("Second message, saved A");
        fixture.controller.submitEditMessage();
        QTRY_VERIFY(static_cast<bool>(releaseSave));
        QCOMPARE(edits, 1);

        // Same buffer, second submit: refused, no second PUT goes out.
        QVERIFY(!fixture.controller.canSaveEdit());
        fixture.controller.submitEditMessage();
        QTest::qWait(250);
        QCOMPARE(edits, 1);

        releaseSave(); releaseSave = {};
        QTRY_COMPARE(modelMessage(fixture.controller, returnedId).value("content").toString(),
                     QString("Second message, saved A"));
        // The buffer was never changed, so this completion does close the session.
        QTRY_VERIFY(fixture.controller.editingMessageId().isEmpty());
        QCOMPARE(edits, 1);
    }

    // The in-flight guard feeds the canSaveEdit property, so every transition of the guard
    // must notify editSessionChanged (the property's NOTIFY signal), otherwise a QML save
    // control bound to canSaveEdit keeps a stale enabled state after the save settles.
    void inlineEditSaveInFlightTransitionsNotifyTheSaveControl()
    {
        QTemporaryDir draftDirectory;
        NativeDraftService draftStorage(draftDirectory.path()); NativeDrafts drafts(&draftStorage);
        Fixture fixture; fixture.controller.useDrafts(&drafts);
        auto older = message("First message", messageId);
        auto newer = message("Second message", returnedId);
        newer["created_at"] = "2026-01-02T10:00:01.000Z";
        std::function<void()> releaseSave;
        bool holdFirstPut = true;
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "GET" && path.endsWith("/messages/"))
                return {QJsonArray{older, newer}};
            if (request.method == "PUT") {
                newer["payload"] = request.body.value("payload");
                Response response{newer};
                if (holdFirstPut) {
                    holdFirstPut = false;
                    response.defer = [&](std::function<void()> send) { releaseSave = std::move(send); };
                }
                return response;
            }
            return {path.endsWith(messageId) ? older : newer};
        };
        QVERIFY(fixture.start());
        QTRY_VERIFY(drafts.ready(streamId, topicId));

        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        fixture.controller.setDraft("Second message, saved A");
        QSignalSpy notified(&fixture.controller, &NativeController::editSessionChanged);
        QVERIFY(notified.isValid());

        // Taking the save in flight flips canSaveEdit to false, so it must notify.
        fixture.controller.submitEditMessage();
        QTRY_VERIFY(static_cast<bool>(releaseSave));
        QVERIFY(!fixture.controller.canSaveEdit());
        QVERIFY2(notified.count() >= 1, "in-flight insertion must emit editSessionChanged");

        // Type after Save so the completion lands on a newer revision and the session
        // stays open; clearing the guard flips canSaveEdit back and must notify again.
        fixture.controller.setDraft("Second message, typed B");
        notified.clear();
        releaseSave(); releaseSave = {};
        QTRY_VERIFY(fixture.controller.canSaveEdit());
        QVERIFY2(notified.count() >= 1, "in-flight completion must emit editSessionChanged");
    }

    // Reference contract (workspace_ui 61dacef): features/workspace-reply
    // serializes a reply as "[Name](urn:quote:<uuid>)" + blank line + answer
    // (workspace-reply.lib.ts:190-212); the composer preview header is
    // "Reply: Name" with an 80-character summary body
    // (message-composer-reply-quote-preview.ui.tsx, QUOTE_PREVIEW_MAX=80 in
    // message-composer-constants.lib.ts:3).

    void replySessionComposesTheQuoteLineOnAConfirmedSend()
    {
        Fixture fixture;
        const QString peerId = "9a9a9a9a-9a9a-4a9a-8a9a-9a9a9a9a9a9a";
        fixture.users = [&](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", peerId}, {"username", "bobby"}, {"first_name", "Bobby"}}}}; };
        auto note = message("Peer note\nsecond line", messageId);
        note["is_own"] = false; note["author_uuid"] = peerId;
        QJsonObject posted; int posts = 0;
        const auto savedContent = [&]{ return posted.value("payload").toObject().value("content").toString(); };
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "POST") {
                ++posts; posted = request.body;
                auto saved = message(savedContent(), returnedId);
                return {saved, 201};
            }
            if (request.method == "GET" && path.endsWith("/messages/")) return {QJsonArray{note}};
            return {message(savedContent(), returnedId)};
        };
        QVERIFY(fixture.start());
        fixture.controller.setDraft("carry-over answer");

        QVERIFY(fixture.controller.beginReply(messageId));
        QCOMPARE(fixture.controller.replyMessageId(), messageId);
        QTRY_COMPARE(fixture.controller.replyAuthor(), QString("Bobby"));
        QCOMPARE(fixture.controller.replyExcerpt(), QString("Peer note second line"));
        // The answer must not be blank: canSend stays gated on the draft
        // (reference disables send when the composer value is empty).
        fixture.controller.setDraft("");
        QVERIFY(!fixture.controller.canSend());
        fixture.controller.send();
        QTest::qWait(200);
        QCOMPARE(posts, 0);

        // Starting a reply keeps whatever the composer already holds as the answer.
        fixture.controller.setDraft("my answer");
        QVERIFY(fixture.controller.canSend());
        fixture.controller.send();
        QTRY_COMPARE(posts, 1);
        QCOMPARE(savedContent(), QString("[Bobby](urn:quote:%1)\n\nmy answer").arg(messageId));
        QTRY_VERIFY(fixture.controller.replyMessageId().isEmpty());
        QTRY_VERIFY(fixture.controller.draft().isEmpty());

        // The echoed message comes back with the quote frame in its row map.
        const auto row = modelMessage(fixture.controller, returnedId);
        QCOMPARE(row.value("quoteId").toString(), messageId);
        QCOMPARE(row.value("displayContent").toString(), QString("my answer"));
        // A leading anchor carries its author in the link text: the frame renders
        // ready without any extra fetch (docs/NATIVE_DESKTOP_REPLIES.md).
        QCOMPARE(row.value("quoteAuthorName").toString(), QString("Bobby"));
        QCOMPARE(row.value("quoteStatus").toString(), QString("ready"));
    }

    // An inline (non-leading) urn:quote anchor stays in the body text for the
    // markdown renderer, and openMessageInChat resolves the origin for the click.
    void replyAnchorInsideTheBodyStaysRenderableAndNavigable()
    {
        Fixture fixture;
        const QString peerId = "9a9a9a9a-9a9a-4a9a-8a9a-9a9a9a9a9a9a";
        fixture.users = [&](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", peerId}, {"username", "bobby"}, {"first_name", "Bobby"}}}}; };
        auto note = message("Peer note", messageId);
        note["is_own"] = false; note["author_uuid"] = peerId;
        QJsonObject posted; int posts = 0; int lookups = 0;
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "POST") {
                ++posts; posted = request.body;
                return {message(posted.value("payload").toObject().value("content").toString(), returnedId), 201};
            }
            if (request.method == "GET" && path.endsWith("/messages/")) {
                ++lookups;
                return {QJsonArray{note}};
            }
            return {message(posted.value("payload").toObject().value("content").toString(), returnedId)};
        };
        QVERIFY(fixture.start());
        fixture.controller.setDraft("note [Bobby](urn:quote:" + messageId + ") above");
        fixture.controller.send();
        QTRY_COMPARE(posts, 1);
        QVariantMap row;
        // The echo lands asynchronously; wait for the row instead of racing it.
        QTRY_VERIFY_WITH_TIMEOUT(([&] { row = modelMessage(fixture.controller, returnedId);
            return !row.isEmpty(); })(), 5000);
        QCOMPARE(row.value("quoteId").toString(), QString());
        QVERIFY(row.value("displayContent").toString().contains(QLatin1String("(urn:quote:")));
        // Clicking the anchor routes to the origin: the shipped message-link
        // machinery resolves it and arms the highlight flash on the row.
        fixture.controller.openMessageInChat(messageId);
        QTRY_COMPARE(fixture.controller.anchorMessageId(), messageId);
    }

    // A transport failure while resolving a quote must not freeze the preview:
    // the reference loader keeps retriable failures out of the resolved store
    // (messenger-quote-loader.lib.ts distinguishes stale from unavailable).
    void transientQuoteLookupFailureRecoversAfterTheNetworkReturns()
    {
        Fixture fixture;
        fixture.aggregateMessages = true; // route every history GET to this handler (quote lookups included)
        const QString peerId = "9a9a9a9a-9a9a-4a9a-8a9a-9a9a9a9a9a9a";
        fixture.users = [&](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", peerId}, {"username", "bobby"}, {"first_name", "Bobby"}}}}; };
        const QString farId = "4b4b4b4b-4b4b-4b4b-8b4b-4b4b4b4b4b4b";
        const auto reply = message(QStringLiteral("[Bobby](urn:quote:%1)\n\nmy answer").arg(farId), returnedId);
        const auto far = message("far origin", farId);
        int lookups = 0; bool recover = false;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method != "GET") return {QJsonArray{reply}};
            if (QUrlQuery(request.target).hasQueryItem("uuid")) {
                ++lookups;
                if (!recover) return {QJsonObject{}, 500};
                return {QJsonArray{far}};
            }
            return {QJsonArray{reply}};
        };
        QVERIFY(fixture.start());
        QVariantMap row;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            row = modelMessage(fixture.controller, returnedId);
            return lookups >= 1 && !row.isEmpty();
        })(), 5000);
        QTest::qWait(300);
        row = modelMessage(fixture.controller, returnedId);
        QVERIFY2(row.value("quoteStatus").toString() != QLatin1String("unavailable"),
                 qPrintable(row.value("quoteStatus").toString()));
        recover = true;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            row = modelMessage(fixture.controller, returnedId);
            return row.value("quoteStatus").toString() == QLatin1String("ready");
        })(), 9000);
        QCOMPARE(row.value("quoteContent").toString(), QString("far origin"));
    }

    // A confirmed edit of the origin must flow into every quote preview that
    // references it, even when the origin lives in another conversation
    // (reference keeps quote content inside the shared message store).
    void quotePreviewFollowsAConfirmedEditOfTheOrigin()
    {
        Fixture fixture;
        fixture.extraTopic = true;
        fixture.aggregateMessages = true; // route quote/verification GETs to this handler
        const QString farId = "4b4b4b4b-4b4b-4b4b-8b4b-4b4b4b4b4b4b";
        const auto reply = message(QStringLiteral("[Bobby](urn:quote:%1)\n\nmy answer").arg(farId), returnedId);
        QString originContent = "far origin"; int puts = 0;
        const auto fresh = [&] { auto m = message(originContent, farId, secondTopicId); return m; };
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            const auto query = QUrlQuery(request.target);
            if (request.method == "PUT") {
                ++puts; originContent = request.body.value("payload").toObject().value("content").toString();
                return {QJsonObject{}, 200};
            }
            if (path.endsWith("/" + farId)) return {fresh()};
            if (query.hasQueryItem("uuid")) return {QJsonArray{fresh()}};
            if (query.queryItemValue("topic_uuid") == secondTopicId) return {QJsonArray{fresh()}};
            return {QJsonArray{reply}};
        };
        QVERIFY(fixture.start());
        QVariantMap row;
        QTRY_VERIFY_WITH_TIMEOUT(([&] { row = modelMessage(fixture.controller, returnedId);
            return row.value("quoteStatus").toString() == QLatin1String("ready"); })(), 5000);
        QCOMPARE(row.value("quoteContent").toString(), QString("far origin"));

        fixture.controller.selectTopic(secondTopicId);
        QTRY_VERIFY(!modelMessage(fixture.controller, farId).isEmpty());
        QVERIFY(fixture.controller.beginEditMessage(farId));
        fixture.controller.setDraft("edited origin");
        fixture.controller.submitEditMessage();
        QTRY_COMPARE(puts, 1);
        // The session closes only after the server confirms the mutation; wait for
        // that before leaving the conversation so the verification is not cancelled.
        QTRY_VERIFY(fixture.controller.editingMessageId().isEmpty());

        fixture.controller.selectTopic(topicId);
        QTRY_VERIFY_WITH_TIMEOUT(([&] { row = modelMessage(fixture.controller, returnedId);
            return row.value("quoteContent").toString() == QLatin1String("edited origin"); })(), 5000);
    }

    // After a transport failure the retry is self-driving: the lookup re-issues
    // even though the test performs no further projections (review #119, round 2).
    void failedQuoteLookupRetriesWithoutFurtherProjections()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const QString farId = "4b4b4b4b-4b4b-4b4b-8b4b-4b4b4b4b4b4b";
        const auto reply = message(QStringLiteral("[Bobby](urn:quote:%1)\n\nmy answer").arg(farId), returnedId);
        const auto far = message("far origin", farId);
        int lookups = 0; bool recover = false;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method != "GET") return {QJsonArray{reply}};
            if (QUrlQuery(request.target).hasQueryItem("uuid")) {
                ++lookups;
                if (!recover) return {QJsonObject{}, 500};
                return {QJsonArray{far}};
            }
            return {QJsonArray{reply}};
        };
        QVERIFY(fixture.start());
        QVariantMap row;
        QTRY_VERIFY_WITH_TIMEOUT(([&] { row = modelMessage(fixture.controller, returnedId);
            return lookups >= 1 && !row.isEmpty(); })(), 5000);
        // No further projection happens here; only a self-scheduled timer can make
        // the lookup count grow.
        recover = true;
        const int baseline = lookups;
        QTest::qWait(6000);
        QVERIFY2(lookups > baseline, "the failed lookup never retried by itself");
        QTRY_VERIFY_WITH_TIMEOUT(([&] { row = modelMessage(fixture.controller, returnedId);
            return row.value("quoteStatus").toString() == QLatin1String("ready"); })(), 9000);
    }

    // A confirmed deletion of the origin drops the cached preview; the frame then
    // re-resolves against the server and lands on unavailable (review #119).
    void quotePreviewBecomesUnavailableAfterTheOriginIsDeleted()
    {
        Fixture fixture;
        fixture.extraTopic = true;
        fixture.aggregateMessages = true;
        const QString farId = "4b4b4b4b-4b4b-4b4b-8b4b-4b4b4b4b4b4b";
        const auto reply = message(QStringLiteral("[Bobby](urn:quote:%1)\n\nmy answer").arg(farId), returnedId);
        bool deleted = false; int deletes = 0;
        const auto fresh = [&] { return message("far origin", farId, secondTopicId); };
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            const auto query = QUrlQuery(request.target);
            if (request.method == "DELETE") { deleted = true; ++deletes; return {QJsonObject{}, 200}; }
            if (request.method != "GET") return {QJsonArray{reply}};
            if (path.endsWith("/" + farId)) return deleted ? Response{QJsonObject{}, 404} : Response{fresh()};
            if (query.hasQueryItem("uuid")) return deleted ? Response{QJsonArray{}} : Response{QJsonArray{fresh()}};
            if (query.queryItemValue("topic_uuid") == secondTopicId)
                return deleted ? Response{QJsonArray{}} : Response{QJsonArray{fresh()}};
            return {QJsonArray{reply}};
        };
        QVERIFY(fixture.start());
        QVariantMap row;
        QTRY_VERIFY_WITH_TIMEOUT(([&] { row = modelMessage(fixture.controller, returnedId);
            return row.value("quoteStatus").toString() == QLatin1String("ready"); })(), 5000);

        fixture.controller.selectTopic(secondTopicId);
        QTRY_VERIFY(!modelMessage(fixture.controller, farId).isEmpty());
        fixture.controller.deleteMessage(farId);
        QTRY_COMPARE(deletes, 1);

        fixture.controller.selectTopic(topicId);
        QTRY_VERIFY_WITH_TIMEOUT(([&] { row = modelMessage(fixture.controller, returnedId);
            return row.value("quoteStatus").toString() == QLatin1String("unavailable"); })(), 5000);
    }

    // A message consisting of only a quote anchor keeps an empty answer body;
    // the frame replaces the anchor instead of the body duplicating it.
    void quoteOnlyMessageKeepsAnEmptyAnswerBody()
    {
        Fixture fixture;
        auto quoteOnly = message(QStringLiteral("[Aela](urn:quote:%1)").arg(messageId), returnedId);
        quoteOnly["is_own"] = false;
        fixture.messages = [&](const Request &request) -> Response {
            if (request.method != "GET") return {QJsonArray{}};
            return {QJsonArray{message("Peer note", messageId), quoteOnly}};
        };
        QVERIFY(fixture.start());
        const auto row = modelMessage(fixture.controller, returnedId);
        QCOMPARE(row.value("quoteId").toString(), messageId);
        QVERIFY(row.contains("displayContent"));
        QCOMPARE(row.value("displayContent").toString(), QString());
    }

    void replyCancelFailureAndRouteChangePreserveTheAnswer()
    {
        Fixture fixture; fixture.extraTopic = true;
        auto note = message("Peer note", messageId);
        note["is_own"] = false;
        auto ownNote = message("Own note", returnedId);
        ownNote["created_at"] = "2026-01-02T10:00:01.000Z";
        bool failing = false; int posts = 0;
        QString postedContent;
        fixture.messages = [&](const Request &request) -> Response {
            const auto path = request.target.path();
            if (request.method == "POST") {
                if (failing) return {QJsonObject{{"detail", "nope"}}, 500};
                ++posts;
                postedContent = request.body.value("payload").toObject().value("content").toString();
                return {message(postedContent, returnedId), 201};
            }
            if (request.method == "GET" && path.endsWith("/messages/"))
                return {QUrlQuery(request.target).queryItemValue("topic_uuid") == topicId
                    ? QJsonArray{note, ownNote} : QJsonArray{}};
            return {message(postedContent, returnedId)};
        };
        QVERIFY(fixture.start());

        // Cancel closes the session and leaves the typed answer alone.
        QVERIFY(fixture.controller.beginReply(messageId));
        fixture.controller.setDraft("half typed");
        fixture.controller.cancelReply();
        QVERIFY(fixture.controller.replyMessageId().isEmpty());
        QCOMPARE(fixture.controller.draft(), QString("half typed"));

        // A failed send keeps both the session and the answer (edit-session precedent).
        failing = true;
        QVERIFY(fixture.controller.beginReply(messageId));
        fixture.controller.send();
        QTRY_VERIFY(!fixture.controller.error().isEmpty());
        QCOMPARE(fixture.controller.replyMessageId(), messageId);
        QCOMPARE(fixture.controller.draft(), QString("half typed"));
        QCOMPARE(posts, 0);
        failing = false;
        fixture.controller.send();
        QTRY_COMPARE(posts, 1);
        QTRY_VERIFY(fixture.controller.replyMessageId().isEmpty());

        // Leaving the conversation ends the session (reply slot is conversation-scoped).
        QVERIFY(fixture.controller.beginReply(messageId));
        fixture.controller.selectTopic(secondTopicId);
        QVERIFY(fixture.controller.replyMessageId().isEmpty());
        QTRY_VERIFY(!fixture.controller.busy());

        // A message that cannot be quoted reports instead of starting a session.
        fixture.controller.selectTopic(topicId);
        QTRY_VERIFY(!fixture.controller.busy());
        QVERIFY(!fixture.controller.beginReply("00000000-0000-4000-8000-000000000000"));
        QVERIFY(fixture.controller.replyMessageId().isEmpty());
        QVERIFY(!fixture.controller.error().isEmpty());

        // Reply and edit sessions are mutually exclusive.
        QVERIFY(fixture.controller.beginReply(messageId));
        QVERIFY(fixture.controller.beginEditMessage(returnedId));
        QVERIFY(fixture.controller.replyMessageId().isEmpty());
        QCOMPARE(fixture.controller.editingMessageId(), returnedId);
        QVERIFY(fixture.controller.beginReply(messageId));
        QVERIFY(fixture.controller.editingMessageId().isEmpty());
        QCOMPARE(fixture.controller.replyMessageId(), messageId);
    }

    void quoteReferenceRowsResolveLocallyAndFetchAbsentOrigins()
    {
        Fixture fixture; fixture.aggregateMessages = true;
        const QString peerId = "9a9a9a9a-9a9a-4a9a-8a9a-9a9a9a9a9a9a";
        fixture.users = [&](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", peerId}, {"username", "bobby"}, {"first_name", "Bobby"}}}}; };
        const QString absent = "2b2b2b2b-2b2b-4b2b-8b2b-2b2b2b2b2b2b";
        const QString gone = "3c3c3c3c-3c3c-4c3c-8c3c-3c3c3c3c3c3c";
        const QString farId = "4d4d4d4d-4d4d-4d4d-8d4d-4d4d4d4d4d4d";
        auto origin = message("Origin text", messageId);
        origin["is_own"] = false; origin["author_uuid"] = peerId;
        auto replied = message(QString("[Bobby](urn:quote:%1)\n\nanswer here").arg(messageId), returnedId);
        auto quotedRemote = message(QString("[Far](urn:quote:%1)\n\nremote answer").arg(absent), farId);
        auto quotedGone = message(QString("[Gone](urn:quote:%1?text=kept%20selection)\n\nlate").arg(gone),
                                  "5e5e5e5e-5e5e-4e5e-8e5e-5e5e5e5e5e5e");
        int absentLookups = 0;
        fixture.messages = [&](const Request &request) -> Response {
            const auto query = QUrlQuery(request.target);
            if (query.hasQueryItem("uuid")) {
                if (query.queryItemValue("uuid") == absent) {
                    ++absentLookups;
                    return Response{QJsonArray{message("Far body", absent, secondTopicId)}};
                }
                return Response{QJsonArray{}};
            }
            return Response{QJsonArray{origin, replied, quotedRemote, quotedGone}};
        };
        QVERIFY(fixture.start());

        // Origin inside the loaded window resolves synchronously.
        const auto row = modelMessage(fixture.controller, returnedId);
        QCOMPARE(row.value("quoteId").toString(), messageId);
        QCOMPARE(row.value("quoteLabel").toString(), QString("Bobby"));
        QVERIFY(row.value("quoteText").toString().isEmpty());
        QCOMPARE(row.value("quoteStatus").toString(), QString("ready"));
        QCOMPARE(row.value("quoteAuthorName").toString(), QString("Bobby"));
        QCOMPARE(row.value("quoteContent").toString(), QString("Origin text"));
        QCOMPARE(row.value("displayContent").toString(), QString("answer here"));

        // An origin outside the window fetches GET /messages/?uuid= exactly once.
        auto remote = modelMessage(fixture.controller, farId);
        QCOMPARE(remote.value("quoteStatus").toString(), QString("loading"));
        QTRY_COMPARE(modelMessage(fixture.controller, farId).value("quoteStatus").toString(), QString("ready"));
        QCOMPARE(modelMessage(fixture.controller, farId).value("quoteContent").toString(), QString("Far body"));
        QCOMPARE(absentLookups, 1);

        // A missing origin becomes the reference's "Message unavailable" state;
        // the ?text= selection survives decoding.
        QTRY_COMPARE(modelMessage(fixture.controller, "5e5e5e5e-5e5e-4e5e-8e5e-5e5e5e5e5e5e")
            .value("quoteStatus").toString(), QString("unavailable"));
        QCOMPARE(modelMessage(fixture.controller, "5e5e5e5e-5e5e-4e5e-8e5e-5e5e5e5e5e5e")
            .value("quoteText").toString(), QString("kept selection"));
    }

    void openMessageInChatAnchorsLoadedMessagesAndReportsUnknownOnes()
    {
        Fixture fixture; fixture.aggregateMessages = true;
        auto note = message("Peer note", messageId);
        note["is_own"] = false;
        fixture.messages = [&](const Request &) -> Response { return Response{QJsonArray{note}}; };
        QVERIFY(fixture.start());
        fixture.controller.openMessageInChat(messageId);
        QTRY_COMPARE(fixture.controller.anchorMessageId(), messageId);
        fixture.controller.openMessageInChat("6f6f6f6f-6f6f-4f6f-8f6f-6f6f6f6f6f6f");
        QTRY_VERIFY(fixture.controller.anchorMessageId().isEmpty());
        QVERIFY(!fixture.controller.notice().isEmpty());
    }

    void attachmentUploadKeepsItsConversationAndEscapesMarkdown()
    {
        QTemporaryDir draftDirectory;
        NativeDraftService draftStorage(draftDirectory.path()); NativeDrafts drafts(&draftStorage);
        Fixture fixture;
        fixture.controller.useDrafts(&drafts);
        fixture.extraTopic = true;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray bytes("Hello attachment\n");
        QTemporaryDir directory;
        QFile input(directory.filePath("report[one](two).txt"));
        QVERIFY(input.open(QIODevice::WriteOnly));
        QCOMPARE(input.write(bytes), bytes.size());
        input.close();
        fixture.files = [&](const Request &) {
            return Response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId},
                {"name", input.fileName().section('/', -1)}, {"content_type", "text/plain"},
                {"size_bytes", bytes.size()}, {"hash", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}}, 200, 200, {}};
        };
        QVERIFY(fixture.start());
        QTRY_VERIFY(drafts.ready(streamId, topicId));
        fixture.controller.setDraft("Initial text");
        QVERIFY(fixture.controller.canAttach());
        fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QVERIFY(fixture.controller.fileBusy());
        QVERIFY(!fixture.controller.canSend());
        fixture.controller.selectTopic(secondTopicId);
        fixture.controller.setDraft("Second draft");
        QTRY_VERIFY(fixture.controller.canSend());
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QCOMPARE(fixture.controller.draft(), "Second draft");
        fixture.controller.selectTopic(topicId);
        QVERIFY(fixture.controller.draft().startsWith("Initial text\n[report\\[one\\](two).txt](<urn:file:"));
        NativeMarkdown markdown;
        const auto rendered = markdown.render(fixture.controller.draft());
        QVERIFY(rendered.value("html").toString().contains("urn:file:" + returnedId));
        QVERIFY(rendered.value("plain").toString().contains("report[one](two).txt"));
        QVERIFY(fixture.controller.canSend());
        bool done = false; drafts.flush(this, [&](bool okay) { done = okay; }); QTRY_VERIFY(done);
        draftStorage.shutdown(); QTRY_VERIFY(draftStorage.stopped());
        NativeDraftStore store(draftDirectory.path()); QCOMPARE(store.open(drafts.owner()), NativeDraftService::Failure::None);
        QVERIFY(store.latest(streamId, topicId).record->text.contains("urn:file:" + returnedId));
    }

    void lateOrdinaryUploadDoesNotReplaceAnEncryptedMemoryDraft()
    {
        QTemporaryDir directory; NativeDraftService storage(directory.filePath("drafts")); NativeDrafts drafts(&storage);
        Fixture fixture; fixture.controller.useDrafts(&drafts); FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        QFile input(directory.filePath("attachment.txt")); QVERIFY(input.open(QIODevice::WriteOnly)); input.write("fixture"); input.close();
        std::function<void()> complete;
        const auto release = qScopeGuard([&] { if (complete) complete(); });
        fixture.files = [&](const Request &) {
            Response response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId}, {"name", "attachment.txt"},
                {"content_type", "text/plain"}, {"size_bytes", 7},
                {"hash", QString::fromLatin1(QCryptographicHash::hash("fixture", QCryptographicHash::Sha256).toHex())}}};
            response.defer = [&](auto send) { complete = std::move(send); }; return response;
        };
        QVERIFY(fixture.start()); QTRY_VERIFY(drafts.ready(streamId, topicId));
        fixture.controller.setDraft("Ordinary input"); fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QTRY_VERIFY(bool(complete)); fixture.encrypted = true; fixture.controller.refresh();
        QTRY_VERIFY(!fixture.controller.busy());
        QVERIFY(fixture.controller.streams().first().toMap().value("encrypted").toBool()
            || fixture.controller.streams().first().toMap().value("encryption").toBool());
        fixture.controller.setDraft("Encrypted memory input"); complete(); complete = {};
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QVERIFY(fixture.controller.draft().startsWith("Encrypted memory input\n[attachment.txt]"));
        bool done = false; drafts.flush(this, [&](bool okay) { done = okay; }); QTRY_VERIFY(done);
        storage.shutdown(); QTRY_VERIFY(storage.stopped());
        NativeDraftStore store(directory.filePath("drafts")); QCOMPARE(store.open(drafts.owner()), NativeDraftService::Failure::None);
        const auto prior = store.latest(streamId, topicId); QVERIFY(prior.record);
        QCOMPARE(prior.record->text, "Ordinary input");
    }

    void logoutClearsPendingAttachmentUiAndLateDrafts()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray bytes("Hello attachment\n");
        QTemporaryDir directory;
        QFile input(directory.filePath("report.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly));
        input.write(bytes);
        input.close();
        fixture.files = [&](const Request &) {
            return Response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId}, {"name", "report.txt"},
                {"content_type", "text/plain"}, {"size_bytes", bytes.size()},
                {"hash", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}}, 200, 350, {}};
        };
        QVERIFY(fixture.start());
        fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QVERIFY(fixture.controller.fileBusy());
        fixture.controller.logout();
        QVERIFY(!fixture.controller.fileBusy());
        QVERIFY(fixture.controller.notice().isEmpty());
        QTest::qWait(400);
        QVERIFY(fixture.controller.draft().isEmpty());
        QVERIFY(!fixture.controller.canAttach());
    }

    void attachmentDownloadUsesVerifiedFileClientAndLocalDestination()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        QTemporaryDir directory;
        int fileRequests = 0;
        fixture.files = [&](const Request &request) {
            ++fileRequests;
            if (request.target.path().endsWith("/actions/download"))
                return Response{QJsonObject{}}; // The mock server serializes exactly two bytes: {}.
            return Response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId}, {"name", "download.bin"},
                {"content_type", "application/octet-stream"}, {"size_bytes", 2},
                {"hash", QString::fromLatin1(QCryptographicHash::hash("{}", QCryptographicHash::Sha256).toHex())}}};
        };
        QVERIFY(fixture.start());
        fixture.controller.downloadFile(returnedId, QUrl("https://example.invalid/destination"));
        QCOMPARE(fileRequests, 0);
        const auto destination = directory.filePath("saved.bin");
        fixture.controller.downloadFile(returnedId, QUrl::fromLocalFile(destination));
        QTRY_VERIFY(!fixture.controller.fileBusy());
        QVERIFY(fixture.controller.error().isEmpty());
        QCOMPARE(fileRequests, 2);
        QFile output(destination);
        QVERIFY(output.open(QIODevice::ReadOnly));
        QCOMPARE(output.readAll(), "{}");
        QVERIFY(fixture.controller.notice().contains("saved.bin"));
    }

    void encryptedConversationNeverUploadsPlaintextAttachment()
    {
        Fixture fixture;
        fixture.encrypted = true;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        int uploads = 0;
        fixture.files = [&](const Request &) { ++uploads; return Response{}; };
        QVERIFY(fixture.start());
        QVERIFY(!fixture.controller.canAttach());
        fixture.controller.attachFiles({QUrl::fromLocalFile("/tmp/irrelevant.txt")});
        QCOMPARE(uploads, 0);
        QVERIFY(!fixture.controller.fileBusy());
    }

    void sessionRenewalOnlyCancelsTransfersWhenCredentialGenerationChanges_data()
    {
        QTest::addColumn<bool>("rotate");
        QTest::newRow("same-token") << false;
        QTest::newRow("rotated-token") << true;
    }

    void sessionRenewalOnlyCancelsTransfersWhenCredentialGenerationChanges()
    {
        QFETCH(bool, rotate);
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        int refreshes = 0;
        fixture.authentication = [&](const Request &request) {
            auto response = sessionResponse(request);
            if (rotate && request.body.value("grant_type") == "refresh_token") {
                auto token = response.data.toObject();
                token.insert("access_token", QString("synthetic-rotated-%1").arg(++refreshes));
                response.data = token;
            }
            return response;
        };
        const QByteArray bytes("Hello attachment\n");
        QTemporaryDir directory;
        QFile input(directory.filePath("report.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly));
        input.write(bytes);
        input.close();
        fixture.files = [&](const Request &) {
            return Response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId}, {"name", "report.txt"},
                {"content_type", "text/plain"}, {"size_bytes", bytes.size()},
                {"hash", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}}, 200, 450, {}};
        };
        QVERIFY(fixture.start());
        fixture.controller.setDraft("Keep my text");
        fixture.controller.attachFiles({QUrl::fromLocalFile(input.fileName())});
        QVERIFY(fixture.controller.fileBusy());
        QSignalSpy renewed(&fixture.session, &Session::authenticated);
        fixture.session.requestRefresh();
        QTRY_COMPARE(renewed.size(), 1);
        if (!rotate)
            QVERIFY(fixture.controller.fileBusy());
        QTRY_VERIFY(!fixture.controller.fileBusy());
        if (rotate) {
            QCOMPARE(fixture.controller.draft(), "Keep my text");
            QVERIFY(fixture.controller.notice().contains("interrupted"));
            QTest::qWait(500);
            QCOMPARE(fixture.controller.draft(), "Keep my text");
        } else {
            QVERIFY(fixture.controller.draft().contains("urn:file:"));
            QVERIFY(fixture.controller.notice().contains("ready"));
        }
    }

    void snapshotBarrierWaitsForSelectedHistoryAndUsers()
    {
        Fixture fixture;
        fixture.messages = [](const Request &) { return Response{QJsonArray{message()}, 200, 250, {}}; };
        fixture.users = [](const Request &) { return Response{QJsonArray{}, 200, 100, {}}; };
        QVERIFY(fixture.start());
        QSignalSpy ready(&fixture.controller, &NativeController::snapshotReloaded);
        fixture.controller.reloadSnapshot();
        QVERIFY(fixture.controller.busy());
        QTest::qWait(150);
        QCOMPARE(ready.size(), 0);
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(!fixture.controller.busy());
    }

    void failedSnapshotCannotAcknowledgeRealtimeBaseline()
    {
        Fixture fixture;
        bool fail = false;
        fixture.messages = [&fail](const Request &) {
            return fail ? Response{QJsonObject{}, 403, 0, {}} : Response{QJsonArray{message()}};
        };
        QVERIFY(fixture.start());
        QSignalSpy ready(&fixture.controller, &NativeController::snapshotReloaded);
        QSignalSpy failed(&fixture.controller, &NativeController::snapshotFailed);
        fail = true;
        fixture.controller.reloadSnapshot();
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(ready.size(), 0);
    }

    void realtimeMessagesAreOwnerScopedAndFenceOldSnapshots()
    {
        Fixture fixture;
        fixture.messages = [](const Request &) { return Response{QJsonArray{message()}}; };
        QVERIFY(fixture.start());
        auto body = message("Live message", returnedId);
        body.insert("kind", "message.created");
        QJsonObject event{{"uuid", secondTopicId}, {"project_id", projectId}, {"user_uuid", userId},
                          {"payload", body}};
        QVERIFY(fixture.controller.applyRealtimeEvent(event));
        QCOMPARE(fixture.controller.messages().size(), 2);
        event.insert("user_uuid", streamId);
        QVERIFY(!fixture.controller.applyRealtimeEvent(event));
        QCOMPARE(fixture.controller.messages().size(), 2);
        event.insert("user_uuid", userId);
        event.insert("payload", QJsonObject{{"kind", "message.deleted"}, {"uuid", returnedId},
                                            {"stream_uuid", streamId}, {"topic_uuid", topicId}});
        QVERIFY(fixture.controller.applyRealtimeEvent(event));
        QCOMPARE(fixture.controller.messages().size(), 1);
        event.insert("payload", QJsonObject{{"kind", "future.domain.event"}});
        QVERIFY(fixture.controller.applyRealtimeEvent(event));
        event.insert("payload", QJsonObject{{"kind", "message.created"}, {"uuid", "bad"}});
        QVERIFY(!fixture.controller.applyRealtimeEvent(event));
    }

    void refreshReconcilesRemoteDeletionAndNewTopics()
    {
        Fixture fixture;
        bool deleted = false;
        fixture.messages = [&deleted](const Request &) {
            return Response{deleted ? QJsonArray{message()} : QJsonArray{message(), message("Deleted elsewhere", returnedId)}};
        };
        QVERIFY(fixture.start());
        QCOMPARE(fixture.controller.messages().size(), 2);
        fixture.controller.setDraft("Keep this draft");
        deleted = true;
        fixture.extraTopic = true;
        fixture.controller.refresh();
        QTRY_COMPARE(fixture.controller.messages().size(), 1);
        QTRY_COMPARE(fixture.controller.topics().size(), 2);
        QCOMPARE(fixture.controller.selectedTopic(), topicId);
        QCOMPARE(fixture.controller.draft(), "Keep this draft");
    }

    void refreshBridgesMoreThanOnePageOfNewArrivals()
    {
        Fixture fixture;
        int total = 50;
        fixture.messages = [&total](const Request &request) {
            const auto marker = QUrlQuery(request.target).queryItemValue("page_marker");
            int highest = total;
            if (!marker.isEmpty())
                highest = marker.right(12).toInt(nullptr, 16) - 1;
            QJsonArray items;
            for (int n = highest; n > std::max(0, highest - 50); --n)
                items.append(historyMessage(n));
            Response response{items};
            if (highest > 50)
                response.headers.insert("X-Pagination-Marker", historyMessage(highest - 49).value("uuid").toString().toUtf8());
            return response;
        };
        QVERIFY(fixture.start());
        QCOMPARE(fixture.controller.messages().size(), 50);
        total = 110;
        fixture.controller.refresh();
        QTRY_COMPARE(fixture.controller.messages().size(), 110);
        for (int n = 1; n <= 110; ++n)
            QCOMPARE(fixture.controller.messages().at(n - 1).toMap().value("id").toString(),
                     historyMessage(n).value("uuid").toString());
    }

    void verifiesMutationOutsideLatestPage()
    {
        Fixture fixture;
        auto old = historyMessage(1);
        fixture.messages = [&old](const Request &request) {
            const auto path = request.target.path();
            if (path.endsWith("/actions/star/invoke")) {
                old.insert("starred", true);
                return Response{old};
            }
            if (path.endsWith("/messages/" + old.value("uuid").toString()))
                return Response{old};
            const auto marker = QUrlQuery(request.target).queryItemValue("page_marker");
            const int highest = marker.isEmpty() ? 100 : 50;
            QJsonArray items;
            for (int n = highest; n > highest - 50; --n)
                items.append(n == 1 ? old : historyMessage(n));
            Response response{items};
            if (marker.isEmpty())
                response.headers.insert("X-Pagination-Marker", historyMessage(51).value("uuid").toString().toUtf8());
            return response;
        };
        QVERIFY(fixture.start());
        QCOMPARE(fixture.controller.messages().size(), 50);
        fixture.controller.loadOlder();
        QTRY_COMPARE(fixture.controller.messages().size(), 100);
        fixture.controller.starMessage(old.value("uuid").toString());
        QTRY_VERIFY(fixture.controller.messages().at(0).toMap().value("starred").toBool());
        QCOMPARE(fixture.controller.messages().size(), 100);
    }

    void uncertainSendKeepsUuidAcrossAnotherConversation()
    {
        Fixture fixture;
        fixture.extraTopic = true;
        QVector<QString> firstTopicUuids;
        QHash<QString, QJsonObject> saved;
        fixture.messages = [&firstTopicUuids, &saved](const Request &request) {
            const auto path = request.target.path();
            if (request.method == "POST") {
                const auto topic = request.body.value("topic_uuid").toString();
                const auto id = request.body.value("uuid").toString();
                const auto json = message(request.body.value("payload").toObject().value("content").toString(), id, topic);
                saved.insert(id, json);
                if (topic == topicId) {
                    firstTopicUuids.append(id);
                    if (firstTopicUuids.size() == 1)
                        return Response{QJsonObject{}, 503};
                }
                return Response{json};
            }
            if (!path.endsWith('/'))
                return Response{saved.value(path.section('/', -1))};
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        fixture.controller.setDraft("Uncertain first send");
        fixture.controller.send();
        QTRY_VERIFY(!fixture.controller.busy());
        QVERIFY(!fixture.controller.error().isEmpty());
        fixture.controller.selectTopic(secondTopicId);
        QTRY_VERIFY(!fixture.controller.busy());
        fixture.controller.setDraft("Confirmed second send");
        fixture.controller.send();
        QTRY_VERIFY(fixture.controller.draft().isEmpty());
        fixture.controller.selectTopic(topicId);
        QTRY_VERIFY(!fixture.controller.busy());
        QCOMPARE(fixture.controller.draft(), "Uncertain first send");
        fixture.controller.send();
        QTRY_COMPARE(firstTopicUuids.size(), 2);
        QCOMPARE(firstTopicUuids.at(0), firstTopicUuids.at(1));
        QTRY_VERIFY(fixture.controller.draft().isEmpty());
    }

    void controllerKeepsOnlyOneOrdinaryFallbackUnderDraftPressure()
    {
        QTemporaryDir directory; NativeDraftService service(directory.path()); NativeDrafts drafts(&service);
        Fixture fixture; fixture.controller.useDrafts(&drafts);
        const auto topicFor = [](int i) { return QString("a1000000-0000-4000-8000-%1").arg(i, 12, 16, QLatin1Char('0')); };
        fixture.topics = [&](const Request &) {
            QJsonArray rows{creationTopic(topicId, streamId, "Initial")};
            for (int i = 0; i < 80; ++i) rows.append(creationTopic(topicFor(i), streamId, "Draft " + QString::number(i)));
            return Response{rows};
        };
        QVERIFY(fixture.start());
        for (int i = 0; i < 80; ++i) {
            const auto topic = topicFor(i); fixture.controller.selectTopic(topic);
            QTRY_VERIFY(drafts.ready(streamId, topic));
            const auto body = QString::number(i) + QString(39990, QChar(0x754c));
            fixture.controller.setDraft(body);
            bool done = false; drafts.flush(this, [&](bool okay) { done = okay; }); QTRY_VERIFY(done);
            QCOMPARE(fixture.controller.draft(), body);
        }
        const auto usage = fixture.controller.draftMemory();
        QVERIFY2(usage.textBytes <= 3 * 40000 * qsizetype(sizeof(QChar)), qPrintable(QString::number(usage.textBytes)));
        QVERIFY(usage.entries <= 1);
        QVERIFY(usage.verifiedRoutes <= NativeDrafts::MaximumCachedRoutes + 1);
        fixture.controller.selectTopic(topicFor(0)); QTRY_VERIFY(drafts.ready(streamId, topicFor(0)));
        QCOMPARE(fixture.controller.draft(), QString("0") + QString(39990, QChar(0x754c)));
    }

    void pendingAttachmentKeepsItsCaptionAcrossMetadataEviction()
    {
        QTemporaryDir directory; NativeDraftService storage(directory.filePath("drafts")); NativeDrafts drafts(&storage);
        Fixture fixture; fixture.controller.useDrafts(&drafts); FileClient files(&fixture.api); fixture.controller.useFiles(&files);
        const auto topicFor = [](int i) { return QString("a2000000-0000-4000-8000-%1").arg(i, 12, 16, QLatin1Char('0')); };
        fixture.topics = [&](const Request &) {
            QJsonArray rows{creationTopic(topicId, streamId, "Initial")};
            for (int i = 0; i < 140; ++i) rows.append(creationTopic(topicFor(i), streamId, "Draft " + QString::number(i)));
            return Response{rows};
        };
        QFile file(directory.filePath("fixture.txt")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("fixture"); file.close();
        std::function<void()> complete; const auto release = qScopeGuard([&] { if (complete) complete(); });
        fixture.files = [&](const Request &) {
            Response response{QJsonObject{{"uuid", returnedId}, {"stream_uuid", streamId}, {"name", "fixture.txt"},
                {"content_type", "text/plain"}, {"size_bytes", 7},
                {"hash", QString::fromLatin1(QCryptographicHash::hash("fixture", QCryptographicHash::Sha256).toHex())}}};
            response.defer = [&](auto send) { complete = std::move(send); }; return response;
        };
        QVERIFY(fixture.start()); QTRY_VERIFY(drafts.ready(streamId, topicId));
        fixture.controller.setDraft("Initial caption"); fixture.controller.attachFiles({QUrl::fromLocalFile(file.fileName())});
        QTRY_VERIFY(bool(complete)); fixture.controller.setDraft("Newer caption");
        for (int i = 0; i < 140; ++i) {
            fixture.controller.selectTopic(topicFor(i)); QTRY_VERIFY(drafts.ready(streamId, topicFor(i)));
        }
        QVERIFY(drafts.retained(streamId, topicId)); complete(); complete = {};
        QTRY_VERIFY(!fixture.controller.fileBusy());
        fixture.controller.selectTopic(topicId); QTRY_VERIFY(drafts.ready(streamId, topicId));
        QVERIFY(fixture.controller.draft().startsWith("Newer caption\n[fixture.txt]"));
        bool done = false; drafts.flush(this, [&](bool okay) { done = okay; }); QTRY_VERIFY(done);
    }

    void durableComposerRetainsNewerSameTextWhileSendIsBeingVerified_data()
    {
        QTest::addColumn<bool>("durable");
        QTest::newRow("memory") << false;
        QTest::newRow("disk") << true;
    }

    void durableComposerRetainsNewerSameTextWhileSendIsBeingVerified()
    {
        QFETCH(bool, durable);
        QTemporaryDir directory; NativeDraftService service(directory.path()); NativeDrafts drafts(&service);
        Fixture fixture; fixture.extraTopic = true;
        if (durable) fixture.controller.useDrafts(&drafts);
        QJsonObject saved; std::function<void()> confirm;
        const auto release = qScopeGuard([&] { if (confirm) confirm(); });
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST") {
                saved = message(request.body.value("payload").toObject().value("content").toString(), returnedId);
                return Response{saved};
            }
            if (request.target.path().endsWith(returnedId)) {
                Response response{saved}; response.defer = [&](auto send) { confirm = std::move(send); }; return response;
            }
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start()); if (durable) QTRY_VERIFY(drafts.ready(streamId, topicId));
        fixture.controller.setDraft("A"); fixture.controller.send(); QTRY_VERIFY(bool(confirm));
        fixture.controller.setDraft("B"); fixture.controller.setDraft("A");
        fixture.controller.selectTopic(secondTopicId); fixture.controller.selectTopic(topicId);
        confirm(); confirm = {};
        QTRY_VERIFY(!fixture.controller.busy()); QCOMPARE(fixture.controller.draft(), "A");
        bool done = false; drafts.flush(this, [&](bool okay) { done = okay; }); QTRY_VERIFY(done);
    }

    void durableSendConsumesOnlyAfterCanonicalReadBack()
    {
        QTemporaryDir directory; NativeDraftService service(directory.path()); NativeDrafts drafts(&service);
        Fixture fixture; fixture.controller.useDrafts(&drafts);
        QJsonObject saved; bool allowReadBack = false;
        fixture.messages = [&](const Request &request) {
            if (request.method == "POST") {
                saved = message(request.body.value("payload").toObject().value("content").toString(), returnedId);
                return Response{saved};
            }
            if (request.target.path().endsWith(returnedId)) return Response{allowReadBack ? saved : QJsonObject{}, allowReadBack ? 200 : 503};
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start()); QTRY_VERIFY(drafts.ready(streamId, topicId));
        fixture.controller.setDraft("Confirmed only"); fixture.controller.send();
        QTRY_VERIFY(!fixture.controller.error().isEmpty()); QCOMPARE(fixture.controller.draft(), "Confirmed only");
        allowReadBack = true; fixture.controller.send(); QTRY_VERIFY(fixture.controller.draft().isEmpty());
        bool done = false; drafts.flush(this, [&](bool okay) { done = okay; }); QTRY_VERIFY(done);
        service.shutdown(); QTRY_VERIFY(service.stopped());
        NativeDraftStore store(directory.path()); QCOMPARE(store.open(drafts.owner()), NativeDraftService::Failure::None);
        QVERIFY(!store.latest(streamId, topicId).record);
    }

    void draftStorageFailureDoesNotCancelMessaging()
    {
        QTemporaryDir directory; NativeDraftService service(directory.path()); NativeDrafts drafts(&service);
        Fixture fixture;
        QVERIFY(fixture.start());
        const auto identity = QJsonDocument(QJsonArray{fixture.api.server().toString(QUrl::FullyEncoded), projectId, userId})
            .toJson(QJsonDocument::Compact);
        const auto owner = QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
        NativeDraftStore lock(directory.path()); QCOMPARE(lock.open(owner), NativeDraftService::Failure::None);
        fixture.controller.useDrafts(&drafts);
        QTRY_VERIFY(drafts.failure() != NativeDraftService::Failure::None);
        QVERIFY(fixture.controller.error().isEmpty()); QVERIFY(!fixture.controller.notice().isEmpty());
        fixture.controller.setDraft("Keep input despite a storage failure");
        QTRY_VERIFY(drafts.failure() != NativeDraftService::Failure::None);
        QCOMPARE(fixture.controller.draft(), "Keep input despite a storage failure");
        QVERIFY(fixture.controller.canSend());
        lock.close(); drafts.retry();
        bool done = false; drafts.flush(this, [&](bool okay) { done = okay; }); QTRY_VERIFY(done);
        QTRY_VERIFY(fixture.controller.notice().isEmpty());
    }

    void protectedFragmentsAreNotSentAsOrdinaryPlaintext()
    {
        Fixture fixture; QVERIFY(fixture.start()); fixture.controller.setDraft(":::test-only protected fragment:::");
        QVERIFY(!fixture.controller.canSend()); fixture.controller.send();
        QVERIFY(std::none_of(fixture.server.requests.cbegin(), fixture.server.requests.cend(), [](const Request &r) {
            return r.method == "POST" && r.target.path().contains("/messages/");
        }));
    }

    void durableDraftsFollowTheVerifiedAccountAcrossLogoutAndLogin()
    {
        QTemporaryDir directory; NativeDraftService service(directory.path()); NativeDrafts drafts(&service);
        Fixture fixture; fixture.controller.useDrafts(&drafts);
        const auto anotherUser = secondTopicId;
        bool other = false;
        fixture.authentication = [&](const Request &request) {
            auto response = sessionResponse(request);
            if (other && request.target.path().endsWith("/actions/introspect"))
                response.data = QJsonObject{{"project_id", projectId}, {"user_info", QJsonObject{{"uuid", anotherUser}}}};
            return response;
        };
        QVERIFY(fixture.start()); QTRY_VERIFY(drafts.ready(streamId, topicId));
        const auto firstOwner = drafts.owner();
        fixture.controller.setDraft("First account input");
        fixture.controller.logout(); QVERIFY(fixture.controller.draft().isEmpty()); QVERIFY(drafts.owner().isEmpty());
        other = true; QVERIFY(fixture.start()); QTRY_VERIFY(drafts.ready(streamId, topicId));
        QVERIFY(drafts.owner() != firstOwner); QVERIFY(fixture.controller.draft().isEmpty());
        fixture.controller.setDraft("Second account input");
        fixture.controller.logout(); other = false; QVERIFY(fixture.start());
        QTRY_VERIFY(drafts.ready(streamId, topicId)); QCOMPARE(drafts.owner(), firstOwner);
        QTRY_COMPARE(fixture.controller.draft(), "First account input");
        bool done = false; drafts.flush(this, [&](bool okay) { done = okay; }); QTRY_VERIFY(done);
    }

    void sendConfirmationClearsMatchingDraftAfterRouteRoundTrip()
    {
        Fixture fixture;
        fixture.extraTopic = true;
        auto saved = message();
        fixture.messages = [&saved](const Request &request) {
            if (request.method == "POST") {
                saved = message(request.body.value("payload").toObject().value("content").toString(), returnedId);
                return Response{saved};
            }
            if (request.target.path().endsWith(returnedId))
                return Response{saved, 200, 200};
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        fixture.controller.setDraft("Return to the original conversation");
        fixture.controller.send();
        QTRY_VERIFY(std::any_of(fixture.server.requests.begin(), fixture.server.requests.end(), [](const Request &request) {
            return request.target.path().endsWith(returnedId);
        }));
        fixture.controller.selectTopic(secondTopicId);
        fixture.controller.selectTopic(topicId);
        QTRY_VERIFY(fixture.controller.draft().isEmpty());
        QTRY_COMPARE(fixture.controller.messages().size(), 1);
        QCOMPARE(fixture.controller.messages().first().toMap().value("id").toString(), returnedId);
    }

    void encryptedConversationCannotSendPlaintext()
    {
        Fixture fixture;
        fixture.encrypted = true;
        QVERIFY(fixture.start());
        fixture.controller.setDraft("Must not be sent");
        QVERIFY(!fixture.controller.canSend());
        fixture.controller.send();
        QVERIFY(std::none_of(fixture.server.requests.begin(), fixture.server.requests.end(), [](const Request &request) {
            return request.method == "POST" && request.target.path().contains("/messages/");
        }));
    }

    void refreshReconcilesAllPreviouslyLoadedRows()
    {
        Fixture fixture;
        QMap<int, QJsonObject> rows;
        for (int n = 1; n <= 100; ++n)
            rows.insert(n, historyMessage(n));
        fixture.messages = [&rows](const Request &request) { return historyResponse(rows, request); };
        QVERIFY(fixture.start());
        fixture.controller.loadOlder();
        QTRY_COMPARE(fixture.controller.messages().size(), 100);
        rows.remove(20);
        rows[21].insert("payload", QJsonObject{{"kind", "markdown"}, {"content", "Edited remotely"}});
        fixture.controller.refresh();
        QTRY_COMPARE(fixture.controller.messages().size(), 99);
        QCOMPARE(fixture.controller.messages().at(19).toMap().value("content").toString(), "Edited remotely");
    }

    void refreshReplacesDeletedOlderCursor()
    {
        Fixture fixture;
        QMap<int, QJsonObject> rows;
        for (int n = 1; n <= 100; ++n)
            rows.insert(n, historyMessage(n));
        fixture.messages = [&rows](const Request &request) { return historyResponse(rows, request); };
        QVERIFY(fixture.start());
        rows.remove(51);
        fixture.controller.refresh();
        QTRY_COMPARE(fixture.controller.messages().first().toMap().value("id").toString(),
                     historyMessage(50).value("uuid").toString());
        fixture.controller.loadOlder();
        QTRY_COMPARE(fixture.controller.messages().size(), 99);
        QVERIFY(fixture.controller.error().isEmpty());
    }

    void deletedOlderCursorRecoversWithoutManualRefresh()
    {
        Fixture fixture;
        QMap<int, QJsonObject> rows;
        for (int n = 1; n <= 100; ++n)
            rows.insert(n, historyMessage(n));
        fixture.messages = [&rows](const Request &request) { return historyResponse(rows, request); };
        QVERIFY(fixture.start());
        rows.remove(51);
        fixture.controller.loadOlder();
        QTRY_COMPARE(fixture.controller.messages().size(), 99);
        QVERIFY(fixture.controller.error().isEmpty());
    }

    void missingCursorRecoveryIsBounded()
    {
        Fixture fixture;
        QMap<int, QJsonObject> rows;
        for (int n = 1; n <= 100; ++n)
            rows.insert(n, historyMessage(n));
        int olderRequests = 0;
        fixture.messages = [&rows, &olderRequests](const Request &request) {
            if (!QUrlQuery(request.target).queryItemValue("page_marker").isEmpty()) {
                ++olderRequests;
                return Response{QJsonObject{}, 404, 0, {}};
            }
            return historyResponse(rows, request);
        };
        QVERIFY(fixture.start());
        fixture.controller.loadOlder();
        QTRY_VERIFY(!fixture.controller.error().isEmpty());
        QVERIFY(!fixture.controller.busy());
        QCOMPARE(olderRequests, 2);
        QTest::qWait(100);
        QCOMPARE(olderRequests, 2);
        QCOMPARE(fixture.controller.messages().size(), 50);
    }

    void delayedHistoryCannotUndoVerifiedMutation_data()
    {
        QTest::addColumn<QString>("operation");
        for (const auto *operation : {"send", "edit", "star", "delete"})
            QTest::newRow(operation) << QString::fromLatin1(operation);
    }

    void delayedHistoryCannotUndoVerifiedMutation()
    {
        QFETCH(QString, operation);
        Fixture fixture;
        QJsonObject existing = message("Before");
        QJsonObject sent;
        bool deleted = false;
        bool delayNextSnapshot = false;
        int delayedSnapshots = 0;
        fixture.messages = [&](const Request &request) {
            const auto path = request.target.path();
            if (request.method == "POST" && path.endsWith("/messages/")) {
                sent = message(request.body.value("payload").toObject().value("content").toString(), returnedId);
                return Response{sent};
            }
            if (request.method == "PUT") {
                existing.insert("payload", request.body.value("payload"));
                return Response{existing};
            }
            if (request.method == "DELETE") {
                deleted = true;
                return Response{QJsonObject{}};
            }
            if (path.endsWith("/actions/star/invoke")) {
                existing.insert("starred", true);
                return Response{existing};
            }
            if (path.endsWith("/messages/")) {
                QJsonArray items;
                if (!deleted)
                    items.append(existing);
                if (!sent.isEmpty())
                    items.append(sent);
                Response response{items};
                if (delayNextSnapshot) {
                    response.delay = 350;
                    delayNextSnapshot = false;
                    ++delayedSnapshots;
                }
                return response;
            }
            return Response{path.endsWith(returnedId) ? sent : existing};
        };
        QVERIFY(fixture.start());
        delayNextSnapshot = true;
        fixture.controller.refresh();
        QTRY_COMPARE(delayedSnapshots, 1);
        const auto changed = [&] {
            const auto items = fixture.controller.messages();
            if (operation == "delete")
                return items.isEmpty();
            if (operation == "send")
                return items.size() == 2 && fixture.controller.draft().isEmpty();
            return !items.isEmpty() && (operation == "star"
                ? items.first().toMap().value("starred").toBool()
                : items.first().toMap().value("content").toString() == "After");
        };
        if (operation == "send") {
            fixture.controller.setDraft("Sent during an older snapshot");
            fixture.controller.send();
        } else if (operation == "edit") {
            fixture.controller.editMessage(messageId, "After");
        } else if (operation == "star") {
            fixture.controller.starMessage(messageId);
        } else {
            fixture.controller.deleteMessage(messageId);
        }
        QTRY_VERIFY(changed());
        QTest::qWait(450);
        QTRY_VERIFY(!fixture.controller.busy());
        QVERIFY(changed());
    }

    void delayedUsersNotifyDisplayedMessageAuthors()
    {
        Fixture fixture;
        fixture.messages = [](const Request &) { return Response{QJsonArray{message()}}; };
        fixture.users = [](const Request &) {
            return Response{QJsonArray{QJsonObject{{"uuid", userId}, {"username", "Resolved author"}}}, 200, 700, {}};
        };
        QVERIFY(fixture.start());
        bool authorNotified = false;
        connect(&fixture.controller, &NativeController::messagesChanged, &fixture.controller, [&] {
            const auto rows = fixture.controller.messages();
            if (!rows.isEmpty() && rows.first().toMap().value("author") == "Resolved author")
                authorNotified = true;
        });
        QTRY_VERIFY(authorNotified);
    }

    void liveTopicUpdateSurvivesDelayedTopicSnapshot()
    {
        Fixture fixture;
        QVERIFY(fixture.start());
        int requests = 0;
        fixture.topics = [&requests](const Request &) {
            const bool stale = ++requests == 1;
            return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"name", stale ? "Old topic" : "Live topic"}}}, 200, stale ? 250 : 0, {}};
        };
        fixture.controller.selectStream(streamId);
        QTRY_COMPARE(requests, 1);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated",
            QJsonObject{{"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId}, {"name", "Live topic"}})));
        QCOMPARE(fixture.controller.topics().first().toMap().value("name").toString(), "Live topic");
        QTest::qWait(350);
        QCOMPARE(fixture.controller.topics().first().toMap().value("name").toString(), "Live topic");
    }

    void snapshotBarrierJoinsUsersTopicsAndHistoryAndRejectsFailure()
    {
        Fixture fixture;
        fixture.messages = [](const Request &) { return Response{QJsonArray{message()}, 200, 400, {}}; };
        QVERIFY(fixture.start());
        fixture.controller.setDraft("Keep the draft through snapshot reload");
        fixture.users = [](const Request &) { return Response{QJsonArray{}, 200, 200, {}}; };
        fixture.topics = [](const Request &) {
            return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"name", "Native topic"}}}, 200, 300, {}};
        };
        QSignalSpy loaded(&fixture.controller, &NativeController::snapshotReloaded);
        QSignalSpy failed(&fixture.controller, &NativeController::snapshotFailed);
        fixture.controller.reloadSnapshot();
        QTest::qWait(350);
        QCOMPARE(loaded.size(), 0);
        QTRY_COMPARE(loaded.size(), 1);
        QCOMPARE(failed.size(), 0);
        QCOMPARE(fixture.controller.draft(), "Keep the draft through snapshot reload");
        fixture.users = [](const Request &) { return Response{QJsonObject{}, 503, 0, {}}; };
        fixture.controller.reloadSnapshot();
        QTRY_COMPARE(failed.size(), 1);
        QTest::qWait(450);
        QCOMPARE(loaded.size(), 1);
    }

    void obsoleteSnapshotFailureCannotCancelNewSnapshotBarrier()
    {
        Fixture fixture;
        QVERIFY(fixture.start());
        int userRequests = 0;
        fixture.users = [&userRequests](const Request &) {
            if (++userRequests == 1)
                return Response{QJsonObject{}, 503, 250, {}};
            return Response{QJsonArray{}, 200, 400, {}};
        };
        QSignalSpy loaded(&fixture.controller, &NativeController::snapshotReloaded);
        QSignalSpy failed(&fixture.controller, &NativeController::snapshotFailed);
        fixture.controller.reloadSnapshot();
        QTRY_COMPARE(userRequests, 1);
        fixture.controller.reloadSnapshot();
        QTRY_COMPARE(userRequests, 2);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 1500);
        QCOMPARE(failed.size(), 0);
    }

    void integratedRealtimeAppliesAndAcknowledgesLiveMessage_data()
    {
        QTest::addColumn<bool>("aggregate"); QTest::newRow("topic") << false; QTest::newRow("aggregate") << true;
    }
    void integratedRealtimeAppliesAndAcknowledgesLiveMessage()
    {
        QFETCH(bool, aggregate);
        QTemporaryDir directory; NativeCacheService cache(directory.path(), {}, true);
        Fixture fixture; fixture.aggregateMessages = aggregate; fixture.extraTopic = aggregate;
        fixture.controller.useCache(&cache);
        RealtimeGateway gateway(fixture.server.serverPort());
        QVector<QJsonObject> events;
        fixture.messages = [](const Request &request) {
            return Response{QUrlQuery(request.target).queryItemValue("topic_uuid") == secondTopicId ? QJsonArray{} : QJsonArray{message()}};
        };
        fixture.realtime = [&events](const Request &request) {
            if (request.target.path().endsWith("/epoch/"))
                return Response{QJsonObject{{"epoch_generation", "controller-generation"},
                    {"epoch_version", 10}, {"current_epoch_version", 10}, {"minimum_epoch_version", 1}}};
            QJsonArray rows;
            const auto after = QUrlQuery(request.target).queryItemValue("epoch_version>").toInt();
            for (const auto &event : events)
                if (event.value("epoch_version").toInt() > after)
                    rows.append(event);
            return Response{rows};
        };
        QVERIFY(fixture.start(gateway.origin()));
        if (aggregate) {
            fixture.controller.selectTopic(secondTopicId); QTRY_VERIFY(!fixture.controller.busy()); QVERIFY(flushCache(cache));
            fixture.controller.selectTopic({}); QTRY_COMPARE(fixture.controller.messages().size(), 1); QTRY_VERIFY(!fixture.controller.busy());
        }
        RealtimeClient realtime(&fixture.api, &fixture.session);
        fixture.controller.useRealtime(&realtime);
        QTRY_COMPARE(fixture.controller.syncState(), "live");
        QTRY_COMPARE(gateway.sockets.size(), 1);
        const auto event = controllerEvent("message.created", message("Delivered over the native realtime path", returnedId, aggregate ? secondTopicId : topicId));
        events.append(event);
        gateway.sockets.last()->sendTextMessage(QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact)));
        QTRY_COMPARE(fixture.controller.messages().size(), 2);
        QTRY_COMPARE(realtime.acknowledgedEpoch(), quint64(12));
        QCOMPARE(fixture.controller.messages().last().toMap().value("content").toString(), "Delivered over the native realtime path");
        if (aggregate) {
            const auto savedAggregate = cacheEntry(cache, cache.lease(), {NativeCache::Kind::History, streamId, {}, {}});
            const auto savedTopic = cacheEntry(cache, cache.lease(), {NativeCache::Kind::History, streamId, secondTopicId, {}});
            QVERIFY(savedAggregate && savedTopic); QVERIFY(!savedAggregate->fresh && !savedTopic->fresh);
            QCOMPARE(savedAggregate->rows.size(), 2); QCOMPARE(savedTopic->rows.size(), 1);
            QCOMPARE(savedTopic->rows.first().toObject().value("uuid").toString(), returnedId);
        }
    }

    void sameOwnerRenewalReloadsCancelledRouteRequestsAndPreservesDraft()
    {
        Fixture fixture;
        RealtimeGateway gateway(fixture.server.serverPort());
        int refreshes = 0;
        fixture.authentication = [&refreshes](const Request &request) {
            auto response = sessionResponse(request);
            if (request.body.value("grant_type") == "refresh_token") {
                auto token = response.data.toObject();
                token.insert("access_token", QString("project-token-%1").arg(++refreshes));
                response.data = token;
            }
            return response;
        };
        fixture.realtime = [](const Request &request) {
            if (request.target.path().endsWith("/epoch/"))
                return Response{QJsonObject{{"epoch_generation", "controller-generation"},
                    {"epoch_version", 10}, {"current_epoch_version", 10}, {"minimum_epoch_version", 1}}};
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start(gateway.origin()));
        RealtimeClient realtime(&fixture.api, &fixture.session);
        fixture.controller.useRealtime(&realtime);
        QTRY_COMPARE(fixture.controller.syncState(), "live");
        fixture.controller.setDraft("Unsent before token renewal");
        int topicRequests = 0;
        fixture.topics = [&topicRequests](const Request &) {
            const bool pending = ++topicRequests == 1;
            return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"name", "Recovered topic"}}}, 200, pending ? 500 : 0, {}};
        };
        fixture.controller.selectStream(streamId);
        QTRY_COMPARE(topicRequests, 1);
        fixture.session.requestRefresh();
        QTRY_COMPARE(refreshes, 2);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.controller.topics().size(), 1, 1500);
        QCOMPARE(fixture.controller.topics().first().toMap().value("name").toString(), "Recovered topic");
        fixture.controller.selectTopic(topicId);
        QCOMPARE(fixture.controller.draft(), "Unsent before token renewal");
    }
    // Additive private slots for NativeControllerTest. SOURCE-ONLY / NOT RUN.
    // Reflection keeps the missing-capability RED executable on the old core.
    void historyCommitCountsAcceptedEmptyHistoryBeforeTheModelSignal()
    {
        Fixture f;
        f.aggregateMessages = true;
        f.messages = [](const Request &) { return Response{QJsonArray{}, 200, 1200}; };
        auto &c = f.controller;
        QVERIFY(c.property("historyCommitRevision").isValid());
        QVERIFY(c.property("messageHistoryReady").isValid());
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), qulonglong(0));
        QVERIFY(!c.property("messageHistoryReady").toBool());
        QVector<qulonglong> published;
        const auto connection = connect(&c, &NativeController::messagesChanged, this, [&] {
            if (c.selectedTopic() == topicId && c.property("messageHistoryReady").toBool())
                published.append(c.property("historyCommitRevision").toULongLong());
        });
        const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
        c.login(f.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QVERIFY(QTest::qWaitFor([&] { return c.projects().size() == 1; }, 2000));
        c.openProject(projectId);
        QVERIFY(QTest::qWaitFor([&] { return c.streams().size() == 1; }, 2000));
        c.selectStream(streamId);
        QVERIFY(QTest::qWaitFor([&] { return !c.topics().isEmpty(); }, 2000));
        c.selectTopic(topicId); // Keep the current delayed HTTP history in flight.
        const auto pending = [&]() -> QNetworkReply * {
            for (auto *reply : f.api.findChildren<QNetworkReply *>())
                if (!reply->isFinished() && reply->url().path() == "/api/workspace/v1/messenger/messages/"
                    && QUrlQuery(reply->url()).queryItemValue("topic_uuid") == topicId) return reply;
            return nullptr;
        };
        QTRY_VERIFY(pending()); // Actual requested current history, not a sleep.
        QVERIFY(c.messages().isEmpty());
        QVERIFY(!c.property("messageHistoryReady").toBool());
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), qulonglong(0));
        QTRY_VERIFY(c.property("messageHistoryReady").toBool());
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), qulonglong(1));
        QVERIFY(!published.isEmpty());
        QCOMPARE(published.first(), qulonglong(1)); // Counter visible inside publication.
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), qulonglong(1));
    }

    void historyCommitDoesNotCountLiveRowsOrOlderAdmission()
    {
        Fixture f;
        QMap<int, QJsonObject> rows;
        for (int n = 1; n <= 120; ++n) rows.insert(n, historyMessage(n));
        f.messages = [&](const Request &request) { return historyResponse(rows, request); };
        auto &c = f.controller;
        QVERIFY(c.property("historyCommitRevision").isValid());
        QVERIFY(f.start());
        QTRY_COMPARE(c.messages().size(), 50);
        QTRY_VERIFY(c.property("messageHistoryReady").toBool());
        const auto first = c.property("historyCommitRevision").toULongLong();
        const auto historyGets = f.totalHistoryGets();
        const auto live = historyMessage(121);
        QVERIFY(c.applyRealtimeEvent(controllerEvent("message.created", live)));
        QCOMPARE(c.messages().size(), 51);
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), first);
        QCOMPARE(f.totalHistoryGets(), historyGets);
        QVERIFY(c.property("messageHistoryReady").toBool());
        c.loadOlder();
        // Pagination admission/loading retains the accepted current baseline.
        QVERIFY(c.property("messageHistoryReady").toBool());
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), first);
        QTRY_COMPARE(c.messages().size(), 101);
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), first + 1);
        QCOMPARE(f.totalHistoryGets(), historyGets + 1);
        QVERIFY(c.property("messageHistoryReady").toBool());
    }

    void historyCommitIgnoresTheOldRouteReplyAndRouteClear()
    {
        Fixture f; f.extraTopic = true;
        f.messages = [](const Request &request) {
            const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            return Response{QJsonArray{message("Owned " + topic,
                topic == topicId ? messageId : returnedId, topic)}, 200, topic == topicId ? 1200 : 0};
        };
        auto &c = f.controller;
        QVERIFY(c.property("historyCommitRevision").isValid());
        c.login(f.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QVERIFY(QTest::qWaitFor([&] { return c.projects().size() == 1; }, 2000));
        c.openProject(projectId);
        QVERIFY(QTest::qWaitFor([&] { return c.streams().size() == 1; }, 2000));
        c.selectStream(streamId);
        QVERIFY(QTest::qWaitFor([&] { return !c.topics().isEmpty(); }, 2000));
        c.selectTopic(topicId); // Keep the current delayed HTTP history in flight.
        const auto oldReply = [&]() -> QNetworkReply * {
            for (auto *reply : f.api.findChildren<QNetworkReply *>())
                if (!reply->isFinished() && reply->url().path() == "/api/workspace/v1/messenger/messages/"
                    && QUrlQuery(reply->url()).queryItemValue("topic_uuid") == topicId) return reply;
            return nullptr;
        };
        QTRY_VERIFY(oldReply());
        const auto before = c.property("historyCommitRevision").toULongLong();
        c.selectTopic(secondTopicId);
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), before);
        QVERIFY(!c.property("messageHistoryReady").toBool());
        QTRY_COMPARE(modelMessage(c, returnedId).value("content").toString(), "Owned " + secondTopicId);
        QTRY_VERIFY(c.property("messageHistoryReady").toBool());
        const auto current = c.property("historyCommitRevision").toULongLong();
        QCOMPARE(current, before + 1);
        QTRY_VERIFY(f.api.findChildren<QNetworkReply *>().isEmpty());
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), current);
        QVERIFY(modelMessage(c, messageId).isEmpty());
        c.selectStream({});
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), current);
        QVERIFY(!c.property("messageHistoryReady").toBool());
    }

    void historyCommitCountsWarmCacheAndStaleHydrationWithoutClaimingReadiness()
    {
        QTemporaryDir directory; qint64 now = 1000;
        NativeCacheService cache(directory.path(), {}, true, [&] { return now; });
        Fixture f; f.extraTopic = true; f.controller.useCache(&cache);
        bool fail = false;
        f.messages = [&](const Request &request) {
            if (fail) return Response{QJsonObject{}, 503, 100};
            const auto topic = QUrlQuery(request.target).queryItemValue("topic_uuid");
            return Response{QJsonArray{message(topic, topic == topicId ? messageId : returnedId, topic)}};
        };
        auto &c = f.controller;
        QVERIFY(c.property("historyCommitRevision").isValid());
        QVERIFY(f.start()); QTRY_VERIFY(c.property("messageHistoryReady").toBool());
        QVERIFY(flushCache(cache));
        const auto first = c.property("historyCommitRevision").toULongLong();
        c.selectTopic(secondTopicId); QTRY_VERIFY(c.property("messageHistoryReady").toBool());
        QVERIFY(flushCache(cache));
        const auto gets = f.topicHistoryGets;
        c.selectTopic(topicId); QTRY_VERIFY(c.property("messageHistoryReady").toBool());
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), first + 2);
        QCOMPARE(f.topicHistoryGets, gets); // Actual fresh-cache publication.
        now += 120001; fail = true;
        const auto warm = c.property("historyCommitRevision").toULongLong();
        c.selectTopic(secondTopicId);
        QTRY_COMPARE(modelMessage(c, returnedId).value("content").toString(), secondTopicId);
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), warm + 1);
        QVERIFY(!c.property("messageHistoryReady").toBool());
        QTRY_VERIFY(!c.error().isEmpty());
        QVERIFY(!c.property("messageHistoryReady").toBool());
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), warm + 1);
    }

    void historyCommitRejectsAnOlderSnapshotAfterLiveRevisionAndCountsOnlyReplacement()
    {
        Fixture f;
        auto row = historyMessage(1);
        bool delayed = false;
        f.messages = [&](const Request &) { return Response{QJsonArray{row}, 200, delayed ? 1200 : 0}; };
        auto &c = f.controller;
        QVERIFY(c.property("historyCommitRevision").isValid());
        QVERIFY(f.start()); QTRY_VERIFY(c.property("messageHistoryReady").toBool());
        const auto initial = c.property("historyCommitRevision").toULongLong();
        const auto gets = f.topicHistoryGets;
        delayed = true; c.refresh();
        QTRY_COMPARE(f.topicHistoryGets, gets + 1);
        QPointer<QNetworkReply> old;
        for (auto *reply : f.api.findChildren<QNetworkReply *>())
            if (!reply->isFinished() && reply->url().path() == "/api/workspace/v1/messenger/messages/"
                && QUrlQuery(reply->url()).queryItemValue("topic_uuid") == topicId) old = reply;
        QVERIFY(old);
        row["payload"] = QJsonObject{{"kind", "markdown"}, {"content", "Newer live body"}};
        QVERIFY(c.applyRealtimeEvent(controllerEvent("message.updated", row)));
        QCOMPARE(modelMessage(c, row.value("uuid").toString()).value("content").toString(), QString("Newer live body"));
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), initial);
        delayed = false;
        QTRY_COMPARE(f.topicHistoryGets, gets + 2); // Existing revision fence revalidates.
        QTRY_VERIFY(c.property("messageHistoryReady").toBool());
        QCOMPARE(c.property("historyCommitRevision").toULongLong(), initial + 1);
        QCOMPARE(modelMessage(c, row.value("uuid").toString()).value("content").toString(), QString("Newer live body"));
    }

    void deepLinkCopyProducesTheCanonicalUrl()
    {
        Fixture fixture;
        auto row = message("Link me");
        fixture.messages = [row](const Request &) { return Response{QJsonArray{row}}; };
        QVERIFY(fixture.start());
        const auto origin = fixture.server.origin();
        const QString expected = origin.toString() + "/org/127.0.0.1-"
            + QString::number(origin.port()) + "/project/" + projectId + "/message/" + messageId;
        QCOMPARE(fixture.controller.messageLink(messageId), expected);
        fixture.controller.copyMessageLink(messageId);
        QCOMPARE(QGuiApplication::clipboard()->text(), expected);
        QVERIFY(fixture.controller.messageLink("ffffffff-ffff-4fff-8fff-ffffffffffff").isEmpty());
    }

    void deepLinkOpensTargetTopicAndAnchorsTheMessage()
    {
        Fixture fixture; fixture.extraTopic = true; fixture.aggregateMessages = true;
        const QString target = "77777777-7777-4777-8777-777777777777";
        const auto origin = fixture.server.origin();
        int lookups = 0;
        QString lookupProject, lookupUuid;
        fixture.messages = [&](const Request &request) -> Response {
            const auto query = QUrlQuery(request.target);
            if (query.hasQueryItem("uuid")) {
                lookupProject = query.queryItemValue("project_id");
                lookupUuid = query.queryItemValue("uuid");
                ++lookups;
                return Response{QJsonArray{message("Deep body", target, secondTopicId)}};
            }
            if (request.method == "GET" && query.queryItemValue("topic_uuid") == secondTopicId)
                return Response{QJsonArray{message("Deep body", target, secondTopicId)}};
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        QSignalSpy anchors(&fixture.controller, &NativeController::anchorChanged);
        fixture.controller.openMessageLink("ew://open/org/127.0.0.1-" + QString::number(origin.port())
            + "/project/" + projectId + "/message/" + target);
        QTRY_COMPARE(fixture.controller.selectedTopic(), secondTopicId);
        QTRY_COMPARE(fixture.controller.anchorMessageId(), target);
        QVERIFY(anchors.count() >= 1);
        QCOMPARE(lookups, 1);
        QCOMPARE(lookupProject, projectId);
        QCOMPARE(lookupUuid, target);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.anchorMessageId().isEmpty(), 4500);
    }

    void deepLinkOpensUncataloguedTopicAndAnchorsTheMessage()
    {
        // Issue #55: the target topic lives in another channel and is absent
        // from the currently loaded topic catalog, so openTopic() runs
        // beginTopicOpening(). Its intermediate pre-commit stateChanged
        // emissions must not cancel the pending anchor, and the bounded
        // older-history search must still reach the message.
        Fixture fixture; fixture.aggregateMessages = true;
        const QString otherStream = "39393939-3939-4939-8939-393939393939";
        const QString ghostTopic = "8b8b8b8b-8b8b-4b8b-8b8b-8b8b8b8b8b8b";
        const QString target = "7c7c7c7c-7c7c-4c7c-8c7c-7c7c7c7c7c7c";
        // ghostTopic (and its channel) are absent from the loaded catalogs;
        // both are resolved through their detail GETs while the deep link runs.
        fixture.topics = [&](const Request &request) {
            if (QUrlQuery(request.target).queryItemValue("stream_uuid") == otherStream)
                return Response{QJsonArray{QJsonObject{{"uuid", ghostTopic}, {"project_id", projectId},
                    {"stream_uuid", otherStream}, {"name", "Ghost topic"}}}};
            return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"name", "Native topic"}}}};
        };
        auto targetRow = message("Deep uncatalogued body", target, ghostTopic);
        targetRow.insert("stream_uuid", otherStream);
        const auto fallback = fixture.server.handler;
        fixture.server.handler = [&](const Request &request) -> Response {
            if (request.target.path() == "/api/workspace/v1/messenger/streams/" + otherStream)
                return Response{creationChannel(otherStream, "Second channel")};
            // Delayed topic metadata: the canonical GET lands after beginTopicOpening()
            // has already emitted its own stateChanged signals.
            if (request.target.path() == "/api/workspace/v1/messenger/stream_topics/" + ghostTopic)
                return Response{creationTopic(ghostTopic, otherStream, "Ghost topic"), 200, 250, {}};
            return fallback(request);
        };
        int olderPages = 0;
        fixture.messages = [&](const Request &request) -> Response {
            const auto query = QUrlQuery(request.target);
            if (query.hasQueryItem("uuid")) return Response{QJsonArray{targetRow}};
            if (query.queryItemValue("topic_uuid") == ghostTopic) {
                if (query.hasQueryItem("page_marker")) {
                    ++olderPages;
                    return Response{QJsonArray{targetRow}, 200, 0, {}};
                }
                auto filler = message("Newer ghost row", "7d7d7d7d-7d7d-4d7d-8d7d-7d7d7d7d7d7d", ghostTopic);
                filler.insert("stream_uuid", otherStream);
                return Response{QJsonArray{filler}, 200, 0, {{"X-Pagination-Marker", "older-1"}}};
            }
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        QSignalSpy anchors(&fixture.controller, &NativeController::anchorChanged);
        const auto origin = fixture.server.origin();
        fixture.controller.openMessageLink("ew://open/org/127.0.0.1-" + QString::number(origin.port())
            + "/project/" + projectId + "/message/" + target);
        QTRY_COMPARE(fixture.controller.selectedStream(), otherStream);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.controller.selectedTopic(), ghostTopic, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.controller.anchorMessageId(), target, 5000);
        QVERIFY(anchors.count() >= 1);
        QVERIFY(olderPages >= 1); // The target was only on an older page.
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.anchorMessageId().isEmpty(), 4500);
    }

    void deepLinkForeignOrganizationOrProjectKeepsSelection()
    {
        Fixture fixture; fixture.aggregateMessages = true;
        const auto origin = fixture.server.origin();
        QVERIFY(fixture.start());
        const auto beforeStream = fixture.controller.selectedStream();
        const auto beforeTopic = fixture.controller.selectedTopic();
        fixture.controller.openMessageLink("ew://open/org/somewhere-else/project/" + projectId
            + "/message/" + messageId);
        QTest::qWait(200);
        QCOMPARE(fixture.controller.selectedStream(), beforeStream);
        QCOMPARE(fixture.controller.selectedTopic(), beforeTopic);
        QVERIFY(fixture.controller.anchorMessageId().isEmpty());
        QVERIFY(!fixture.controller.notice().isEmpty());
        fixture.controller.openMessageLink("ew://open/org/127.0.0.1-" + QString::number(origin.port())
            + "/project/9e9e9e9e-9e9e-4e9e-8e9e-9e9e9e9e9e9e/message/" + messageId);
        QTest::qWait(200);
        QCOMPARE(fixture.controller.selectedStream(), beforeStream);
        QCOMPARE(fixture.controller.selectedTopic(), beforeTopic);
        QVERIFY(fixture.controller.anchorMessageId().isEmpty());
        QVERIFY(!fixture.controller.notice().isEmpty());
    }

    void deepLinkMissingMessageLandsSafely()
    {
        Fixture fixture; fixture.aggregateMessages = true;
        fixture.messages = [](const Request &request) {
            const auto query = QUrlQuery(request.target);
            if (query.hasQueryItem("uuid")) return Response{QJsonArray{}};
            if (request.method == "GET" && query.hasQueryItem("topic_uuid"))
                return Response{QJsonArray{message("Current body")}};
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        const auto origin = fixture.server.origin();
        fixture.controller.openMessageLink("ew://open/org/127.0.0.1-" + QString::number(origin.port())
            + "/project/" + projectId + "/message/7a7a7a7a-7a7a-4a7a-8a7a-7a7a7a7a7a7a");
        QTest::qWait(300);
        QCOMPARE(fixture.controller.selectedStream(), streamId);
        QCOMPARE(fixture.controller.selectedTopic(), topicId);
        QVERIFY(fixture.controller.anchorMessageId().isEmpty());
        QVERIFY(!fixture.controller.notice().isEmpty());
    }

    void deepLinkBeforeLoginAppliesWhenTheSessionSettles()
    {
        Fixture fixture; fixture.extraTopic = true; fixture.aggregateMessages = true;
        const QString target = "78787878-7878-4787-8787-787878787878";
        fixture.messages = [&](const Request &request) {
            const auto query = QUrlQuery(request.target);
            if (query.hasQueryItem("uuid"))
                return Response{QJsonArray{message("Cold body", target, secondTopicId)}};
            if (request.method == "GET" && query.queryItemValue("topic_uuid") == secondTopicId)
                return Response{QJsonArray{message("Cold body", target, secondTopicId)}};
            return Response{QJsonArray{}};
        };
        const auto origin = fixture.server.origin();
        fixture.controller.openMessageLink("ew://open/org/127.0.0.1-" + QString::number(origin.port())
            + "/project/" + projectId + "/message/" + target);
        QVERIFY(fixture.start());
        QTRY_COMPARE(fixture.controller.selectedTopic(), secondTopicId);
        QTRY_VERIFY(fixture.controller.anchorMessageId() == target
            || fixture.controller.anchorMessageId().isEmpty()); // highlight may already have cleared
    }

    // ---- Anchor boundary paging (issue #71, docs/NATIVE_DESKTOP_ANCHOR_PAGING.md) ----

    static QJsonObject anchoredHistoryRow(int number)
    {
        const auto id = QStringLiteral("8e8e8e8e-8e8e-4e8e-8e8e-")
            + QString("%1").arg(number, 12, 16, QLatin1Char('0'));
        auto row = message(QStringLiteral("Anchor history %1").arg(number), id);
        row.insert("created_at", QDateTime::fromString("2026-01-02T10:00:00Z", Qt::ISODate)
                         .addSecs(number).toString(Qt::ISODateWithMs));
        return row;
    }

    // A deep-link anchor below the bounded older walk's reach opens the reference's
    // around-anchor window (desc + asc pages at page_marker=<anchor>, see
    // workspace_ui 61dacef7 messenger-messages.api.test.ts:169-246): the anchor sits
    // mid-window with newer rows below it, and the newer boundary pages outward.
    void anchorMidHistoryOpensWindowWithNewerRows()
    {
        Fixture fixture; fixture.aggregateMessages = true;
        const QString anchor = anchoredHistoryRow(50).value("uuid").toString();
        int olderWalks = 0;
        int ascAnchorPages = 0;
        fixture.messages = [&](const Request &request) -> Response {
            const auto query = QUrlQuery(request.target);
            if (query.hasQueryItem("uuid"))
                return Response{QJsonArray{anchoredHistoryRow(50)}};
            const auto dir = query.queryItemValue("sort_dir");
            const auto marker = query.queryItemValue("page_marker");
            if (marker.isEmpty()) {
                QJsonArray tail;
                for (int number = 500; number >= 451; --number) tail.append(anchoredHistoryRow(number));
                Response response{tail, 200, 0, {}};
                response.headers.insert("X-Pagination-Marker",
                    anchoredHistoryRow(451).value("uuid").toString().toUtf8());
                return response;
            }
            if (dir == "asc" && marker == anchor) {
                ++ascAnchorPages;
                QJsonArray page;
                for (int number = 51; number <= 100; ++number) page.append(anchoredHistoryRow(number));
                Response response{page, 200, 0, {}};
                response.headers.insert("X-Pagination-Marker", "newer-2");
                return response;
            }
            if (dir == "desc" && marker == anchor) {
                QJsonArray page;
                for (int number = 49; number >= 1; --number) page.append(anchoredHistoryRow(number));
                return Response{page}; // the older end is reached: no marker
            }
            if (dir == "asc" && marker == "newer-2") {
                QJsonArray page;
                for (int number = 101; number <= 120; ++number) page.append(anchoredHistoryRow(number));
                return Response{page}; // the newer end is reached: no marker
            }
            ++olderWalks; // the bounded walk must not run when the around window succeeds
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        const auto origin = fixture.server.origin();
        fixture.controller.openMessageLink("ew://open/org/127.0.0.1-" + QString::number(origin.port())
            + "/project/" + projectId + "/message/" + anchor);
        QTRY_COMPARE(fixture.controller.anchorMessageId(), anchor);
        QVERIFY(ascAnchorPages >= 1); // the around-anchor newer page was requested
        QCOMPARE(olderWalks, 0);
        QVERIFY(!modelMessage(fixture.controller, anchoredHistoryRow(55).value("uuid").toString()).isEmpty());
        QVERIFY(!modelMessage(fixture.controller, anchoredHistoryRow(45).value("uuid").toString()).isEmpty());
        QVERIFY(modelMessage(fixture.controller, anchoredHistoryRow(500).value("uuid").toString()).isEmpty());
        QVERIFY(fixture.controller.canLoadNewer());
        QVERIFY(!fixture.controller.canLoadOlder()); // the older page carried no marker
        fixture.controller.loadNewer();
        QTRY_VERIFY(!modelMessage(fixture.controller, anchoredHistoryRow(120).value("uuid").toString()).isEmpty());
        QCOMPARE(fixture.controller.anchorMessageId(), anchor); // highlight survives newer paging
        QVERIFY(!fixture.controller.canLoadNewer()); // the newer page carried no marker
    }

    // exo-review (#140): a malformed row on an around-anchor page must reject that
    // page (markers are only valid if its rows were accepted) and fall back to the
    // bounded older walk instead of committing a window with a silent gap.
    void malformedAroundPageFallsBackToOlderWalk()
    {
        Fixture fixture; fixture.aggregateMessages = true;
        const QString anchor = anchoredHistoryRow(50).value("uuid").toString();
        int aroundCommits = 0;
        fixture.messages = [&](const Request &request) -> Response {
            const auto query = QUrlQuery(request.target);
            if (query.hasQueryItem("uuid"))
                return Response{QJsonArray{anchoredHistoryRow(50)}};
            const auto dir = query.queryItemValue("sort_dir");
            const auto marker = query.queryItemValue("page_marker");
            if (marker.isEmpty()) {
                QJsonArray tail;
                for (int number = 500; number >= 451; --number) tail.append(anchoredHistoryRow(number));
                Response response{tail, 200, 0, {}};
                response.headers.insert("X-Pagination-Marker", "tail-1");
                return response;
            }
            if (dir == "asc" && marker == anchor)
                return Response{QJsonArray{anchoredHistoryRow(51)}, 200, 0, {{"X-Pagination-Marker", "newer-2"}}};
            if (dir == "desc" && marker == anchor) {
                ++aroundCommits;
                QJsonArray page;
                page.append(QJsonObject{{"uuid", "not-a-uuid"}, {"project_id", projectId}});
                return Response{page};
            }
            if (dir == "asc" && marker == "newer-2")
                return Response{QJsonArray{}};
            // bounded older walk path: the target sits on the first older page
            return Response{QJsonArray{anchoredHistoryRow(50), anchoredHistoryRow(49)}};
        };
        QVERIFY(fixture.start());
        const auto origin = fixture.server.origin();
        fixture.controller.openMessageLink("ew://open/org/127.0.0.1-" + QString::number(origin.port())
            + "/project/" + projectId + "/message/" + anchor);
        QTRY_COMPARE(fixture.controller.anchorMessageId(), anchor); // reached via the walk
        QCOMPARE(aroundCommits, 1); // the around page was requested once and rejected
        // The tail window survived: the malformed around page did not replace it.
        QVERIFY(!modelMessage(fixture.controller, anchoredHistoryRow(500).value("uuid").toString()).isEmpty());
        QVERIFY(!fixture.controller.canLoadNewer()); // no newer boundary from a rejected window
    }

    void tailWindowHasNoNewerBoundary()
    {
        Fixture fixture; fixture.aggregateMessages = true;
        int ascPages = 0;
        fixture.messages = [&](const Request &request) -> Response {
            if (QUrlQuery(request.target).queryItemValue("sort_dir") == "asc") {
                ++ascPages;
                return Response{QJsonArray{}};
            }
            return Response{QJsonArray{message("Current body")}};
        };
        QVERIFY(fixture.start());
        QTest::qWait(200);
        QVERIFY(!fixture.controller.canLoadNewer()); // a tail window has no newer side
        fixture.controller.loadNewer();
        QTest::qWait(200);
        QCOMPARE(ascPages, 0); // loadNewer stays inert without a newer boundary
    }

    void newerPageFailureKeepsWindowAndAnchor()
    {
        Fixture fixture; fixture.aggregateMessages = true;
        const QString anchor = anchoredHistoryRow(50).value("uuid").toString();
        fixture.messages = [&](const Request &request) -> Response {
            const auto query = QUrlQuery(request.target);
            if (query.hasQueryItem("uuid"))
                return Response{QJsonArray{anchoredHistoryRow(50)}};
            const auto dir = query.queryItemValue("sort_dir");
            const auto marker = query.queryItemValue("page_marker");
            if (marker.isEmpty()) {
                QJsonArray tail;
                for (int number = 500; number >= 451; --number) tail.append(anchoredHistoryRow(number));
                return Response{tail};
            }
            if (dir == "asc" && marker == anchor) {
                QJsonArray page;
                for (int number = 51; number <= 100; ++number) page.append(anchoredHistoryRow(number));
                Response response{page, 200, 0, {}};
                response.headers.insert("X-Pagination-Marker", "newer-2");
                return response;
            }
            if (dir == "desc" && marker == anchor)
                return Response{QJsonArray{anchoredHistoryRow(49)}};
            if (dir == "asc" && marker == "newer-2")
                return Response{QJsonObject{{"error", "boom"}}, 500, 0, {}};
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        const auto origin = fixture.server.origin();
        fixture.controller.openMessageLink("ew://open/org/127.0.0.1-" + QString::number(origin.port())
            + "/project/" + projectId + "/message/" + anchor);
        QTRY_COMPARE(fixture.controller.anchorMessageId(), anchor);
        QTRY_VERIFY(fixture.controller.canLoadNewer());
        fixture.controller.loadNewer();
        QTRY_VERIFY(!fixture.controller.busy());
        QVERIFY(!fixture.controller.error().isEmpty());
        QCOMPARE(fixture.controller.anchorMessageId(), anchor);
        QVERIFY(fixture.controller.canLoadNewer()); // the marker is kept for a retry
        QVERIFY(modelMessage(fixture.controller, anchoredHistoryRow(101).value("uuid").toString()).isEmpty());
    }

    // ---- Activity mentions page (issue #42, docs/NATIVE_DESKTOP_ACTIVITY_MENTIONS.md) ----

    static QJsonObject mentionMessage(int number, bool read = false, bool own = false)
    {
        const auto id = QStringLiteral("77777777-7777-4777-8777-")
            + QString("%1").arg(number, 12, 16, QLatin1Char('0'));
        auto row = message(QStringLiteral("Mention %1").arg(number), id);
        row.insert("read", read);
        row.insert("is_own", own);
        row.insert("mentioned", true);
        row.insert("created_at", QDateTime::fromString("2026-01-02T10:00:00Z", Qt::ISODate)
                         .addSecs(number).toString(Qt::ISODateWithMs));
        return row;
    }

    // Serves the mentioned=true message page like the Workspace API:
    // read=false narrows to unread rows, page_limit caps the page and the
    // continuation cursor rides the X-Pagination-Marker header.
    static Response mentionPageResponse(const QMap<int, QJsonObject> &rows, const Request &request)
    {
        const auto query = QUrlQuery(request.target);
        const bool unreadOnly = query.queryItemValue("read") == "false";
        const int limit = qMax(1, query.queryItemValue("page_limit").toInt());
        const auto marker = query.queryItemValue("page_marker");
        const int boundary = marker.isEmpty() ? std::numeric_limits<int>::max()
                                             : marker.right(12).toInt(nullptr, 16);
        QJsonArray items;
        bool more = false;
        for (auto it = rows.cend(); it != rows.cbegin();) {
            --it;
            if (it.key() >= boundary)
                continue;
            if (unreadOnly && it.value().value("read").toBool())
                continue;
            if (items.size() == limit) {
                more = true;
                break;
            }
            items.append(it.value());
        }
        Response response{items, 200, 0, {}};
        if (more)
            response.headers.insert("X-Pagination-Marker",
                items.last().toObject().value("uuid").toString().toUtf8());
        return response;
    }

    void activityMentionCollectionsIssueServerFilteredQueries()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto rows = std::make_shared<QMap<int, QJsonObject>>(QMap<int, QJsonObject>{
            {1, mentionMessage(1, true)}, {2, mentionMessage(2)}, {3, mentionMessage(3)}});
        auto queries = std::make_shared<QVector<QUrlQuery>>();
        fixture.messages = [rows, queries](const Request &request) {
            queries->append(QUrlQuery(request.target));
            return mentionPageResponse(*rows, request);
        };
        QVERIFY(fixture.start());
        QVERIFY(!fixture.controller.mentionsVisible());
        QCOMPARE(fixture.controller.mentionsFilter(), QString("all"));
        fixture.controller.showMentions();
        QVERIFY(fixture.controller.mentionsVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.mentionsPage().value("loading").toBool(), 4000);

        // The All collection query is mentioned-scoped without a read filter.
        // Measured in workspace_ui activity-mentions.api.ts:70-77.
        const auto mentionsAll = [&] {
            for (const auto &query : *queries)
                if (query.queryItemValue("page_limit") == "50"
                    && query.queryItemValue("mentioned") == "true") return query;
            return QUrlQuery{};
        }();
        QVERIFY(mentionsAll.hasQueryItem("page_limit"));
        QCOMPARE(mentionsAll.queryItemValue("mentioned"), QStringLiteral("true"));
        QVERIFY(!mentionsAll.hasQueryItem("read"));
        QCOMPARE(mentionsAll.queryItemValue("sort_key"), QStringLiteral("created_at"));
        QCOMPARE(mentionsAll.queryItemValue("sort_dir"), QStringLiteral("desc"));
        // The unread-mentions index prefetch walks 100 per page (api.ts:21,110-121).
        const bool hasIndexWalk = std::any_of(queries->cbegin(), queries->cend(),
            [](const QUrlQuery &query) { return query.queryItemValue("page_limit") == "100"
                && query.queryItemValue("read") == "false"; });
        QVERIFY(hasIndexWalk);

        auto page = fixture.controller.mentionsPage();
        auto list = page.value("rows").toList();
        QCOMPARE(list.size(), 3);
        // Newest first (activity-page.ui.tsx:239 sortUniqueActivityMessages reverse).
        QCOMPARE(list.first().toMap().value("id").toString(),
            mentionMessage(3).value("uuid").toString());
        // Index owns the dot: only the two unread rows light up (tests 590, 633).
        QVERIFY(list.at(0).toMap().value("unread").toBool());
        QVERIFY(list.at(1).toMap().value("unread").toBool());
        QVERIFY(!list.at(2).toMap().value("unread").toBool());

        // Unread is a separate server-filtered collection (activity-mentions.api.ts:73).
        queries->clear();
        fixture.controller.selectMentionsFilter("unread");
        QCOMPARE(fixture.controller.mentionsFilter(), QString("unread"));
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.mentionsPage().value("loading").toBool(), 4000);
        const auto mentionsUnread = [&] {
            for (const auto &query : *queries)
                if (query.queryItemValue("page_limit") == "50"
                    && query.queryItemValue("mentioned") == "true") return query;
            return QUrlQuery{};
        }();
        QVERIFY(mentionsUnread.hasQueryItem("page_limit"));
        QCOMPARE(mentionsUnread.queryItemValue("mentioned"), QStringLiteral("true"));
        QCOMPARE(mentionsUnread.queryItemValue("read"), QStringLiteral("false"));
        list = fixture.controller.mentionsPage().value("rows").toList();
        QCOMPARE(list.size(), 2);
        QVERIFY(list.at(0).toMap().value("unread").toBool());
        QVERIFY(list.at(1).toMap().value("unread").toBool());

        // Every filter entry is a fresh first page: no page_marker leaks across
        // collections (activity-page.ui.tsx:283-297 collectionKey reset).
        fixture.controller.selectMentionsFilter("all");
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.mentionsPage().value("loading").toBool(), 4000);
        for (const auto &query : *queries)
            if (query.queryItemValue("page_limit") == "50" && query.queryItemValue("mentioned") == "true")
                QVERIFY(!query.hasQueryItem("page_marker"));
        QCOMPARE(fixture.controller.mentionsPage().value("rows").toList().size(), 3);
    }

    void activityMentionPaginationFollowsMarkerNewestFirst()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        auto rows = std::make_shared<QMap<int, QJsonObject>>();
        for (int n = 1; n <= 60; ++n)
            rows->insert(n, mentionMessage(n, true)); // read=true keeps the index walk empty
        auto queries = std::make_shared<QVector<QUrlQuery>>();
        fixture.messages = [rows, queries](const Request &request) {
            queries->append(QUrlQuery(request.target));
            return mentionPageResponse(*rows, request);
        };
        QVERIFY(fixture.start());
        fixture.controller.showMentions();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.mentionsPage().value("rows").toList().size() == 50, 4000);
        auto page = fixture.controller.mentionsPage();
        QVERIFY(page.value("canLoadMore").toBool());

        fixture.controller.loadMoreMentions();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.mentionsPage().value("rows").toList().size() == 60, 4000);
        // The continuation rode X-Pagination-Marker into page_marker (api-client.cpp:450).
        const bool markedRequest = std::any_of(queries->cbegin(), queries->cend(),
            [](const QUrlQuery &query) { return query.queryItemValue("page_limit") == "50"
                && query.hasQueryItem("page_marker"); });
        QVERIFY(markedRequest);
        const auto list = fixture.controller.mentionsPage().value("rows").toList();
        QCOMPARE(list.size(), 60);
        QSet<QString> ids;
        for (const auto &value : list) ids.insert(value.toMap().value("id").toString());
        QCOMPARE(ids.size(), 60); // dedupe by uuid (activity-page.ui.tsx:95-98)
        QCOMPARE(list.first().toMap().value("id").toString(), mentionMessage(60).value("uuid").toString());
        QCOMPARE(list.last().toMap().value("id").toString(), mentionMessage(1).value("uuid").toString());
        QVERIFY(!fixture.controller.mentionsPage().value("canLoadMore").toBool());
    }

    void activityMentionRealtimeMergesWithoutRefetch()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto rows = std::make_shared<QMap<int, QJsonObject>>(QMap<int, QJsonObject>{
            {1, mentionMessage(1)}, {2, mentionMessage(2, false, true)}});
        auto queries = std::make_shared<QVector<QUrlQuery>>();
        fixture.messages = [rows, queries](const Request &request) {
            queries->append(QUrlQuery(request.target));
            return mentionPageResponse(*rows, request);
        };
        QVERIFY(fixture.start());
        fixture.controller.showMentions();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.mentionsPage().value("rows").toList().size() == 2, 4000);
        const auto rowWith = [&](const QString &id) {
            for (const auto &value : fixture.controller.mentionsPage().value("rows").toList())
                if (value.toMap().value("id").toString() == id) return value.toMap();
            return QVariantMap{};
        };
        // Own messages that mention the user are listed (activity-page.test.tsx:370).
        QVERIFY(rowWith(mentionMessage(2).value("uuid").toString()).contains("id"));
        const int fetchCount = queries->size();
        QVERIFY(fetchCount >= 1);

        // A live mention merges into the open page without refetching the page
        // (activity-page.test.tsx:489; classification via message.mentioned per
        // activity-realtime-applier.lib.ts:65-75 and :113-127).
        auto live = mentionMessage(90);
        live["author_uuid"] = QStringLiteral("66666666-6666-4666-8666-666666666666");
        const auto liveId = live.value("uuid").toString();
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", live, 13)));
        QTRY_VERIFY_WITH_TIMEOUT(rowWith(liveId).contains("id"), 2000);
        QCOMPARE(queries->size(), fetchCount); // no page request was issued
        auto list = fixture.controller.mentionsPage().value("rows").toList();
        QCOMPARE(list.first().toMap().value("id").toString(), liveId); // newest first
        QVERIFY(rowWith(liveId).value("unread").toBool()); // upserted into the unread index

        // A read event owns only the dot; the row stays in the All collection
        // (activity-realtime-applier.lib.ts:70-76).
        auto readEvent = mentionMessage(90);
        readEvent["read"] = true;
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.read", readEvent, 14)));
        QTRY_VERIFY(!rowWith(liveId).value("unread").toBool());
        QVERIFY(rowWith(liveId).contains("id"));

        // mentioned === false removes the uuid from the unread index
        // (activity-realtime-applier.lib.ts:77-79).
        auto notMention = mentionMessage(1);
        notMention["mentioned"] = false;
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", notMention, 15)));
        QTRY_VERIFY(!rowWith(mentionMessage(1).value("uuid").toString()).value("unread").toBool());

        // A batch read event clears exactly the listed dots
        // (activity-realtime-applier.lib.ts:60-66).
        const QJsonObject batch{{"kind", "messages.read"}, {"project_id", projectId},
            {"message_uuids", QJsonArray{mentionMessage(2).value("uuid").toString()}}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("messages.read", batch, 16)));
        QTRY_VERIFY(!rowWith(mentionMessage(2).value("uuid").toString()).value("unread").toBool());

        // Deleting the message drops the live row (activity-realtime-applier.lib.ts:118-124).
        const QJsonObject deleted{{"kind", "message.deleted"}, {"uuid", liveId},
            {"stream_uuid", streamId}, {"topic_uuid", topicId}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.deleted", deleted, 17)));
        QTRY_VERIFY(!rowWith(liveId).contains("id"));
        QCOMPARE(fixture.controller.mentionsPage().value("rows").toList().size(), 2);

        // topic.read clears the topic's mention dots (activity-realtime-applier.lib.ts:90-96).
        const QJsonObject topicRead{{"kind", "topic.read"},
            {"uuid", topicId}, {"project_id", projectId}, {"stream_uuid", streamId},
            {"name", "Native topic"}, {"is_done", false}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.read", topicRead, 18)));
        QTRY_VERIFY(!rowWith(mentionMessage(1).value("uuid").toString()).value("unread").toBool());
        QVERIFY(!rowWith(mentionMessage(2).value("uuid").toString()).value("unread").toBool());
    }

    // Deletion is reconciled across every mention store (issue #56): the
    // fetched collection rows, the live rows and the unread index all drop
    // the uuid immediately, without a refetch, and no later page response
    // can resurrect the deleted card.
    void activityMentionDeletionPrunesFetchedAllRowsWithoutRefetch()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto rows = std::make_shared<QMap<int, QJsonObject>>(QMap<int, QJsonObject>{
            {1, mentionMessage(1)}, {2, mentionMessage(2)}, {3, mentionMessage(3, true)}});
        auto queries = std::make_shared<QVector<QUrlQuery>>();
        fixture.messages = [rows, queries](const Request &request) {
            queries->append(QUrlQuery(request.target));
            return mentionPageResponse(*rows, request);
        };
        QVERIFY(fixture.start());
        fixture.controller.showMentions();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.mentionsPage().value("rows").toList().size() == 3, 4000);
        const auto rowWith = [&](const QString &id) {
            for (const auto &value : fixture.controller.mentionsPage().value("rows").toList())
                if (value.toMap().value("id").toString() == id) return value.toMap();
            return QVariantMap{};
        };

        // Row 2 now lives in both sources: the fetched collection and the
        // live map (message.created re-upserts the same uuid).
        QVERIFY(fixture.controller.applyRealtimeEvent(
            controllerEvent("message.created", mentionMessage(2), 13)));
        const auto id2 = mentionMessage(2).value("uuid").toString();
        const int fetchCount = queries->size();

        // A confirmed deletion drops the fetched row (which is also a live
        // row) from the open All collection without issuing a refetch.
        const QJsonObject deleted{{"kind", "message.deleted"}, {"uuid", id2},
            {"stream_uuid", streamId}, {"topic_uuid", topicId}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.deleted", deleted, 14)));
        QTRY_VERIFY(!rowWith(id2).contains("id"));
        QCOMPARE(queries->size(), fetchCount); // pruned in place, no refetch
        QVERIFY(rowWith(mentionMessage(1).value("uuid").toString()).contains("id"));
        QVERIFY(rowWith(mentionMessage(3).value("uuid").toString()).contains("id"));

        // Reading a non-deleted message still preserves its All card; the
        // read only clears the dot (activity-realtime-applier.lib.ts:70-76).
        auto readEvent = mentionMessage(1);
        readEvent["read"] = true;
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.read", readEvent, 15)));
        QTRY_VERIFY(!rowWith(mentionMessage(1).value("uuid").toString()).value("unread").toBool());
        QVERIFY(rowWith(mentionMessage(1).value("uuid").toString()).contains("id"));

        // The deleted uuid stays tombstoned: the next real refresh (Unread
        // filter) keeps it gone even though the fixture's server still serves it.
        fixture.controller.selectMentionsFilter("unread");
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.mentionsPage().value("loading").toBool(), 4000);
        QVERIFY(!rowWith(id2).contains("id"));
    }

    void activityMentionDeletionPrunesUnreadCollectionInPlace()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto rows = std::make_shared<QMap<int, QJsonObject>>(QMap<int, QJsonObject>{
            {1, mentionMessage(1)}, {2, mentionMessage(2)}, {3, mentionMessage(3, true)}});
        auto queries = std::make_shared<QVector<QUrlQuery>>();
        fixture.messages = [rows, queries](const Request &request) {
            queries->append(QUrlQuery(request.target));
            return mentionPageResponse(*rows, request);
        };
        QVERIFY(fixture.start());
        fixture.controller.showMentions();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.mentionsPage().value("rows").toList().size() == 3, 4000);
        fixture.controller.selectMentionsFilter("unread");
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.mentionsPage().value("rows").toList().size() == 2, 4000);
        const int fetchCount = queries->size();

        // The deletion drops the fetched row from the open Unread collection
        // too, without a refetch of that collection.
        const auto id1 = mentionMessage(1).value("uuid").toString();
        const QJsonObject deleted{{"kind", "message.deleted"}, {"uuid", id1},
            {"stream_uuid", streamId}, {"topic_uuid", topicId}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.deleted", deleted, 13)));
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.mentionsPage().value("rows").toList().size() == 1, 4000);
        QCOMPARE(queries->size(), fetchCount);
    }

    void activityMentionDeletedDuringOutstandingPageCannotBeResurrected()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto rows = std::make_shared<QMap<int, QJsonObject>>(QMap<int, QJsonObject>{
            {1, mentionMessage(1)}, {2, mentionMessage(2, true)}});
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        auto released = std::make_shared<bool>(false);
        fixture.messages = [rows, gates, released](const Request &request) {
            const QUrlQuery query(request.target);
            // Hold the first All collection page in flight, frozen at the
            // pre-deletion snapshot that includes row 2.
            if (!*released && query.queryItemValue("mentioned") == "true"
                && query.queryItemValue("page_limit") == "50") {
                Response response{QJsonArray{rows->value(1), rows->value(2)}, 200, 0, {}};
                response.defer = [gates](std::function<void()> send) { gates->push_back(std::move(send)); };
                return response;
            }
            return mentionPageResponse(*rows, request);
        };
        QVERIFY(fixture.start());
        fixture.controller.showMentions();
        QTRY_VERIFY_WITH_TIMEOUT(!gates->empty(), 2000); // the first page is in flight

        // The deletion of row 2 arrives while that page response is still
        // outstanding.
        const auto id2 = mentionMessage(2).value("uuid").toString();
        const QJsonObject deleted{{"kind", "message.deleted"}, {"uuid", id2},
            {"stream_uuid", streamId}, {"topic_uuid", topicId}};
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.deleted", deleted, 13)));
        *released = true;
        for (auto &release : *gates) release();
        gates->clear();

        // The late page may not resurrect the deleted row: only row 1 lands.
        QTRY_VERIFY_WITH_TIMEOUT(
            fixture.controller.mentionsPage().value("rows").toList().size() == 1, 4000);
        const auto list = fixture.controller.mentionsPage().value("rows").toList();
        for (const auto &value : list)
            QVERIFY(value.toMap().value("id").toString() != id2);
        QVERIFY(list.first().toMap().value("id").toString()
            == mentionMessage(1).value("uuid").toString());
    }

    void activityMentionIndexFallbackUntilWalkCompletes()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto rows = std::make_shared<QMap<int, QJsonObject>>(QMap<int, QJsonObject>{
            {1, mentionMessage(1, true)}, {2, mentionMessage(2)}, {3, mentionMessage(3)}});
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        auto released = std::make_shared<bool>(false);
        fixture.messages = [rows, gates, released](const Request &request) {
            const auto query = QUrlQuery(request.target);
            if (!*released && query.queryItemValue("page_limit") == "100") {
                Response response{QJsonArray{}, 200, 0, {}}; // authoritative index: nobody is unread
                response.defer = [gates](std::function<void()> send) { gates->push_back(std::move(send)); };
                return response;
            }
            return mentionPageResponse(*rows, request);
        };
        QVERIFY(fixture.start());
        fixture.controller.showMentions();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.mentionsPage().value("rows").toList().size() == 3, 4000);
        // Fallback rule: without the index the row's own read flag decides
        // (activity-page.test.tsx:556) - rows 2 and 3 look unread, row 1 does not.
        const auto rowWith = [&](const QString &id) {
            for (const auto &value : fixture.controller.mentionsPage().value("rows").toList())
                if (value.toMap().value("id").toString() == id) return value.toMap();
            return QVariantMap{};
        };
        QVERIFY(rowWith(mentionMessage(2).value("uuid").toString()).value("unread").toBool());
        QVERIFY(!rowWith(mentionMessage(1).value("uuid").toString()).value("unread").toBool());
        // Once the (empty) index walk completes the index is authoritative and
        // the fallback dot flips off (activity-page.test.tsx:590, 633).
        for (auto &release : *gates) release();
        gates->clear();
        QTRY_VERIFY_WITH_TIMEOUT(
            !rowWith(mentionMessage(2).value("uuid").toString()).value("unread").toBool(), 4000);
    }

    void activityMentionStaleOwnerSwitchDiscardsPage()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto rows = std::make_shared<QMap<int, QJsonObject>>(QMap<int, QJsonObject>{
            {1, mentionMessage(1)}});
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        auto released = std::make_shared<bool>(false);
        fixture.messages = [rows, gates, released](const Request &request) {
            const QUrlQuery query(request.target);
            if (!*released && query.hasQueryItem("mentioned")) {
                Response response{QJsonArray{}, 200, 0, {}};
                response.defer = [gates](std::function<void()> send) { gates->push_back(std::move(send)); };
                return response;
            }
            return mentionPageResponse(*rows, request);
        };
        QVERIFY(fixture.start());
        fixture.controller.showMentions();
        QTRY_VERIFY_WITH_TIMEOUT(!gates->empty(), 2000); // the first page is in flight
        fixture.controller.logout();
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.authenticated(), 4000);
        for (auto &release : *gates) release();
        gates->clear();
        *released = true;
        QTest::qWait(250); // give the late responses a chance to misfire
        QVERIFY(!fixture.controller.mentionsVisible());
        QVERIFY(fixture.controller.mentionsPage().value("rows").toList().isEmpty());
    }

    // ---- Inbox page (issue #44, docs/NATIVE_DESKTOP_INBOX.md) ----

    // Stream/topic fixtures measured against workspace_ui
    // 61dacef78dab2f5d0be76f283bebf49fe29b84af inbox-page.ui.tsx: DM + channel
    // streams mixing active/passive counters, a passive-only stream that must
    // stay out of the inbox, and a stream whose only unread lives at stream
    // level (reference fallback-row rule).
    static QJsonObject inboxStream(const QString &id, const QString &name,
        qint64 unread, qint64 active, qint64 passive, bool isPrivate = false,
        const QString &directUser = QString(),
        const QString &updatedAt = QStringLiteral("2026-06-22T10:10:00Z"))
    {
        return {{"uuid", id}, {"project_id", projectId}, {"user_uuid", userId}, {"owner", userId},
            {"name", name}, {"description", ""}, {"role", "owner"}, {"notification_mode", "all_messages"},
            {"source_name", "native"}, {"provider_uuid", QJsonValue::Null}, {"provider_identity", QJsonValue::Null},
            {"invite_only", false}, {"announce", false}, {"private", isPrivate}, {"is_archived", false},
            {"encryption", false}, {"current_encryption_key_uuid", QJsonValue::Null},
            {"direct_user_uuid", directUser.isEmpty() ? QJsonValue::Null : QJsonValue(directUser)},
            {"default_topic_uuid", QJsonValue::Null}, {"unread_count", unread},
            {"active_unread_count", active}, {"passive_unread_count", passive},
            {"last_message_uuid", QJsonValue::Null}, {"updated_at", updatedAt}};
    }

    static QJsonObject inboxTopic(const QString &id, const QString &stream, const QString &name,
        qint64 unread, qint64 active, qint64 passive,
        const QString &updatedAt = QStringLiteral("2026-06-22T10:10:00Z"))
    {
        return {{"uuid", id}, {"project_id", projectId}, {"user_uuid", userId}, {"stream_uuid", stream},
            {"name", name}, {"is_done", false}, {"is_default", false}, {"notification_mode", "default"},
            {"unread_count", unread}, {"active_unread_count", active}, {"passive_unread_count", passive},
            {"last_message_uuid", QJsonValue::Null}, {"created_at", "2026-06-22T10:00:00Z"},
            {"updated_at", updatedAt}};
    }

    void loginInbox(Fixture &fixture, int streamCount)
    {
        fixture.controller.login(fixture.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QVERIFY(QTest::qWaitFor([&fixture] { return fixture.controller.projects().size() == 1; }, 2000));
        fixture.controller.openProject(projectId);
        QVERIFY(QTest::qWaitFor([&fixture, streamCount] {
            return fixture.controller.streams().size() == streamCount; }, 3000));
        QVERIFY(QTest::qWaitFor([&fixture] { return !fixture.controller.busy(); }, 3000));
    }

    static QVariantMap inboxCard(const QVariantMap &page, const QString &section, const QString &stream)
    {
        for (const auto &value : page.value(section).toList())
            if (value.toMap().value("id").toString() == stream) return value.toMap();
        return {};
    }

    static QVariantMap inboxRow(const QVariantMap &page, const QString &section,
        const QString &stream, const QString &topic)
    {
        for (const auto &value : inboxCard(page, section, stream).value("rows").toList())
            if (value.toMap().value("topic").toString() == topic) return value.toMap();
        return {};
    }

    void inboxGroupsDirectAndChannelsExcludingPassiveTraffic()
    {
        Fixture fixture;
        fixture.users = [](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa")},
                {"username", "alice"}, {"first_name", "Alice"}, {"email", "alice@example.invalid"}}}}; };
        fixture.streams = [](const Request &) { return Response{QJsonArray{
            inboxStream(QStringLiteral("a1000000-0000-4000-8000-000000000001"), "alice", 2, 2, 0,
                true, QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa")),
            inboxStream(streamId, "Product team", 4, 4, 0),
            inboxStream(QStringLiteral("a1000000-0000-4000-8000-000000000002"), "Quiet", 5, 0, 5),
            inboxStream(QStringLiteral("a1000000-0000-4000-8000-000000000003"), "Notice", 2, 2, 0)}}; };
        fixture.topics = [](const Request &request) {
            const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
            if (stream == QStringLiteral("a1000000-0000-4000-8000-000000000001"))
                // A blank topic title renders inbox.allMessages (inbox-page.ui.tsx:64-73).
                return Response{QJsonArray{inboxTopic(
                    QStringLiteral("b1000000-0000-4000-8000-000000000001"),
                    QStringLiteral("a1000000-0000-4000-8000-000000000001"), "  ", 2, 2, 0)}};
            if (stream == streamId)
                return Response{QJsonArray{
                    inboxTopic(topicId, streamId, "General Topic", 3, 3, 0, "2026-06-22T10:10:00Z"),
                    inboxTopic(secondTopicId, streamId, "Planning", 0, 0, 0),
                    inboxTopic(QStringLiteral("b1000000-0000-4000-8000-000000000002"), streamId,
                        "Releases", 1, 1, 0, "2026-06-22T15:00:00Z")}};
            if (stream == QStringLiteral("a1000000-0000-4000-8000-000000000002"))
                return Response{QJsonArray{inboxTopic(
                    QStringLiteral("b1000000-0000-4000-8000-000000000003"),
                    QStringLiteral("a1000000-0000-4000-8000-000000000002"), "Lurk", 5, 0, 5)}};
            if (stream == QStringLiteral("a1000000-0000-4000-8000-000000000003"))
                return Response{QJsonArray{inboxTopic(
                    QStringLiteral("b1000000-0000-4000-8000-000000000004"),
                    QStringLiteral("a1000000-0000-4000-8000-000000000003"), "Bulletin", 0, 0, 0)}};
            return Response{QJsonArray{}};
        };
        loginInbox(fixture, 4);
        const auto before = fixture.controller.inboxPage();
        QVERIFY(!before.value("loading").toBool());
        QVERIFY(!before.value("error").toBool());
        QVERIFY(!fixture.controller.inboxVisible());
        fixture.controller.showInbox();
        QVERIFY(fixture.controller.inboxVisible());
        // Topic rows refine once the per-stream catalogs land (lazy native
        // hydration documented in the design doc).
        QVERIFY(QTest::qWaitFor([&fixture] {
            const auto page = fixture.controller.inboxPage();
            const auto dm = inboxCard(page, "direct",
                QStringLiteral("a1000000-0000-4000-8000-000000000001"));
            const auto dmRows = dm.value("rows").toList();
            const auto productRows = inboxCard(page, "channels", streamId).value("rows").toList();
            return dmRows.size() == 1 && dmRows.at(0).toMap().value("topic").toString()
                       == QStringLiteral("b1000000-0000-4000-8000-000000000001")
                && productRows.size() == 2;
        }, 4000));
        const auto page = fixture.controller.inboxPage();

        const auto direct = page.value("direct").toList();
        QCOMPARE(direct.size(), 1);
        const auto dmCard = direct.first().toMap();
        QCOMPARE(dmCard.value("id").toString(),
            QStringLiteral("a1000000-0000-4000-8000-000000000001"));
        QCOMPARE(dmCard.value("title").toString(), QStringLiteral("Alice")); // peer display name
        QVERIFY(dmCard.value("direct").toBool());
        // Card badge = stream active unread (inbox-page.ui.tsx:158).
        QCOMPARE(dmCard.value("badge").toString(), QStringLiteral("2"));
        const auto dmRows = dmCard.value("rows").toList();
        QCOMPARE(dmRows.size(), 1);
        const auto dmRow = dmRows.first().toMap();
        QCOMPARE(dmRow.value("title").toString(), QStringLiteral("Alice · All messages"));
        QCOMPARE(dmRow.value("topic").toString(),
            QStringLiteral("b1000000-0000-4000-8000-000000000001"));
        QCOMPARE(dmRow.value("badge").toString(), QStringLiteral("2"));
        QVERIFY(!dmRow.value("time").toString().isEmpty());

        // The passive-only stream stays out of the inbox entirely
        // (hasUnread, inbox-page.ui.tsx:54-61; resolveWorkspaceActiveUnreadCount
        // messenger-notification-mode.lib.ts:37-41).
        const auto channels = page.value("channels").toList();
        QCOMPARE(channels.size(), 2);
        const auto product = channels.at(0).toMap();
        QCOMPARE(product.value("id").toString(), streamId);
        QCOMPARE(product.value("badge").toString(), QStringLiteral("4"));
        const auto productRows = product.value("rows").toList();
        QCOMPARE(productRows.size(), 2); // unread topics only (inbox-page.ui.tsx:86-90)
        // Recency order: Releases (15:00Z) before General Topic (10:10Z).
        QCOMPARE(productRows.at(0).toMap().value("title").toString(),
            QStringLiteral("Product team · Releases"));
        QCOMPARE(productRows.at(1).toMap().value("title").toString(),
            QStringLiteral("Product team · General Topic"));
        const auto notice = channels.at(1).toMap();
        QCOMPARE(notice.value("id").toString(),
            QStringLiteral("a1000000-0000-4000-8000-000000000003"));
        // Stream-level unread with all topics read renders ONE fallback row
        // (buildInboxStreamGroup, inbox-page.ui.tsx:86-95).
        const auto noticeRows = notice.value("rows").toList();
        QCOMPARE(noticeRows.size(), 1);
        QCOMPARE(noticeRows.first().toMap().value("title").toString(), QStringLiteral("Notice"));
        QVERIFY(noticeRows.first().toMap().value("topic").toString().isEmpty());
        QCOMPARE(noticeRows.first().toMap().value("badge").toString(), QStringLiteral("2"));

        fixture.controller.closeInbox();
        QVERIFY(!fixture.controller.inboxVisible());
    }

    void inboxRowOpensTargetAndClosesPage()
    {
        Fixture fixture;
        fixture.streams = [](const Request &) { return Response{QJsonArray{
            inboxStream(streamId, "Product team", 3, 3, 0),
            inboxStream(QStringLiteral("a1000000-0000-4000-8000-000000000003"), "Notice", 2, 2, 0)}}; };
        fixture.topics = [](const Request &request) {
            const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
            if (stream == streamId)
                return Response{QJsonArray{inboxTopic(topicId, streamId, "General Topic", 3, 3, 0)}};
            return Response{QJsonArray{}};
        };
        loginInbox(fixture, 2);
        fixture.controller.showInbox();
        // Wait for the refined topic row (the slot owns the canonical DTO
        // that openInboxRow hands to the topic-opening machinery).
        QVERIFY(QTest::qWaitFor([&fixture] {
            const auto rows = inboxCard(fixture.controller.inboxPage(), "channels", streamId)
                .value("rows").toList();
            return rows.size() == 1 && rows.at(0).toMap().value("topic").toString() == topicId;
        }, 4000));
        // A topic row opens the topic and the page unmounts (reference row click
        // navigates, inbox-page.ui.tsx:108-110).
        fixture.controller.openInboxRow(streamId, topicId);
        QVERIFY(!fixture.controller.inboxVisible());
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.selectedTopic() == topicId, 4000);

        fixture.controller.showInbox();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.inboxVisible(), 2000);
        fixture.controller.closeInbox();
        // The fallback row has no topic target and selects the stream.
        fixture.controller.openInboxRow(QStringLiteral("a1000000-0000-4000-8000-000000000003"), QString());
        QVERIFY(!fixture.controller.inboxVisible());
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.selectedStream()
            == QStringLiteral("a1000000-0000-4000-8000-000000000003"), 4000);
    }

    void inboxBadgeFormattingMatchesReferenceLimit()
    {
        Fixture fixture;
        fixture.streams = [](const Request &) { return Response{QJsonArray{
            inboxStream(QStringLiteral("a1000000-0000-4000-8000-000000000005"), "Thousand", 1000, 1000, 0),
            inboxStream(QStringLiteral("a1000000-0000-4000-8000-000000000006"), "Over", 1001, 1001, 0)}}; };
        fixture.topics = [](const Request &) { return Response{QJsonArray{}}; };
        loginInbox(fixture, 2);
        fixture.controller.showInbox();
        // UNREAD_COUNT_DISPLAY_LIMIT = 1000 with floor(count/1000)+"k+" only
        // above it (count-format.lib.ts): 1000 -> "1000", 1001 -> "1k+".
        const auto thousand = inboxCard(fixture.controller.inboxPage(), "channels",
            QStringLiteral("a1000000-0000-4000-8000-000000000005"));
        const auto over = inboxCard(fixture.controller.inboxPage(), "channels",
            QStringLiteral("a1000000-0000-4000-8000-000000000006"));
        QCOMPARE(thousand.value("badge").toString(), QStringLiteral("1000"));
        QCOMPARE(over.value("badge").toString(), QStringLiteral("1k+"));
        QCOMPARE(thousand.value("rows").toList().at(0).toMap().value("badge").toString(),
            QStringLiteral("1000"));
        QCOMPARE(over.value("rows").toList().at(0).toMap().value("badge").toString(),
            QStringLiteral("1k+"));
    }

    void inboxRealtimeCounterUpdateRecomputesPage()
    {
        Fixture fixture;
        fixture.streams = [](const Request &) { return Response{QJsonArray{
            inboxStream(streamId, "Product team", 3, 3, 0),
            inboxStream(QStringLiteral("a1000000-0000-4000-8000-000000000002"), "Quiet", 0, 0, 0),
            inboxStream(QStringLiteral("a1000000-0000-4000-8000-000000000003"), "Notice", 2, 2, 0)}}; };
        fixture.topics = [](const Request &) { return Response{QJsonArray{}}; };
        loginInbox(fixture, 3);
        fixture.controller.showInbox();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.inboxPage().value("channels").toList().size() == 2, 4000);

        // stream.updated raises the muted stream's active counters -> it joins
        // the inbox (reference store applies the replaced counters live).
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated",
            inboxStream(QStringLiteral("a1000000-0000-4000-8000-000000000002"), "Quiet", 7, 7, 0), 20)));
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.inboxPage().value("channels").toList().size() == 3, 4000);
        QVERIFY(!inboxCard(fixture.controller.inboxPage(), "channels",
            QStringLiteral("a1000000-0000-4000-8000-000000000002")).isEmpty());

        // stream.read zeroes the counters -> the stream leaves the inbox.
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.read",
            inboxStream(QStringLiteral("a1000000-0000-4000-8000-000000000003"), "Notice", 0, 0, 0), 21)));
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.inboxPage().value("channels").toList().size() == 2, 4000);
        QVERIFY(inboxCard(fixture.controller.inboxPage(), "channels",
            QStringLiteral("a1000000-0000-4000-8000-000000000003")).isEmpty());
    }

    // Issue #65: a whole-topic move (a complete owned topic.updated whose
    // stream_uuid differs from the slot that holds the topic) must reconcile
    // every Inbox slot: the vacated card goes away, exactly one row survives
    // at the destination, and navigation receives the current placement,
    // never the stale source DTO (openInboxRow hands the slot DTO to
    // beginTopicOpening as verified canonical metadata).
    void inboxTopicMoveAcrossStreamsReconcilesSlots()
    {
        const auto alpha = QStringLiteral("a1000000-0000-4000-8000-0000000000a1");
        const auto beta = QStringLiteral("a1000000-0000-4000-8000-0000000000b2");
        const auto moved = QStringLiteral("b1000000-0000-4000-8000-0000000000c3");
        const auto nightly = QStringLiteral("b1000000-0000-4000-8000-0000000000d4");
        Fixture fixture;
        fixture.streams = [=](const Request &) { return Response{QJsonArray{
            inboxStream(alpha, "Alpha", 3, 3, 0),
            inboxStream(beta, "Beta", 1, 1, 0)}}; };
        fixture.topics = [=](const Request &request) {
            const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
            if (stream == alpha)
                return Response{QJsonArray{
                    inboxTopic(moved, alpha, "Migration", 3, 3, 0, "2026-06-22T09:00:00Z"),
                    inboxTopic(secondTopicId, alpha, "Planning", 0, 0, 0)}};
            if (stream == beta)
                return Response{QJsonArray{
                    inboxTopic(nightly, beta, "Nightly", 1, 1, 0)}};
            return Response{QJsonArray{}};
        };
        loginInbox(fixture, 2);
        fixture.controller.showInbox();
        QVERIFY(QTest::qWaitFor([&fixture, alpha, moved] {
            return !inboxRow(fixture.controller.inboxPage(), "channels", alpha, moved).isEmpty();
        }, 4000));

        // The complete owned move of the still-unread topic to Beta, followed
        // by the stream counter updates the server pairs the move with.
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated",
            inboxTopic(moved, beta, "Migration", 3, 3, 0, "2026-06-22T11:30:00Z"), 40)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated",
            inboxStream(beta, "Beta", 4, 4, 0), 41)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated",
            inboxStream(alpha, "Alpha", 0, 0, 0), 42)));
        // Exactly one row survives, under Beta with the moved badge; Alpha
        // leaves the inbox once no unread topics or stream messages remain.
        QVERIFY(QTest::qWaitFor([&fixture, alpha, beta, moved] {
            const auto page = fixture.controller.inboxPage();
            return inboxCard(page, "channels", alpha).isEmpty()
                && inboxRow(page, "channels", beta, moved).value("badge").toString()
                    == QStringLiteral("3");
        }, 4000));
        QCOMPARE(inboxCard(fixture.controller.inboxPage(), "channels", beta)
            .value("rows").toList().size(), 2); // Nightly + the moved topic

        // Clicking the surviving row opens the topic at its current home.
        fixture.controller.openInboxRow(beta, moved);
        QVERIFY(!fixture.controller.inboxVisible());
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.selectedTopic() == moved, 4000);
        fixture.controller.showInbox();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.inboxVisible(), 2000);

        // A rename at the destination updates the single row in place.
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated",
            inboxTopic(moved, beta, "Migration v2", 3, 3, 0, "2026-06-22T12:00:00Z"), 43)));
        QVERIFY(QTest::qWaitFor([&fixture, beta, moved] {
            const auto rows = inboxCard(fixture.controller.inboxPage(), "channels", beta)
                .value("rows").toList();
            return rows.size() == 2 && rows.at(0).toMap().value("title").toString()
                == QStringLiteral("Beta · Migration v2");
        }, 4000));

        // Moving the topic back revives Alpha and vacates Beta.
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated",
            inboxTopic(moved, alpha, "Migration v2", 3, 3, 0, "2026-06-22T13:00:00Z"), 44)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated",
            inboxStream(alpha, "Alpha", 3, 3, 0), 45)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated",
            inboxStream(beta, "Beta", 1, 1, 0), 46)));
        QVERIFY(QTest::qWaitFor([&fixture, alpha, beta, moved] {
            const auto page = fixture.controller.inboxPage();
            const auto alphaRows = inboxCard(page, "channels", alpha).value("rows").toList();
            const auto betaRows = inboxCard(page, "channels", beta).value("rows").toList();
            return alphaRows.size() == 1
                && alphaRows.at(0).toMap().value("topic").toString() == moved
                && betaRows.size() == 1;
        }, 4000));

        // Later read and delete events affect the surviving destination row.
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.read",
            inboxTopic(moved, alpha, "Migration v2", 0, 0, 0, "2026-06-22T13:30:00Z"), 47)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.read",
            inboxStream(alpha, "Alpha", 0, 0, 0), 48)));
        QVERIFY(QTest::qWaitFor([&fixture, alpha] {
            return inboxCard(fixture.controller.inboxPage(), "channels", alpha).isEmpty();
        }, 4000));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.deleted",
            QJsonObject{{"uuid", moved}, {"stream_uuid", alpha}}, 49)));
        // The tombstone leaves no row anywhere and does not disturb Beta.
        QTest::qWait(250);
        const auto page = fixture.controller.inboxPage();
        QVERIFY(inboxCard(page, "channels", alpha).isEmpty());
        QCOMPARE(inboxCard(page, "channels", beta).value("rows").toList().size(), 1);
    }

    // Issue #65: the destination slot may not be hydrated yet (the stream had
    // no unread when the Inbox opened). The move must invalidate toward the
    // destination and refetch its complete slot instead of fabricating a
    // partial one; the fetched snapshot, not the event DTO, owns the row.
    void inboxTopicMoveToUnhydratedStreamRefetchesDestination()
    {
        const auto alpha = QStringLiteral("a1000000-0000-4000-8000-0000000000a1");
        const auto beta = QStringLiteral("a1000000-0000-4000-8000-0000000000b2");
        const auto moved = QStringLiteral("b1000000-0000-4000-8000-0000000000c3");
        Fixture fixture;
        fixture.streams = [=](const Request &) { return Response{QJsonArray{
            inboxStream(alpha, "Alpha", 3, 3, 0),
            inboxStream(beta, "Beta", 0, 0, 0)}}; };
        fixture.topics = [=](const Request &request) {
            const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
            if (stream == alpha)
                return Response{QJsonArray{
                    inboxTopic(moved, alpha, "Migration", 3, 3, 0)}};
            if (stream == beta)
                // Server-side truth after the move. The counters deliberately
                // differ from the event DTO so a refetch cannot be confused
                // with a direct upsert of the event payload.
                return Response{QJsonArray{
                    inboxTopic(moved, beta, "Migration", 4, 4, 0, "2026-06-22T11:45:00Z")}};
            return Response{QJsonArray{}};
        };
        loginInbox(fixture, 2);
        fixture.controller.showInbox();
        // Beta carries no unread at open time, so only Alpha hydrates a slot.
        QVERIFY(QTest::qWaitFor([&fixture, alpha, moved] {
            return !inboxRow(fixture.controller.inboxPage(), "channels", alpha, moved).isEmpty();
        }, 4000));
        QVERIFY(inboxCard(fixture.controller.inboxPage(), "channels", beta).isEmpty());

        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated",
            inboxTopic(moved, beta, "Migration", 3, 3, 0, "2026-06-22T11:30:00Z"), 50)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated",
            inboxStream(beta, "Beta", 3, 3, 0), 51)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated",
            inboxStream(alpha, "Alpha", 0, 0, 0), 52)));
        // Alpha's stale row disappears and the refetched Beta slot owns the
        // complete destination state (the fetched badge 4, not the event's 3).
        QVERIFY(QTest::qWaitFor([&fixture, alpha, beta, moved] {
            const auto page = fixture.controller.inboxPage();
            return inboxCard(page, "channels", alpha).isEmpty()
                && inboxRow(page, "channels", beta, moved).value("badge").toString()
                    == QStringLiteral("4");
        }, 4000));
        fixture.controller.openInboxRow(beta, moved);
        QVERIFY(!fixture.controller.inboxVisible());
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.selectedTopic() == moved, 4000);
    }

    // Reviewer follow-up on #65: a slot snapshot generated before a placement
    // event must not resurrect the old placement when it lands after it. The
    // first Alpha fetch is held in flight (deterministic defer gate), the move
    // A->B is applied, then the stale snapshot is released.
    void delayedInboxSlotCannotResurrectMovedTopic()
    {
        const auto alpha = QStringLiteral("a1000000-0000-4000-8000-0000000000a1");
        const auto beta = QStringLiteral("a1000000-0000-4000-8000-0000000000b2");
        const auto moved = QStringLiteral("b1000000-0000-4000-8000-0000000000c3");
        const auto nightly = QStringLiteral("b1000000-0000-4000-8000-0000000000d4");
        Fixture fixture;
        std::function<void()> releaseAlpha;
        bool gated = false;
        fixture.streams = [&](const Request &) { return Response{QJsonArray{
            inboxStream(alpha, "Alpha", 3, 3, 0),
            inboxStream(beta, "Beta", 1, 1, 0)}}; };
        fixture.topics = [&](const Request &request) -> Response {
            const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
            if (stream == alpha) {
                // The pre-move snapshot: still lists the topic under Alpha.
                Response response{QJsonArray{
                    inboxTopic(moved, alpha, "Migration", 3, 3, 0, "2026-06-22T09:00:00Z"),
                    inboxTopic(secondTopicId, alpha, "Planning", 0, 0, 0)}};
                if (!gated) {
                    gated = true;
                    response.defer = [&](std::function<void()> send) { releaseAlpha = std::move(send); };
                }
                return response;
            }
            if (stream == beta)
                return Response{QJsonArray{inboxTopic(nightly, beta, "Nightly", 1, 1, 0)}};
            return Response{QJsonArray{}};
        };
        loginInbox(fixture, 2);
        fixture.controller.showInbox();
        // Alpha's slot response is held in flight; Beta's slot lands first.
        QVERIFY(QTest::qWaitFor([&fixture, &gated, beta, nightly] {
            return gated && !inboxRow(fixture.controller.inboxPage(), "channels", beta, nightly).isEmpty();
        }, 4000));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated",
            inboxTopic(moved, beta, "Migration", 3, 3, 0, "2026-06-22T11:30:00Z"), 60)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated",
            inboxStream(beta, "Beta", 4, 4, 0), 61)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated",
            inboxStream(alpha, "Alpha", 0, 0, 0), 62)));
        QVERIFY(QTest::qWaitFor([&fixture, beta, moved] {
            return inboxRow(fixture.controller.inboxPage(), "channels", beta, moved)
                .value("badge").toString() == QStringLiteral("3");
        }, 4000));
        // The stale snapshot lands after the move: it must not revive Alpha.
        releaseAlpha();
        QTest::qWait(300);
        const auto page = fixture.controller.inboxPage();
        QVERIFY2(inboxCard(page, "channels", alpha).isEmpty(),
            "An in-flight slot snapshot restored the topic to its vacated stream");
        QCOMPARE(inboxRow(page, "channels", beta, moved).value("badge").toString(),
            QStringLiteral("3"));
    }

    // Reviewer follow-up (PR #75): the placement overlay supplements a fetch
    // that started AFTER the event too. When the fetch reflects a later read
    // whose updated_at ties with the event's, the snapshot counters must win
    // - the topic left the inbox and must not reappear unread.
    void inboxSlotSnapshotCountersWinOverEqualTimestampPlacementEvent()
    {
        const auto alpha = QStringLiteral("a1000000-0000-4000-8000-0000000000a1");
        const auto beta = QStringLiteral("a1000000-0000-4000-8000-0000000000b2");
        const auto moved = QStringLiteral("b1000000-0000-4000-8000-0000000000c3");
        Fixture fixture;
        fixture.streams = [&](const Request &) { return Response{QJsonArray{
            inboxStream(alpha, "Alpha", 3, 3, 0),
            inboxStream(beta, "Beta", 0, 0, 0)}}; };
        fixture.topics = [&](const Request &request) -> Response {
            const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
            if (stream == alpha)
                return Response{QJsonArray{
                    inboxTopic(moved, alpha, "Migration", 3, 3, 0, "2026-06-22T11:30:00Z")}};
            if (stream == beta)
                // Fetched after the move event: the topic was read while
                // resolving; the server keeps the same updated_at.
                return Response{QJsonArray{
                    inboxTopic(moved, beta, "Migration", 0, 0, 0, "2026-06-22T11:30:00Z")}};
            return Response{QJsonArray{}};
        };
        loginInbox(fixture, 2);
        fixture.controller.showInbox();
        // Beta carries no unread at open, so only Alpha hydrates a slot.
        QVERIFY(QTest::qWaitFor([&fixture, alpha, moved] {
            return !inboxRow(fixture.controller.inboxPage(), "channels", alpha, moved).isEmpty();
        }, 4000));
        // The move event arrives at 11:30Z with unread counters; the
        // destination refetch then answers with the fresher read state at
        // the same updated_at.
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.updated",
            inboxTopic(moved, beta, "Migration", 3, 3, 0, "2026-06-22T11:30:00Z"), 80)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated",
            inboxStream(beta, "Beta", 3, 3, 0), 81)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated",
            inboxStream(alpha, "Alpha", 0, 0, 0), 82)));
        QVERIFY(QTest::qWaitFor([&fixture, alpha] {
            return inboxCard(fixture.controller.inboxPage(), "channels", alpha).isEmpty();
        }, 4000));
        // The topic is read at its destination: no topic row may appear.
        QTest::qWait(250);
        QVERIFY2(inboxRow(fixture.controller.inboxPage(), "channels", beta, moved).isEmpty(),
            "A tie on updated_at let the event DTO overwrite fresher read counters");
    }

    void delayedInboxSlotCannotResurrectDeletedTopic()
    {
        const auto alpha = QStringLiteral("a1000000-0000-4000-8000-0000000000a1");
        const auto moved = QStringLiteral("b1000000-0000-4000-8000-0000000000c3");
        Fixture fixture;
        std::function<void()> releaseAlpha;
        bool gated = false;
        fixture.streams = [&](const Request &) { return Response{QJsonArray{
            inboxStream(alpha, "Alpha", 3, 3, 0)}}; };
        fixture.topics = [&](const Request &request) -> Response {
            const auto stream = QUrlQuery(request.target).queryItemValue("stream_uuid");
            if (stream == alpha) {
                Response response{QJsonArray{
                    inboxTopic(moved, alpha, "Migration", 3, 3, 0),
                    inboxTopic(secondTopicId, alpha, "Planning", 0, 0, 0)}};
                if (!gated) {
                    gated = true;
                    response.defer = [&](std::function<void()> send) { releaseAlpha = std::move(send); };
                }
                return response;
            }
            return Response{QJsonArray{}};
        };
        loginInbox(fixture, 1);
        fixture.controller.showInbox();
        // Alpha's slot response is held in flight.
        QVERIFY(QTest::qWaitFor([&gated] { return gated; }, 4000));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("topic.deleted",
            QJsonObject{{"uuid", moved}, {"stream_uuid", alpha}}, 70)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("stream.updated",
            inboxStream(alpha, "Alpha", 0, 0, 0), 71)));
        releaseAlpha();
        QTest::qWait(300);
        QVERIFY2(inboxCard(fixture.controller.inboxPage(), "channels", alpha).isEmpty(),
            "An in-flight slot snapshot resurrected a deleted topic");
    }

    void inboxDeeplinkOpensTheInboxPage()
    {
        Fixture fixture;
        fixture.streams = [](const Request &) { return Response{QJsonArray{
            inboxStream(streamId, "Product team", 1, 1, 0)}}; };
        loginInbox(fixture, 1);
        const auto origin = fixture.server.origin();
        // /inbox is allowlisted (native-deeplink.cpp:39); the reference
        // navigates the inbox page for it.
        fixture.controller.openMessageLink(QStringLiteral("ew://open/org/127.0.0.1-")
            + QString::number(origin.port()) + "/project/" + projectId + "/inbox");
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.inboxVisible(), 4000);
        fixture.controller.closeInbox();
        // A foreign project route keeps the safe no-navigation behaviour.
        fixture.controller.openMessageLink(QStringLiteral("ew://open/org/127.0.0.1-")
            + QString::number(origin.port())
            + "/project/99999999-9999-4999-8999-999999999999/inbox");
        QTest::qWait(250);
        QVERIFY(!fixture.controller.inboxVisible());
    }

    void inboxEmptyAndErrorStates()
    {
        Fixture ok;
        ok.streams = [](const Request &) { return Response{QJsonArray{
            inboxStream(streamId, "Product team", 0, 0, 0)}}; };
        loginInbox(ok, 1);
        ok.controller.showInbox();
        const auto clean = ok.controller.inboxPage();
        QVERIFY(!clean.value("loading").toBool());
        QVERIFY(!clean.value("error").toBool());
        QVERIFY(clean.value("direct").toList().isEmpty() && clean.value("channels").toList().isEmpty());

        // A failed catalog keeps m_catalogVerified false: loading resolves to
        // the error card (inbox-page.ui.tsx:196-207 state derivation).
        Fixture failing;
        failing.streams = [](const Request &) { return Response{QJsonArray{}, 500}; };
        failing.controller.login(failing.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QVERIFY(QTest::qWaitFor([&failing] { return failing.controller.projects().size() == 1; }, 2000));
        failing.controller.openProject(projectId);
        QVERIFY(QTest::qWaitFor([&failing] {
            return failing.controller.streams().isEmpty() && failing.controller.authenticated()
                && !failing.controller.error().isEmpty(); }, 3000));
        failing.controller.showInbox();
        const auto page = failing.controller.inboxPage();
        QVERIFY(page.value("error").toBool());
        QVERIFY(!page.value("loading").toBool());
    }

    // Issue #64: a failed first topic-list request must not pin the stream in
    // the hydration set. Closing and reopening Inbox must retry the request and
    // refine the fallback card into topic rows (docs/NATIVE_DESKTOP_INBOX.md
    // failure-recovery section; acceptance criteria in the issue).
    void inboxFailedTopicHydrationRetriesOnReopen()
    {
        Fixture fixture;
        const auto stream = QStringLiteral("a1000000-0000-4000-8000-000000000064");
        fixture.streams = [stream](const Request &) { return Response{QJsonArray{
            inboxStream(stream, "Product team", 4, 4, 0)}}; };
        auto topicGets = std::make_shared<int>(0);
        fixture.topics = [stream, topicGets](const Request &request) {
            if (QUrlQuery(request.target).queryItemValue("stream_uuid") != stream)
                return Response{QJsonArray{}};
            if (++*topicGets == 1)
                return Response{QJsonArray{}, 500}; // transient server error
            return Response{QJsonArray{inboxTopic(topicId, stream, "General Topic", 3, 3, 0)}};
        };
        loginInbox(fixture, 1);
        fixture.controller.showInbox();
        QVERIFY(QTest::qWaitFor([&] { return *topicGets >= 1; }, 4000));
        QTest::qWait(250); // let the failure settle before inspecting the page
        // The failed stream keeps only the stream-level fallback row.
        const auto failedRows = inboxCard(fixture.controller.inboxPage(), "channels", stream)
            .value("rows").toList();
        QCOMPARE(failedRows.size(), 1);
        QVERIFY(failedRows.at(0).toMap().value("topic").toString().isEmpty());
        // Close + reopen must re-dispatch the hydration request (#64).
        fixture.controller.closeInbox();
        fixture.controller.showInbox();
        QVERIFY(QTest::qWaitFor([&] { return *topicGets >= 2; }, 4000));
        QVERIFY(QTest::qWaitFor([&] {
            return inboxRow(fixture.controller.inboxPage(), "channels", stream, topicId)
                .value("badge").toString() == QStringLiteral("3");
        }, 4000));
    }

    // Issue #64 acceptance: an unsuccessful later pagination page releases the
    // hydration marker just like a first-page failure.
    void inboxFailedTopicPaginationReleasesHydration()
    {
        Fixture fixture;
        const auto stream = QStringLiteral("a1000000-0000-4000-8000-000000000064");
        const auto lateTopic = QStringLiteral("b1000000-0000-4000-8000-000000000064");
        fixture.streams = [stream](const Request &) { return Response{QJsonArray{
            inboxStream(stream, "Product team", 5, 5, 0)}}; };
        auto cycles = std::make_shared<int>(0);
        auto failedOnce = std::make_shared<bool>(false);
        fixture.topics = [stream, lateTopic, cycles, failedOnce](const Request &request) {
            const QUrlQuery query(request.target);
            if (query.queryItemValue("stream_uuid") != stream)
                return Response{QJsonArray{}};
            const auto marker = query.queryItemValue("page_marker");
            if (marker.isEmpty()) ++*cycles;
            if (*cycles == 1 && !marker.isEmpty()) {
                *failedOnce = true;
                return Response{QJsonArray{}, 500}; // second page fails
            }
            if (marker.isEmpty())
                return Response{QJsonArray{inboxTopic(topicId, stream, "General Topic", 3, 3, 0)},
                    200, 0, {{"X-Pagination-Marker", "p2"}}};
            return Response{QJsonArray{inboxTopic(lateTopic, stream, "Release notes", 2, 2, 0,
                "2026-06-22T11:00:00Z")}};
        };
        loginInbox(fixture, 1);
        fixture.controller.showInbox();
        QVERIFY(QTest::qWaitFor([&] { return *failedOnce; }, 4000));
        QTest::qWait(250);
        // A mid-pagination failure leaves the slot unhydrated: fallback row only.
        const auto failedRows = inboxCard(fixture.controller.inboxPage(), "channels", stream)
            .value("rows").toList();
        QCOMPARE(failedRows.size(), 1);
        QVERIFY(failedRows.at(0).toMap().value("topic").toString().isEmpty());
        // Reopen retries from page one and lands both pages.
        fixture.controller.closeInbox();
        fixture.controller.showInbox();
        QVERIFY(QTest::qWaitFor([&] {
            const auto page = fixture.controller.inboxPage();
            return inboxRow(page, "channels", stream, topicId).contains("badge")
                && inboxRow(page, "channels", stream, lateTopic).contains("badge");
        }, 4000));
    }

    // Issue #64 acceptance: a successful hydration that is still in flight stays
    // deduplicated across a close/reopen cycle (no duplicate collection).
    void inboxInFlightHydrationStaysDeduplicatedOnReopen()
    {
        Fixture fixture;
        const auto stream = QStringLiteral("a1000000-0000-4000-8000-000000000064");
        fixture.streams = [stream](const Request &) { return Response{QJsonArray{
            inboxStream(stream, "Product team", 4, 4, 0)}}; };
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        auto topicGets = std::make_shared<int>(0);
        fixture.topics = [stream, gates, topicGets](const Request &request) {
            if (QUrlQuery(request.target).queryItemValue("stream_uuid") != stream)
                return Response{QJsonArray{}};
            ++*topicGets;
            Response response{QJsonArray{inboxTopic(topicId, stream, "General Topic", 3, 3, 0)}};
            response.defer = [gates](std::function<void()> send) { gates->push_back(std::move(send)); };
            return response;
        };
        loginInbox(fixture, 1);
        fixture.controller.showInbox();
        QVERIFY(QTest::qWaitFor([&] { return *topicGets == 1 && gates->size() == 1; }, 4000));
        fixture.controller.closeInbox();
        fixture.controller.showInbox();
        QTest::qWait(250);
        QCOMPARE(*topicGets, 1); // the in-flight hydration was not re-dispatched
        (*gates)[0]();
        QVERIFY(QTest::qWaitFor([&] {
            return inboxRow(fixture.controller.inboxPage(), "channels", stream, topicId)
                .value("badge").toString() == QStringLiteral("3");
        }, 4000));
    }

    // Issue #64 acceptance: a stale session's late failure must not release the
    // NEW session's in-flight marker, so the new hydration stays deduplicated.
    void inboxStaleHydrationFailureCannotPoisonNewSession()
    {
        Fixture fixture;
        const auto stream = QStringLiteral("a1000000-0000-4000-8000-000000000064");
        fixture.streams = [stream](const Request &) { return Response{QJsonArray{
            inboxStream(stream, "Product team", 4, 4, 0)}}; };
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        auto topicGets = std::make_shared<int>(0);
        fixture.topics = [stream, gates, topicGets](const Request &request) {
            if (QUrlQuery(request.target).queryItemValue("stream_uuid") != stream)
                return Response{QJsonArray{}};
            ++*topicGets;
            Response response{*topicGets == 1 ? QJsonArray{}
                : QJsonArray{inboxTopic(topicId, stream, "General Topic", 3, 3, 0)},
                *topicGets == 1 ? 500 : 200};
            response.defer = [gates](std::function<void()> send) { gates->push_back(std::move(send)); };
            return response;
        };
        loginInbox(fixture, 1);
        fixture.controller.showInbox();
        QVERIFY(QTest::qWaitFor([&] { return *topicGets == 1 && gates->size() == 1; }, 4000));
        fixture.controller.logout();
        QVERIFY(QTest::qWaitFor([&fixture] { return !fixture.controller.authenticated(); }, 4000));
        loginInbox(fixture, 1);
        fixture.controller.showInbox();
        QVERIFY(QTest::qWaitFor([&] { return *topicGets == 2 && gates->size() == 2; }, 4000));
        // The old session's GET fails now, after the new session started its own.
        (*gates)[0]();
        gates->erase(gates->begin());
        QTest::qWait(250);
        fixture.controller.closeInbox();
        fixture.controller.showInbox();
        QTest::qWait(250);
        QCOMPARE(*topicGets, 2); // the stale failure must not free the new marker
        (*gates)[0]();
        QVERIFY(QTest::qWaitFor([&] {
            return inboxRow(fixture.controller.inboxPage(), "channels", stream, topicId)
                .value("badge").toString() == QStringLiteral("3");
        }, 4000));
    }

    // Issue #54: opening a sidebar destination closes the previously opened
    // page, so exactly one page (and one sidebar selection) stays active and
    // closing the active page returns directly to the conversation. Draft
    // contents survive the transitions. The reference routes one destination
    // at a time (workspace_ui 61dacef78dab2f5d0be76f283bebf49fe29b84af router:
    // each of inbox-page.ui.tsx, activity-mentions.ui.tsx and the drafts page
    // replaces the previous destination instead of stacking beside it).
    void draftsMentionsInboxNavigationIsMutuallyExclusive()
    {
        QTemporaryDir draftDirectory; QVERIFY(draftDirectory.isValid());
        NativeDraftService draftStorage(draftDirectory.path()); NativeDrafts drafts(&draftStorage);
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto rows = std::make_shared<QMap<int, QJsonObject>>(
            QMap<int, QJsonObject>{{1, mentionMessage(1, false)}});
        fixture.messages = [rows](const Request &request) { return mentionPageResponse(*rows, request); };
        fixture.controller.useDrafts(&drafts);
        QVERIFY(fixture.start());
        QTRY_VERIFY(drafts.ready(streamId, topicId));
        fixture.controller.setDraft("Kept across navigation");
        QCOMPARE(fixture.controller.draft(), QStringLiteral("Kept across navigation"));

        fixture.controller.showDrafts();
        QVERIFY(fixture.controller.draftsVisible());
        QTRY_VERIFY(!fixture.controller.draftPage().value("rows").toList().isEmpty());
        // Drafts -> Mentions closes the drafts page.
        fixture.controller.showMentions();
        QVERIFY(fixture.controller.mentionsVisible());
        QVERIFY(!fixture.controller.draftsVisible());
        QVERIFY(!fixture.controller.inboxVisible());
        // Mentions -> Drafts closes mentions and the draft survives the round trip.
        fixture.controller.showDrafts();
        QVERIFY(fixture.controller.draftsVisible());
        QVERIFY(!fixture.controller.mentionsVisible());
        QCOMPARE(fixture.controller.draft(), QStringLiteral("Kept across navigation"));
        // Inbox shares the same exclusivity with both pages.
        fixture.controller.showInbox();
        QVERIFY(fixture.controller.inboxVisible());
        QVERIFY(!fixture.controller.draftsVisible());
        fixture.controller.showMentions();
        QVERIFY(fixture.controller.mentionsVisible());
        QVERIFY(!fixture.controller.inboxVisible());
        fixture.controller.showInbox();
        QVERIFY(fixture.controller.inboxVisible());
        QVERIFY(!fixture.controller.mentionsVisible());
        // Closing the one active page returns to the conversation: no page open.
        fixture.controller.closeInbox();
        QVERIFY(!fixture.controller.inboxVisible() && !fixture.controller.draftsVisible()
            && !fixture.controller.mentionsVisible());
        // Draft contents survived every transition.
        QCOMPARE(fixture.controller.draft(), QStringLiteral("Kept across navigation"));
        QTRY_VERIFY(fixture.api.findChildren<QNetworkReply *>().isEmpty());
        drafts.pauseRemote(true);
        bool stopped = false;
        drafts.flush(this, [&stopped](bool) { stopped = true; });
        QTRY_VERIFY(stopped);
        draftStorage.shutdown();
        QTRY_VERIFY(draftStorage.stopped());
    }

    void loginWizardStartsOnOrganizationStep()
    {
        Fixture fixture;
        auto &controller = fixture.controller;
        QCOMPARE(controller.loginStep(), QStringLiteral("organization"));
        QVERIFY(controller.organizationSettings().isEmpty());
        QVERIFY(!controller.organizationLoading());
        QVERIFY(controller.defaultOrganizationUrl().isEmpty());
        QVERIFY(!controller.isOrganizationUrlValid(QStringLiteral("not a url")));
        QVERIFY(!controller.isOrganizationUrlValid(QStringLiteral("https://bad..host")));
        // Reference new URL(...).origin semantics: userinfo and query are
        // stripped by origin normalization, so the address stays valid.
        QVERIFY(controller.isOrganizationUrlValid(QStringLiteral("https://user@acme.example")));
        QVERIFY(controller.isOrganizationUrlValid(QStringLiteral("https://acme.example/with?query")));
        QVERIFY(controller.isOrganizationUrlValid(QStringLiteral("https://acme.example")));
        QVERIFY(controller.isOrganizationUrlValid(fixture.server.origin().toString()));
        controller.continueFromOrganization(QStringLiteral("not a url"));
        QCOMPARE(controller.error(), QStringLiteral("Server address must be a valid HTTPS URL"));
        QCOMPARE(controller.loginStep(), QStringLiteral("organization"));
        QVERIFY(fixture.server.requests.isEmpty());
        QVERIFY(!controller.organizationLoading());
    }

    void loginWizardDiscoveryGatesCredentials()
    {
        Fixture fixture;
        auto &controller = fixture.controller;
        const auto origin = fixture.server.origin().toString();
        controller.organizationInputChanged(origin);
        controller.discoverOrganization(origin);
        QTRY_VERIFY(!controller.organizationLoading());
        QCOMPARE(controller.organizationSettings().value("realmName").toString(), QStringLiteral("Acme Realm"));
        QCOMPARE(controller.organizationSettings().value("realmIconUrl").toString(), origin + "/avatars/realm.png");
        QCOMPARE(controller.organizationSettings().value("registrationUrl").toString(),
                 QStringLiteral("https://reg.example/signup"));
        QCOMPARE(controller.organizationSettings().value("organizationOrigin").toString(), origin);
        // Blur discovery never advances the wizard by itself.
        QCOMPARE(controller.loginStep(), QStringLiteral("organization"));
        controller.continueFromOrganization(origin);
        QCOMPARE(controller.loginStep(), QStringLiteral("credentials"));
        QVERIFY(controller.error().isEmpty());
        // The checked origin advances without a second discovery request.
        const auto discoveryCount = std::count_if(fixture.server.requests.cbegin(),
            fixture.server.requests.cend(), [](const Request &request) {
                return request.target.path().endsWith("/server_settings");
            });
        QCOMPARE(discoveryCount, 1);
    }

    void loginWizardPendingAdvanceCompletesWithResponse()
    {
        Fixture fixture;
        std::function<void()> release;
        fixture.serverSettings = [&release](const Request &) {
            Response response{organizationSettingsDto()};
            response.defer = [&release](std::function<void()> send) { release = std::move(send); };
            return response;
        };
        auto &controller = fixture.controller;
        const auto origin = fixture.server.origin().toString();
        controller.organizationInputChanged(origin);
        controller.continueFromOrganization(origin);
        QCOMPARE(controller.loginStep(), QStringLiteral("organization"));
        QVERIFY(controller.organizationLoading());
        QTRY_VERIFY(static_cast<bool>(release)); // the discovery request must reach the mock first
        QTRY_VERIFY(static_cast<bool>(release));
        release();
        QTRY_VERIFY(!controller.organizationLoading());
        QCOMPARE(controller.loginStep(), QStringLiteral("credentials"));
        QVERIFY(controller.error().isEmpty());
    }

    void loginWizardFailureKeepsCredentialsHidden()
    {
        Fixture fixture;
        std::function<void()> release;
        fixture.serverSettings = [&release](const Request &) {
            Response response{QJsonObject{}, 503};
            response.defer = [&release](std::function<void()> send) { release = std::move(send); };
            return response;
        };
        auto &controller = fixture.controller;
        const auto origin = fixture.server.origin().toString();
        controller.organizationInputChanged(origin);
        controller.continueFromOrganization(origin);
        QTRY_VERIFY(static_cast<bool>(release));
        release();
        QTRY_VERIFY(!controller.organizationLoading());
        QCOMPARE(controller.loginStep(), QStringLiteral("organization"));
        QVERIFY(controller.organizationSettings().isEmpty());
        QCOMPARE(controller.error(),
                 QStringLiteral("Could not load organization settings. Check the server address and try again."));
        const auto discoveryCount = std::count_if(fixture.server.requests.cbegin(),
            fixture.server.requests.cend(), [](const Request &request) {
                return request.target.path().endsWith("/server_settings");
            });
        // A checked failed origin repeats the copy instead of refetching.
        controller.continueFromOrganization(origin);
        QCOMPARE(controller.loginStep(), QStringLiteral("organization"));
        QCOMPARE(controller.error(),
                 QStringLiteral("Could not load organization settings. Check the server address and try again."));
        const auto repeatedCount = std::count_if(fixture.server.requests.cbegin(),
            fixture.server.requests.cend(), [](const Request &request) {
                return request.target.path().endsWith("/server_settings");
            });
        QCOMPARE(repeatedCount, discoveryCount);
    }

    void loginWizardAdvanceNormalizesRealmToOrigin()
    {
        Fixture fixture;
        auto &controller = fixture.controller;
        const auto origin = fixture.server.origin().toString();
        // Discovery and sign-in must both act on the normalized origin, the
        // way the reference submits `new URL(realm).origin` after the check.
        const auto noisy = origin + "/join/team?next=1";
        controller.organizationInputChanged(noisy);
        controller.continueFromOrganization(noisy);
        QTRY_VERIFY(!controller.organizationLoading());
        QCOMPARE(controller.loginStep(), QStringLiteral("credentials"));
        QCOMPARE(controller.organizationRealm(), origin);
        controller.login(controller.organizationRealm(), QStringLiteral("synthetic-user"),
            QStringLiteral("synthetic-password"), QString{});
        QTRY_COMPARE(controller.projects().size(), 1);
    }

    void loginWizardStaleDiscoveryIsIgnored()
    {
        Fixture fixture;
        std::vector<std::function<void()>> releases;
        int call = 0;
        fixture.serverSettings = [&releases, &call](const Request &) {
            Response response{organizationSettingsDto(call == 0
                ? QStringLiteral("First Realm") : QStringLiteral("Second Realm"))};
            response.defer = [&releases, &call](std::function<void()> send) {
                releases.push_back(std::move(send));
                ++call;
            };
            return response;
        };
        auto &controller = fixture.controller;
        const auto origin = fixture.server.origin().toString();
        controller.organizationInputChanged(origin);
        controller.discoverOrganization(origin);
        // A newer check supersedes the in-flight one (reference fetchIdRef guard).
        controller.organizationInputChanged(QStringLiteral("https://other.example"));
        controller.discoverOrganization(origin);
        QTRY_VERIFY(call == 2);
        releases[1]();
        QTRY_VERIFY(!controller.organizationLoading());
        QCOMPARE(controller.organizationSettings().value("realmName").toString(), QStringLiteral("Second Realm"));
        releases[0]();
        QTest::qWait(50);
        QCOMPARE(controller.organizationSettings().value("realmName").toString(), QStringLiteral("Second Realm"));
        QVERIFY(!controller.organizationLoading());
    }

    void loginWizardRealmEditInvalidatesCheck()
    {
        Fixture fixture;
        auto &controller = fixture.controller;
        const auto origin = fixture.server.origin().toString();
        controller.organizationInputChanged(origin);
        controller.discoverOrganization(origin);
        QTRY_VERIFY(!controller.organizationLoading());
        controller.continueFromOrganization(origin);
        QCOMPARE(controller.loginStep(), QStringLiteral("credentials"));
        // Editing the address to a different origin returns to the
        // organization step and discards the completed check.
        MockServer second;
        second.handler = [](const Request &request) {
            if (request.target.path().endsWith("/server_settings"))
                return Response{organizationSettingsDto()};
            return sessionResponse(request);
        };
        const auto otherOrigin = second.origin().toString();
        controller.organizationInputChanged(otherOrigin);
        QCOMPARE(controller.loginStep(), QStringLiteral("organization"));
        QVERIFY(controller.organizationSettings().isEmpty());
        controller.discoverOrganization(otherOrigin);
        QTRY_VERIFY(!controller.organizationLoading());
        QCOMPARE(controller.organizationSettings().value("organizationOrigin").toString(), otherOrigin);
        QCOMPARE(controller.organizationSettings().value("realmIconUrl").toString(),
                 otherOrigin + "/avatars/realm.png");
    }

    void defaultOrganizationShortcutFillsRealmAndClearsState()
    {
        qputenv("WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_URL", "https://public.example");
        qputenv("WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_NAME", "Public Test Org");
        Fixture fixture;
        auto &controller = fixture.controller;
        QCOMPARE(controller.defaultOrganizationUrl(), QStringLiteral("https://public.example"));
        QCOMPARE(controller.defaultOrganizationName(), QStringLiteral("Public Test Org"));
        const auto origin = fixture.server.origin().toString();
        controller.organizationInputChanged(origin);
        controller.discoverOrganization(origin);
        QTRY_VERIFY(!controller.organizationLoading());
        controller.continueFromOrganization(origin);
        QCOMPARE(controller.loginStep(), QStringLiteral("credentials"));
        controller.selectDefaultOrganization();
        QCOMPARE(controller.organizationRealm(), QStringLiteral("https://public.example"));
        QVERIFY(controller.organizationSettings().isEmpty());
        QVERIFY(controller.error().isEmpty());
        QCOMPARE(controller.loginStep(), QStringLiteral("organization"));
        qunsetenv("WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_URL");
        qunsetenv("WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_NAME");
    }

    void defaultOrganizationNameFallsBackToTranslatedCopy()
    {
        qputenv("WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_URL", "https://public.example");
        qunsetenv("WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_NAME");
        Fixture fixture;
        QCOMPARE(fixture.controller.defaultOrganizationName(), QStringLiteral("Exordos Core Public"));
        qunsetenv("WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_URL");
    }

    void loginRecordsOrganizationHistory()
    {
        QTemporaryDir historyDirectory;
        QVERIFY(historyDirectory.isValid());
        const auto storePath = historyDirectory.filePath("history.ini");
        NativePreferences preferences(storePath);
        Fixture fixture;
        auto &controller = fixture.controller;
        controller.setPreferences(&preferences);
        const auto origin = fixture.server.origin().toString();
        controller.login(origin, QStringLiteral("synthetic-user"), QStringLiteral("synthetic-password"), QString{});
        QTRY_COMPARE(controller.projects().size(), 1);
        controller.openProject(projectId);
        QTRY_COMPARE(preferences.organizationHistory().size(), 1);
        QCOMPARE(preferences.organizationHistory().first(), QUrl(origin).toString());
        // Path fragments normalize to the origin and dedupe to the freshest slot.
        preferences.recordOrganizationOrigin(QUrl(origin).toString() + "/some/path");
        QCOMPARE(preferences.organizationHistory().size(), 1);
        NativePreferences reopened(storePath);
        QCOMPARE(reopened.organizationHistory(), preferences.organizationHistory());
    }

    // ---- desktop notifications (issue #3, docs/NATIVE_DESKTOP_NOTIFICATIONS.md) ----

    // The fixture auto-opens its first conversation; notification candidates
    // must therefore target a background conversation. Open the second topic
    // so messages in topicId are off-screen candidates everywhere below.
    // Detail endpoints must serve parse-valid DTOs for the route to resolve.
    void openBackgroundConversation(Fixture &fixture)
    {
        Q_UNUSED(fixture);
        fixture.controller.selectTopic(secondTopicId);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.selectedTopic() == secondTopicId, 2000);
    }

    void notificationPolicyMatrix_data()
    {
        QTest::addColumn<QString>("row");
        QTest::addColumn<bool>("notify");
        QTest::addColumn<QString>("trigger");
        QTest::newRow("dm") << "dm" << true << QStringLiteral("dm");
        QTest::newRow("own") << "own" << false << QString();
        QTest::newRow("read") << "read" << false << QString();
        QTest::newRow("mention-default-mode") << "mention-default" << true << QStringLiteral("mention");
        QTest::newRow("mention-mentions-only") << "mention-mentions-only" << true << QStringLiteral("mention");
        QTest::newRow("wildcard") << "wildcard" << true << QStringLiteral("wildcard_mention");
        QTest::newRow("followed-topic") << "followed-topic" << true << QStringLiteral("followed_topic");
        QTest::newRow("plain-stream-default-mode") << "plain-stream" << false << QStringLiteral("stream");
        QTest::newRow("plain-stream-all-messages") << "plain-stream-all" << true << QStringLiteral("stream");
        QTest::newRow("stream-muted") << "stream-muted" << false << QString();
        QTest::newRow("stream-muted-mention") << "stream-muted-mention" << false << QString();
        QTest::newRow("topic-mute-wins-over-stream-all") << "topic-mute" << false << QString();
        QTest::newRow("follow-unmutes-muted-stream") << "follow-unmute" << true << QStringLiteral("followed_topic");
        QTest::newRow("focused-on-screen") << "focused-on-screen" << false << QString();
        QTest::newRow("focused-off-screen") << "focused-off-screen" << true << QStringLiteral("stream");
    }
    void notificationPolicyMatrix()
    {
        QFETCH(QString, row);
        QFETCH(bool, notify);
        QFETCH(QString, trigger);
        NotificationCandidate c;
        c.owner = QStringLiteral("owner");
        c.messageUuid = QStringLiteral("m1");
        c.streamUuid = streamId;
        c.topicUuid = topicId;
        c.authorUuid = notificationAuthor;
        c.content = QStringLiteral("Ping");
        c.streamMode = QStringLiteral("default");
        c.topicMode = QStringLiteral("default");
        if (row == "dm") c.streamPrivate = true;
        else if (row == "own") c.isOwn = true;
        else if (row == "read") c.read = true;
        else if (row == "mention-default") c.mentioned = true;
        else if (row == "mention-mentions-only") { c.mentioned = true; c.streamMode = QStringLiteral("mentions_only"); }
        else if (row == "wildcard") c.content = QStringLiteral("standup @all tomorrow");
        else if (row == "followed-topic") c.topicMode = QStringLiteral("follow");
        else if (row == "plain-stream-all") c.streamMode = QStringLiteral("all_messages");
        else if (row == "stream-muted") c.streamMode = QStringLiteral("muted");
        else if (row == "stream-muted-mention") { c.streamMode = QStringLiteral("muted"); c.mentioned = true; }
        else if (row == "topic-mute") { c.streamMode = QStringLiteral("all_messages"); c.topicMode = QStringLiteral("mute"); }
        else if (row == "follow-unmute") { c.streamMode = QStringLiteral("muted"); c.topicMode = QStringLiteral("follow"); }
        else if (row == "focused-on-screen") { c.viewport.windowFocused = true; c.viewport.conversationOnScreen = true; }
        else if (row == "focused-off-screen") { c.streamMode = QStringLiteral("all_messages"); c.viewport.windowFocused = true; }
        const auto decision = notification_policy::shouldNotify(c, false);
        QCOMPARE(decision.notify, notify);
        if (!trigger.isEmpty()) QCOMPARE(decision.trigger, trigger);
    }

    void notificationTitleFormatsFollowTheReference()
    {
        const QString mid = QString::fromUtf8(" \xC2\xB7 ");
        NotificationTitleContext dm;
        dm.direct = true;
        dm.senderName = QStringLiteral("X");
        dm.conversationName = QStringLiteral("Personal notes");
        // Count wording is delegated to Qt plural catalogs (the locale guard
        // verifies the catalog); the structural contract is count + place.
        QCOMPARE(NotificationService::formatTitle(dm, 1), QStringLiteral("X"));
        QCOMPARE(NotificationService::formatTitle(dm, 2),
            QStringLiteral("2 messages") + mid + QStringLiteral("Personal notes"));
        NotificationTitleContext channel;
        channel.senderName = QStringLiteral("X");
        channel.channelName = QStringLiteral("Native channel");
        channel.topicName = QStringLiteral("Native topic");
        QCOMPARE(NotificationService::formatTitle(channel, 2),
            QStringLiteral("2 messages from X") + mid + QStringLiteral("Native channel")
                + mid + QStringLiteral("Native topic"));
        channel.topicName.clear();
        QCOMPARE(NotificationService::formatTitle(channel, 1), QStringLiteral("X") + mid + QStringLiteral("Native channel"));
        // Electron caps notification text at 200 characters (main.ts).
        NotificationTitleContext huge;
        huge.channelName = QString(300, QChar('x'));
        QVERIFY(NotificationService::formatTitle(huge, 1).size() <= 200);
    }

    void twoBackgroundMessagesFromOneAuthorShareOneTag()
    {
        Fixture fixture;
        RecordingNotifications sent;
        fixture.streams = [](const Request &) { return Response{QJsonArray{notificationStreamDto()}}; };
        fixture.topics = [](const Request &) {
            return Response{QJsonArray{notificationTopicDto(),
                notificationTopicDto(secondTopicId, QStringLiteral("Second topic"))}};
        };
        QVERIFY(fixture.start());
        fixture.controller.notifications()->setTransport(&sent);
        openBackgroundConversation(fixture);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("user.updated",
            QJsonObject{{"uuid", notificationAuthor}, {"project_id", projectId},
                        {"username", "author-x"}, {"first_name", "X"},
                        {"updated_at", "2026-01-02T10:00:00Z"}}, 12)));
        const auto first = incomingDto(historyMessage(500), notificationAuthor);
        const auto second = incomingDto(historyMessage(501), notificationAuthor);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", first, 13)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", second, 14)));
        const QString mid = QString::fromUtf8(" \xC2\xB7 ");
        const QString place = QStringLiteral("Native channel") + mid + QStringLiteral("Native topic");
        QCOMPARE(sent.shows.size(), 2);
        QCOMPARE(sent.shows[0].title, QStringLiteral("X") + mid + place);
        QCOMPARE(sent.shows[0].body, QStringLiteral("Message 500"));
        QCOMPARE(sent.shows[1].title, QStringLiteral("2 messages from X") + mid + place);
        QCOMPARE(sent.shows[1].body, QStringLiteral("Message 501"));
        QCOMPARE(sent.shows[1].tag, sent.shows[0].tag);
        // Re-delivering the same event changes nothing (processed marker + dedup).
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", first, 15)));
        QCOMPARE(sent.shows.size(), 2);
        // Same author, a different background topic: separate bucket, separate
        // tag. (secondTopicId is the visible conversation here, so the message
        // must target a third topic to stay an off-screen candidate.)
        auto otherTopic = incomingDto(historyMessage(502), notificationAuthor);
        otherTopic["topic_uuid"] = QStringLiteral("33333333-3333-4333-8333-333333333333");
        fixture.topics = [](const Request &) {
            return Response{QJsonArray{notificationTopicDto(),
                notificationTopicDto(secondTopicId, "Second topic"),
                notificationTopicDto(QStringLiteral("33333333-3333-4333-8333-333333333333"),
                    QStringLiteral("Third topic"))}};
        };
        fixture.controller.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.topics().size() == 3, 4000);
        QCOMPARE(fixture.controller.selectedTopic(), secondTopicId);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", otherTopic, 16)));
        QCOMPARE(sent.shows.size(), 3);
        QVERIFY(sent.shows[2].tag != sent.shows[0].tag);
        fixture.controller.notifications()->setTransport(nullptr);
    }

    void readingTheNewestNotificationMessageClosesTheTag()
    {
        Fixture fixture;
        RecordingNotifications sent;
        fixture.streams = [](const Request &) { return Response{QJsonArray{notificationStreamDto()}}; };
        fixture.topics = [](const Request &) {
            return Response{QJsonArray{notificationTopicDto(),
                notificationTopicDto(secondTopicId, QStringLiteral("Second topic"))}};
        };
        QVERIFY(fixture.start());
        auto *service = fixture.controller.notifications();
        service->setTransport(&sent);
        openBackgroundConversation(fixture);
        const auto first = incomingDto(historyMessage(500), notificationAuthor);
        const auto second = incomingDto(historyMessage(501), notificationAuthor);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", first, 13)));
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", second, 14)));
        const auto tag = sent.lastTag();
        QCOMPARE(service->aggregateCount(tag), 2);
        // Reading the older message keeps the tag and shrinks the count.
        auto readOlder = first;
        readOlder["read"] = true;
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.read", readOlder, 15)));
        QVERIFY(!sent.closed.contains(tag));
        QCOMPARE(service->aggregateCount(tag), 1);
        // Reading the newest message closes the whole tag.
        auto readNewest = second;
        readNewest["read"] = true;
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.read", readNewest, 16)));
        QVERIFY(sent.closed.contains(tag));
        QCOMPARE(service->aggregateCount(tag), 0);
        // A read for a message nobody notified about closes its fallback tag
        // (untracked uuids fall back to the msg: scheme in the reference).
        auto stray = incomingDto(historyMessage(503), notificationAuthor);
        auto strayRead = stray;
        strayRead["read"] = true;
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.read", strayRead, 17)));
        const auto owner = tag.section(QStringLiteral("::"), 0, 0).mid(7); // strip "bucket:"
        QVERIFY(sent.closed.contains(
            NotificationAggregateRegistry::fallbackTag(owner, stray.value(QStringLiteral("uuid")).toString())));
        QCOMPARE(service->aggregateCount(tag), 0);
        service->setTransport(nullptr);
    }

    void readingBulkReadEventsClosesNotificationTags()
    {
        Fixture fixture;
        RecordingNotifications sent;
        fixture.streams = [](const Request &) { return Response{QJsonArray{notificationStreamDto()}}; };
        fixture.topics = [](const Request &) {
            return Response{QJsonArray{notificationTopicDto(),
                notificationTopicDto(secondTopicId, QStringLiteral("Second topic"))}};
        };
        QVERIFY(fixture.start());
        auto *service = fixture.controller.notifications();
        service->setTransport(&sent);
        openBackgroundConversation(fixture);
        const auto only = incomingDto(historyMessage(500), notificationAuthor);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", only, 13)));
        const auto tag = sent.lastTag();
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("messages.read",
            QJsonObject{{"project_id", projectId}, {"message_uuids", QJsonArray{only.value("uuid").toString()}}}, 14)));
        QVERIFY(sent.closed.contains(tag));
        QCOMPARE(service->aggregateCount(tag), 0);
        service->setTransport(nullptr);
    }

    void focusedConversationOnScreenSuppressesNotifications()
    {
        Fixture fixture;
        RecordingNotifications sent;
        fixture.streams = [](const Request &) { return Response{QJsonArray{notificationStreamDto()}}; };
        fixture.topics = [](const Request &) {
            return Response{QJsonArray{notificationTopicDto(),
                notificationTopicDto(secondTopicId, "Second topic")}};
        };
        QVERIFY(fixture.start());
        fixture.controller.notifications()->setTransport(&sent);
        fixture.controller.selectTopic(secondTopicId);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.selectedTopic() == secondTopicId, 2000);
        fixture.controller.setWindowFocused(true);
        // The open conversation never notifies, even for fresh unread arrivals.
        auto inView = incomingDto(historyMessage(500), notificationAuthor);
        inView["topic_uuid"] = secondTopicId;
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", inView, 13)));
        QVERIFY(sent.shows.isEmpty());
        // A different conversation does notify while focused, without attention.
        auto off = incomingDto(historyMessage(501), notificationAuthor);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", off, 14)));
        QCOMPARE(sent.shows.size(), 1);
        QCOMPARE(sent.attentionRequests, 0);
        fixture.controller.notifications()->setTransport(nullptr);
    }

    void genericNotificationFiresAfterTheMetadataGrace()
    {
        Fixture fixture;
        RecordingNotifications sent;
        QVERIFY(fixture.start());
        auto *service = fixture.controller.notifications();
        service->setTransport(&sent);
        service->setMetadataGrace(120, 40);
        // The candidate stream is not in the loaded catalog at all, so
        // metadata cannot resolve and the generic fallback must fire.
        auto stray = incomingDto(historyMessage(600), notificationAuthor);
        stray["stream_uuid"] = QStringLiteral("9e9e9e9e-9e9e-4e9e-8e9e-9e9e9e9e9e9e");
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", stray, 13)));
        QVERIFY(sent.shows.isEmpty()); // inside the grace window
        QTRY_VERIFY_WITH_TIMEOUT(sent.shows.size() == 1, 3000);
        QCOMPARE(sent.shows.first().title, QStringLiteral("You have a new message"));
        QVERIFY(!sent.shows.first().body.contains(QStringLiteral("://")));
        QCOMPARE(sent.shows.first().body, QString::fromUtf8("synthetic-user \xC2\xB7 127.0.0.1"));
        QTRY_VERIFY(sent.shows.size() == 1);
        service->setTransport(nullptr);
    }

    void attentionRequestFollowsWindowFocus()
    {
        Fixture fixture;
        RecordingNotifications sent;
        fixture.streams = [](const Request &) { return Response{QJsonArray{notificationStreamDto()}}; };
        fixture.topics = [](const Request &) {
            return Response{QJsonArray{notificationTopicDto(),
                notificationTopicDto(secondTopicId, QStringLiteral("Second topic"))}};
        };
        QVERIFY(fixture.start());
        auto *service = fixture.controller.notifications();
        service->setTransport(&sent);
        openBackgroundConversation(fixture);
        fixture.controller.setWindowFocused(false);
        auto first = incomingDto(historyMessage(500), notificationAuthor);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", first, 13)));
        QCOMPARE(sent.shows.size(), 1);
        QCOMPARE(sent.attentionRequests, 1);
        QCOMPARE(sent.attentionClears, 0);
        // Gaining focus clears the indication once; a focused delivery asks no more.
        fixture.controller.setWindowFocused(true);
        QCOMPARE(sent.attentionClears, 1);
        auto second = incomingDto(historyMessage(501), notificationAuthor);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", second, 14)));
        QCOMPARE(sent.shows.size(), 2);
        QCOMPARE(sent.attentionRequests, 1);
        service->setTransport(nullptr);
    }

    void notificationClickOpensTheConversationAndRetiresTheTag()
    {
        Fixture fixture;
        RecordingNotifications sent;
        fixture.streams = [](const Request &) { return Response{QJsonArray{notificationStreamDto()}}; };
        fixture.topics = [](const Request &) {
            return Response{QJsonArray{notificationTopicDto(),
                notificationTopicDto(secondTopicId, QStringLiteral("Second topic"))}};
        };
        QVERIFY(fixture.start());
        auto *service = fixture.controller.notifications();
        service->setTransport(&sent);
        openBackgroundConversation(fixture);
        auto arrival = incomingDto(historyMessage(500), notificationAuthor);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", arrival, 13)));
        QCOMPARE(sent.shows.size(), 1);
        const auto tag = sent.lastTag();
        service->click(tag);
        QVERIFY(sent.closed.contains(tag));
        QCOMPARE(service->aggregateCount(tag), 0);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.selectedStream() == streamId
            && fixture.controller.selectedTopic() == topicId, 3000);
        // Clicking a dead tag does nothing further.
        service->click(tag);
        QCOMPARE(sent.shows.size(), 1);
        service->setTransport(nullptr);
    }

    void dbusNotifyCallMatchesTheFreedesktopSignature()
    {
        auto bus = QDBusConnection::sessionBus();
        if (!bus.isConnected())
            QSKIP("the session bus is unavailable");
        FakeFreedesktopNotifications daemon;
        new FakeFreedesktopNotificationsAdaptor(&daemon);
        if (!bus.registerObject(QStringLiteral("/org/freedesktop/Notifications"), &daemon))
            QSKIP("cannot export the fake notification service");
        if (!bus.registerService(QStringLiteral("org.freedesktop.Notifications")))
            QSKIP("a real notification daemon owns org.freedesktop.Notifications");
        workspace::DBusNotificationTransport transport([] { return quintptr{}; });
        QVERIFY(transport.show(QStringLiteral("tag:signature"), QStringLiteral("Title text"),
            QStringLiteral("Body text")));
        QCOMPARE(daemon.notifyCalls.size(), 1);
        const auto &call = daemon.notifyCalls.first();
        QCOMPARE(call.summary, QStringLiteral("Title text"));
        QCOMPARE(call.body, QStringLiteral("Body text"));
        QCOMPARE(call.replacesId, 0u);
        // Click routing only survives daemons that do not synthesize the
        // implicit default action when the client declares it explicitly.
        QVERIFY(call.actions.contains(QStringLiteral("default")));
        transport.closeByTag(QStringLiteral("tag:signature"));
        QTRY_VERIFY_WITH_TIMEOUT(daemon.closedIds.size() == 1, 2000);
        bus.unregisterObject(QStringLiteral("/org/freedesktop/Notifications"));
        bus.unregisterService(QStringLiteral("org.freedesktop.Notifications"));
    }

    void mutedBackgroundTopicSilencesNotifications()
    {
        Fixture fixture;
        RecordingNotifications sent;
        fixture.streams = [](const Request &) { return Response{QJsonArray{notificationStreamDto()}}; };
        fixture.counters = [](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path == "/api/workspace/v1/messenger/stream_topics/" + secondTopicId)
                return Response{notificationTopicDto(secondTopicId,
                    QStringLiteral("Quiet topic"), QStringLiteral("mute"))};
            if (path == "/api/workspace/v1/messenger/stream_topics/" + topicId)
                return Response{notificationTopicDto()};
            return Response{notificationStreamDto()};
        };
        QVERIFY(fixture.start()); // the visible conversation is topicId
        auto *service = fixture.controller.notifications();
        service->setTransport(&sent);
        service->setMetadataGrace(2000, 150);
        auto arrival = incomingDto(historyMessage(500), notificationAuthor);
        // Background topic outside the loaded catalog: the mute state is only
        // visible through the detail endpoint, which the resolver must consult.
        arrival["topic_uuid"] = secondTopicId;
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", arrival, 13)));
        const auto catalogSize = fixture.controller.topics().size();
        QTest::qWait(2600); // beyond the grace window a misjudged candidate would have shown
        QVERIFY(sent.shows.isEmpty());
        // The backfilled topic must never reshape the active catalog.
        QCOMPARE(fixture.controller.topics().size(), catalogSize);
        service->setTransport(nullptr);
    }

    void signOutClosesActiveNotifications()
    {
        Fixture fixture;
        RecordingNotifications sent;
        fixture.streams = [](const Request &) { return Response{QJsonArray{notificationStreamDto()}}; };
        fixture.topics = [](const Request &) {
            return Response{QJsonArray{notificationTopicDto(),
                notificationTopicDto(secondTopicId, QStringLiteral("Second topic"))}};
        };
        QVERIFY(fixture.start());
        auto *service = fixture.controller.notifications();
        service->setTransport(&sent);
        openBackgroundConversation(fixture);
        auto arrival = incomingDto(historyMessage(500), notificationAuthor);
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", arrival, 13)));
        QCOMPARE(sent.shows.size(), 1);
        const auto tag = sent.lastTag();
        fixture.controller.logout();
        // Banners must not outlive the account that produced them.
        QVERIFY(sent.closed.contains(tag));
        service->setTransport(nullptr);
    }

    void notificationStateStaysBounded()
    {
        Fixture fixture;
        RecordingNotifications sent;
        QVERIFY(fixture.start()); // visible topicId suppresses candidates, decisions recorded
        auto *service = fixture.controller.notifications();
        service->setTransport(&sent);
        for (int i = 0; i < 300; ++i) {
            const auto dto = incomingDto(historyMessage(600 + i), notificationAuthor);
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", dto, 1000 + i)));
        }
        QCOMPARE(sent.shows.size(), 0);
        QVERIFY(service->processedCount() <= 2048);
        // Unknown-stream candidates hold on the grace window: that store is
        // bounded too (a hostile sender cannot exhaust client memory).
        service->setMetadataGrace(60000, 50);
        for (int i = 0; i < 100; ++i) {
            auto dto = incomingDto(historyMessage(1600 + i), notificationAuthor);
            dto["stream_uuid"] = QStringLiteral("99999999-9999-4999-8999-999999999999");
            dto["topic_uuid"] = QStringLiteral("99999999-9999-4999-8999-999999999999");
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", dto, 5000 + i)));
        }
        QVERIFY(service->pendingCount() <= 64);
        service->setTransport(nullptr);
    }

    void aggregateEvictionClosesTheEvictedBanner()
    {
        // The aggregate store is bounded; an evicted bucket must not leave its
        // already-shown desktop banner open untracked forever.
        RecordingNotifications sent;
        NotificationService service;
        service.setTransport(&sent);
        for (int i = 0; i < 132; ++i) {
            NotificationCandidate candidate;
            candidate.owner = QStringLiteral("owner-eviction");
            candidate.messageUuid =
                QStringLiteral("aaaa0000-0000-4000-8000-%1").arg(i, 12, 10, QLatin1Char('0'));
            candidate.streamUuid = QStringLiteral("stream-evict");
            candidate.topicUuid = QStringLiteral("topic-evict");
            candidate.authorUuid = candidate.messageUuid;
            candidate.content = QStringLiteral("Eviction probe %1").arg(i);
            candidate.senderName = QStringLiteral("Author %1").arg(i);
            candidate.topicName = QStringLiteral("Evicting");
            candidate.streamMode = QStringLiteral("all_messages");
            candidate.topicMode = QStringLiteral("default");
            candidate.streamKnown = true;
            candidate.topicKnown = true;
            candidate.createdMicros = 480LL * 1000000 + i;
            service.ingest(candidate);
        }
        QVERIFY(sent.shows.size() >= 129);
        QVERIFY2(!sent.closed.isEmpty(),
                 "eviction must tag-close the evicted desktop banner");
    }

    void aggregateEvictionKeepsTheNewestTiedRow()
    {
        // Eviction must never drop the message the banner just displayed,
        // even when every row shares one timestamp (burst of same-millisecond
        // messages). Pre-fix the tie winner was hash iteration order, so the
        // fresh row could be evicted immediately (banner body goes stale).
        RecordingNotifications sent;
        NotificationService service;
        service.setTransport(&sent);
        QString newestUuid;
        for (int i = 0; i < 66; ++i) {
            NotificationCandidate candidate;
            candidate.owner = QStringLiteral("owner-tie");
            newestUuid = QStringLiteral("ffff0000-0000-4000-8000-%1")
                             .arg(65 - i, 12, 10, QLatin1Char('0'));
            candidate.messageUuid = newestUuid;
            candidate.streamUuid = QStringLiteral("stream-tie");
            candidate.topicUuid = QStringLiteral("topic-tie");
            candidate.authorUuid = QStringLiteral("eeee0000-0000-4000-8000-eeeeeeeeeeee");
            candidate.content = QStringLiteral("Tied body %1").arg(i);
            candidate.senderName = QStringLiteral("Tie");
            candidate.topicName = QStringLiteral("Ties");
            candidate.streamMode = QStringLiteral("all_messages");
            candidate.topicMode = QStringLiteral("default");
            candidate.streamKnown = true;
            candidate.topicKnown = true;
            candidate.createdMicros = 480LL * 1000000; // identical timestamps
            service.ingest(candidate);
        }
        const auto tag = sent.lastTag();
        QVERIFY(!tag.isEmpty());
        // The newest message must still be displayable (its body is not the
        // evicted one): the count kept growing and rows stayed at the bound.
        QCOMPARE(service.aggregateCount(tag), 66);
        QCOMPARE(service.aggregateRowCount(tag), 64);
        service.setTransport(nullptr);
    }

    void signOutClosesGenericFallbackBanners()
    {
        // Generic fallback banners (metadata unresolved past the grace window)
        // carry the msg: tag scheme; sign-out must close them too.
        Fixture fixture;
        RecordingNotifications sent;
        fixture.streams = [](const Request &) { return Response{QJsonArray{notificationStreamDto()}}; };
        fixture.topics = [](const Request &) {
            return Response{QJsonArray{notificationTopicDto(),
                notificationTopicDto(secondTopicId, QStringLiteral("Second topic"))}};
        };
        QVERIFY(fixture.start());
        auto *service = fixture.controller.notifications();
        service->setTransport(&sent);
        service->setMetadataGrace(150, 30);
        openBackgroundConversation(fixture);
        auto arrival = incomingDto(historyMessage(520), notificationAuthor);
        arrival["stream_uuid"] = QStringLiteral("99999999-9999-4999-8999-999999999998");
        arrival["topic_uuid"] = QStringLiteral("99999999-9999-4999-8999-999999999998");
        QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", arrival, 21)));
        QTest::qWait(600); // grace (150 ms) + retry tick elapse, generic banner shows
        QVERIFY(!sent.shows.isEmpty());
        const auto tag = sent.lastTag();
        fixture.controller.logout();
        QVERIFY2(sent.closed.contains(tag),
                 "sign-out must tag-close generic fallback banners");
        service->setTransport(nullptr);
    }

    void aggregateRowsPerTagAreBounded()
    {
        // The aggregate count may grow freely, but one bucket must not retain
        // an unbounded number of message bodies on the client.
        RecordingNotifications sent;
        NotificationService service;
        service.setTransport(&sent);
        for (int i = 0; i < 70; ++i) {
            NotificationCandidate candidate;
            candidate.owner = QStringLiteral("owner-rows");
            candidate.messageUuid =
                QStringLiteral("bbbb0000-0000-4000-8000-%1").arg(i, 12, 10, QLatin1Char('0'));
            candidate.streamUuid = QStringLiteral("stream-rows");
            candidate.topicUuid = QStringLiteral("topic-rows");
            candidate.authorUuid = QStringLiteral("cccc0000-0000-4000-8000-cccccccccccc");
            candidate.content = QStringLiteral("Body %1").arg(i);
            candidate.senderName = QStringLiteral("Rower");
            candidate.topicName = QStringLiteral("Rows");
            candidate.streamMode = QStringLiteral("all_messages");
            candidate.topicMode = QStringLiteral("default");
            candidate.streamKnown = true;
            candidate.topicKnown = true;
            candidate.createdMicros = 480LL * 1000000 + i;
            service.ingest(candidate);
        }
        const auto tag = sent.lastTag();
        QVERIFY(!tag.isEmpty());
        QCOMPARE(service.aggregateCount(tag), 70);
        QCOMPARE(service.aggregateRowCount(tag), 64);
        service.setTransport(nullptr);
    }

    void genericFallbackOverflowClosesOldestBanners()
    {
        // Beyond the tracking bound the oldest generic banner is closed to
        // free a slot; no shown banner may end up untracked after sign-out.
        Fixture fixture;
        RecordingNotifications sent;
        fixture.streams = [](const Request &) { return Response{QJsonArray{notificationStreamDto()}}; };
        fixture.topics = [](const Request &) {
            return Response{QJsonArray{notificationTopicDto(),
                notificationTopicDto(secondTopicId, QStringLiteral("Second topic"))}};
        };
        QVERIFY(fixture.start());
        auto *service = fixture.controller.notifications();
        service->setTransport(&sent);
        service->setMetadataGrace(40, 10);
        openBackgroundConversation(fixture);
        for (int i = 0; i < 300; ++i) {
            auto dto = incomingDto(historyMessage(4000 + i), notificationAuthor);
            dto["stream_uuid"] = QStringLiteral("99999999-9999-4999-8999-999999999997");
            dto["topic_uuid"] = QStringLiteral("99999999-9999-4999-8999-999999999997");
            QVERIFY(fixture.controller.applyRealtimeEvent(controllerEvent("message.created", dto, 9000 + i)));
            if (i % 40 == 39) QTest::qWait(300);
        }
        QVERIFY(sent.shows.size() > 256);
        fixture.controller.logout();
        const QSet<QString> closedSet(sent.closed.begin(), sent.closed.end());
        for (const auto &show : sent.shows)
            QVERIFY2(closedSet.contains(show.tag),
                     qPrintable(QStringLiteral("banner %1 was never closed").arg(show.tag)));
        service->setTransport(nullptr);
    }

    // docs/NATIVE_DESKTOP_SIDEBAR_PREVIEWS.md (issue #2): the reference preview line is
    // whitespace-normalised and clamped to 120 characters with an ASCII "..." marker
    // (workspace_ui 61dacef entities/messenger/messenger-topic-list.lib.ts:61-92 with
    // DEFAULT_WORKSPACE_MESSAGE_SUMMARY_OPTIONS maxLength 120).
    void sidebarPreviewSummaryCollapsesAndClamps()
    {
        QCOMPARE(NativeController::summarizeRowPreview(QString()), QString());
        QCOMPARE(NativeController::summarizeRowPreview(QStringLiteral("  a\n\n  b  ")),
            QStringLiteral("a b"));
        const QString exact(120, QLatin1Char('y'));
        QCOMPARE(NativeController::summarizeRowPreview(exact), exact);
        const auto clamped = NativeController::summarizeRowPreview(QString(140, QLatin1Char('x')));
        QCOMPARE(clamped.size(), 120);
        QVERIFY2(clamped.endsWith(QStringLiteral("...")), qPrintable(clamped));
        QCOMPARE(clamped.left(117), QString(117, QLatin1Char('x')));
    }

    // workspace_ui 61dacef shared/lib/datetime.lib.ts:65-82 formatMessageTimeRelative:
    // same local day -> HH:mm, previous local day -> the yesterday label alone (no clock),
    // anything older -> the compact day + short month with no year.
    void sidebarRowTimeFollowsReferenceFormatter()
    {
        const QLocale english(QLocale::English);
        const QDate today(2026, 11, 11);
        const QString yesterday = QStringLiteral("Yesterday");
        QCOMPARE(NativeController::formatRowTime(
            QDateTime(QDate(2026, 11, 11), QTime(9, 5)), english, today, yesterday),
            QStringLiteral("09:05"));
        QCOMPARE(NativeController::formatRowTime(
            QDateTime(QDate(2026, 11, 11), QTime(23, 59)), english, today, yesterday),
            QStringLiteral("23:59"));
        QCOMPARE(NativeController::formatRowTime(
            QDateTime(QDate(2026, 11, 10), QTime(23, 59)), english, today, yesterday),
            yesterday);
        QCOMPARE(NativeController::formatRowTime(
            QDateTime(QDate(2026, 11, 5), QTime(0, 0)), english, today, yesterday),
            QStringLiteral("Nov 5"));
        // The compact style carries no year, so a date one year back keeps the same shape.
        QCOMPARE(NativeController::formatRowTime(
            QDateTime(QDate(2025, 11, 5), QTime(0, 0)), english, today, yesterday),
            QStringLiteral("Nov 5"));
    }

    // workspace_ui 61dacef entities/messenger/messenger-sidebar.lib.ts:236-290: the default
    // sort mode is last_message and the comparator is group rank then activity descending,
    // with unknown activity last. Native has no stream pin DTO yet (issue #98).
    void sidebarStreamsSortByActivityDescending()
    {
        Fixture test;
        const auto second = QStringLiteral("33333333-3333-4333-8333-444444444444");
        const auto third = QStringLiteral("33333333-3333-4333-8333-555555555555");
        const auto streamRow = [](const QString &uuid, const QString &name, const QString &updatedAt) {
            QJsonObject row{{"uuid", uuid}, {"project_id", projectId}, {"name", name},
                {"encryption", false}, {"unread_count", 0}, {"active_unread_count", 0},
                {"passive_unread_count", 0}, {"last_message_uuid", QJsonValue::Null}};
            if (!updatedAt.isEmpty()) row["updated_at"] = updatedAt;
            return row;
        };
        test.streams = [&](const Request &) {
            return Response{QJsonArray{
                streamRow(streamId, "Oldest", QStringLiteral("2026-01-01T10:00:00Z")),
                streamRow(second, "Newest", QStringLiteral("2026-01-03T10:00:00Z")),
                streamRow(third, "Undated", QString())}};
        };
        test.controller.login(test.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        if (!QTest::qWaitFor([&] { return test.controller.projects().size() == 1; }, 2000)) { QFAIL("project list"); return; }
        test.controller.openProject(projectId);
        if (!QTest::qWaitFor([&] { return test.controller.streams().size() == 3; }, 2000)) { QFAIL("stream list"); return; }
        QStringList order;
        for (const auto &row : test.controller.streams())
            order.append(row.toMap().value("name").toString());
        const QStringList expected{"Newest", "Oldest", "Undated"};
        QCOMPARE(order, expected);
    }

    // A hydrated last message gives the row its sender, the summary and the row time; the
    // own-message sender is common.you (workspace_ui messenger-topic-list.lib.ts:169-172).
    void sidebarRowsExposePreviewAndRowTime()
    {
        Fixture test;
        const auto last = QStringLiteral("88888888-8888-4888-8888-000000000abc");
        test.streams = [&](const Request &request) {
            if (request.method == "GET" && request.target.path().endsWith("/streams/"))
                return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Native channel"}, {"encryption", false}, {"unread_count", 0},
                    {"active_unread_count", 0}, {"passive_unread_count", 0},
                    {"last_message_uuid", last}, {"updated_at", "2026-01-02T10:00:05Z"}}}};
            return Response{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                {"name", "Native channel"}, {"encryption", false}, {"unread_count", 0},
                {"active_unread_count", 0}, {"passive_unread_count", 0},
                {"last_message_uuid", last}, {"updated_at", "2026-01-02T10:00:05Z"}}};
        };
        test.topics = [&](const Request &) {
            return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"name", "Native topic"}, {"is_done", false},
                {"is_default", false}, {"unread_count", 0}, {"active_unread_count", 0},
                {"passive_unread_count", 0}, {"last_message_uuid", last},
                {"updated_at", "2026-01-02T10:00:05Z"}}}};
        };
        test.messages = [&](const Request &) {
            auto row = message("Alpha   line\n\nsecond line", last);
            row["created_at"] = "2026-01-02T10:00:05.000Z";
            return Response{QJsonArray{row}};
        };
        QVERIFY(test.start());
        const auto topicRow = test.controller.topics().first().toMap();
        QCOMPARE(topicRow.value("previewSender").toString(), QStringLiteral("You"));
        QCOMPARE(topicRow.value("previewText").toString(), QStringLiteral("Alpha line second line"));
        QVERIFY(!topicRow.value("previewLocked").toBool());
        // The row time is the last-message instant under the same formatter the contract
        // case pins; asserting against the current date keeps this case locale-correct.
        QCOMPARE(topicRow.value("rowTime").toString(), NativeController::formatRowTime(
            QDateTime(QDate(2026, 1, 2), QTime(10, 0, 5), Qt::UTC), QLocale::system(),
            QDate::currentDate(), QStringLiteral("Yesterday")));
        const auto streamRow = test.controller.streams().first().toMap();
        QCOMPARE(streamRow.value("previewText").toString(), QStringLiteral("Alpha line second line"));
        QCOMPARE(streamRow.value("rowTime").toString(), topicRow.value("rowTime").toString());
    }

    // A row whose last message is an E2EE payload the client cannot present previews as the
    // locked label branch (workspace_ui sidebar-workspace-message-preview.ui.tsx), so the
    // projection must not leak cipher text and must flag the row.
    void sidebarRowsMarkUnreadableEncryptedPreview()
    {
        Fixture test;
        test.encrypted = true;
        const auto last = QStringLiteral("88888888-8888-4888-8888-000000000abd");
        test.streams = [&](const Request &request) {
            if (request.method == "GET" && request.target.path().endsWith("/streams/"))
                return Response{QJsonArray{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Native channel"}, {"encryption", true}, {"unread_count", 0},
                    {"active_unread_count", 0}, {"passive_unread_count", 0},
                    {"last_message_uuid", last}, {"updated_at", "2026-01-02T10:00:05Z"}}}};
            return Response{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                {"name", "Native channel"}, {"encryption", true}, {"unread_count", 0},
                {"active_unread_count", 0}, {"passive_unread_count", 0},
                {"last_message_uuid", last}, {"updated_at", "2026-01-02T10:00:05Z"}}};
        };
        test.messages = [&](const Request &) {
            auto row = message(QStringLiteral("AQIDZmFrZWxvY2tlZHBheWxvYWQ="), last);
            row["created_at"] = "2026-01-02T10:00:05.000Z";
            row["payload"] = QJsonObject{{"kind", "e2ee"},
                {"key_uuid", QStringLiteral("66666666-6666-4666-8666-666666666666")},
                {"content", "AQIDZmFrZWxvY2tlZHBheWxvYWQ="}};
            return Response{QJsonArray{row}};
        };
        // Both topic DTOs must carry the same last message or the row loses its pointer
        // before the projection runs.
        test.topics = [&](const Request &) {
            return Response{QJsonArray{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"name", "Native topic"}, {"is_done", false},
                {"is_default", false}, {"unread_count", 0}, {"active_unread_count", 0},
                {"passive_unread_count", 0}, {"last_message_uuid", last},
                {"updated_at", "2026-01-02T10:00:05Z"}}}};
        };
        test.counters = [&](const Request &request) {
            const auto path = request.target.path();
            if (path.endsWith("/streams/" + streamId))
                return Response{QJsonObject{{"uuid", streamId}, {"project_id", projectId},
                    {"name", "Native channel"}, {"encryption", true}, {"unread_count", 0},
                    {"active_unread_count", 0}, {"passive_unread_count", 0},
                    {"last_message_uuid", last}, {"updated_at", "2026-01-02T10:00:05Z"}}};
            return Response{QJsonObject{{"uuid", topicId}, {"project_id", projectId},
                {"stream_uuid", streamId}, {"name", "Native topic"}, {"is_done", false},
                {"is_default", false}, {"unread_count", 0}, {"active_unread_count", 0},
                {"passive_unread_count", 0}, {"last_message_uuid", last},
                {"updated_at", "2026-01-02T10:00:05Z"}}};
        };
        QVERIFY(test.start());
        const auto row = test.controller.topics().first().toMap();
        QVERIFY2(row.value("previewLocked").toBool(), qPrintable(row.value("previewText").toString()));
        QVERIFY2(!row.value("previewText").toString().contains(QStringLiteral("ZmFrZQ")),
            qPrintable(row.value("previewText").toString()));
    }

    // docs/NATIVE_DESKTOP_SIDEBAR_PREVIEWS.md section 2: a row whose last message the client
    // does not hold in memory is filled asynchronously from the disk cache by the bounded
    // pump, and the projection re-emits once the body resolves. This is the same best-effort
    // rule the reference applies to messages it has not hydrated.
    void sidebarPreviewsHydrateFromTheCacheAsynchronously()
    {
        Fixture test;
        const auto hidden = QStringLiteral("88888888-8888-4888-8888-000000000b00");
        const auto second = QStringLiteral("33333333-3333-4333-8333-666666666666");
        const auto streamRow = [](const QString &uuid, const QString &name, const QJsonValue &last) {
            return QJsonObject{{"uuid", uuid}, {"project_id", projectId}, {"name", name},
                {"encryption", false}, {"unread_count", 0}, {"active_unread_count", 0},
                {"passive_unread_count", 0}, {"last_message_uuid", last},
                {"updated_at", "2026-01-02T10:00:05Z"}};
        };
        test.streams = [&](const Request &request) {
            if (request.method == "GET" && request.target.path().endsWith("/streams/"))
                return Response{QJsonArray{streamRow(streamId, "Native channel", QJsonValue::Null),
                    streamRow(second, "Cached channel", QString(hidden))}};
            return Response{streamRow(streamId, "Native channel", QJsonValue::Null)};
        };
        // The hidden message is never served to client memory: only the cache holds it.
        test.messages = [](const Request &) { return Response{QJsonArray{}}; };
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativeCacheService cache(directory.path(), {}, true);
        test.controller.useCache(&cache);
        // The scene has two streams, so the single-stream start() helper does not apply;
        // the login flow is driven the way the multi-stream order case drives it.
        test.controller.login(test.server.origin().toString(), "synthetic-user", "synthetic-password", "");
        QTRY_COMPARE(test.controller.projects().size(), 1);
        test.controller.openProject(projectId);
        QTRY_COMPARE(test.controller.streams().size(), 2);
        auto cached = message("Cached preview body", hidden);
        // The body row belongs to the captured owner, like every real Body write.
        cached["user_uuid"] = userId;
        bool stored = false;
        QVERIFY(cache.putMessageBody(cache.lease(), cached, &cache, [&stored](bool okay) { stored = okay; }));
        QVERIFY(QTest::qWaitFor([&stored] { return stored; }, 2000));
        // The first projections ran before the body existed, so the pump must resolve the
        // pointer through the cache and re-emit the row without another snapshot.
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            for (const auto &entry : test.controller.streams()) {
                const auto row = entry.toMap();
                if (row.value("id").toString() == second)
                    return row.value("previewText").toString() == QStringLiteral("Cached preview body")
                        && !row.value("rowTime").toString().isEmpty();
            }
            return false;
        })(), 5000);
        // A message the cache also lacks keeps the empty preview line instead of a spinner
        // or a stale value; the negative lookup is remembered, so projections stay silent.
        const auto unknown = QStringLiteral("88888888-8888-4888-8888-000000000b99");
        test.streams = [&](const Request &request) {
            if (request.method == "GET" && request.target.path().endsWith("/streams/"))
                return Response{QJsonArray{streamRow(streamId, "Native channel", QJsonValue::Null),
                    streamRow(second, "Cached channel", QString(hidden)),
                    streamRow(unknown, "Unknown channel", QStringLiteral("99999999-9999-4999-8999-999999999999"))}};
            return Response{streamRow(streamId, "Native channel", QJsonValue::Null)};
        };
        test.controller.refresh();
        QTRY_COMPARE(test.controller.streams().size(), 3);
        for (const auto &entry : test.controller.streams()) {
            const auto row = entry.toMap();
            if (row.value("id").toString() == unknown) {
                QCOMPARE(row.value("previewText").toString(), QString());
                QCOMPARE(row.value("rowTime").toString(), QString());
            }
        }
    }

    // --- media viewer (issue #8, docs/NATIVE_DESKTOP_MEDIA_VIEWER.md) ---

    void mediaViewerGalleryCollectsImageSlotsAndRejectsUnknownFile()
    {
        Fixture fixture;
        QVERIFY(fixture.start());
        auto *viewer = fixture.controller.mediaViewer();
        QVERIFY(viewer);
        QVariantList rows;
        rows << mediaViewerRow("m1", mvFileA)
             << mediaViewerRow("m2", mvFileB)
             << mediaViewerRow("m3", mvFileC, QStringLiteral("e2ee"))
             << mediaViewerRow("m4", mvFileA)
             << QVariantMap{{"id", "m5"}, {"kind", "markdown"}, {"content", "No media here"}, {"displayContent", "No media here"}};
        QVERIFY(!viewer->openFromMessages(rows, QStringLiteral("dddddddd-0000-4000-8000-00000000dd04")));
        QVERIFY(!viewer->open());
        QVERIFY(!viewer->openFromMessages(QVariantList{}, mvFileA));
        QVERIFY(viewer->openFromMessages(rows, mvFileB));
        QVERIFY(viewer->open());
        QCOMPARE(viewer->count(), 2);
        QCOMPARE(viewer->index(), 1);
        const auto items = viewer->items();
        QCOMPARE(items.size(), 2);
        QCOMPARE(items.at(0).toMap().value("uuid").toString(), mvFileA);
        QCOMPARE(items.at(1).toMap().value("uuid").toString(), mvFileB);
        // Reopening replaces the session and re-anchors the start index.
        QVERIFY(viewer->openFromMessages(rows, mvFileA));
        QVERIFY(viewer->open());
        QCOMPARE(viewer->count(), 2);
        QCOMPARE(viewer->index(), 0);
        viewer->close();
        QVERIFY(!viewer->open());
        QCOMPARE(viewer->count(), 0);
    }

    void mediaViewerNavigationClampsAtBothEndsWithoutWrapAround()
    {
        Fixture fixture;
        QVERIFY(fixture.start());
        auto *viewer = fixture.controller.mediaViewer();
        QVariantList rows;
        rows << mediaViewerRow("m1", mvFileA) << mediaViewerRow("m2", mvFileB) << mediaViewerRow("m3", mvFileC);
        QVERIFY(viewer->openFromMessages(rows, mvFileB));
        QCOMPARE(viewer->index(), 1);
        QVERIFY(viewer->canPrev());
        QVERIFY(viewer->canNext());
        viewer->prev();
        QCOMPARE(viewer->index(), 0);
        QVERIFY(!viewer->canPrev());
        viewer->prev();
        QCOMPARE(viewer->index(), 0);
        viewer->goTo(9);
        QCOMPARE(viewer->index(), 2);
        QVERIFY(!viewer->canNext());
        viewer->next();
        QCOMPARE(viewer->index(), 2);
        viewer->goTo(-4);
        QCOMPARE(viewer->index(), 0);
    }

    void mediaViewerSlotsStageBytesAndMarkFailures()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray bytes("PNG");
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        fixture.files = [&](const Request &request) {
            const auto path = request.target.path();
            if (path.contains(mvFileB))
                return Response{QJsonObject{}, 500};
            if (path.endsWith("/actions/download")) {
                Response result;
                result.rawBody = bytes;
                result.rawContentType = "image/png";
                return result;
            }
            return Response{QJsonObject{{"uuid", mvFileA}, {"stream_uuid", streamId}, {"name", "m1.png"},
                {"content_type", "image/png"}, {"size_bytes", bytes.size()}, {"hash", sha}}};
        };
        QVERIFY(fixture.start());
        auto *viewer = fixture.controller.mediaViewer();
        QVariantList rows;
        rows << mediaViewerRow("m1", mvFileA) << mediaViewerRow("m2", mvFileB);
        QVERIFY(viewer->openFromMessages(rows, mvFileA));
        QCOMPARE(viewer->items().at(0).toMap().value("state").toString(), QStringLiteral("loading"));
        QTRY_COMPARE_WITH_TIMEOUT(viewer->items().at(0).toMap().value("state").toString(), QStringLiteral("ready"), 4000);
        QTRY_COMPARE_WITH_TIMEOUT(viewer->items().at(1).toMap().value("state").toString(), QStringLiteral("error"), 4000);
        const auto ready = viewer->items().at(0).toMap();
        const QUrl url(ready.value("url").toString());
        QVERIFY(url.isLocalFile());
        QFile staged(url.toLocalFile());
        QVERIFY(staged.open(QIODevice::ReadOnly));
        QCOMPARE(staged.readAll(), bytes);
        QCOMPARE(ready.value("name").toString(), QStringLiteral("m1.png"));
        QCOMPARE(viewer->items().at(1).toMap().value("url").toString(), QString());
    }

    void mediaViewerRetiredSessionIgnoresLateSlotCompletions()
    {
        Fixture fixture;
        FileClient files(&fixture.api);
        fixture.controller.useFiles(&files);
        const QByteArray bytes("PNG");
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        QList<std::function<void()>> releasedA, releasedB;
        fixture.files = [&](const Request &request) {
            const auto path = request.target.path();
            if (path.endsWith("/actions/download")) {
                Response result;
                result.rawBody = bytes;
                result.rawContentType = "image/png";
                return result;
            }
            const auto isB = path.contains(mvFileB);
            Response result{QJsonObject{{"uuid", isB ? mvFileB : mvFileA}, {"stream_uuid", streamId},
                {"name", isB ? QStringLiteral("b.png") : QStringLiteral("a.png")},
                {"content_type", "image/png"}, {"size_bytes", bytes.size()}, {"hash", sha}}};
            auto &sink = isB ? releasedB : releasedA;
            result.defer = [&sink](std::function<void()> send) { sink.append(std::move(send)); };
            return result;
        };
        QVERIFY(fixture.start());
        auto *viewer = fixture.controller.mediaViewer();
        QVariantList firstRows;
        firstRows << mediaViewerRow("m1", mvFileA);
        QVERIFY(viewer->openFromMessages(firstRows, mvFileA));
        QTRY_VERIFY_WITH_TIMEOUT(!releasedA.isEmpty(), 2000);
        // Retire the session while its metadata response is still held...
        viewer->close();
        QVERIFY(!viewer->open());
        // ...and open a new session on another file before releasing it.
        QVariantList secondRows;
        secondRows << mediaViewerRow("m2", mvFileB);
        QVERIFY(viewer->openFromMessages(secondRows, mvFileB));
        QCOMPARE(viewer->count(), 1);
        while (!releasedA.isEmpty())
            releasedA.takeFirst()();
        QTest::qWait(250);
        // A completion from the retired session must not touch the new slots.
        QCOMPARE(viewer->count(), 1);
        QCOMPARE(viewer->index(), 0);
        QCOMPARE(viewer->items().at(0).toMap().value("state").toString(), QStringLiteral("loading"));
        QTRY_VERIFY_WITH_TIMEOUT(!releasedB.isEmpty(), 2000);
        while (!releasedB.isEmpty())
            releasedB.takeFirst()();
        QTRY_COMPARE_WITH_TIMEOUT(viewer->items().at(0).toMap().value("state").toString(), QStringLiteral("ready"), 4000);
        QCOMPARE(viewer->items().at(0).toMap().value("name").toString(), QStringLiteral("b.png"));
    }

    void mediaViewerClosesWhenSessionOrConversationEnds()
    {
        Fixture fixture;
        QVERIFY(fixture.start());
        auto *viewer = fixture.controller.mediaViewer();
        QVariantList rows;
        rows << mediaViewerRow("m1", mvFileA);
        QVERIFY(viewer->openFromMessages(rows, mvFileA));
        QVERIFY(viewer->open());
        fixture.controller.logout();
        QTRY_VERIFY_WITH_TIMEOUT(!viewer->open(), 2000);
    }
};

QTEST_MAIN(NativeControllerTest)
#include "native-controller-test.moc"
